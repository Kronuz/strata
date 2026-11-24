// catalog's sweep and closing policy: the pass, the soft caps, and S3-FIFO's queues, ghost and order
#pragma once

#include "strata/core/catalog.hh"

namespace strata {

template <class Index, class H>
void catalog<Index, H>::start() {
	if (started_.exchange(true)) { return; }
	stopped_.store(false);
	listener_ = h_.budget->add_listener([l = life_] {
		std::lock_guard g(l->mu);
		if (l->c) { l->c->post_pass(); }
	});
	arm_period();
}

template <class Index, class H>
void catalog<Index, H>::stop_sweep() {
	stopped_.store(true);
	if (const std::uint64_t id = std::exchange(listener_, 0)) { h_.budget->remove_listener(id); }
}

template <class Index, class H>
void catalog<Index, H>::post_pass() {
	if (stopped_.load(std::memory_order_acquire) || pass_queued_.exchange(true, std::memory_order_acq_rel)) { return; }
	try {
		post([this] { run_pass(); });
	} catch (...) {
		pass_queued_.store(false, std::memory_order_release);  // nothing is queued: the next post must not be refused
		throw;
	}
}

template <class Index, class H>
auto catalog<Index, H>::policy_pass(const policy_limits& lim) -> policy_result {
	policy_result r;
	passes_.fetch_add(1, std::memory_order_relaxed);
	h_.budget->tick();
	const bool down = shutting_.load(std::memory_order_acquire);
	const std::uint64_t now = exec_now();
	pass_at_.store(now, std::memory_order_relaxed);
	// What the caps ask for, in the unit each is released in: hot indexes and mapped records (demotion). The
	// demotions and closes begun and not ended are relief already posted, and come off the shares.
	const auto sub = [](std::uint64_t x, std::uint64_t by) { return x > by ? x - by : 0; };
	std::uint64_t over_hot = 0;
	if (h_.soft_hot != 0) {  // hot indexes: slots whose maps count is above zero (C22)
		const std::uint64_t hot = counts_.holders_mapped.load(std::memory_order_relaxed);
		const std::uint64_t pending = pending_hot_.load(std::memory_order_relaxed);  // read after the count: see closing_over()
		over_hot = sub(hot > h_.soft_hot ? hot - h_.soft_hot : 0, pending);
	}
	const std::uint64_t maps_share = std::max(share(resource::maps), share(resource::vnodes)), pending_maps = pending_maps_.load(std::memory_order_relaxed);
	std::uint64_t over_maps = sub(maps_share, pending_maps);
	struct candidate {
		std::uint32_t idx;
		std::uint64_t last, idle, maps;
		bool hot, due;
	};
	std::vector<candidate> cands;
	std::uint64_t posts = 0;
	const std::uint32_t made = slots_made();
	const std::uint64_t visits = std::min<std::uint64_t>(lim.visits, made);
	for (std::uint64_t v = 0; v < visits; ++v) {
		if (cursor_ >= made) { cursor_ = 0; }
		const std::uint32_t i = cursor_++;
		slot& s = at(i);
		const std::uint64_t w = s.word.load(std::memory_order_acquire);
		const slot_state st = word::state(w);
		if (st == slot_state::failed) {
			if (word::pins(w) == 0) { (void)erase_failed(i, now, false); }
		} else if (st == slot_state::closing) {
			if (posts < lim.transitions && s.stuck.load(std::memory_order_acquire) && !s.retrying.exchange(true, std::memory_order_acq_rel)) {
				++posts;
				post([this, i] { run_stuck_retry(i); });
			}
		} else if ((st == slot_state::open || st == slot_state::owed) && word::pins(w) == 0 && !down) {
			if (st == slot_state::owed && posts < lim.transitions && begin_bound_close(i)) {
				++posts;
				continue;
			}
			if (now < s.probe_at.load(std::memory_order_relaxed)) { continue; }  // refused lately, or a failed commit's probe
			const std::uint64_t last = s.last_use.load(std::memory_order_relaxed), maps = s.counts.get(resource::maps);
			const bool hot = st == slot_state::open && maps != 0;
			const bool due = !hot && s.due.load(std::memory_order_acquire) != 0;
			cands.push_back({i, last, now > last ? now - last : 0, maps, hot, due});
		}
	}
	std::sort(cands.begin(), cands.end(), [](const candidate& a, const candidate& b) { return a.last < b.last; });
	for (const candidate& c : cands) {
		if (posts >= lim.transitions) { break; }
		if (c.due) {  // a slot demoted for a closing cap whose close the demotion's end could not begin
			if (begin_due_close(c.idx)) {
				++posts, ++r.closes;
				pass_closes_.fetch_add(1, std::memory_order_relaxed);
			}
		} else if (c.hot) {
			if (c.idle >= h_.demote_idle_ns || over_hot + over_maps != 0) {
				if (begin_demote(c.idx)) {
					++posts, ++r.demotes;
					pass_demotes_.fetch_add(1, std::memory_order_relaxed);
					over_hot = sub(over_hot, 1);
					over_maps = sub(over_maps, c.maps);
				}
			}
		} else if (c.idle >= h_.close_idle_ns) {  // idle long enough: closed whatever the caps say, and not ghosted
			if (begin_close(c.idx)) {
				++posts, ++r.closes;
				pass_closes_.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
	evict(lim, r, posts);
	r.again = posts != 0 && (over_hot + over_maps != 0 || closing_over() != 0);
	return r;
}

template <class Index, class H>
void catalog<Index, H>::clear_freq(std::uint32_t idx) {
	hook("policy.clear", false);
	at(idx).word.fetch_and(~word::freq_mask, std::memory_order_acq_rel);
}

template <class Index, class H>
void catalog<Index, H>::decay(std::uint32_t idx) {
	slot& s = at(idx);
	std::uint64_t w = s.word.load(std::memory_order_relaxed);
	while (word::freq(w) > 0) {
		hook("policy.decay", false);
		if (s.word.compare_exchange_weak(w, w - word::one_freq, std::memory_order_acq_rel, std::memory_order_relaxed)) { return; }
	}
}

template <class Index, class H>
auto catalog<Index, H>::queue_sizes() const -> queue_sizes_t {
	std::lock_guard l(policy_mu_);
	return {qsize_[1], qsize_[2], ghost_.size()};
}

template <class Index, class H>
unsigned catalog<Index, H>::queue_of(std::uint32_t idx) const {
	std::lock_guard l(policy_mu_);
	return at(idx).q;
}

template <class Index, class H>
std::uint64_t catalog<Index, H>::ghost_bytes() const {
	std::lock_guard l(policy_mu_);
	return ghost_.bytes();
}

template <class Index, class H>
void catalog<Index, H>::over_soft() noexcept {
	if (!started_.load(std::memory_order_relaxed) || pass_queued_.load(std::memory_order_relaxed)) { return; }
	const std::uint64_t band = h_.pass_posts > 1 ? h_.pass_posts - 1 : 0;
	bool over = false;
	if (h_.soft_hot != 0) {  // the count before the relief, as in policy_pass: a race over-counts
		const std::uint64_t hot = counts_.holders_mapped.load(std::memory_order_relaxed);
		over = hot > h_.soft_hot + band + pending_hot_.load(std::memory_order_relaxed);
	}
	if (!over && h_.soft_open != 0) {
		const std::uint64_t live = live_.load(std::memory_order_relaxed), closing = closing_.load(std::memory_order_relaxed);
		over = live > h_.soft_open + band + closing + pending_open_.load(std::memory_order_relaxed);
	}
	if (!over) { return; }
	try {
		post_pass();
	} catch (...) {  // the next miss, unpin or period posts it
	}
}

template <class Index, class H>
void catalog<Index, H>::push_tail_locked(std::uint32_t idx, unsigned q) {
	slot& s = at(idx);
	assert(s.q == 0 && (q == 1 || q == 2));
	s.q = static_cast<std::uint8_t>(q), s.q_next = none, s.q_prev = qtail_[q];
	(qtail_[q] != none ? at(qtail_[q]).q_next : qhead_[q]) = idx;
	qtail_[q] = idx;
	++qsize_[q];
}

template <class Index, class H>
void catalog<Index, H>::unlink_locked(std::uint32_t idx) {
	slot& s = at(idx);
	const unsigned q = s.q;
	assert(q == 1 || q == 2);
	(s.q_prev != none ? at(s.q_prev).q_next : qhead_[q]) = s.q_next;
	(s.q_next != none ? at(s.q_next).q_prev : qtail_[q]) = s.q_prev;
	s.q = 0, s.q_prev = s.q_next = none;
	--qsize_[q];
}

template <class Index, class H>
void catalog<Index, H>::policy_enqueue(std::uint32_t idx) {
	slot& s = at(idx);
	const std::uint64_t h = key_hash64(s.key);
	std::lock_guard l(policy_mu_);
	if (s.q != 0) { return; }
	const bool seen = ghost_.take(h);
	if (seen) { ghost_hits_.fetch_add(1, std::memory_order_relaxed); }
	push_tail_locked(idx, seen ? 2 : 1);
}

template <class Index, class H>
void catalog<Index, H>::policy_unlink(std::uint32_t idx) {
	std::lock_guard l(policy_mu_);
	if (at(idx).q != 0) { unlink_locked(idx); }
}

template <class Index, class H>
void catalog<Index, H>::policy_ghost(std::uint64_t h) {
	std::lock_guard l(policy_mu_);
	ghost_.add(h);
	ghost_adds_.fetch_add(1, std::memory_order_relaxed);
}

template <class Index, class H>
void catalog<Index, H>::policy_requeue(std::uint32_t idx, std::uint64_t gen, unsigned q) {
	std::lock_guard l(policy_mu_);
	const std::uint64_t w = at(idx).word.load(std::memory_order_acquire);
	const slot_state st = word::state(w);
	if (at(idx).q == 0 && word::gen(w) == gen && st != slot_state::closing && st != slot_state::free) { push_tail_locked(idx, q); }
}

template <class Index, class H>
std::uint64_t catalog<Index, H>::closing_over() const noexcept {
	const auto sub = [](std::uint64_t x, std::uint64_t by) { return x > by ? x - by : 0; };
	// Each share is read before the relief posted against it (a drop lowers a close's booking before the catalog's
	// count), the open count before the closing count (a close's end lowers the closing count first) and the closing
	// count before the booked closes (a close's begin releases its booking first), so a race over-counts and never
	// ends the pass short; the loads are statements because argument order is unspecified.
	const std::uint64_t descriptors = share(resource::descriptors), vnodes = share(resource::vnodes);
	std::atomic_thread_fence(std::memory_order_acquire);  // acquire: a drop the shares saw has lowered its booking
	const std::uint64_t booked_desc = pending_descriptors(), booked_vnodes = pending_vnodes();
	std::uint64_t over = sub(descriptors, booked_desc) + sub(vnodes, booked_vnodes);
	if (h_.soft_open != 0) {
		const std::uint64_t live = live_.load(std::memory_order_acquire);  // acquire: sees the `closing_` lowered before it
		const std::uint64_t closing = closing_.load(std::memory_order_acquire);  // acquire: sees the booking released before it
		const std::uint64_t booked = pending_open_.load(std::memory_order_relaxed);
		over += sub(sub(live, closing + booked), h_.soft_open);
	}
	return over;
}

template <class Index, class H>
bool catalog<Index, H>::pick(std::uint32_t& idx, std::uint64_t& gen, unsigned& q, std::uint64_t& round) {
	std::lock_guard l(policy_mu_);
	round = qsize_[1] + qsize_[2];
	if (round == 0) { return false; }
	q = (qsize_[1] > small_cap_ || qsize_[2] == 0) ? 1 : 2;
	idx = qhead_[q];
	unlink_locked(idx);
	gen = word::gen(at(idx).word.load(std::memory_order_acquire));
	return true;
}

template <class Index, class H>
void catalog<Index, H>::evict(const policy_limits& lim, policy_result& r, std::uint64_t& posts) {
	std::uint64_t skipped = 0;
	const std::uint64_t now = exec_now();
	for (std::uint32_t idx; r.visited < lim.visits && posts < lim.transitions && closing_over() != 0;) {
		std::uint64_t gen, round;
		unsigned q;
		if (!pick(idx, gen, q, round)) { break; }
		++r.visited;
		slot& s = at(idx);
		const std::uint64_t w = s.word.load(std::memory_order_acquire);
		if (word::freq(w) != 0) {  // used since it entered: small moves to main with the frequency cleared, main decrements
			if (q == 1) {
				clear_freq(idx);
				promoted_.fetch_add(1, std::memory_order_relaxed), ++r.promoted;
			} else {
				decay(idx);
				decayed_.fetch_add(1, std::memory_order_relaxed), ++r.decayed;
			}
			policy_requeue(idx, gen, 2);
			skipped = 0;
			continue;
		}
		const slot_state st = word::state(w);
		if (now < s.probe_at.load(std::memory_order_relaxed)) {  // refused lately, or a failed commit's probe: not again yet
			busy_.fetch_add(1, std::memory_order_relaxed), ++r.busy;
			policy_requeue(idx, gen, q);
		} else if (st == slot_state::open && word::pins(w) == 0 && s.counts.get(resource::maps) != 0) {  // hot: demote first, for the close it books
			if (begin_demote(idx, true, q == 1)) {
				++posts, ++r.demotes;
				evict_demotes_.fetch_add(1, std::memory_order_relaxed), pass_demotes_.fetch_add(1, std::memory_order_relaxed);
			}
			policy_requeue(idx, gen, q);
		} else {
			if (st == slot_state::open || st == slot_state::owed) {
				if (q == 1) { s.to_ghost.store(true, std::memory_order_relaxed); }
				if (begin_close(idx, true)) {
					++posts, ++r.closes;
					evictions_.fetch_add(1, std::memory_order_relaxed), pass_closes_.fetch_add(1, std::memory_order_relaxed);
					skipped = 0;
					continue;
				}
				s.to_ghost.store(false, std::memory_order_relaxed);
			}
			busy_.fetch_add(1, std::memory_order_relaxed), ++r.busy;  // pinned, in a transition, or used since: put back
			policy_requeue(idx, gen, q);
		}
		if (++skipped > round) { break; }  // a full round with nothing to take
	}
}

template <class Index, class H>
void catalog<Index, H>::run_pass() {
	bool again = false;
	try {
		again = pass();
	} catch (...) {
	}
	pass_queued_.store(false, std::memory_order_release);  // a crossing from here on posts its own
	arm_period();
	if (again) {
		pass_reposts_.fetch_add(1, std::memory_order_relaxed);
		post_pass();
	}
}

template <class Index, class H>
void catalog<Index, H>::arm_period() {
	if (stopped_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire) || shut_done_.load(std::memory_order_acquire)) { return; }
	if (period_armed_.exchange(true, std::memory_order_acq_rel)) { return; }
	h_.timer(h_.sweep_period_ns, [l = life_] {
		std::lock_guard g(l->mu);
		if (l->c) {
			l->c->period_armed_.store(false, std::memory_order_release);
			l->c->post_pass();
		}
	});
}

template <class Index, class H>
std::uint64_t catalog<Index, H>::share(resource r) const noexcept {
	const std::uint64_t ex = h_.budget->excess(r), used = h_.budget->used(r);
	if (ex == 0 || used == 0) { return 0; }
	return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(double(ex) * double(counts_.get(r)) / double(used)));
}

}  // namespace strata
