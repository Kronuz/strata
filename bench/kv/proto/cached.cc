// format-14-kv Q1 and Q2's prototype (docs/design/format-14-kv/q1-q2.md): the block layouts read
// through D2's CLOCK cache at a calibrated hit rate, and the merge's CPU per record.
//
//   --mode reads  one layout and codec; per hit-rate target, the cache's capacity is calibrated
//                 so scrambled-zipfian gets reach it, then gets of present and absent keys, a seek
//                 and 10, and YCSB E's scans run through the cache (a miss decodes into a recycled
//                 buffer). Also the bytes, the build per record, and a full scan from the
//                 compressed bytes. One JSON line per target.
//   --mode merge  split only: four key-ordered sources (codec --src) merged into one output (codec
//                 --codec): keys through termdict cursors, values read by decoding each source's
//                 blocks in order, the output's termdict and value blocks written. One JSON line.
//
// Split seeks with td_cursor: a floor lookup, a binary search of the block's restarts, then a walk
// of one run by find_from's matching rule that builds no key until it reaches the target, whose key
// is the target's shared prefix plus its own suffix. termdict's reader::scan instead builds every
// key from the block's start (--seek walk reproduces it); td_cursor is the shape
// termdict::reader::cursor::seek would take.
#include "blocks.hh"
#include "clock_cache.hh"
#include "../kvgen.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kvproto;
using clk = std::chrono::steady_clock;

volatile std::uint64_t sink_;

double ns_since(clk::time_point t0) { return std::chrono::duration<double, std::nano>(clk::now() - t0).count(); }

// --- td_cursor: termdict entries in key order, from a seek target ------------------------------

class td_cursor {
public:
	td_cursor(std::string_view blocks, std::string_view index) : blocks_(blocks), map_(a_.parse(index)) {}
	td_cursor(const td_cursor&) = delete;
	td_cursor& operator=(const td_cursor&) = delete;

	// The first entry with key >= from; false if there is none. With walk, as termdict's
	// reader::scan positions today: every key from the floor block's start is built.
	bool seek(std::string_view from) {
		const auto offset = map_.floor_value(from);
		if (!offset) {
			b_.reset();
			return next();
		}
		open(*offset);
		if (walk) {
			while (next()) {
				if (std::string_view(key_) >= from) { return true; }
			}
			return false;
		}
		std::size_t lo = 0, hi = b_->restarts();
		while (hi - lo > 1) {
			const std::size_t mid = (lo + hi) / 2;
			if (b_->compare_restart(mid, from) <= 0) { lo = mid; } else { hi = mid; }
		}
		constexpr std::size_t ri = termdict::restart_interval;
		const char* p = nullptr;
		auto sfx = b_->run(lo, p);
		std::size_t m = 0;  // how much of from the previous entry matched
		const std::size_t last = std::min(b_->count(), (lo + 1) * ri);
		for (std::size_t i = lo * ri; i < last; ++i) {
			std::uint64_t shared = 0, suffix = 0, value = 0;
			termdict::block::header(p, end_, shared, suffix, value);
			sfx.start(suffix);
			const char* val = p;
			p += value;
			auto at_suffix = sfx;
			if (i != lo * ri && shared > m) {
				sfx.skip(suffix);  // sorts like its predecessor: below from
				continue;
			}
			if (i == lo * ri || shared == m) {
				const std::size_t n = std::min<std::size_t>(suffix, from.size() - m);
				std::size_t j = 0;
				char c = 0;
				for (; j < n; ++j) {
					c = sfx.next();
					if (c != from[m + j]) { break; }
				}
				const bool at_or_above = (j == suffix && m + j == from.size()) || (j < n ? std::uint8_t(c) > std::uint8_t(from[m + j]) : m + j == from.size());
				if (!at_or_above) {
					sfx.skip(suffix - j - (j < n ? 1 : 0));
					m += j;
					continue;
				}
			}
			// The target: its first `shared` bytes are from's.
			key_.assign(from.substr(0, std::size_t(shared)));
			key_.resize(std::size_t(shared + suffix));
			at_suffix.decode(suffix, key_.data() + shared);
			s_ = at_suffix;
			p_ = p;
			value_ = std::string_view(val, std::size_t(value));
			index_ = i + 1;
			return true;
		}
		index_ = last;  // the whole run sorts below from: the next entry is the target
		return next();
	}

