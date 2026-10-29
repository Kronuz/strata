// The write buffer's pieces (docs/design/write-buffer.md D3, D4, D9): a lane of records in
// chunks addressed by 32-bit references, the version map readable while the lease holder writes
// it, the position index, and the runs a scan merges. The index (index.hh) assembles them.
#pragma once

#include "strata/core/borrow.hh"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace strata::wbuf {

// A record reference (D3): a chunk index (15 bits) and an offset in 8-byte units (17 bits).
using ref_t = std::uint32_t;
inline constexpr ref_t no_ref = ~ref_t(0);
inline constexpr unsigned unit_bits = 17;
// Load-bearing: a buffer whose chunk table is full freezes (D3). Chunks double from 1 KiB to
// 1 MiB, so the table holds about 32 GiB of records. The small first chunk keeps an index with a
// few buffered documents near the base's memory (CR6).
inline constexpr std::size_t max_chunks = std::size_t(1) << (32 - unit_bits);
inline constexpr std::size_t min_chunk = 1024, max_chunk = std::size_t(8) << unit_bits;
inline constexpr ref_t make_ref(std::size_t chunk, std::size_t offset) noexcept { return ref_t(chunk << unit_bits | offset >> 3); }
inline constexpr std::size_t ref_chunk(ref_t r) noexcept { return r >> unit_bits; }
inline constexpr std::size_t ref_offset(ref_t r) noexcept { return std::size_t(r & ((ref_t(1) << unit_bits) - 1)) << 3; }

// The version map (D4): 8-byte atomic slots, a 32-bit hash beside a record reference, at most
// 7/8 full, probed linearly. Nothing is removed while the buffer lives: a delete points the key's
// slot at a tombstone. The lease holder fills a record before it stores the slot with release,
// and a reader's acquire load of a slot names a complete record. A rejection restores the slots
// its transaction changed in reverse; reserve() runs first, so no growth moves them meanwhile.
class version_map {
public:
	static constexpr std::uint64_t empty = ~std::uint64_t(0);
	struct place {
		std::size_t i = 0;
		std::uint64_t old = empty;  // the slot before the store
	};

	version_map() = default;
	version_map(const version_map&) = delete;
	version_map& operator=(const version_map&) = delete;
	~version_map() { delete t_.load(std::memory_order_relaxed); }

