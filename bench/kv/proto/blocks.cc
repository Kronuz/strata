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
#include "../kvgen.hh"
#include "termdict.hh"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;

enum class codec { none, lz4, zstd1, zstd3 };

codec parse_codec(std::string_view s) {
	if (s == "none") { return codec::none; }
	if (s == "lz4") { return codec::lz4; }
	if (s == "zstd1") { return codec::zstd1; }
	if (s == "zstd3") { return codec::zstd3; }
	throw std::invalid_argument("codec");
}

void put_leb(std::string& out, std::uint64_t v) {
	while (v >= 0x80) {
		out.push_back(char(v | 0x80));
		v >>= 7;
	}
	out.push_back(char(v));
}
std::uint64_t get_leb(const char*& p) {
	std::uint64_t v = 0;
	for (unsigned shift = 0;; shift += 7) {
		const auto b = std::uint8_t(*p++);
		v |= std::uint64_t(b & 0x7F) << shift;
		if (b < 0x80) { return v; }
	}
}

ZSTD_CCtx* cctx() {
	static ZSTD_CCtx* c = ZSTD_createCCtx();
	return c;
}
ZSTD_DCtx* dctx() {
	static ZSTD_DCtx* d = ZSTD_createDCtx();
	return d;
}

// Stored as is when the codec saves less than an eighth, as RocksDB does; the first byte says.
void compress(codec c, std::string_view raw, std::string& out) {
	const std::size_t at = out.size();
	out.push_back(char(c));
	std::size_t n = 0;
	switch (c) {
	case codec::none: break;
	case codec::lz4: {
		const int cap = LZ4_compressBound(int(raw.size()));
		out.resize(at + 1 + std::size_t(cap));
		n = std::size_t(LZ4_compress_default(raw.data(), out.data() + at + 1, int(raw.size()), cap));
		break;
	}
	case codec::zstd1:
	case codec::zstd3: {
		const std::size_t cap = ZSTD_compressBound(raw.size());
		out.resize(at + 1 + cap);
		n = ZSTD_compressCCtx(cctx(), out.data() + at + 1, cap, raw.data(), raw.size(), c == codec::zstd1 ? 1 : 3);
		if (ZSTD_isError(n)) { throw std::runtime_error("zstd"); }
		break;
	}
	}
	if (c == codec::none || n == 0 || n > raw.size() - raw.size() / 8) {
		out.resize(at);
		out.push_back(char(codec::none));
		out.append(raw);
	} else {
		out.resize(at + 1 + n);
	}
}

// Decodes into `buf`, whose capacity is reused across calls; returns the raw bytes. A block
// stored raw is read in place.
std::string_view decompress(std::string_view stored, std::size_t raw_size, std::string& buf) {
	const auto c = codec(stored[0]);
	stored.remove_prefix(1);
	switch (c) {
	case codec::none: return stored;
	case codec::lz4:
		buf.resize(raw_size);
		if (LZ4_decompress_safe(stored.data(), buf.data(), int(stored.size()), int(raw_size)) != int(raw_size)) { throw std::runtime_error("lz4"); }
		return buf;
	case codec::zstd1:
	case codec::zstd3: {
		buf.resize(raw_size);
		const std::size_t n = ZSTD_decompressDCtx(dctx(), buf.data(), raw_size, stored.data(), stored.size());
		if (n != raw_size) { throw std::runtime_error("zstd"); }
		return buf;
	}
	}
	return {};
}

struct block_ref {
	std::uint64_t offset;
	std::uint32_t stored, raw;
};

// --- inline: keys and values together --------------------------------------------------------

struct inline_table {
	std::string data;
	std::vector<block_ref> blocks;
	std::string first_keys;            // the blocks' first keys, back to back
	std::vector<std::uint32_t> first;  // where each first key starts in first_keys, plus the end
	std::string_view first_key(std::size_t b) const { return std::string_view(first_keys).substr(first[b], first[b + 1] - first[b]); }
	std::uint64_t index_bytes() const { return first_keys.size() + blocks.size() * 12; }

	// The block that may hold key: the last whose first key is <= key.
	std::optional<std::size_t> block_of(std::string_view key) const {
		std::size_t lo = 0, hi = blocks.size();
		if (hi == 0 || key < first_key(0)) { return std::nullopt; }
		while (hi - lo > 1) {
			const std::size_t mid = (lo + hi) / 2;
			if (first_key(mid) <= key) { lo = mid; } else { hi = mid; }
		}
		return lo;
	}
};

