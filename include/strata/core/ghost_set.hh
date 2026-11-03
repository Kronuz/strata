// The S3-FIFO ghost (cold-tier D6, CT-R10): the 64-bit hashes of the keys the policy closed last,
// oldest dropped first, a hit removing its entry. A node pool in a doubly linked list, found
// through `hash_slots` (the 32-bit half of the hash, the full 64 bits as the test of equality), so
// it holds at most `capacity` hashes at 16 B each and allocates only as it fills. Not thread-safe:
// the catalog's policy mutex guards it.
#pragma once

#include "strata/core/index.hh"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace strata {

class ghost_set {
public:
	explicit ghost_set(std::size_t capacity = 0) noexcept : cap_(capacity) {}

	// Remembers `h`; the oldest hash goes when the set is full. A hash already present becomes the newest.
	void add(std::uint64_t h) {
		if (!cap_) { return; }
		take(h);
		std::uint32_t n;
		if (size_ == cap_) {
			n = oldest_;
			erase_oldest();
		} else if (free_ != none) {
			n = free_;
			free_ = nodes_[n].next;
		} else {
			n = static_cast<std::uint32_t>(nodes_.size());
			nodes_.push_back({});
		}
		node& x = nodes_[n];
		x.hash = h, x.older = newest_, x.newer = none;
		if (newest_ != none) { nodes_[newest_].newer = n; }
		newest_ = n;
		if (oldest_ == none) { oldest_ = n; }
		table_.insert(key32(h), n);
		++size_;
	}
	// Removes `h` and says whether it was there.
	bool take(std::uint64_t h) {
		const std::uint32_t* f = table_.find(key32(h), [&](std::uint32_t v) { return nodes_[v].hash == h; });
		if (!f) { return false; }
		const std::uint32_t n = *f;
		table_.erase(key32(h), [&](std::uint32_t v) { return v == n; });
		unlink(n);
		nodes_[n].next = free_, free_ = n;
		--size_;
		return true;
	}
	bool contains(std::uint64_t h) const noexcept {
		return table_.find(key32(h), [&](std::uint32_t v) { return nodes_[v].hash == h; }) != nullptr;
	}
	std::size_t size() const noexcept { return size_; }
	std::size_t capacity() const noexcept { return cap_; }

private:
	static constexpr std::uint32_t none = ~std::uint32_t(0);
	struct node {
		std::uint64_t hash = 0;
		std::uint32_t older = none, newer = none;
		std::uint32_t next = none;  // the free list
	};
	static std::uint32_t key32(std::uint64_t h) noexcept { return static_cast<std::uint32_t>(h >> 32); }
	void unlink(std::uint32_t n) noexcept {
		node& x = nodes_[n];
		(x.older != none ? nodes_[x.older].newer : oldest_) = x.newer;
		(x.newer != none ? nodes_[x.newer].older : newest_) = x.older;
	}
	void erase_oldest() {
		const std::uint32_t n = oldest_;
		table_.erase(key32(nodes_[n].hash), [&](std::uint32_t v) { return v == n; });
		unlink(n);
		--size_;
	}

	std::size_t cap_, size_ = 0;
	std::vector<node> nodes_;
	std::uint32_t newest_ = none, oldest_ = none, free_ = none;
	index_detail::hash_slots table_;
};

}  // namespace strata