	// The reference stored for the entry eq accepts, or no_ref.
	template <class Eq>
	ref_t find(std::uint32_t h, Eq&& eq) const noexcept {
		const table* t = t_.load(std::memory_order_acquire);
		if (!t) { return no_ref; }
		for (std::size_t i = home(h, t->mask);; i = (i + 1) & t->mask) {
			const std::uint64_t s = t->slots[i].load(std::memory_order_acquire);
			if (s == empty) { return no_ref; }
			if (std::uint32_t(s >> 32) == h && eq(ref_t(s))) { return ref_t(s); }
		}
	}
	// The lease holder's: the entry's slot, or the empty slot it would take. reserve() first.
	template <class Eq>
	place locate(std::uint32_t h, Eq&& eq) const noexcept {
		const table* t = t_.load(std::memory_order_relaxed);
		assert(t && used_ < t->mask);
		for (std::size_t i = home(h, t->mask);; i = (i + 1) & t->mask) {
			const std::uint64_t s = t->slots[i].load(std::memory_order_relaxed);
			if (s == empty || (std::uint32_t(s >> 32) == h && eq(ref_t(s)))) { return {i, s}; }
		}
	}
	static ref_t ref_of(const place& p) noexcept { return p.old == empty ? no_ref : ref_t(p.old); }
	// Points the place at r: the record is complete, so the store releases it.
	void store(const place& p, std::uint32_t h, ref_t r) noexcept {
		assert(r != no_ref);
		if (p.old == empty) { ++used_; }
		t_.load(std::memory_order_relaxed)->slots[p.i].store(std::uint64_t(h) << 32 | r, std::memory_order_release);
	}
	// Undoes store(p, ...): the most recent store to an empty slot is the last in its probe run.
	void restore(const place& p) noexcept {
		if (p.old == empty) { --used_; }
		t_.load(std::memory_order_relaxed)->slots[p.i].store(p.old, std::memory_order_release);
	}
	// Room for n more entries without growth: one table at the size that holds them.
	void reserve(std::size_t n) {
		const table* t = t_.load(std::memory_order_relaxed);
		std::size_t size = t ? t->mask + 1 : 16;
		while (8 * (used_ + n) > 7 * size) { size *= 2; }
		if (!t || size != t->mask + 1) { grow(size); }
	}
	// Whether a table exists: the lease holder's test for a built docid map (WB-23).
	bool built() const noexcept { return t_.load(std::memory_order_relaxed) != nullptr; }
	// Moves the tables growth replaced to `out`: readers may still probe them (D10).
	template <class Out>
	void take_retired(Out& out) {
		for (auto& t : retired_) { out.push_back(std::shared_ptr<const void>(std::move(t))); }
		retired_.clear();
	}
	std::size_t size() const noexcept { return used_; }
	std::uint64_t bytes() const noexcept {
		const table* t = t_.load(std::memory_order_relaxed);
		return t ? sizeof(table) + (t->mask + 1) * sizeof(std::uint64_t) : 0;
	}

private:
	struct table {
		std::size_t mask;
		std::unique_ptr<std::atomic<std::uint64_t>[]> slots;
	};
	static std::size_t home(std::uint32_t h, std::size_t mask) noexcept { return std::size_t((std::uint64_t(h) * 0x9E3779B97F4A7C15ull) >> 32) & mask; }
	// A table of n slots, filled, then published. The old one waits in retired_ for the lease
	// holder to stamp it with a view generation (D10, WB-23).
	void grow(std::size_t n) {
		auto next = std::make_unique<table>(table{n - 1, std::make_unique<std::atomic<std::uint64_t>[]>(n)});
		for (std::size_t i = 0; i < n; ++i) { next->slots[i].store(empty, std::memory_order_relaxed); }
		std::unique_ptr<table> old(t_.load(std::memory_order_relaxed));
		if (old) {
			for (std::size_t i = 0; i <= old->mask; ++i) {
				const std::uint64_t s = old->slots[i].load(std::memory_order_relaxed);
				if (s == empty) { continue; }
				std::size_t j = home(std::uint32_t(s >> 32), next->mask);
				while (next->slots[j].load(std::memory_order_relaxed) != empty) { j = (j + 1) & next->mask; }
				next->slots[j].store(s, std::memory_order_relaxed);
			}
		}
		t_.store(next.release(), std::memory_order_release);
		if (old) { retired_.push_back(std::move(old)); }
	}
	std::atomic<table*> t_{nullptr};
	std::size_t used_ = 0;
	std::vector<std::unique_ptr<table>> retired_;
};

// An append-only array whose entries never move: blocks of `base`, 2 x base, ... entries in a
// directory fixed at creation, so nothing is replaced (the position index, D9). One writer
// appends; publish() releases the count, and readers on other threads read below published().
template <class T>
class stable_array {
public:
	stable_array() = default;
	stable_array(const stable_array&) = delete;
	stable_array& operator=(const stable_array&) = delete;
	~stable_array() {
		for (auto& b : blocks_) { delete[] b.load(std::memory_order_relaxed); }
	}
	std::size_t size() const noexcept { return size_; }
	std::size_t published() const noexcept { return published_.load(std::memory_order_acquire); }
	const T& operator[](std::size_t i) const noexcept { return at(i); }
	void push_back(const T& x) {
		const auto [b, o] = locate(size_);
		if (b >= nblocks) { throw std::length_error("strata: a position index is full"); }
		T* blk = blocks_[b].load(std::memory_order_relaxed);
		if (!blk) { blocks_[b].store(blk = new T[base << b], std::memory_order_release); }
		blk[o] = x;
		++size_;
	}
	// Drops the entries from n on (a rejected transaction's, never published).
	void rewind(std::size_t n) noexcept {
		assert(n <= size_ && n >= published_.load(std::memory_order_relaxed));
		size_ = n;
	}
	// The group's end: readers may read every entry below size().
	void publish() noexcept { published_.store(size_, std::memory_order_release); }
	std::uint64_t bytes() const noexcept {
		std::uint64_t n = sizeof(*this);
		for (std::size_t b = 0; b < nblocks && blocks_[b].load(std::memory_order_relaxed); ++b) { n += (base << b) * sizeof(T); }
		return n;
	}

private:
	// Load-bearing: base x (2^nblocks - 1) is 2^29 - base entries, about what a lane's 32,768 chunks
	// hold at the smallest record. The first block is small: an open index with a few documents pays for it.
	static constexpr std::size_t base_bits = 4, base = std::size_t(1) << base_bits, nblocks = 29 - base_bits;
	static std::pair<std::size_t, std::size_t> locate(std::size_t i) noexcept {
		const std::size_t b = std::size_t(std::bit_width(i / base + 1)) - 1;
		return {b, i - base * ((std::size_t(1) << b) - 1)};
	}
	const T& at(std::size_t i) const noexcept {
		const auto [b, o] = locate(i);
		return blocks_[b].load(std::memory_order_acquire)[o];
	}
	std::atomic<T*> blocks_[nblocks] = {};
	std::size_t size_ = 0;
	std::atomic<std::size_t> published_{0};
};

