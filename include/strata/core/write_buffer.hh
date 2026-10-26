// The write buffer's pieces (docs/design/write-buffer.md D3, D4, D9): a lane of records in
// chunks addressed by 32-bit references, the version map readable while the lease holder writes
// it, and the position index. The index (index.hh) assembles them into buffers.
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
#include <utility>
#include <vector>

namespace strata::wbuf {

// A record reference (D3): a chunk index (15 bits) and an offset in 8-byte units (17 bits).
using ref_t = std::uint32_t;
inline constexpr ref_t no_ref = ~ref_t(0);
inline constexpr unsigned unit_bits = 17;
// Load-bearing: a buffer whose chunk table is full freezes (D3), at least 128 MiB of records.
inline constexpr std::size_t max_chunks = std::size_t(1) << (32 - unit_bits);
inline constexpr std::size_t min_chunk = 4096, max_chunk = std::size_t(8) << unit_bits;
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
	// Room for n more entries without growth.
	void reserve(std::size_t n) {
		for (;;) {
			const table* t = t_.load(std::memory_order_relaxed);
			if (t && 8 * (used_ + n) <= 7 * (t->mask + 1)) { return; }
			grow(t ? 2 * (t->mask + 1) : 16);
		}
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
	// A table twice the size, filled, then published. The old one is freed here: no reader
	// outside the lease holder probes a buffer before views carry buffers (step 1, item 4, D10).
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
	}
	std::atomic<table*> t_{nullptr};
	std::size_t used_ = 0;
};

// An append-only array whose entries never move: blocks of 64, 128, 256, ... entries, so a
// reader may hold an entry's address while the lease holder appends (the position index, D9).
template <class T>
class stable_array {
public:
	stable_array() = default;
	stable_array(const stable_array&) = delete;
	stable_array& operator=(const stable_array&) = delete;
	~stable_array() {
		for (T* b : blocks_) { delete[] b; }
	}
	std::size_t size() const noexcept { return size_; }
	const T& operator[](std::size_t i) const noexcept { return at(i); }
	void push_back(const T& x) {
		const auto [b, o] = locate(size_);
		if (b == blocks_.size()) { blocks_.push_back(new T[base << b]); }
		blocks_[b][o] = x;
		++size_;
	}
	// Drops the entries from n on (a rejected transaction's, which nobody has seen).
	void rewind(std::size_t n) noexcept {
		assert(n <= size_);
		size_ = n;
	}
	std::uint64_t bytes() const noexcept { return blocks_.capacity() * sizeof(T*) + ((base << blocks_.size()) - base) * sizeof(T); }

private:
	static constexpr std::size_t base = 64;
	static std::pair<std::size_t, std::size_t> locate(std::size_t i) noexcept {
		const std::size_t b = std::size_t(std::bit_width(i / base + 1)) - 1;
		return {b, i - base * ((std::size_t(1) << b) - 1)};
	}
	const T& at(std::size_t i) const noexcept {
		const auto [b, o] = locate(i);
		return blocks_[b][o];
	}
	std::vector<T*> blocks_;
	std::size_t size_ = 0;
};

// A lane (D3): records of one type in chunks of 4 KiB doubling to 1 MiB, each record at a fixed
// stride, addressed by reference. One writer appends; nothing moves; the records are destroyed
// with the lane, rejected ones included. Its gate refuses appends while a flush borrows from it.
// The chunk directory is fixed at creation and never replaced (WB-19): 8 inline entries, then 12
// blocks of 8 << k entries, 32,768 in all; a block is allocated when the first chunk lands in it.
template <class Record>
class lane {
public:
	static constexpr std::size_t stride = (sizeof(Record) + 7) & ~std::size_t(7);
	static_assert(alignof(Record) <= 8 && stride <= min_chunk);

	lane() = default;
	lane(const lane&) = delete;
	lane& operator=(const lane&) = delete;
	~lane() {
		for (std::size_t i = 0; i < count_; ++i) {
			const chunk& c = entry(i);
			for (std::size_t at = 0; at < c.used; at += stride) { std::launder(reinterpret_cast<Record*>(c.p + at))->~Record(); }
			::operator delete(c.p);
		}
		for (auto& b : blocks_) { delete[] b.load(std::memory_order_relaxed); }
	}

	// Whether a record may need a chunk past the directory's limit: the buffer freezes first.
	bool full() const noexcept { return count_ + 1 >= max_chunks; }
	template <class... Args>
	ref_t append(Args&&... args) {
		gate_.require_unpinned();
		if (!count_ || entry(count_ - 1).used + stride > entry(count_ - 1).size) {
			if (count_ == max_chunks) { throw std::length_error("strata: a write buffer's chunk directory is full"); }
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
		c.used += stride;
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
	std::size_t count_ = 0;
	std::uint64_t bytes_ = 0;
	borrow_gate gate_;
};

}  // namespace strata::wbuf
