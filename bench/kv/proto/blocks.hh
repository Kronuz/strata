// format-14-kv's block layouts, shared by the prototypes (blocks.cc, cached.cc): codecs with a
// codec byte per block, RocksDB's inline block table, and strata's split shape (a termdict over the
// keys, values alone in blocks by ordinal). See blocks.cc for what each layout models.
#pragma once

#include "termdict.hh"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kvproto {

enum class codec { none, lz4, zstd1, zstd3 };

inline codec parse_codec(std::string_view s) {
	if (s == "none") { return codec::none; }
	if (s == "lz4") { return codec::lz4; }
	if (s == "zstd1") { return codec::zstd1; }
	if (s == "zstd3") { return codec::zstd3; }
	throw std::invalid_argument("codec");
}

inline void put_leb(std::string& out, std::uint64_t v) {
	while (v >= 0x80) {
		out.push_back(char(v | 0x80));
		v >>= 7;
	}
	out.push_back(char(v));
}
inline std::uint64_t get_leb(const char*& p) {
	std::uint64_t v = 0;
	for (unsigned shift = 0;; shift += 7) {
		const auto b = std::uint8_t(*p++);
		v |= std::uint64_t(b & 0x7F) << shift;
		if (b < 0x80) { return v; }
	}
}

inline ZSTD_CCtx* cctx() {
	static ZSTD_CCtx* c = ZSTD_createCCtx();
	return c;
}
inline ZSTD_DCtx* dctx() {
	static ZSTD_DCtx* d = ZSTD_createDCtx();
	return d;
}

// Stored as is when the codec saves less than an eighth, as RocksDB does; the first byte says.
inline void compress(codec c, std::string_view raw, std::string& out) {
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
inline std::string_view decompress(std::string_view stored, std::size_t raw_size, std::string& buf) {
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

inline inline_table build_inline(const std::vector<std::string>& keys, const std::vector<std::string_view>& values, std::size_t block_bytes, std::uint32_t restart_every, codec c) {
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

// Writes a split table from records in key order: each key's ordinal into the termdict, the
// values into blocks of up to block_bytes, each block's codec from codec_of(block number).
template <class CodecOf>
class split_writer {
public:
	split_writer(std::string_view alphabet, std::size_t block_bytes, CodecOf codec_of) : keys_(alphabet), block_bytes_(block_bytes), codec_of_(codec_of) {}

	void add(std::string_view key, std::string_view value) {
		if (!lengths_.empty() && pending_ + value.size() + 2 > block_bytes_) { cut(); }
		ord_.clear();
		put_leb(ord_, count_++);
		keys_.add(key, ord_);
		lengths_.push_back(std::uint32_t(value.size()));
		values_.append(value);
		pending_ += value.size() + 2;
	}

	split_table finish() {
		if (!lengths_.empty()) { cut(); }
		t_.keys = keys_.finish();
		return std::move(t_);
	}

private:
	void cut() {
		raw_.clear();
		put_leb(raw_, lengths_.size());
		for (auto n : lengths_) { put_leb(raw_, n); }
		raw_.append(values_);
		const std::uint64_t at = t_.data.size();
		compress(codec_of_(t_.blocks.size()), raw_, t_.data);
		t_.blocks.push_back({at, std::uint32_t(t_.data.size() - at), std::uint32_t(raw_.size())});
		t_.first.push_back(std::uint32_t(count_ - lengths_.size()));
		lengths_.clear();
		values_.clear();
		pending_ = 0;
	}

	split_table t_;
	termdict::builder keys_;
	std::size_t block_bytes_;
	CodecOf codec_of_;
	std::string ord_, raw_, values_;
	std::vector<std::uint32_t> lengths_;
	std::size_t pending_ = 0;
	std::uint64_t count_ = 0;
};

// The bytes the keys use, as termdict's builder takes them.
inline std::string alphabet_of(const std::vector<std::string>& keys) {
	bool used[256] = {};
	for (const auto& k : keys) {
		for (char ch : k) { used[std::uint8_t(ch)] = true; }
	}
	std::string alphabet;
	for (unsigned ch = 0; ch < 256; ++ch) {
		if (used[ch]) { alphabet.push_back(char(ch)); }
	}
	return alphabet;
}

template <class CodecOf>
split_table build_split_with(const std::vector<std::string>& keys, const std::vector<std::string_view>& values, std::size_t block_bytes, CodecOf codec_of) {
	split_writer<CodecOf> w(alphabet_of(keys), block_bytes, codec_of);
	for (std::size_t i = 0; i < keys.size(); ++i) { w.add(keys[i], values[i]); }
	return w.finish();
}

inline split_table build_split(const std::vector<std::string>& keys, const std::vector<std::string_view>& values, std::size_t block_bytes, codec c) {
	return build_split_with(keys, values, block_bytes, [c](std::size_t) { return c; });
}

inline std::uint32_t ordinal_of(std::string_view v) {
	const char* p = v.data();
	return std::uint32_t(get_leb(p));
}

// --- frame64: format 13's stored frames -------------------------------------------------------

struct frame_table {
	std::string data;
	std::vector<block_ref> frames;
	std::vector<std::uint32_t> first;
};

inline frame_table build_frames(const std::vector<std::string_view>& values) {
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

}  // namespace kvproto