// A position-index entry (D9): the key's first 8 bytes, big-endian and zero padded, and the
// record's reference, so sorts and merges compare words and read a record only on a tie.
// Trivial, so a position-index block's pages stay untouched until entries land in them.
struct pos_entry {
	std::uint32_t hi, lo;
	ref_t ref;
	std::uint64_t prefix() const noexcept { return std::uint64_t(hi) << 32 | lo; }
};
static_assert(sizeof(pos_entry) == 12 && std::is_trivial_v<pos_entry>);
inline std::uint64_t key_head(std::string_view key) noexcept {
	std::uint64_t p = 0;
	for (std::size_t i = 0; i < key.size() && i < 8; ++i) { p |= std::uint64_t(static_cast<unsigned char>(key[i])) << (56 - 8 * i); }
	return p;
}
inline pos_entry make_entry(std::string_view key, ref_t r) noexcept {
	const std::uint64_t p = key_head(key);
	return {std::uint32_t(p >> 32), std::uint32_t(p), r};
}

// A buffer's runs (D9): immutable sorted levels of position-index entries, by key and, within a
// key, newest first, in a binary counter: a level merges into the one before it while that one is
// at most run_ratio times its size. The set names the position-index entries it covers.
inline constexpr std::size_t run_ratio = 8;
inline constexpr std::size_t fresh_cap = 16384;  // levels up to this belong to fresh jobs, bigger ones to compaction
inline constexpr std::size_t tail_build = 64;    // a scan's private tail past this asks for a build
inline constexpr std::size_t tail_select = 32;   // a long tail sorts only its smallest entries first
using level = std::shared_ptr<const std::vector<pos_entry>>;
struct run_set {
	std::vector<level> levels;  // biggest first
	std::size_t cover = 0;      // position-index entries below this are in a level
	std::uint64_t bytes() const noexcept {
		std::uint64_t n = sizeof(*this) + levels.capacity() * sizeof(level);
		for (const auto& l : levels) { n += l->capacity() * sizeof(pos_entry); }
		return n;
	}
};
// A job's result: the consecutive levels it consumed from the set it was posted with, the level
// that replaces them, and for a fresh job the new cover. Only the lease holder applies it.
struct splice {
	std::vector<level> consumed;
	level produced;
	std::size_t cover = 0;
	bool fresh = false, failed = false;
};
// A posted job's slot in its buffer: the job fills `out`, then stores ready with release.
struct run_job {
	splice out;
	std::atomic<bool> ready{false};
	bool live = false;  // posted and not yet taken: the lease holder's
};
template <class Less>
std::vector<pos_entry> merge_levels(const std::vector<pos_entry>& x, const std::vector<pos_entry>& y, Less& less) {
	std::vector<pos_entry> out(x.size() + y.size());
	std::merge(x.begin(), x.end(), y.begin(), y.end(), out.begin(), less);
	return out;
}
inline bool counter_merges(const std::vector<level>& st, std::size_t ratio = run_ratio) noexcept {
	return st.size() >= 2 && st[st.size() - 2]->size() <= ratio * st.back()->size();
}
// A fresh job: sorts the backlog [from, cover) given as `fresh` and merges it into the posted
// levels' small tail while the counter asks, up to `cap` entries a level.
template <class Less>
splice fresh_splice(std::vector<level> lv, std::vector<pos_entry> fresh, std::size_t cover, std::size_t cap, Less less) {
	std::sort(fresh.begin(), fresh.end(), less);
	splice sp;
	sp.fresh = true;
	sp.cover = cover;
	if (fresh.empty()) { return sp; }  // keyless records only: the cover moves
	std::size_t keep = lv.size();  // lv[0, keep) are still the posted levels
	lv.push_back(std::make_shared<const std::vector<pos_entry>>(std::move(fresh)));
	while (counter_merges(lv) && lv[lv.size() - 2]->size() <= cap) {
		auto merged = std::make_shared<const std::vector<pos_entry>>(merge_levels(*lv[lv.size() - 2], *lv.back(), less));
		lv.pop_back();
		if (lv.size() <= keep) {
			sp.consumed.insert(sp.consumed.begin(), lv.back());
			keep = lv.size() - 1;
		}
		lv.back() = std::move(merged);
	}
	sp.produced = lv.back();
	return sp;
}
// A compaction job: merges the posted big levels' tail while the counter asks.
template <class Less>
splice compact_splice(const std::vector<level>& big, Less less) {
	splice sp;
	std::vector<level> st = big;
	std::size_t first = st.size();
	while (counter_merges(st)) {
		auto merged = std::make_shared<const std::vector<pos_entry>>(merge_levels(*st[st.size() - 2], *st.back(), less));
		st.pop_back();
		st.back() = std::move(merged);
		first = st.size() - 1;
	}
	if (first < big.size()) {
		sp.consumed.assign(big.begin() + std::ptrdiff_t(first), big.end());
		sp.produced = st.back();
	}
	return sp;
}
// The set `cur` with the splice applied, or null when its consumed levels are no longer there
// (the splice is dropped) or it changes nothing.
inline std::unique_ptr<run_set> apply_splice(const run_set* cur, const splice& sp) {
	if (sp.failed || (!sp.produced && !sp.fresh)) { return nullptr; }
	auto next = std::make_unique<run_set>();
	if (cur) {
		next->levels = cur->levels;
		next->cover = cur->cover;
	}
	auto& lv = next->levels;
	std::size_t at = lv.size();
	if (!sp.consumed.empty()) {
		at = std::size_t(std::find(lv.begin(), lv.end(), sp.consumed.front()) - lv.begin());
		if (at + sp.consumed.size() > lv.size() || !std::equal(sp.consumed.begin(), sp.consumed.end(), lv.begin() + std::ptrdiff_t(at))) { return nullptr; }
		lv.erase(lv.begin() + std::ptrdiff_t(at), lv.begin() + std::ptrdiff_t(at + sp.consumed.size()));
	}
	if (sp.produced) { lv.insert(lv.begin() + std::ptrdiff_t(at), sp.produced); }
	if (sp.fresh) {
		if (sp.cover < next->cover) { return nullptr; }
		next->cover = sp.cover;
	}
	return next;
}