// A decoded inline block: entries, then restart offsets (u32 each), then their count (u32).
struct inline_block {
	std::string_view raw;
	const char* entries_end = nullptr;
	const char* restarts = nullptr;
	std::uint32_t n_restarts = 0;
	void parse(std::string_view r) {
		raw = r;
		std::memcpy(&n_restarts, r.data() + r.size() - 4, 4);
		restarts = r.data() + r.size() - 4 - 4 * std::size_t(n_restarts);
		entries_end = restarts;
	}
	const char* restart(std::uint32_t i) const {
		std::uint32_t off;
		std::memcpy(&off, restarts + 4 * std::size_t(i), 4);
		return raw.data() + off;
	}
	// Entry at p: shared, unshared, value length, then the suffix and the value.
	static std::string_view restart_key(const char* p) {
		get_leb(p);  // shared: 0 at a restart
		const std::uint64_t unshared = get_leb(p);
		get_leb(p);
		return {p, std::size_t(unshared)};
	}
	std::optional<std::string_view> find(std::string_view key, std::string& scratch) const {
		std::uint32_t lo = 0, hi = n_restarts;
		while (hi - lo > 1) {
			const std::uint32_t mid = (lo + hi) / 2;
			if (restart_key(restart(mid)) <= key) { lo = mid; } else { hi = mid; }
		}
		const char* p = restart(lo);
		const char* stop = lo + 1 < n_restarts ? restart(lo + 1) : entries_end;
		scratch.clear();
		while (p < stop) {
			const std::uint64_t shared = get_leb(p), unshared = get_leb(p), vlen = get_leb(p);
			scratch.resize(shared);
			scratch.append(p, unshared);
			p += unshared;
			const int cmp = std::string_view(scratch).compare(key);
			if (cmp == 0) { return std::string_view(p, vlen); }
			if (cmp > 0) { return std::nullopt; }
			p += vlen;
		}
		return std::nullopt;
	}
	// Visits every entry from the first key >= from, until f returns false; false if it did.
	template <class F>
	bool walk(std::string_view from, std::string& scratch, F&& f) const {
		const char* p = raw.data();
		if (!from.empty()) {
			std::uint32_t lo = 0, hi = n_restarts;
			while (hi - lo > 1) {
				const std::uint32_t mid = (lo + hi) / 2;
				if (restart_key(restart(mid)) <= from) { lo = mid; } else { hi = mid; }
			}
			p = restart(lo);
		}
		scratch.clear();
		while (p < entries_end) {
			const std::uint64_t shared = get_leb(p), unshared = get_leb(p), vlen = get_leb(p);
			scratch.resize(shared);
			scratch.append(p, unshared);
			p += unshared;
			const std::string_view v(p, vlen);
			p += vlen;
			if (std::string_view(scratch) < from) { continue; }
			if (!f(std::string_view(scratch), v)) { return false; }
		}
		return true;
	}
};

inline_table build_inline(const std::vector<std::string>& keys, const std::vector<std::string_view>& values, std::size_t block_bytes, std::uint32_t restart_every, codec c) {
	inline_table t;
	std::string raw, last;
	std::vector<std::uint32_t> restarts;
	std::uint32_t in_block = 0;
	auto cut = [&] {
		for (std::uint32_t r : restarts) { raw.append(reinterpret_cast<const char*>(&r), 4); }
		const auto n = std::uint32_t(restarts.size());
		raw.append(reinterpret_cast<const char*>(&n), 4);
		const std::uint64_t at = t.data.size();
		compress(c, raw, t.data);
		t.blocks.push_back({at, std::uint32_t(t.data.size() - at), std::uint32_t(raw.size())});
		raw.clear();
		restarts.clear();
		in_block = 0;
	};
	t.first.push_back(0);
	for (std::size_t i = 0; i < keys.size(); ++i) {
		const std::string_view k = keys[i], v = values[i];
		const std::size_t entry = 3 + k.size() + v.size();
		if (in_block && raw.size() + entry + 4 * (restarts.size() + 2) > block_bytes) { cut(); }
		if (in_block == 0) {
			t.first_keys += k;
			t.first.push_back(std::uint32_t(t.first_keys.size()));
		}
		std::size_t shared = 0;
		if (in_block % restart_every == 0) {
			restarts.push_back(std::uint32_t(raw.size()));
		} else {
			while (shared < k.size() && shared < last.size() && k[shared] == last[shared]) { ++shared; }
		}
		put_leb(raw, shared);
		put_leb(raw, k.size() - shared);
		put_leb(raw, v.size());
		raw.append(k.substr(shared));
		raw.append(v);
		last.assign(k);
		++in_block;
	}
	if (in_block) { cut(); }
	return t;
}

