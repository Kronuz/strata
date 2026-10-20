// format-14-kv D1's micro-prototype (docs/design/format-14-kv.md): key-ordered blocks of a
// chosen size and codec, read the way a point get and a scan would read them.
//
// Layouts:
//   inline  RocksDB's shape: each block holds keys and values, keys prefix-coded against the
//           previous entry, a restart every --restart entries, the restart offsets at the end;
//           a pinned array of each block's first key finds the block.
//   split   strata's shape kept: the keys in a termdict (format 13's K1: each value the key's
//           ordinal), the values alone in blocks by ordinal ([count][lengths][values], LEB128);
//           ordinal = key rank, so the value blocks are key-ordered too.
//   frame64 format 13 today: values in zstd frames of up to 64 KiB, each get decoding its
//           frame into a fresh std::string (the measured loss's baseline); frame64ctx also
//           creates a zstd decompression context per get, as ZSTD_decompress does.
//
// Per configuration it prints one JSON line: bytes, build cost, a get on a decoded block
// (what a cache hit costs past the lookup), a get that decodes its block into a reused buffer
// (a miss served from the page cache), a seek of 10, and a full scan from the compressed bytes.
#include "blocks.hh"
#include "../kvgen.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kvproto;
using clk = std::chrono::steady_clock;

// Runs batches of `batch` calls of f(i) until at least `ms` milliseconds pass; ns per call.
template <class F>
double timed(double ms, std::size_t batch, F&& f) {
	std::size_t calls = 0;
	const auto start = clk::now();
	double elapsed = 0;
	do {
		for (std::size_t i = 0; i < batch; ++i) { f(calls + i); }
		calls += batch;
		elapsed = std::chrono::duration<double, std::milli>(clk::now() - start).count();
	} while (elapsed < ms);
	return elapsed * 1e6 / double(calls);
}

volatile std::uint64_t sink_;

}  // namespace