// A lane (D3): records of one type in chunks of 1 KiB doubling to 1 MiB, each at its own stride (a
// fixed one, or the record's stride() when it varies), addressed by reference. One writer appends;
// nothing moves; the records are destroyed with the lane or cut by truncate(), as a rejected
// transaction's are. Its gate refuses appends while a flush borrows from it.
// The chunk directory is fixed at creation and never replaced (WB-19): 8 inline entries, then 12
// blocks of 8 << k entries, 32,768 in all; a block is allocated when the first chunk lands in it.
template <class Record>
class lane {
public:
	static constexpr std::size_t stride = (sizeof(Record) + 7) & ~std::size_t(7);
	static_assert(alignof(Record) <= 8 && stride <= min_chunk);
	// A record's bytes in the lane: its own stride() when it varies, else the fixed one.
	static std::size_t stride_of(const Record& r) noexcept {
		if constexpr (requires { r.stride(); }) {
			return r.stride();
		} else {
			return stride;
		}
	}

	// `limit` chunks at most (tests and tuning lower it).
	explicit lane(std::size_t limit = max_chunks) noexcept : limit_(limit) { assert(limit >= 1 && limit <= max_chunks); }
	lane(const lane&) = delete;
	lane& operator=(const lane&) = delete;
	~lane() {
		truncate({});
		for (auto& b : blocks_) { delete[] b.load(std::memory_order_relaxed); }
	}

	// The lane's end, and a cut back to it: the records after it are destroyed and the chunks
	// opened after it freed.
	struct mark {
		std::size_t chunks = 0, used = 0;
	};
	mark tell() const noexcept { return {count_, count_ ? entry(count_ - 1).used : 0}; }
	void truncate(mark m) noexcept {
		for (; count_ > m.chunks; --count_) {
			chunk& c = entry(count_ - 1);
			destroy(c, 0);
			::operator delete(c.p);
			bytes_ -= c.size;
			c = {};
		}
		if (count_) { destroy(entry(count_ - 1), m.used); }
	}