// --- split: a termdict over the keys, values alone by ordinal ---------------------------------

struct split_table {
	termdict::tables keys;
	std::string data;
	std::vector<block_ref> blocks;
	std::vector<std::uint32_t> first;  // each block's first ordinal
	std::uint64_t index_bytes() const { return blocks.size() * 16; }
	std::size_t block_of(std::uint32_t ord) const { return std::size_t(std::upper_bound(first.begin(), first.end(), ord) - first.begin()) - 1; }
};

// A decoded value block: [count][lengths][values]; starts is reused across decodes.
struct split_block {
	std::uint32_t first = 0;
	std::vector<std::uint32_t> starts;
	std::string_view raw;
	void parse(std::string_view r, std::uint32_t first_ord) {
		raw = r;
		first = first_ord;
		const char* p = r.data();
		const std::uint64_t n = get_leb(p);
		starts.resize(n + 1);
		std::uint32_t at = 0;
		for (std::uint64_t i = 0; i < n; ++i) {
			starts[i] = at;
			at += std::uint32_t(get_leb(p));
		}
		starts[n] = at;
		const auto header = std::uint32_t(p - r.data());
		for (auto& s : starts) { s += header; }
	}
	std::string_view value(std::uint32_t ord) const {
		const std::uint32_t i = ord - first;
		return raw.substr(starts[i], starts[i + 1] - starts[i]);
	}
	std::uint32_t count() const { return std::uint32_t(starts.size() - 1); }
};

split_table build_split(const std::vector<std::string>& keys, const std::vector<std::string_view>& values, std::size_t block_bytes, codec c) {
	split_table t;
	std::string alphabet;
	{
		bool used[256] = {};
		for (const auto& k : keys) {
			for (char ch : k) { used[std::uint8_t(ch)] = true; }
		}
		for (unsigned ch = 0; ch < 256; ++ch) {
			if (used[ch]) { alphabet.push_back(char(ch)); }
		}
	}
	termdict::builder b(alphabet);
	std::string v;
	for (std::size_t i = 0; i < keys.size(); ++i) {
		v.clear();
		put_leb(v, i);
		b.add(keys[i], v);
	}
	t.keys = b.finish();
	std::string raw;
	for (std::size_t first = 0; first < values.size();) {
		std::size_t last = first, bytes = 0;
		while (last < values.size() && (last == first || bytes + values[last].size() + 2 <= block_bytes)) { bytes += values[last++].size() + 2; }
		raw.clear();
		put_leb(raw, last - first);
		for (std::size_t i = first; i < last; ++i) { put_leb(raw, values[i].size()); }
		for (std::size_t i = first; i < last; ++i) { raw.append(values[i]); }
		const std::uint64_t at = t.data.size();
		compress(c, raw, t.data);
		t.blocks.push_back({at, std::uint32_t(t.data.size() - at), std::uint32_t(raw.size())});
		t.first.push_back(std::uint32_t(first));
		first = last;
	}
	return t;
}

std::uint32_t ordinal_of(std::string_view v) {
	const char* p = v.data();
	return std::uint32_t(get_leb(p));
}

// --- frame64: format 13's stored frames -------------------------------------------------------

struct frame_table {
	std::string data;
	std::vector<block_ref> frames;
	std::vector<std::uint32_t> first;
};

frame_table build_frames(const std::vector<std::string_view>& values) {
	frame_table t;
	std::string raw;
	for (std::size_t first = 0; first < values.size();) {
		std::size_t last = first, bytes = 0;
		while (last < values.size() && (last == first || bytes + values[last].size() <= 64 * 1024)) { bytes += values[last++].size(); }
		raw.clear();
		put_leb(raw, last - first);
		for (std::size_t i = first; i < last; ++i) { put_leb(raw, values[i].size()); }
		for (std::size_t i = first; i < last; ++i) { raw.append(values[i]); }
		const std::uint64_t at = t.data.size();
		compress(codec::zstd3, raw, t.data);
		t.frames.push_back({at, std::uint32_t(t.data.size() - at), std::uint32_t(raw.size())});
		t.first.push_back(std::uint32_t(first));
		first = last;
	}
	return t;
}

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