	// The next entry; false past the last.
	bool next() {
		constexpr std::size_t ri = termdict::restart_interval;
		for (;;) {
			if (b_ && index_ < b_->count()) {
				if (index_ % ri == 0) { s_ = b_->run(index_ / ri, p_); }
				std::uint64_t shared = 0, suffix = 0, value = 0;
				termdict::block::header(p_, end_, shared, suffix, value);
				s_.start(suffix);
				key_.resize(std::size_t(shared + suffix));
				s_.decode(suffix, key_.data() + shared);
				value_ = std::string_view(p_, std::size_t(value));
				p_ += value;
				++index_;
				return true;
			}
			const std::uint64_t offset = b_ ? b_->next_block() : 0;
			if (offset >= blocks_.size()) { return false; }
			open(offset);
		}
	}

	std::string_view key() const noexcept { return key_; }
	std::string_view value() const noexcept { return value_; }

	bool walk = false;

private:
	void open(std::uint64_t offset) {
		b_.emplace(blocks_, offset, a_);
		end_ = blocks_.data() + b_->next_block();
		index_ = 0;
	}

	std::string_view blocks_;
	termdict::alphabet a_;
	fst::map map_;
	std::optional<termdict::block> b_;
	const char* end_ = nullptr;
	const char* p_ = nullptr;
	termdict::block::suffixes s_;
	std::size_t index_ = 0;
	std::string key_;
	std::string_view value_;
};

// --- the cache's values: decoded, parsed blocks from a recycling pool -------------------------

struct decoded {
	std::string buf;
	std::string_view raw;
	split_block sb;
	inline_block ib;
};

struct pool {
	std::vector<std::unique_ptr<decoded>> all;
	std::vector<decoded*> free;
	decoded* take() {
		if (free.empty()) {
			all.push_back(std::make_unique<decoded>());
			return all.back().get();
		}
		decoded* d = free.back();
		free.pop_back();
		return d;
	}
};

struct recycler {
	pool* p = nullptr;
	void operator()(decoded* d) const { p->free.push_back(d); }
};

using cache_t = clock_cache<decoded, recycler>;

struct counters {
	std::uint64_t hits = 0, misses = 0;
	void clear() { hits = misses = 0; }
	double rate() const { return hits + misses ? double(hits) / double(hits + misses) : 0; }
};

// A block through the cache: a hit takes a reference; a miss decodes into a pooled buffer.
template <class Table, class Parse>
cache_t::handle fetch(cache_t& c, pool& pl, const Table& t, std::size_t b, counters& n, Parse&& parse) {
	cache_t::handle h;
	if (c.lookup(b + 1, h)) {
		++n.hits;
		return h;
	}
	++n.misses;
	decoded* d = pl.take();
	const auto& br = t.blocks[b];
	d->raw = decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, d->buf);
	parse(*d, b);
	return c.insert(b + 1, d);
}

// --- the two layouts' readers through the cache -----------------------------------------------

struct inline_reader {
	const inline_table* t;
	cache_t* c;
	pool* pl;
	counters* n;
	std::string scratch;

	cache_t::handle get_block(std::size_t b) {
		return fetch(*c, *pl, *t, b, *n, [](decoded& d, std::size_t) { d.ib.parse(d.raw); });
	}
	std::optional<std::string_view> get(std::string_view key, cache_t::handle& h) {
		const auto b = t->block_of(key);
		if (!b) { return std::nullopt; }
		h = get_block(*b);
		return h.value()->ib.find(key, scratch);
	}

	// A cursor over the blocks, holding one.
	struct cursor {
		inline_reader* r;
		std::size_t b = 0;
		cache_t::handle h{};
		const char* p = nullptr;
		std::string k;
		std::string_view value;

