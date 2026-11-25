// catalog's observation: snapshots, statistics and the shutdown report the tests, the sweep and T4 read
#pragma once

#include "strata/core/catalog.hh"

namespace strata {

template <class Index, class H>
auto catalog<Index, H>::snapshot() const -> std::vector<slot_view> {
	std::vector<slot_view> out;
	const std::uint32_t n = slots_made();
	for (std::uint32_t i = 0; i < n; ++i) {
		const slot& s = at(i);
		const std::uint32_t h = s.key_hash.load(std::memory_order_acquire);
		std::lock_guard l(shard_of(h).mu);
		// The word first: a new owner's word (acquire) shows its key_hash, so the recheck skips it and `key` is never read
		// under the wrong shard's mutex.
		const std::uint64_t w = s.word.load(std::memory_order_acquire);
		hook("snapshot.slot", true);
		if (s.key_hash.load(std::memory_order_acquire) != h) { continue; }
		if (word::state(w) != slot_state::free) { out.push_back({s.key, word::state(w), word::pins(w)}); }
	}
	return out;
}

template <class Index, class H>
std::uint32_t catalog<Index, H>::slots_made() const noexcept {
	std::lock_guard l(free_mu_);
	return next_new_;
}

template <class Index, class H>
std::size_t catalog<Index, H>::waiters_of(std::uint32_t idx) const {  // under the shard mutex
	const slot& s = at(idx);
	std::lock_guard l(shard_of(s.key_hash).mu);
	std::size_t n = 0;
	s.waiters.each([&](const waiter&) { ++n; });
	return n;
}

template <class Index, class H>
std::string catalog<Index, H>::key_at(std::uint32_t idx) const {  // under the shard mutex; empty for a free slot
	const slot& s = at(idx);
	std::lock_guard l(shard_of(s.key_hash).mu);
	return s.key;
}

template <class Index, class H>
std::size_t catalog<Index, H>::table_size() const {
	std::size_t n = 0;
	for (const auto& sh : shards_) {
		std::lock_guard l(sh.mu);
		n += sh.table.size();
	}
	return n;
}

template <class Index, class H>
auto catalog<Index, H>::stats() const noexcept -> stats_t {
	stats_t o;
	o.misses = misses_.load(), o.opens = opens_.load(), o.open_failures = open_failures_.load(), o.failed_fast = failed_fast_.load();
	o.retries = retries_.load(), o.stale_handles = stale_.load(), o.handle_pins = handle_pins_.load(), o.wakes_posted = wakes_.load(), o.wake_reposts = reposts_.load();
	o.promoted = promoted_.load(), o.decayed = decayed_.load(), o.evictions = evictions_.load();
	o.evict_demotes = evict_demotes_.load(), o.busy = busy_.load(), o.ghost_hits = ghost_hits_.load(), o.ghost_adds = ghost_adds_.load();
	o.wake_failures = wake_failures_.load(), o.wake_overloads = wake_overloads_.load(), o.wake_gated = wake_gated_.load();
	o.bound_closes = bound_closes_.load(), o.bound_commits = bound_commits_.load(), o.commit_failures = commit_failures_.load();
	o.passes = passes_.load(), o.pass_demotes = pass_demotes_.load(), o.pass_closes = pass_closes_.load(), o.pass_reposts = pass_reposts_.load();
	o.reclaims = reclaims_.load(), o.erased = erased_.load(), o.stuck_closes = stuck_closes_.load(), o.stuck_retries = stuck_retries_.load();
	o.close_failures = close_failures_.load(), o.shutdown_commits = shutdown_commits_.load(), o.shutdown_commit_failures = shutdown_commit_failures_.load();
	o.payload_close_failures = payload_close_failures_.load();
	o.merge_commits = merge_commits_.load(), o.merge_commit_failures = merge_commit_failures_.load(), o.create_reposts = create_reposts_.load(), o.replaces = replaces_.load(), o.replace_commit_failures = replace_commit_failures_.load();
	o.drain_unpins = drain_unpins_.load(), o.replace_reposts = replace_reposts_.load();
	o.unlinked = unlinked_.load(), o.gone_pins = gone_pins_.load(), o.cancels = cancels_.load();
	for (unsigned i = 0; i < slot_states; ++i) {
		o.waited[i] = waited_[i].load();
		for (unsigned j = 0; j < slot_states; ++j) { o.edges[i][j] = edges_[i][j].load(); }
	}
	return o;
}

template <class Index, class H>
std::vector<shutdown_holder> catalog<Index, H>::shutdown_report() const {
	std::vector<shutdown_holder> out;
	const std::uint32_t n = slots_made();
	for (std::uint32_t i = 0; i < n; ++i) {
		const slot& s = at(i);
		const std::uint32_t h = s.key_hash.load(std::memory_order_acquire);
		std::lock_guard l(shard_of(h).mu);
		const std::uint64_t w = s.word.load(std::memory_order_acquire);
		const slot_state st = word::state(w);
		if (st == slot_state::free || s.key_hash.load(std::memory_order_relaxed) != h) { continue; }
		shutdown_hold why = shutdown_hold::waiting;
		const std::uint8_t held = s.holdup.load(std::memory_order_relaxed);
		if (st == slot_state::closing && s.stuck.load(std::memory_order_acquire)) {
			why = shutdown_hold::view_held;
		} else if (st == slot_state::closing && held == hold_throws) {
			why = shutdown_hold::close_throws;
		} else if (st == slot_state::closing && held == hold_lease) {
			why = shutdown_hold::lease_busy;
		} else if (word::pins(w) != 0) {
			why = shutdown_hold::pinned;
		} else if (st != slot_state::open && st != slot_state::owed && st != slot_state::failed) {
			why = shutdown_hold::transition;
		}
		out.push_back({s.key, std::uint8_t(st), word::pins(w), why});
	}
	return out;
}

template <class Index, class H>
void catalog<Index, H>::set_pins_for_test(std::uint32_t idx, std::uint64_t pins) noexcept {
	slot& s = at(idx);
	std::uint64_t cur = s.word.load(std::memory_order_acquire);
	while (!s.word.compare_exchange_weak(cur, word::with_pins(cur, pins), std::memory_order_acq_rel, std::memory_order_acquire)) {}
}

}  // namespace strata
