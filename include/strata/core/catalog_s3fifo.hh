// The catalog's closing policy as a subobject (cold-tier D6, CT-R10): S3-FIFO's small and main FIFOs and
// its ghost over slot indices, and the order a pass takes candidates in under the policy limits. It owns
// no slot: `Slots` reaches each slot's links and generation (docs/mechanisms/catalog-layout.md).
#pragma once

#include "strata/core/ghost_set.hh"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace strata::catalog_detail {

// What one pass may do: slots the window visits and queue candidates examined (a move, a decrement or a
// requeue counts), and transitions posted. `lifted()` is the policy tests' (and the stress's) unlimited pass.
struct policy_limits {
	std::uint64_t visits = 256, transitions = 16;
	static policy_limits lifted() noexcept { return {~std::uint64_t(0), ~std::uint64_t(0)}; }
};
struct policy_result {
	bool again = false;  // it posted some and the soft caps still call for more
	std::uint64_t visited = 0, promoted = 0, decayed = 0, closes = 0, demotes = 0, busy = 0;
};

// A candidate's fate, as the catalog decides it from the slot's word: `used` (its frequency was set: the
// catalog cleared it in the small FIFO, decremented it in the main one), `closed` (its close began), or
// `kept` (pinned, in a transition, hot and demoted, or refused lately: it goes back where it was).
enum class visit { used, closed, kept };

struct fifo_sizes {
	std::uint64_t small = 0, main = 0, ghost = 0;
};

// `Slots` gives, for a slot index: `prev(i)`, `next(i)` (std::uint32_t&) and `queue(i)` (std::uint8_t&), the
// links this class alone writes, under its mutex; `gen(i)`, the slot's generation now; and `queueable(i, gen)`,
// whether the slot may be queued: `gen` is still its generation and it is neither closing nor free. Both
// are read under the mutex, which `finish_close` takes to unlink a slot once it is closing, so a requeue
// is either refused or unlinked after.
template <class Slots>
class s3fifo {
public:
	static constexpr std::uint32_t none = ~std::uint32_t(0);
	static constexpr unsigned small = 1, main = 2;

	explicit s3fifo(Slots slots) noexcept : slots_(slots) {}

	// The small FIFO is a tenth of `soft_open`, the ghost half of it; with no cap the small FIFO has none.
	void cap(std::uint64_t soft_open) {
		small_cap_ = soft_open / 10;
		ghost_ = ghost_set(static_cast<std::size_t>(soft_open / 2));
	}

	// An open slot enters the small FIFO, or the main one when `h` (its key's hash) is in the ghost, which
	// gives it up. True on a ghost hit. A slot already queued stays where it is.
	bool enqueue(std::uint32_t idx, std::uint64_t h) {
		std::lock_guard l(mu_);
		if (slots_.queue(idx) != 0) { return false; }
		const bool seen = ghost_.take(h);
		push_tail(idx, seen ? main : small);
		return seen;
	}
	// A slot leaves the queue it is in (a close ends).
	void unlink(std::uint32_t idx) {
		std::lock_guard l(mu_);
		if (slots_.queue(idx) != 0) { unlink_locked(idx); }
	}
	// The ghost remembers the hash of a key closed from the small FIFO, its frequency clear.
	void ghost(std::uint64_t h) {
		std::lock_guard l(mu_);
		ghost_.add(h);
	}
	// Puts a slot back at the tail of `q` unless it is in a queue or `Slots::queueable` refuses it.
	void requeue(std::uint32_t idx, std::uint64_t gen, unsigned q) {
		std::lock_guard l(mu_);
		if (slots_.queue(idx) == 0 && slots_.queueable(idx, gen)) { push_tail(idx, q); }
	}

	// The next candidate: the small FIFO's head while it is over its cap or the main FIFO is empty, else the
	// main FIFO's, taken out of its queue with its generation; false when both are empty. `round`: the queued
	// slots, for the caller's bound.
	bool pick(std::uint32_t& idx, std::uint64_t& gen, unsigned& q, std::uint64_t& round) {
		std::lock_guard l(mu_);
		round = size_[small] + size_[main];
		if (round == 0) { return false; }
		q = (size_[small] > small_cap_ || size_[main] == 0) ? small : main;
		idx = head_[q];
		unlink_locked(idx);
		gen = slots_.gen(idx);
		return true;
	}

	// Takes candidates in S3-FIFO's order while `over()` and the limits allow, each decided by `decide(idx, q, r,
	// posts)`: a used one goes to the main FIFO's tail, a kept one back to its own FIFO's tail, its generation
	// checked. A full round of kept candidates ends the pass.
	template <class Over, class Decide>
	void evict(const policy_limits& lim, policy_result& r, std::uint64_t& posts, Over&& over, Decide&& decide) {
		std::uint64_t skipped = 0;
		for (std::uint32_t idx; r.visited < lim.visits && posts < lim.transitions && over();) {
			std::uint64_t gen, round;
			unsigned q;
			if (!pick(idx, gen, q, round)) { break; }
			++r.visited;
			const visit v = decide(idx, q, r, posts);
			if (v == visit::used) {
				requeue(idx, gen, main);
				skipped = 0;
				continue;
			}
			if (v == visit::closed) {
				skipped = 0;
				continue;
			}
			requeue(idx, gen, q);
			if (++skipped > round) { break; }  // a full round with nothing to take
		}
	}

	fifo_sizes sizes() const {
		std::lock_guard l(mu_);
		return {size_[small], size_[main], ghost_.size()};
	}
	// The queue a slot is in: 0 none, 1 small, 2 main.
	unsigned queue_of(std::uint32_t idx) const {
		std::lock_guard l(mu_);
		return slots_.queue(idx);
	}
	std::uint64_t small_cap() const noexcept { return small_cap_; }
	std::uint64_t ghost_capacity() const noexcept { return ghost_.capacity(); }
	// The ghost's bytes now (nodes and table).
	std::uint64_t ghost_bytes() const {
		std::lock_guard l(mu_);
		return ghost_.bytes();
	}

private:
	void push_tail(std::uint32_t idx, unsigned q) {
		assert(slots_.queue(idx) == 0 && (q == small || q == main));
		slots_.queue(idx) = static_cast<std::uint8_t>(q), slots_.next(idx) = none, slots_.prev(idx) = tail_[q];
		(tail_[q] != none ? slots_.next(tail_[q]) : head_[q]) = idx;
		tail_[q] = idx;
		++size_[q];
	}
	void unlink_locked(std::uint32_t idx) {
		const unsigned q = slots_.queue(idx);
		assert(q == small || q == main);
		const std::uint32_t prev = slots_.prev(idx), next = slots_.next(idx);
		(prev != none ? slots_.next(prev) : head_[q]) = next;
		(next != none ? slots_.prev(next) : tail_[q]) = prev;
		slots_.queue(idx) = 0, slots_.prev(idx) = slots_.next(idx) = none;
		--size_[q];
	}

	mutable std::mutex mu_;
	Slots slots_;
	std::uint32_t head_[3] = {none, none, none}, tail_[3] = {none, none, none};
	std::uint64_t size_[3] = {0, 0, 0};
	ghost_set ghost_;
	std::uint64_t small_cap_ = ~std::uint64_t(0);
};

}  // namespace strata::catalog_detail