		explicit cursor(inline_reader* rd) : r(rd) {}
		~cursor() { drop(); }
		std::string_view key() const { return k; }
		void drop() {
			if (h.s) { r->c->release(h); }
			h.s = nullptr;
		}
		const inline_block& blk() const { return h.value()->ib; }
		void decode() {
			const std::uint64_t shared = get_leb(p), unshared = get_leb(p), vlen = get_leb(p);
			k.resize(std::size_t(shared));
			k.append(p, std::size_t(unshared));
			p += unshared;
			value = std::string_view(p, std::size_t(vlen));
			p += vlen;
		}
		bool load(std::size_t nb) {
			drop();
			if (nb >= r->t->blocks.size()) { return false; }
			b = nb;
			h = r->get_block(b);
			p = blk().raw.data();
			k.clear();
			return true;
		}
		bool seek(std::string_view from) {
			const auto ob = r->t->block_of(from);
			if (!load(ob ? *ob : 0)) { return false; }
			if (ob) {
				std::uint32_t lo = 0, hi = blk().n_restarts;
				while (hi - lo > 1) {
					const std::uint32_t mid = (lo + hi) / 2;
					if (inline_block::restart_key(blk().restart(mid)) <= from) { lo = mid; } else { hi = mid; }
				}
				p = blk().restart(lo);
				while (p < blk().entries_end) {
					decode();
					if (std::string_view(k) >= from) { return true; }
				}
				return next();
			}
			return next();
		}
		bool next() {
			while (p >= blk().entries_end) {
				if (!load(b + 1)) { return false; }
			}
			decode();
			return true;
		}
	};
};

struct split_reader {
	const split_table* t;
	const termdict::reader* dict;
	cache_t* c;
	pool* pl;
	counters* n;
	bool walk = false;

	cache_t::handle get_block(std::size_t b) {
		return fetch(*c, *pl, *t, b, *n, [this](decoded& d, std::size_t bn) { d.sb.parse(d.raw, t->first[bn]); });
	}
	std::optional<std::string_view> get(std::string_view key, cache_t::handle& h) {
		const auto v = dict->find(key);
		if (!v) { return std::nullopt; }
		const std::uint32_t ord = ordinal_of(*v);
		h = get_block(t->block_of(ord));
		return h.value()->sb.value(ord);
	}

	struct cursor {
		split_reader* r;
		td_cursor keys;
		cache_t::handle h{};
		std::string_view value;

		explicit cursor(split_reader* rd) : r(rd), keys(rd->t->keys.blocks, rd->t->keys.index) { keys.walk = rd->walk; }
		~cursor() { drop(); }
		void drop() {
			if (h.s) { r->c->release(h); }
			h.s = nullptr;
		}
		void load() {
			const std::uint32_t ord = ordinal_of(keys.value());
			if (!h.s || ord < h.value()->sb.first || ord >= h.value()->sb.first + h.value()->sb.count()) {
				drop();
				h = r->get_block(r->t->block_of(ord));
			}
			value = h.value()->sb.value(ord);
		}
		bool seek(std::string_view from) {
			if (!keys.seek(from)) { return false; }
			load();
			return true;
		}
		bool next() {
			if (!keys.next()) { return false; }
			load();
			return true;
		}
		std::string_view key() const { return keys.key(); }
	};
};

// --- the workload ------------------------------------------------------------------------------

struct data {
	std::vector<std::string> keys;  // in key order
	std::vector<std::string_view> values;
	std::string value_bytes;
	std::vector<std::uint32_t> rank_of;  // record id -> its key's rank
	std::uint64_t logical = 0;
};

data make_data(std::uint64_t records, std::uint64_t value_size, std::uint64_t seed) {
	data d;
	std::vector<std::uint64_t> ids(records);
	for (std::uint64_t i = 0; i < records; ++i) { ids[i] = i; }
	std::sort(ids.begin(), ids.end(), [](std::uint64_t a, std::uint64_t b) { return kvbench::scatter(a) < kvbench::scatter(b); });
	d.keys.resize(records);
	d.values.resize(records);
	d.rank_of.resize(records);
	d.value_bytes.reserve(records * value_size);
	kvbench::values gen(seed);
	for (std::uint64_t i = 0; i < records; ++i) {
		d.keys[i] = kvbench::key_of(ids[i]);
		d.rank_of[ids[i]] = std::uint32_t(i);
		d.value_bytes.append(gen.next(value_size));
	}
	for (std::uint64_t i = 0; i < records; ++i) { d.values[i] = std::string_view(d.value_bytes).substr(i * value_size, value_size); }
	d.logical = records * (kvbench::key_size + value_size);
	return d;
}

struct streams {
	std::vector<std::uint32_t> warm, gets, seeks, e_from, e_len;
	std::string absent;  // 16-byte keys back to back
	std::size_t absent_count = 0;
};

