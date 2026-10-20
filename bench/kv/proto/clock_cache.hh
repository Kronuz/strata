// format-14-kv D2's decoded-block cache, shared by the prototypes (cache.cc, cached.cc): a
// 16-way set-associative CLOCK. One 64-bit meta word per slot holds the state, a reference count,
// and a CLOCK countdown, so a hit is one compare-and-swap and a release one atomic subtraction;
// eviction sweeps only the key's own set and hands the evicted value to Recycle. No locks.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace kvproto {

inline std::uint64_t mix(std::uint64_t k) noexcept {
	k ^= k >> 33;
	k *= 0xFF51AFD7ED558CCDull;
	k ^= k >> 33;
	k *= 0xC4CEB9FE1A85EC53ull;
	return k ^ (k >> 33);
}

template <class V, class Recycle = std::default_delete<V>>
class clock_cache {
public:
	// meta: state (2 bits), countdown (2 bits), references (the rest).
	static constexpr std::uint64_t empty = 0, visible = 1, busy = 2, state_mask = 3;
	static constexpr unsigned cd_shift = 2;
	static constexpr std::uint64_t cd_mask = std::uint64_t(3) << cd_shift;
	static constexpr unsigned ref_shift = 4;
	static constexpr std::uint64_t ref_one = std::uint64_t(1) << ref_shift;
	static constexpr std::size_t ways = 16;

	struct slot {
		std::atomic<std::uint64_t> meta{0};
		std::atomic<std::uint64_t> key{0};
		V* value = nullptr;
	};
	struct handle {
		slot* s = nullptr;
		V* value() const noexcept { return s->value; }
	};

	explicit clock_cache(std::size_t capacity_blocks, Recycle recycle = {}) : recycle_(recycle) {
		sets_ = std::max<std::size_t>(1, capacity_blocks / ways);
		slots_ = std::make_unique<slot[]>(sets_ * ways);
	}
	~clock_cache() {
		for (std::size_t i = 0; i < sets_ * ways; ++i) {
			if (slots_[i].value) { recycle_(slots_[i].value); }
		}
	}

	// A hit: the key compared without a reference, then one compare-and-swap takes one and
	// raises the countdown, then the key is checked again under the reference.
	bool lookup(std::uint64_t key, handle& h) noexcept {
		slot* set = set_of(key);
		for (std::size_t w = 0; w < ways; ++w) {
			slot& s = set[w];
			if (s.key.load(std::memory_order_relaxed) != key) { continue; }
			std::uint64_t m = s.meta.load(std::memory_order_relaxed);
			while ((m & state_mask) == visible) {
				const std::uint64_t cd = (m & cd_mask) >> cd_shift;
				const std::uint64_t next = ((m + ref_one) & ~cd_mask) | (std::min<std::uint64_t>(3, cd + 1) << cd_shift);
				if (s.meta.compare_exchange_weak(m, next, std::memory_order_acquire, std::memory_order_relaxed)) {
					if (s.key.load(std::memory_order_relaxed) == key) {
						h.s = &s;
						return true;
					}
					s.meta.fetch_sub(ref_one, std::memory_order_release);
					break;
				}
			}
		}
		return false;
	}
	void release(handle& h) noexcept { h.s->meta.fetch_sub(ref_one, std::memory_order_release); }

	// Inserts and returns a referenced handle; evicts within the key's set if it must.
	handle insert(std::uint64_t key, V* value) {
		slot* set = set_of(key);
		for (;;) {
			for (std::size_t w = 0; w < ways; ++w) {
				std::uint64_t m = empty;
				if (set[w].meta.compare_exchange_strong(m, busy, std::memory_order_acquire, std::memory_order_relaxed)) { return publish(set[w], key, value); }
			}
			// Sweep: an unreferenced entry's countdown drops each pass; at zero it is evicted.
			const std::size_t start = hand();
			for (std::size_t i = 0; i < 4 * ways; ++i) {
				slot& s = set[(start + i) % ways];
				std::uint64_t m = s.meta.load(std::memory_order_relaxed);
				if ((m & state_mask) != visible || (m >> ref_shift) != 0) { continue; }
				if (m & cd_mask) {
					s.meta.compare_exchange_weak(m, m - (std::uint64_t(1) << cd_shift), std::memory_order_relaxed);
					continue;
				}
				if (s.meta.compare_exchange_strong(m, busy, std::memory_order_acquire, std::memory_order_relaxed)) {
					recycle_(s.value);
					return publish(s, key, value);
				}
			}
		}
	}

private:
	handle publish(slot& s, std::uint64_t key, V* value) noexcept {
		s.key.store(key, std::memory_order_relaxed);
		s.value = value;
		s.meta.store(visible | (std::uint64_t(1) << cd_shift) | ref_one, std::memory_order_release);
		return {&s};
	}
	slot* set_of(std::uint64_t key) const noexcept { return &slots_[(mix(key) % sets_) * ways]; }
	std::size_t hand() noexcept { return std::size_t(hand_.fetch_add(1, std::memory_order_relaxed)) % ways; }

	std::size_t sets_ = 0;
	std::unique_ptr<slot[]> slots_;
	std::atomic<std::uint64_t> hand_{0};
	Recycle recycle_;
};

}  // namespace kvproto