int main(int argc, char** argv) {
	std::uint64_t records = 1'000'000, value_size = 100, seed = 1;
	std::size_t block_bytes = 4096;
	std::uint32_t restart_every = 16;
	std::string layout = "inline", codec_name = "lz4";
	double ms = 1500;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view a = argv[i];
		const char* v = argv[i + 1];
		if (a == "--records") { records = std::strtoull(v, nullptr, 10); }
		else if (a == "--value") { value_size = std::strtoull(v, nullptr, 10); }
		else if (a == "--block") { block_bytes = std::strtoull(v, nullptr, 10); }
		else if (a == "--restart") { restart_every = std::uint32_t(std::strtoul(v, nullptr, 10)); }
		else if (a == "--layout") { layout = v; }
		else if (a == "--codec") { codec_name = v; }
		else if (a == "--ms") { ms = std::strtod(v, nullptr); }
		else if (a == "--seed") { seed = std::strtoull(v, nullptr, 10); }
		else { std::fprintf(stderr, "unknown %s\n", argv[i]); return 2; }
	}
	const codec c = parse_codec(codec_name);
	const auto t0 = clk::now();

	// Records in key order: ids scatter over the key space, so sorting keys sorts records.
	std::vector<std::uint64_t> ids(records);
	for (std::uint64_t i = 0; i < records; ++i) { ids[i] = i; }
	std::sort(ids.begin(), ids.end(), [](std::uint64_t a, std::uint64_t b) { return kvbench::scatter(a) < kvbench::scatter(b); });
	std::vector<std::string> keys(records);
	std::vector<std::string_view> values(records);
	std::string value_bytes;
	value_bytes.reserve(records * value_size);
	kvbench::values gen(seed);
	for (std::uint64_t i = 0; i < records; ++i) {
		keys[i] = kvbench::key_of(ids[i]);
		value_bytes.append(gen.next(value_size));
	}
	for (std::uint64_t i = 0; i < records; ++i) { values[i] = std::string_view(value_bytes).substr(i * value_size, value_size); }
	const std::uint64_t logical = records * (kvbench::key_size + value_size);

	// Random probes: present keys (uniform), and starts for seeks.
	kvbench::rng r(seed * 31 + 7);
	std::vector<std::uint32_t> probe(1 << 20);
	for (auto& p : probe) { p = std::uint32_t(r.below(records)); }
	const std::size_t mask = probe.size() - 1;

	double build_ns = 0, hit_ns = 0, miss_ns = 0, seek_ns = 0, scan_ns = 0, key_ns = 0;
	std::uint64_t bytes = 0, index_bytes = 0, key_bytes = 0, block_count = 0;
	std::string buf, scratch;

	if (layout == "inline") {
		const auto b0 = clk::now();
		const inline_table t = build_inline(keys, values, block_bytes, restart_every, c);
		build_ns = std::chrono::duration<double, std::nano>(clk::now() - b0).count() / double(records);
		bytes = t.data.size();
		index_bytes = t.index_bytes();
		block_count = t.blocks.size();
		// Every block decoded once: what a cache holds.
		std::vector<std::string> decoded(t.blocks.size());
		std::vector<inline_block> parsed(t.blocks.size());
		for (std::size_t b = 0; b < t.blocks.size(); ++b) {
			const auto& br = t.blocks[b];
			decoded[b] = std::string(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf));
			parsed[b].parse(decoded[b]);
		}
		hit_ns = timed(ms, 1024, [&](std::size_t i) {
			const std::string& k = keys[probe[i & mask]];
			const auto b = t.block_of(k);
			const auto v = parsed[*b].find(k, scratch);
			sink_ += std::uint8_t(v->front());
		});
		miss_ns = timed(ms, 256, [&](std::size_t i) {
			const std::string& k = keys[probe[i & mask]];
			const auto b = *t.block_of(k);
			const auto& br = t.blocks[b];
			inline_block blk;
			blk.parse(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf));
			const auto v = blk.find(k, scratch);
			sink_ += std::uint8_t(v->front());
		});
		// A seek of 10 over decoded blocks (cache hits), crossing a block when it must.
		seek_ns = timed(ms, 256, [&](std::size_t i) {
			const std::string& from = keys[probe[i & mask]];
			std::size_t b = *t.block_of(from);
			int left = 10;
			std::string_view lower = from;
			while (left > 0 && b < parsed.size()) {
				parsed[b].walk(lower, scratch, [&](std::string_view, std::string_view v) {
					sink_ += std::uint8_t(v.front());
					return --left > 0;
				});
				lower = {};
				++b;
			}
		});
		// A full scan from the compressed bytes, as readseq's cold blocks.
		const auto s0 = clk::now();
		for (const auto& br : t.blocks) {
			inline_block blk;
			blk.parse(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf));
			blk.walk({}, scratch, [&](std::string_view, std::string_view v) {
				sink_ += std::uint8_t(v.front());
				return true;
			});
		}
		scan_ns = std::chrono::duration<double, std::nano>(clk::now() - s0).count() / double(records);
	} else if (layout == "split") {
		const auto b0 = clk::now();
		const split_table t = build_split(keys, values, block_bytes, c);
		build_ns = std::chrono::duration<double, std::nano>(clk::now() - b0).count() / double(records);
		bytes = t.data.size();
		index_bytes = t.index_bytes();
		key_bytes = t.keys.blocks.size() + t.keys.index.size();
		block_count = t.blocks.size();
		const termdict::reader dict(t.keys.blocks, t.keys.index);
		std::vector<std::string> decoded(t.blocks.size());
		std::vector<split_block> parsed(t.blocks.size());
		for (std::size_t b = 0; b < t.blocks.size(); ++b) {
			const auto& br = t.blocks[b];
			decoded[b] = std::string(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf));
			parsed[b].parse(decoded[b], t.first[b]);
		}
		key_ns = timed(ms, 1024, [&](std::size_t i) { sink_ += ordinal_of(*dict.find(keys[probe[i & mask]])); });
		hit_ns = timed(ms, 1024, [&](std::size_t i) {
			const std::uint32_t ord = ordinal_of(*dict.find(keys[probe[i & mask]]));
			const auto v = parsed[t.block_of(ord)].value(ord);
			sink_ += std::uint8_t(v.front());
		});
		split_block blk;
		miss_ns = timed(ms, 256, [&](std::size_t i) {
			const std::uint32_t ord = ordinal_of(*dict.find(keys[probe[i & mask]]));
			const std::size_t b = t.block_of(ord);
			const auto& br = t.blocks[b];
			blk.parse(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf), t.first[b]);
			sink_ += std::uint8_t(blk.value(ord).front());
		});
		// A seek of 10: the key dictionary's scan gives ordinals, the value blocks follow.
		seek_ns = timed(ms, 256, [&](std::size_t i) {
			int left = 10;
			dict.scan(keys[probe[i & mask]], [&](std::string_view, std::string_view v) {
				const std::uint32_t ord = ordinal_of(v);
				sink_ += std::uint8_t(parsed[t.block_of(ord)].value(ord).front());
				return --left > 0;
			});
		});
		const auto s0 = clk::now();
		termdict::reader::cursor cur(dict);
		std::size_t b = 0;
		blk.starts.clear();
		while (cur.next()) {
			const std::uint32_t ord = ordinal_of(cur.value());
			if (blk.starts.empty() || ord >= blk.first + blk.count()) {
				const auto& br = t.blocks[b];
				blk.parse(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf), t.first[b]);
				++b;
			}
			sink_ += std::uint8_t(blk.value(ord).front()) + cur.key().size();
		}
		scan_ns = std::chrono::duration<double, std::nano>(clk::now() - s0).count() / double(records);
	} else if (layout == "frame64" || layout == "frame64ctx") {
		// Values by ordinal = key rank here (the best case for a scan); a get decodes its whole
		// frame into a new string, as stored_plugin::reader::stored(ord) does.
		const auto b0 = clk::now();
		const frame_table t = build_frames(values);
		build_ns = std::chrono::duration<double, std::nano>(clk::now() - b0).count() / double(records);
		bytes = t.data.size();
		index_bytes = t.frames.size() * 16;
		block_count = t.frames.size();
		miss_ns = timed(ms, 16, [&](std::size_t i) {
			const std::uint32_t ord = probe[i & mask];
			const std::size_t f = std::size_t(std::upper_bound(t.first.begin(), t.first.end(), ord) - t.first.begin()) - 1;
			const auto& br = t.frames[f];
			std::string fresh;  // a fresh buffer per get, freed at the end, as today
			split_block blk;
			const std::string_view stored = std::string_view(t.data).substr(br.offset, br.stored);
			if (layout == "frame64ctx") {
				// As compressors' decompress_zstd: ZSTD_decompress, which creates and frees a
				// decompression context per call.
				fresh.resize(br.raw);
				if (ZSTD_decompress(fresh.data(), br.raw, stored.data() + 1, stored.size() - 1) != br.raw) { std::abort(); }
				blk.parse(fresh, t.first[f]);
			} else {
				blk.parse(decompress(stored, br.raw, fresh), t.first[f]);
			}
			sink_ += std::uint8_t(blk.value(ord).front());
		});
	} else {
		std::fprintf(stderr, "unknown layout\n");
		return 2;
	}
	const double total_s = std::chrono::duration<double>(clk::now() - t0).count();
	std::printf(
		"{\"layout\":\"%s\",\"codec\":\"%s\",\"block\":%zu,\"restart\":%u,\"records\":%llu,\"value\":%llu,\"logical\":%llu,\"bytes\":%llu,\"index_bytes\":%llu,"
		"\"key_bytes\":%llu,\"blocks\":%llu,\"build_ns\":%.1f,\"key_ns\":%.1f,\"hit_ns\":%.1f,\"miss_ns\":%.1f,\"seek10_ns\":%.1f,\"scan_ns\":%.2f,\"total_s\":%.2f}\n",
		layout.c_str(), codec_name.c_str(), block_bytes, restart_every, (unsigned long long)records, (unsigned long long)value_size, (unsigned long long)logical,
		(unsigned long long)bytes, (unsigned long long)index_bytes, (unsigned long long)key_bytes, (unsigned long long)block_count, build_ns, key_ns, hit_ns, miss_ns, seek_ns,
		scan_ns, total_s);
	return 0;
}