streams make_streams(const data& d, std::uint64_t seed, std::size_t ops) {
	streams s;
	kvbench::rng r(seed * 131 + 17);
	kvbench::scrambled_zipfian z;
	const std::uint64_t n = d.keys.size();
	auto draw = [&](std::vector<std::uint32_t>& out, std::size_t count) {
		out.resize(count);
		for (auto& x : out) { x = d.rank_of[z.next(r, n)]; }
	};
	draw(s.warm, ops);
	draw(s.gets, ops);
	draw(s.seeks, ops / 2);
	draw(s.e_from, ops / 8);
	s.e_len.resize(s.e_from.size());
	for (auto& x : s.e_len) { x = std::uint32_t(1 + r.below(100)); }
	s.absent_count = ops;
	s.absent.resize(ops * kvbench::key_size);
	for (std::size_t i = 0; i < ops; ++i) { kvbench::make_key(kvbench::absent_base + z.next(r, n), s.absent.data() + i * kvbench::key_size); }
	return s;
}

// The smallest capacity (a multiple of 16 blocks) whose hit rate on `stream` reaches target.
std::size_t calibrate(const std::vector<std::uint32_t>& block_of_get, std::size_t blocks, double target) {
	struct nop {
		void operator()(decoded*) const {}
	};
	static decoded dummy;
	auto hit_rate = [&](std::size_t sets) {
		clock_cache<decoded, nop> c(sets * 16);
		for (std::size_t b = 0; b < blocks; ++b) {
			auto h = c.insert(b + 1, &dummy);
			c.release(h);
		}
		std::uint64_t hits = 0;
		const std::size_t half = block_of_get.size() / 2;
		for (std::size_t i = 0; i < block_of_get.size(); ++i) {
			clock_cache<decoded, nop>::handle h;
			if (c.lookup(block_of_get[i] + 1, h)) {
				hits += i >= half;
			} else {
				h = c.insert(block_of_get[i] + 1, &dummy);
			}
			c.release(h);
		}
		return double(hits) / double(block_of_get.size() - half);
	};
	std::size_t lo = 1, hi = blocks / 4 + 2;  // up to 4x the blocks: a set-associative table loses ways to conflicts
	while (lo < hi) {
		const std::size_t mid = (lo + hi) / 2;
		if (hit_rate(mid) >= target) { hi = mid; } else { lo = mid + 1; }
	}
	return lo * 16;
}

struct row {
	double get_ns = 0, absent_ns = 0, seek10_ns = 0, e_ns = 0;
	double get_hit = 0, absent_hit = 0, seek_hit = 0, e_hit = 0;
	std::uint64_t e_records = 0;
};

template <class Reader>
row run_level(Reader& rd, counters& n, const data& d, const streams& s, std::size_t blocks) {
	row out;
	cache_t::handle h{};
	auto drop = [&] {
		if (h.s) { rd.c->release(h); }
		h.s = nullptr;
	};
	// Every block once (a long-running cache has seen them all), then the warm-up gets.
	for (std::size_t b = 0; b < blocks; ++b) {
		h = rd.get_block(b);
		drop();
	}
	for (std::uint32_t pos : s.warm) {
		const auto v = rd.get(d.keys[pos], h);
		sink_ += std::uint8_t(v->front());
		drop();
	}
	n.clear();
	auto t0 = clk::now();
	for (std::uint32_t pos : s.gets) {
		const auto v = rd.get(d.keys[pos], h);
		sink_ += std::uint8_t(v->front()) + v->size();
		drop();
	}
	out.get_ns = ns_since(t0) / double(s.gets.size());
	out.get_hit = n.rate();
	n.clear();
	t0 = clk::now();
	for (std::size_t i = 0; i < s.absent_count; ++i) {
		const auto v = rd.get(std::string_view(s.absent.data() + i * kvbench::key_size, kvbench::key_size), h);
		sink_ += v.has_value();
		drop();
	}
	out.absent_ns = ns_since(t0) / double(s.absent_count);
	out.absent_hit = n.rate();
	n.clear();
	t0 = clk::now();
	{
		typename Reader::cursor cur(&rd);
		for (std::uint32_t pos : s.seeks) {
			bool ok = cur.seek(d.keys[pos]);
			for (int k = 0; ok && k < 10; ++k) {
				sink_ += std::uint8_t(cur.value.front()) + cur.key().size();
				ok = k + 1 < 10 && cur.next();
			}
		}
	}
	out.seek10_ns = ns_since(t0) / double(s.seeks.size());
	out.seek_hit = n.rate();
	n.clear();
	t0 = clk::now();
	{
		typename Reader::cursor cur(&rd);
		for (std::size_t i = 0; i < s.e_from.size(); ++i) {
			bool ok = cur.seek(d.keys[s.e_from[i]]);
			for (std::uint32_t k = 0; ok && k < s.e_len[i]; ++k) {
				sink_ += std::uint8_t(cur.value.front()) + cur.key().size();
				++out.e_records;
				ok = k + 1 < s.e_len[i] && cur.next();
			}
		}
	}
	out.e_ns = ns_since(t0) / double(s.e_from.size());
	out.e_hit = n.rate();
	return out;
}

}  // namespace