	// Whether n more records of at most `bytes` bytes each are sure to fit within the chunk
	// limit, counting the tail's room and the chunks it may still open; a buffer that cannot
	// take a transaction freezes first (CR9).
	bool room_for(std::size_t n, std::size_t bytes) const noexcept {
		assert(bytes >= 8 && bytes <= min_chunk);
		std::size_t have = 0, size = 0, i = count_;
		if (count_) {
			const chunk& t = entry(count_ - 1);
			have = (t.size - t.used) / bytes;
			size = t.size;
		}
		while (have < n) {
			if (i >= limit_) { return false; }
			size = i ? std::min(max_chunk, 2 * size) : min_chunk;
			have += size / bytes;
			++i;
		}
		return true;
	}
	template <class... Args>
	ref_t append(Args&&... args) {
		return append_n(stride, std::forward<Args>(args)...);
	}
	// A record of `bytes` bytes (a multiple of 8, at most a minimum chunk): the constructor lays
	// out what follows the header.
	template <class... Args>
	ref_t append_n(std::size_t bytes, Args&&... args) {
		assert(bytes % 8 == 0 && bytes >= stride && bytes <= min_chunk);
		gate_.require_unpinned();
		if (!count_ || entry(count_ - 1).used + bytes > entry(count_ - 1).size) {
			if (count_ == limit_) { throw std::length_error("strata: a write buffer's chunk directory is full"); }
			const std::size_t n = count_ ? std::min(max_chunk, 2 * entry(count_ - 1).size) : min_chunk;
			const auto [b, o] = locate(count_);
			if (b != npos && !blocks_[b].load(std::memory_order_relaxed)) { blocks_[b].store(new chunk[std::size_t(8) << b](), std::memory_order_release); }
			chunk& c = b == npos ? inline_[o] : blocks_[b].load(std::memory_order_relaxed)[o];
			c = {static_cast<std::byte*>(::operator new(n)), n, 0};
			++count_;
			bytes_ += n;
		}
		chunk& c = entry(count_ - 1);
		new (c.p + c.used) Record(std::forward<Args>(args)...);
		const ref_t r = make_ref(count_ - 1, c.used);
		c.used += bytes;
		assert(stride_of(at(r)) == bytes);
		return r;
	}
	Record& at(ref_t r) noexcept { return *std::launder(reinterpret_cast<Record*>(entry(ref_chunk(r)).p + ref_offset(r))); }
	const Record& at(ref_t r) const noexcept { return *std::launder(reinterpret_cast<const Record*>(entry(ref_chunk(r)).p + ref_offset(r))); }
	std::uint64_t bytes() const noexcept {
		std::uint64_t n = bytes_ + sizeof(*this);
		for (std::size_t b = 0; b < blocks; ++b) { n += blocks_[b].load(std::memory_order_relaxed) ? (std::size_t(8) << b) * sizeof(chunk) : 0; }
		return n;
	}

	// A lease on the records as they are: no append until it ends (core/borrow.hh).
	borrow_gate::lease pin() {
		auto spans = std::make_shared<borrow_gate::lease::spans>();
		spans->reserve(count_);
		for (std::size_t i = 0; i < count_; ++i) { spans->push_back({entry(i).p, entry(i).p + entry(i).used}); }
		std::sort(spans->begin(), spans->end(), [](const auto& a, const auto& b) { return std::less<const void*>{}(a.first, b.first); });
		return gate_.pin(std::shared_ptr<const borrow_gate::lease::spans>(std::move(spans)));
	}
	bool pinned() const noexcept { return gate_.pinned(); }
	void require_unpinned() const { gate_.require_unpinned(); }

private:
	struct chunk {
		std::byte* p = nullptr;
		std::size_t size = 0, used = 0;
	};
	static void destroy(chunk& c, std::size_t from) noexcept {
		for (std::size_t at = from; at < c.used;) {
			Record* r = std::launder(reinterpret_cast<Record*>(c.p + at));
			at += stride_of(*r);
			r->~Record();
		}
		c.used = std::min(c.used, from);
	}
	static constexpr std::size_t blocks = 12, npos = ~std::size_t(0);
	static_assert(8 + 8 * ((std::size_t(1) << blocks) - 1) == max_chunks);
	// Directory index i: inline below 8, else block k and offset within it.
	static std::pair<std::size_t, std::size_t> locate(std::size_t i) noexcept {
		if (i < 8) { return {npos, i}; }
		const std::size_t j = i - 8, k = std::size_t(std::bit_width(j / 8 + 1)) - 1;
		return {k, j - 8 * ((std::size_t(1) << k) - 1)};
	}
	chunk& entry(std::size_t i) noexcept {
		const auto [b, o] = locate(i);
		return b == npos ? inline_[o] : blocks_[b].load(std::memory_order_acquire)[o];
	}
	const chunk& entry(std::size_t i) const noexcept { return const_cast<lane*>(this)->entry(i); }
	chunk inline_[8];
	std::atomic<chunk*> blocks_[blocks] = {};
	const std::size_t limit_;
	std::size_t count_ = 0;
	std::uint64_t bytes_ = 0;
	borrow_gate gate_;
};

}  // namespace strata::wbuf