int main(int argc, char** argv) {
	std::uint64_t records = 1'000'000, value_size = 100, seed = 1;
	std::size_t block_bytes = 4096, ops = 1 << 18;
	std::uint32_t restart_every = 16;
	std::string mode = "reads", layout = "split", codec_name = "lz4", src_name = "lz4", label;
	std::vector<double> targets = {0.60, 0.90, 0.99};
	double lz4_share = 0.10;
	bool seek_walk = false;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view a = argv[i];
		const char* v = argv[i + 1];
		if (a == "--records") { records = std::strtoull(v, nullptr, 10); }
		else if (a == "--value") { value_size = std::strtoull(v, nullptr, 10); }
		else if (a == "--block") { block_bytes = std::strtoull(v, nullptr, 10); }
		else if (a == "--restart") { restart_every = std::uint32_t(std::strtoul(v, nullptr, 10)); }
		else if (a == "--mode") { mode = v; }
		else if (a == "--layout") { layout = v; }
		else if (a == "--codec") { codec_name = v; }
		else if (a == "--src") { src_name = v; }
		else if (a == "--ops") { ops = std::strtoull(v, nullptr, 10); }
		else if (a == "--seed") { seed = std::strtoull(v, nullptr, 10); }
		else if (a == "--lz4-share") { lz4_share = std::strtod(v, nullptr); }
		else if (a == "--label") { label = v; }
		else if (a == "--seek") { seek_walk = std::string_view(v) == "walk"; }
		else if (a == "--targets") {
			targets.clear();
			for (const char* p = v; *p;) {
				char* e = nullptr;
				targets.push_back(std::strtod(p, &e));
				p = *e == ',' ? e + 1 : e;
			}
		} else {
			std::fprintf(stderr, "unknown %s\n", argv[i]);
			return 2;
		}
	}
	if (label.empty()) { label = layout + "-" + codec_name; }
	const auto t_start = clk::now();
	// mixed: zstd 1 on merged output, LZ4 on the flushed share, by a hash of the block number.
	const bool mixed = codec_name == "mixed";
	const codec c = mixed ? codec::zstd1 : parse_codec(codec_name);
	const auto share = std::uint64_t(lz4_share * 1000);
	auto codec_of = [c, mixed, share](std::size_t b) { return mixed && mix(b) % 1000 < share ? codec::lz4 : c; };

	const data d = make_data(records, value_size, seed);

	if (mode == "merge") {
		// Four sources, record i in source i % 4: each key-ordered, interleaved across the range.
		const codec src = parse_codec(src_name);
		constexpr std::size_t k = 4;
		std::vector<split_table> sources;
		for (std::size_t s = 0; s < k; ++s) {
			std::vector<std::string> ks;
			std::vector<std::string_view> vs;
			for (std::size_t i = s; i < records; i += k) {
				ks.push_back(d.keys[i]);
				vs.push_back(d.values[i]);
			}
			sources.push_back(build_split(ks, vs, block_bytes, src));
		}
		std::uint64_t in_bytes = 0;
		for (const auto& t : sources) { in_bytes += t.data.size() + t.keys.blocks.size() + t.keys.index.size(); }
		const auto t0 = clk::now();
		struct input {
			const split_table* t;
			std::unique_ptr<td_cursor> keys;
			std::string buf;
			split_block blk;
			std::size_t next_block = 0;
			bool live = false;
		};
		std::vector<input> in(k);
		std::string alphabet;
		for (std::size_t s = 0; s < k; ++s) {
			in[s].t = &sources[s];
			in[s].keys = std::make_unique<td_cursor>(sources[s].keys.blocks, sources[s].keys.index);
			in[s].live = in[s].keys->next();
			const termdict::reader r(sources[s].keys.blocks, sources[s].keys.index);
			for (char ch : r.alphabet_bytes()) {
				if (alphabet.find(ch) == std::string::npos) { alphabet.push_back(ch); }
			}
		}
		split_writer<decltype(codec_of)> w(alphabet, block_bytes, codec_of);
		std::uint64_t out_records = 0;
		for (;;) {
			std::size_t best = k;
			for (std::size_t s = 0; s < k; ++s) {
				if (in[s].live && (best == k || in[s].keys->key() < in[best].keys->key())) { best = s; }
			}
			if (best == k) { break; }
			input& x = in[best];
			const std::uint32_t ord = ordinal_of(x.keys->value());
			if (x.blk.starts.empty() || ord >= x.blk.first + x.blk.count()) {
				const auto& br = x.t->blocks[x.next_block];
				x.blk.parse(decompress(std::string_view(x.t->data).substr(br.offset, br.stored), br.raw, x.buf), x.t->first[x.next_block]);
				++x.next_block;
			}
			w.add(x.keys->key(), x.blk.value(ord));
			++out_records;
			x.live = x.keys->next();
		}
		const split_table out = w.finish();
		const double merge_ns = ns_since(t0) / double(out_records);
		const std::uint64_t out_bytes = out.data.size() + out.keys.blocks.size() + out.keys.index.size();
		std::printf("{\"mode\":\"merge\",\"label\":\"%s\",\"src\":\"%s\",\"codec\":\"%s\",\"value\":%llu,\"records\":%llu,\"logical\":%llu,\"in_bytes\":%llu,\"out_bytes\":%llu,\"merge_ns\":%.1f,\"total_s\":%.2f}\n",
			label.c_str(), src_name.c_str(), codec_name.c_str(), (unsigned long long)value_size, (unsigned long long)out_records, (unsigned long long)d.logical,
			(unsigned long long)in_bytes, (unsigned long long)out_bytes, merge_ns, std::chrono::duration<double>(clk::now() - t_start).count());
		return out_records == records ? 0 : 1;
	}

	const streams s = make_streams(d, seed, ops);
	double build_ns = 0, scan_ns = 0, keyscan_ns = 0;  // keyscan: the key dictionary alone (split)
	std::uint64_t data_bytes = 0, index_bytes = 0, key_bytes = 0, blocks = 0;
	std::vector<row> rows;
	std::vector<std::size_t> capacities;
	pool pl;
	counters n;

	auto run_targets = [&](auto& rd, auto block_of_pos) {
		std::vector<std::uint32_t> bog(s.warm.size());
		for (std::size_t i = 0; i < bog.size(); ++i) { bog[i] = std::uint32_t(block_of_pos(s.warm[i])); }
		for (double target : targets) {
			const std::size_t cap = calibrate(bog, blocks, target);
			cache_t cache(cap, recycler{&pl});
			rd.c = &cache;
			rows.push_back(run_level(rd, n, d, s, blocks));
			capacities.push_back(cap);
		}
	};

	if (layout == "inline") {
		if (mixed) {
			std::fprintf(stderr, "mixed is split only\n");
			return 2;
		}
		auto t0 = clk::now();
		const inline_table t = build_inline(d.keys, d.values, block_bytes, restart_every, c);
		build_ns = ns_since(t0) / double(records);
		data_bytes = t.data.size();
		index_bytes = t.index_bytes();
		blocks = t.blocks.size();
		t0 = clk::now();
		{
			std::string buf, scratch;
			for (const auto& br : t.blocks) {
				inline_block blk;
				blk.parse(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf));
				blk.walk({}, scratch, [&](std::string_view key, std::string_view v) {
					sink_ += std::uint8_t(v.front()) + key.size();
					return true;
				});
			}
		}
		scan_ns = ns_since(t0) / double(records);
		inline_reader rd{&t, nullptr, &pl, &n, {}};
		run_targets(rd, [&](std::uint32_t pos) { return *t.block_of(d.keys[pos]); });
	} else if (layout == "split") {
		auto t0 = clk::now();
		const split_table t = build_split_with(d.keys, d.values, block_bytes, codec_of);
		build_ns = ns_since(t0) / double(records);
		data_bytes = t.data.size();
		index_bytes = t.index_bytes();
		key_bytes = t.keys.blocks.size() + t.keys.index.size();
		blocks = t.blocks.size();
		const termdict::reader dict(t.keys.blocks, t.keys.index);
		t0 = clk::now();
		{
			td_cursor cur(t.keys.blocks, t.keys.index);
			std::string buf;
			split_block blk;
			std::size_t b = 0;
			while (cur.next()) {
				const std::uint32_t ord = ordinal_of(cur.value());
				if (blk.starts.empty() || ord >= blk.first + blk.count()) {
					const auto& br = t.blocks[b];
					blk.parse(decompress(std::string_view(t.data).substr(br.offset, br.stored), br.raw, buf), t.first[b]);
					++b;
				}
				sink_ += std::uint8_t(blk.value(ord).front()) + cur.key().size();
			}
		}
		scan_ns = ns_since(t0) / double(records);
		t0 = clk::now();
		{
			td_cursor cur(t.keys.blocks, t.keys.index);
			while (cur.next()) { sink_ += cur.key().size() + std::uint8_t(cur.value().front()); }
		}
		keyscan_ns = ns_since(t0) / double(records);
		// td_cursor's seek against a plain walk: every present key must land on itself.
		{
			td_cursor cur(t.keys.blocks, t.keys.index);
			cur.walk = seek_walk;
			for (std::size_t i = 0; i < 4096; ++i) {
				const std::uint32_t pos = s.seeks[i];
				if (!cur.seek(d.keys[pos]) || cur.key() != d.keys[pos] || ordinal_of(cur.value()) != pos) {
					std::fprintf(stderr, "td_cursor::seek wrong at %u\n", pos);
					return 1;
				}
				// An absent key lands on the next present one.
				const std::string_view a(s.absent.data() + i * kvbench::key_size, kvbench::key_size);
				const auto it = std::lower_bound(d.keys.begin(), d.keys.end(), a);
				const bool ok = cur.seek(a);
				if (ok != (it != d.keys.end()) || (ok && cur.key() != *it)) {
					std::fprintf(stderr, "td_cursor::seek wrong on an absent key\n");
					return 1;
				}
			}
		}
		split_reader rd{&t, &dict, nullptr, &pl, &n, seek_walk};
		run_targets(rd, [&](std::uint32_t pos) { return t.block_of(pos); });
	} else {
		std::fprintf(stderr, "unknown layout\n");
		return 2;
	}
	const double total_s = std::chrono::duration<double>(clk::now() - t_start).count();
	for (std::size_t i = 0; i < rows.size(); ++i) {
		const row& r = rows[i];
		std::printf(
			"{\"mode\":\"reads\",\"label\":\"%s\",\"layout\":\"%s\",\"codec\":\"%s\",\"value\":%llu,\"records\":%llu,\"logical\":%llu,\"bytes\":%llu,\"index_bytes\":%llu,\"key_bytes\":%llu,"
			"\"blocks\":%llu,\"build_ns\":%.1f,\"scan_ns\":%.2f,\"keyscan_ns\":%.2f,\"target\":%.2f,\"capacity\":%zu,\"cache_mib\":%.1f,\"get_ns\":%.1f,\"get_hit\":%.4f,\"absent_ns\":%.1f,\"absent_hit\":%.4f,"
			"\"seek10_ns\":%.1f,\"seek_hit\":%.4f,\"e_ns\":%.1f,\"e_hit\":%.4f,\"e_records\":%llu,\"total_s\":%.2f}\n",
			label.c_str(), layout.c_str(), codec_name.c_str(), (unsigned long long)value_size, (unsigned long long)records, (unsigned long long)d.logical, (unsigned long long)data_bytes,
			(unsigned long long)index_bytes, (unsigned long long)key_bytes, (unsigned long long)blocks, build_ns, scan_ns, keyscan_ns, targets[i], capacities[i], double(capacities[i]) * double(block_bytes) / 1048576.0,
			r.get_ns, r.get_hit, r.absent_ns, r.absent_hit, r.seek10_ns, r.seek_hit, r.e_ns, r.e_hit, (unsigned long long)r.e_records, total_s);
	}
	return 0;
}
