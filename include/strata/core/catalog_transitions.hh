// catalog's slot transitions, one state machine over the slot's word: open, wake, demote, close, replace, shutdown, and how each ends
#pragma once

#include "strata/core/catalog.hh"

namespace strata {

template <class Index, class H>
catalog<Index, H>::shutdown_awaiter::~shutdown_awaiter() {
	if (w_) { catalog::abandon(c_, std::exchange(w_, nullptr)); }
}

template <class Index, class H>
bool catalog<Index, H>::shutdown_awaiter::await_ready() {
	w_ = new waiter;
	if (!c_->join_shutdown(w_)) { return false; }
	std::exchange(w_, nullptr)->release();
	return true;
}

template <class Index, class H>
bool catalog<Index, H>::shutdown_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
	w_->h = h;
	int expect = waiter::registered;
	return w_->st.compare_exchange_strong(expect, waiter::suspended, std::memory_order_acq_rel, std::memory_order_acquire);
}

template <class Index, class H>
shutdown_result catalog<Index, H>::shutdown_awaiter::await_resume() {
	if (waiter* w = std::exchange(w_, nullptr)) { w->release(); }
	return c_->shutdown_outcome();
}

template <class Index, class H>
bool catalog<Index, H>::begin_demote(std::uint32_t idx, bool due, bool ghost) {
	slot& s = at(idx);
	std::uint64_t w = s.word.load(std::memory_order_acquire);
	for (;;) {
		if (word::pins(w) != 0 || word::state(w) != slot_state::open) { return false; }
		hook("sweep.demote", false);
		if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::demoting), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
	}
	note(slot_state::open, slot_state::demoting);
	book(s, resource::maps);
	if (due) { book_due(s, ghost); }
	post([this, idx] { run_demote(idx); });
	return true;
}

template <class Index, class H>
bool catalog<Index, H>::begin_close(std::uint32_t idx, bool unused) {
	slot& s = at(idx);
	std::uint64_t w = s.word.load(std::memory_order_acquire);
	slot_state prior;
	for (;;) {
		prior = word::state(w);
		if (word::pins(w) != 0 || (prior != slot_state::open && prior != slot_state::owed) || (unused && word::freq(w) != 0)) { return false; }
		hook("sweep.close", false);
		if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
	}
	release_due(s);  // before note(), as in reopen
	note(prior, slot_state::closing);
	book(s, resource::descriptors);
	post([this, idx, prior] { run_close(idx, prior, close_mode::sweep); });
	return true;
}

template <class Index, class H>
bool catalog<Index, H>::begin_due_close(std::uint32_t idx) {
	slot& s = at(idx);
	const bool ghost = s.due.load(std::memory_order_acquire) == 2;
	if (ghost) { s.to_ghost.store(true, std::memory_order_relaxed); }
	if (begin_close(idx)) { return true; }
	if (ghost) { s.to_ghost.store(false, std::memory_order_relaxed); }
	return false;
}

template <class Index, class H>
bool catalog<Index, H>::begin_bound_close(std::uint32_t idx) {
	slot& s = at(idx);
	std::uint64_t w = s.word.load(std::memory_order_acquire);
	for (;;) {
		if (word::pins(w) != 0 || word::state(w) != slot_state::owed) { return false; }
		if (s.failures.load(std::memory_order_relaxed) < h_.wake_bound || exec_now() < s.probe_at.load(std::memory_order_relaxed)) { return false; }
		hook("sweep.bound", false);
		if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
	}
	release_due(s);  // before note(), as in reopen
	note(slot_state::owed, slot_state::closing);
	book(s, resource::descriptors);
	post([this, idx] { run_close(idx, slot_state::owed, close_mode::bound); });
	return true;
}

template <class Index, class H>
void catalog<Index, H>::note(slot_state from, slot_state to) noexcept {
	edges_[unsigned(from)][unsigned(to)].fetch_add(1, std::memory_order_relaxed);
	const auto has_index = [](slot_state x) { return x != slot_state::free && x != slot_state::failed; };
	const bool closing_moves = (from == slot_state::closing) != (to == slot_state::closing), live_moves = has_index(from) != has_index(to);
	// A begin's release: a closing_over() that reads this `closing_` (acquire) sees the due booking released first. An
	// end's is relaxed: seen alone it over-counts, and an end that lowers `live_` too is ordered by that release.
	if (closing_moves) { (to == slot_state::closing ? closing_.fetch_add(1, std::memory_order_release) : closing_.fetch_sub(1, std::memory_order_relaxed)); }
	if (closing_moves && live_moves) {  // a close's end between its two counts, only under the shard's mutex (close.end)
		try { hook("note.mid", true); } catch (...) {}
	}
	// Release: a closing_over() that reads this `live_` (acquire) sees the `closing_` a close's end lowered just above.
	if (live_moves) { (has_index(to) ? live_.fetch_add(1, std::memory_order_release) : live_.fetch_sub(1, std::memory_order_release)); }
	if (to == slot_state::free) { occupied_.fetch_sub(1, std::memory_order_relaxed); }  // a miss counts its own (B1)
}

template <class Index, class H>
void catalog<Index, H>::book(slot& s, resource r) noexcept {
	if (r == resource::maps) {
		const std::uint32_t m = std::uint32_t(s.counts.get(resource::maps));
		s.pend_maps.store(m, std::memory_order_relaxed);
		pending_maps_.fetch_add(m, std::memory_order_relaxed);
		pending_hot_.fetch_add(1, std::memory_order_relaxed);  // every begun demotion ends in end_at
	} else {
		book_close(s);
	}
}

template <class Index, class H>
void catalog<Index, H>::book_close(slot& s) noexcept {
	s.counts.book(resource::descriptors, counts_);
	s.counts.book(resource::vnodes, counts_);
}

template <class Index, class H>
void catalog<Index, H>::unbook_close(slot& s) noexcept {
	s.counts.unbook(resource::descriptors, counts_);
	s.counts.unbook(resource::vnodes, counts_);
}

template <class Index, class H>
void catalog<Index, H>::unbook(slot& s) noexcept {
	pending_maps_.fetch_sub(s.pend_maps.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
	unbook_close(s);
}

template <class Index, class H>
void catalog<Index, H>::book_due(slot& s, bool ghost) noexcept {
	release_due(s);
	book_close(s);
	pending_open_.fetch_add(1, std::memory_order_relaxed);
	s.due.store(ghost ? 2 : 1, std::memory_order_release);  // 2: the close enters the ghost (it left the small FIFO unused)
}

template <class Index, class H>
void catalog<Index, H>::release_due(slot& s) noexcept {
	if (!s.due.exchange(0, std::memory_order_acq_rel)) { return; }
	unbook_close(s);
	pending_open_.fetch_sub(1, std::memory_order_relaxed);
}

template <class Index, class H>
void catalog<Index, H>::run_open(std::uint32_t idx) {
	slot& s = at(idx);
	std::exception_ptr err;
	opening op{s.key, io_counts{h_.budget, &counts_, &s.counts}, false};
	{
		std::lock_guard lock(shard_of(s.key_hash).mu);
		op.create = s.attempt_create = s.create;
	}
	try {
		s.io = h_.resolve(op);
		op.io = s.io.get();
		s.index = std::make_unique<Index>(h_.open_index(*s.io, h_.index_options, op));
		s.index->guard_calls(&s.word, &pin_held);
		s.reader = s.index->reader();
		h_.open_payload(op, *s.index, s.payload);
		if (!s.payload) { throw std::logic_error("strata: open_payload did not make H"); }
	} catch (...) {
		err = std::current_exception();
		s.payload.reset();
		s.reader = {};
		s.index.reset();
		s.io.reset();
	}
	finish_open(idx, err);
}

template <class Index, class H>
bool catalog<Index, H>::is_absent(const std::exception_ptr& e) {
	if (!e) { return false; }
	try {
		std::rethrow_exception(e);
	} catch (const index_not_created&) {
		return true;
	} catch (...) {
		return false;
	}
}

template <class Index, class H>
void catalog<Index, H>::finish_open(std::uint32_t idx, const std::exception_ptr& err) {
	slot& s = at(idx);
	waiter* list;
	std::uint64_t gen;
	const bool absent = is_absent(err);
	if (!err) { policy_enqueue(idx); }
	{
		std::unique_lock lock(shard_of(s.key_hash).mu);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		assert(word::state(cur) == slot_state::opening);
		if (absent && s.create && !s.attempt_create) {  // a create pinner joined after the attempt began: run it again with the flag
			create_reposts_.fetch_add(1, std::memory_order_relaxed);
			lock.unlock();
			post([this, idx] { run_open(idx); });
			return;
		}
		const bool created = s.attempt_create;
		s.create = s.attempt_create = false;
		s.absent = absent && !created;  // a create pin passes only the entry of an attempt that did not create
		if (err) {
			if (s.failures.load(std::memory_order_relaxed) != UINT8_MAX) { s.failures.fetch_add(1, std::memory_order_relaxed); }
			s.error = err;
			s.retry_at = exec_now() + catalog_detail::backoff_ns(s.failures.load(std::memory_order_relaxed));
		} else {
			s.failures.store(0, std::memory_order_relaxed);
			s.streak = 0;
			s.error = nullptr;
			s.retry_at = 0;
			s.probe_at.store(0, std::memory_order_relaxed);
			clear_failing(s);
		}
		for (;;) {
			const std::uint64_t next = err ? word::next_gen(word::with_pins(word::with_state(cur, slot_state::failed), 0)) : word::with_state(cur, slot_state::open);
			hook("open.end", true);
			if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
				gen = word::gen(next);
				break;
			}
		}
		note(slot_state::opening, err ? slot_state::failed : slot_state::open);
		(err ? open_failures_ : opens_).fetch_add(1, std::memory_order_relaxed);
		list = take(s);
	}
	hook("open.unlocked", false);
	deliver(list, gen, err);
	kick(idx);
}

template <class Index, class H>
std::exception_ptr catalog<Index, H>::wake_gate(const slot& s) {
	if (s.streak == 0 || exec_now() >= s.retry_at) { return nullptr; }
	failed_fast_.fetch_add(1, std::memory_order_relaxed);
	wake_gated_.fetch_add(1, std::memory_order_relaxed);
	return s.error;
}

template <class Index, class H>
std::uint64_t catalog<Index, H>::split_requests(waiter*& list, waiter*& chores) noexcept {
	std::uint64_t n = 0;
	waiter *req = nullptr, *tail = nullptr;
	for (waiter* x = list; x;) {
		waiter* next = x->next;
		if (x->kind == pin_kind::request) {
			++n;
			x->next = req, req = x;
		} else {
			x->next = nullptr;
			(tail ? tail->next : chores) = x;
			tail = x;
		}
		x = next;
	}
	list = req;
	return n;
}

template <class Index, class H>
void catalog<Index, H>::end_at(std::uint32_t idx, slot_state from, slot_state target) {
	slot& s = at(idx);
	waiter *list = nullptr, *chores = nullptr;
	std::uint64_t gen;
	bool wake = false, close_due = false;
	std::exception_ptr gate;
	slot_state to = target;
	{
		std::lock_guard lock(shard_of(s.key_hash).mu);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		assert(word::state(cur) == from);
		assert(from != slot_state::demoting || s.streak == 0);  // a demotion starts only from `{0, open}`: no backoff
		bool request_waiter = false;
		s.waiters.each([&](const waiter& w) { request_waiter |= w.kind == pin_kind::request; });
		if (target == slot_state::owed && request_waiter) {
			gate = wake_gate(s);
			wake = !gate;
		}
		to = wake ? slot_state::waking : target;
		std::uint64_t failed = 0;
		if (!wake) {
			list = take(s);
			if (gate) { failed = split_requests(list, chores); }
		}
		if (from == slot_state::demoting) {  // still `demoting`: no close can begin and take the booking meanwhile
			pending_maps_.fetch_sub(s.pend_maps.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
			pending_hot_.fetch_sub(1, std::memory_order_relaxed);
			// Its waiters take the slot, or the demotion left maps (a view held): the booked close will not come.
			if (word::pins(cur) != failed || s.counts.get(resource::maps) != 0) { release_due(s); }
			close_due = s.due.load(std::memory_order_relaxed) != 0;
		}
		// While still `closing`: once the exchange leaves it, a pass may begin the next close and book it.
		if (from == slot_state::closing) { unbook(s); }
		for (;;) {
			hook("transition.end", true);
			const std::uint64_t next = word::with_pins(word::with_state(cur, to), word::pins(cur) - failed);
			if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		hook("transition.ended", true);
		gen = word::gen(cur);
		note(from, to);
	}
	if (close_due && !stopped_.load(std::memory_order_acquire) && !shutting_.load(std::memory_order_acquire)) {
		// The booked close begins here, with no wait for a pass to come around to the slot (B5); a pin that took the
		// slot released the booking, and any other refusal leaves it for the next pass.
		if (begin_due_close(idx)) {
			pass_closes_.fetch_add(1, std::memory_order_relaxed);
		} else {
			post_pass();
		}
	}
	if (wake) {
		post_wake_chore(idx);
	} else {
		deliver(list, gen, gate);
		if (chores) { deliver(chores, gen, nullptr); }
		kick(idx);
	}
}

template <class Index, class H>
void catalog<Index, H>::post_wake_chore(std::uint32_t idx) {
	wakes_.fetch_add(1, std::memory_order_relaxed);
	post([this, idx] { run_wake(idx); });
}

template <class Index, class H>
void catalog<Index, H>::run_wake(std::uint32_t idx, std::uint64_t delay) {
	slot& s = at(idx);
	std::exception_ptr err;
	bool fenced = false;
	{
		const chore_scope chore{s.index->lease()};
		try {
			if (!chore) {
				reposts_.fetch_add(1, std::memory_order_relaxed);
				after(delay, [this, idx, next = std::min(delay * 2, repost_cap_ns)] { run_wake(idx, next); },
					[this, idx](std::exception_ptr e) noexcept { finish_wake(idx, e); });  // an unposted repost fails the wake
				return;
			}
			s.index->promote();
		} catch (const index_fenced&) {
			fenced = s.index->fenced();  // the wake cannot succeed on a fenced index; a fresh one can
			if (!fenced) { err = std::current_exception(); }
		} catch (...) {  // a timer that throws fails the wake, so no slot is left `waking`
			err = std::current_exception();
		}
	}
	if (fenced) {
		replace_from(idx, slot_state::waking);
		return;
	}
	finish_wake(idx, err);
}

template <class Index, class H>
bool catalog<Index, H>::overload(const std::exception_ptr& e) {  // the budget's hard cap: a refusal, not a fault
	try {
		std::rethrow_exception(e);
	} catch (const too_many_open&) {
		return true;
	} catch (...) {
		return false;
	}
}

template <class Index, class H>
void catalog<Index, H>::finish_wake(std::uint32_t idx, const std::exception_ptr& err) {
	slot& s = at(idx);
	waiter *list, *chores = nullptr;
	std::uint64_t gen;
	std::exception_ptr failure;  // built before the mutex: an allocation failure keeps the cause's own error
	if (err) {
		try {
			hook("wake.failure", false);
			failure = std::make_exception_ptr(wake_failed(err));
		} catch (...) {
			failure = std::current_exception();
		}
	}
	{
		std::lock_guard lock(shard_of(s.key_hash).mu);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		assert(word::state(cur) == slot_state::waking);
		list = take(s);
		std::uint64_t w = 0;
		if (err) {  // request waiters fail, chore waiters keep their pins
			w = split_requests(list, chores);
			if (overload(err)) {
				wake_overloads_.fetch_add(1, std::memory_order_relaxed);
			} else {
				if (s.failures.load(std::memory_order_relaxed) != UINT8_MAX) { s.failures.store(std::uint8_t(s.failures.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed); }
				wake_failures_.fetch_add(1, std::memory_order_relaxed);
			}
			if (s.streak != max_exponent) { ++s.streak; }
			s.error = failure;
			s.retry_at = exec_now() + catalog_detail::backoff_ns(s.streak);
		} else {
			s.failures.store(0, std::memory_order_relaxed);
			s.streak = 0;
			s.error = nullptr;
			s.retry_at = 0;
		}
		for (;;) {
			hook("wake.end", true);
			const std::uint64_t next = err ? word::with_pins(word::with_state(cur, slot_state::owed), word::pins(cur) - w) : word::with_state(cur, slot_state::open);
			if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
				gen = word::gen(next);
				break;
			}
		}
		note(slot_state::waking, err ? slot_state::owed : slot_state::open);
	}
	hook("wake.unlocked", false);
	deliver(list, gen, err);
	if (chores) { deliver(chores, gen, nullptr); }
	kick(idx);
}

template <class Index, class H>
void catalog<Index, H>::commit_merge_output(slot& s) {
	if (!s.index->merge_output_unsynced()) { return; }
	merge_commits_.fetch_add(1, std::memory_order_relaxed);
	try {
		h_.commit(*s.payload, *s.index);
	} catch (...) {
		merge_commit_failures_.fetch_add(1, std::memory_order_relaxed);
	}
}

template <class Index, class H>
void catalog<Index, H>::run_demote(std::uint32_t idx) {
	slot& s = at(idx);
	bool owed = false, fenced = false;
	s.index->check_in();  // publishes finished merges and frees released views, so warm_quiescent() can hold (F6)
	{
		const chore_scope chore{s.index->lease()};
		if (chore) {
			hook("chore.demote", false);
			try {
				if (s.index->fenced()) {
					fenced = true;
				} else {
					commit_merge_output(s);
					owed = s.index->demote() && s.index->promote_owed();
					if (s.counts.get(resource::maps) != 0) { (void)s.index->reclaim(0); }
				}
			} catch (const index_fenced&) {
				fenced = s.index->fenced();
			} catch (...) {
			}
		}
	}
	if (fenced) {
		replace_from(idx, slot_state::demoting);
		return;
	}
	if (s.counts.get(resource::maps) != 0) { hold_off(s); }  // still hot: not again before the next period
	end_at(idx, slot_state::demoting, owed ? slot_state::owed : slot_state::open);
}

template <class Index, class H>
void catalog<Index, H>::hold_off(slot& s) noexcept {
	const std::uint64_t now = exec_now(), p = pass_at_.load(std::memory_order_relaxed);
	const std::uint64_t t = (p != 0 && p <= now && now - p < h_.sweep_period_ns ? p : now) + h_.sweep_period_ns;
	if (s.probe_at.load(std::memory_order_relaxed) < t) { s.probe_at.store(t, std::memory_order_relaxed); }
}

template <class Index, class H>
void catalog<Index, H>::clear_failing(slot& s) noexcept {
	if (s.commit_failed.exchange(false, std::memory_order_relaxed)) { failing_.fetch_sub(1, std::memory_order_relaxed); }
}

template <class Index, class H>
void catalog<Index, H>::run_close(std::uint32_t idx, slot_state prior, close_mode mode, std::uint64_t delay) {
	slot& s = at(idx);
	const close_depth in_chore;
	const bool bound = mode == close_mode::bound, down = mode == close_mode::shutdown;
	bool ready = false, commit_failed = false, reclaimed = false, fenced = false;
	{
		const chore_scope chore{s.index->lease()};
		if (!chore && down) {  // a chore pin's caller holds the lease at {0, ...}: try again shortly
			s.holdup.store(hold_lease, std::memory_order_relaxed);
			after(delay, [this, idx, prior, next = std::min(delay * 2, repost_cap_ns)] { run_close(idx, prior, close_mode::shutdown, next); });
			return;
		}
		if (down) { s.holdup.store(0, std::memory_order_relaxed); }
		if (chore) {
			hook("chore.close", false);
			try {
				if (!down && s.index->fenced()) {
					fenced = true;
				} else if (down) {
					ready = true;
					if (s.index->uncommitted()) {
						shutdown_commits_.fetch_add(1, std::memory_order_relaxed);
						try {
							h_.commit(*s.payload, *s.index);
						} catch (...) {
							shutdown_commit_failures_.fetch_add(1, std::memory_order_relaxed);
							hook("shutdown.name", false);
							std::lock_guard g(shut_mu_);
							result_.uncommitted.push_back(s.key);
						}
					}
				} else if (!bound || s.failures.load(std::memory_order_relaxed) >= h_.wake_bound) {  // an exchange after a reset re-checks the bound
					commit_merge_output(s);
					ready = s.index->close_ready();
					if (ready && prior == slot_state::owed && s.counts.get(resource::maps) != 0) {  // warm with leftover maps (a commit's): demote them away
						(void)s.index->demote();
						if (s.counts.get(resource::maps) != 0) { (void)s.index->reclaim(0); }
					}
					ready = ready && s.counts.get(resource::maps) == 0;
					if (!ready && bound) {
						if (s.index->uncommitted()) {
							bound_commits_.fetch_add(1, std::memory_order_relaxed);
							try {
								h_.commit(*s.payload, *s.index);
							} catch (...) {
								commit_failed = true;
							}
						}
						if (!commit_failed && s.counts.get(resource::maps) != 0) {  // maps from this commit or a chore pin's
							(void)s.index->demote();
							ready = s.index->close_ready() && s.counts.get(resource::maps) == 0;
						}
					}
					if (!ready && !commit_failed && s.index->reclaim_pending() != 0) {
						reclaims_.fetch_add(1, std::memory_order_relaxed);
						(void)s.index->reclaim();
						reclaimed = true;
					}
				}
			} catch (const index_fenced&) {
				fenced = s.index->fenced();
			} catch (...) {
			}
		}
	}
	if (fenced) {  // a fenced index is replaced, not kept (F4)
		run_replace(idx);
		return;
	}
	if (commit_failed) {
		commit_failures_.fetch_add(1, std::memory_order_relaxed);
		s.probe_at.store(exec_now() + commit_probe_ns, std::memory_order_relaxed);
		if (!s.commit_failed.exchange(true, std::memory_order_relaxed)) { failing_.fetch_add(1, std::memory_order_relaxed); }
	}
	if (!ready) {
		if (!reclaimed && !commit_failed) { hold_off(s); }  // refused: not again before the next period
		close_refused(idx, prior);
		return;
	}
	close_payload(s, down);
	try {
		hook("close.call", false);
		s.index->close();
	} catch (...) {
		close_failures_.fetch_add(1, std::memory_order_relaxed);
		if (!s.index->closing()) {  // refused, nothing changed: the sweep tries again, shutdown after a pause
			if (!down) {
				hold_off(s);
				close_refused(idx, prior);
			} else {  // retried while shutting down, as a stuck close is (C25)
				s.holdup.store(hold_throws, std::memory_order_relaxed);
				after(repost_cap_ns, [this, idx, prior] { run_close(idx, prior, close_mode::shutdown); });
			}
			return;
		}
	}
	after_close(idx, mode);
}

template <class Index, class H>
void catalog<Index, H>::replace_from(std::uint32_t idx, slot_state from) {
	slot& s = at(idx);
	bool post_close;
	{
		std::lock_guard lock(shard_of(s.key_hash).mu);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		assert(word::state(cur) == from);
		std::uint64_t waiters = 0;
		if (from == slot_state::waking) {
			s.waiters.each([&](const waiter&) { ++waiters; });
		}
		std::uint64_t holders;
		for (;;) {
			holders = from == slot_state::waking ? word::pins(cur) - waiters : 0;
			s.drain.store(std::uint32_t(holders), std::memory_order_relaxed);
			hook("replace.begin", true);
			if (s.word.compare_exchange_weak(cur, word::with_state(cur, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		hook("replace.exchanged", true);  // as reopen's
		if (from == slot_state::demoting) {  // the demotion ends here, as end_at ends one; the close's relief is booked in its place
			pending_maps_.fetch_sub(s.pend_maps.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
			pending_hot_.fetch_sub(1, std::memory_order_relaxed);
			release_due(s);  // before note(), as in reopen
		}
		note(from, slot_state::closing);
		book(s, resource::descriptors);
		post_close = holders == 0;
	}
	if (post_close) { post([this, idx] { run_replace(idx); }); }
}

template <class Index, class H>
void catalog<Index, H>::run_replace(std::uint32_t idx, std::uint64_t delay) {
	slot& s = at(idx);
	const close_depth in_chore;
	{
		std::lock_guard lock(shard_of(s.key_hash).mu);
		s.replacing = true;
	}
	{
		const chore_scope chore{s.index->lease()};
		if (!chore) {
			replace_reposts_.fetch_add(1, std::memory_order_relaxed);
			after_or_post(delay, [this, idx, next = std::min(delay * 2, repost_cap_ns)] { run_replace(idx, next); });
			return;
		}
		hook("chore.replace", false);
		try {
			if (!s.index->fenced() && s.index->uncommitted()) {
				try {
					h_.commit(*s.payload, *s.index);
				} catch (...) {
					replace_commit_failures_.fetch_add(1, std::memory_order_relaxed);
				}
			}
		} catch (...) {
		}
	}
	close_payload(s, false);
	try {
		hook("close.call", false);
		s.index->close();
	} catch (...) {
		close_failures_.fetch_add(1, std::memory_order_relaxed);
		if (!s.index->closing()) {  // refused, nothing changed: try again shortly
			after_or_post(repost_cap_ns, [this, idx] { run_replace(idx); });
			return;
		}
	}
	replaces_.fetch_add(1, std::memory_order_relaxed);
	after_close(idx, close_mode::replace);
}

template <class Index, class H>
void catalog<Index, H>::close_payload(slot& s, bool down) noexcept {
	try {
		h_.close_payload(*s.payload, *s.index);
	} catch (...) {
		payload_close_failures_.fetch_add(1, std::memory_order_relaxed);
		if (down) {
			try {
				std::lock_guard g(shut_mu_);
				if (std::find(result_.uncommitted.begin(), result_.uncommitted.end(), s.key) == result_.uncommitted.end()) { result_.uncommitted.push_back(s.key); }
			} catch (...) {
			}
		}
	}
}

template <class Index, class H>
void catalog<Index, H>::close_refused(std::uint32_t idx, slot_state prior) {
	slot& s = at(idx);
	s.to_ghost.store(false, std::memory_order_relaxed);
	const std::uint64_t gen = word::gen(s.word.load(std::memory_order_acquire));
	end_at(idx, slot_state::closing, prior);
	hook("refuse.end", false);
	policy_.requeue(idx, gen, 2);
}

template <class Index, class H>
void catalog<Index, H>::after_close(std::uint32_t idx, close_mode mode) {
	slot& s = at(idx);
	if (s.index->closing()) {
		s.stuck.store(true, std::memory_order_release);
		unbook(s);  // the relief does not come until the view goes
		stuck_closes_.fetch_add(1, std::memory_order_relaxed);
		detail::owner_break("strata: a closed index is kept open by a view held past its pin");
		if (mode == close_mode::shutdown || shutting_.load(std::memory_order_acquire)) { arm_stuck_retry(idx); }
		return;
	}
	if (mode == close_mode::bound) { bound_closes_.fetch_add(1, std::memory_order_relaxed); }
	clear_failing(s);
	post([this, idx] { retire(idx); });
}

template <class Index, class H>
void catalog<Index, H>::retire(std::uint32_t idx) {
	slot& s = at(idx);
	s.payload.reset();
	s.reader = {};
	s.index.reset();
	s.io.reset();
	s.stuck.store(false, std::memory_order_relaxed);
	s.retrying.store(false, std::memory_order_relaxed);
	finish_close(idx);
}

template <class Index, class H>
void catalog<Index, H>::arm_stuck_retry(std::uint32_t idx) {
	slot& s = at(idx);
	if (s.retrying.exchange(true, std::memory_order_acq_rel)) { return; }  // a pass's retry or ours is on its way
	try {
		after(repost_cap_ns, [this, idx] { run_stuck_retry(idx); });
	} catch (...) {
		s.retrying.store(false, std::memory_order_release);
	}
}

template <class Index, class H>
void catalog<Index, H>::run_stuck_retry(std::uint32_t idx) {
	slot& s = at(idx);
	bool done = false;
	{
		const chore_scope chore{s.index->lease()};
		if (chore) {
			try {
				done = s.index->try_finish_close();
			} catch (...) {
			}
		}
	}
	stuck_retries_.fetch_add(1, std::memory_order_relaxed);
	if (done) {
		post([this, idx] { retire(idx); });
		return;
	}
	s.retrying.store(false, std::memory_order_release);
	if (shutting_.load(std::memory_order_acquire)) { arm_stuck_retry(idx); }
}

template <class Index, class H>
void catalog<Index, H>::finish_close(std::uint32_t idx) {
	slot& s = at(idx);
	const std::uint32_t h = s.key_hash;
	bool reopen = false;
	std::uint64_t delay = 0;
	policy_.unlink(idx);
	const bool ghost = s.to_ghost.exchange(false, std::memory_order_relaxed);
	std::uint64_t h64;
	{
		std::lock_guard lock(shard_of(h).mu);
		h64 = key_hash64(s.key);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		assert(word::state(cur) == slot_state::closing);
		s.failures.store(0, std::memory_order_relaxed);
		s.streak = 0;
		s.error = nullptr;
		s.retry_at = 0;
		s.probe_at.store(0, std::memory_order_relaxed);
		unbook(s);  // while still `closing`, as in end_at
		for (;;) {
			hook("close.end", true);
			reopen = word::pins(cur) != 0;
			const std::uint64_t next = reopen ? word::next_gen(word::with_state(cur, slot_state::opening)) : word::next_gen(word::make(0, slot_state::free, 0, word::gen(cur)));
			if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(slot_state::closing, reopen ? slot_state::opening : slot_state::free);
		if (reopen && s.replacing) {
			// A fence within 10 s of the last replacement's open counts as a failed open: the reopen waits for the
			// backoff (10 ms doubling to 10 s). A quiet 10 s starts the count over.
			const std::uint64_t now = exec_now();
			const bool repeat = s.replaced_once && std::uint16_t(std::uint16_t(now >> time_unit_bits) - s.last_open) <= (fence_window_ns >> time_unit_bits);
			s.fences = repeat ? std::min<unsigned>(s.fences + 1u, max_exponent) : 0u;
			delay = s.fences == 0 ? 0 : catalog_detail::backoff_ns(s.fences);
			s.last_open = std::uint16_t((now + delay) >> time_unit_bits);
			s.replaced_once = true;
		}
		s.replacing = false;
		if (!reopen) {
			const auto eq = [&](std::uint32_t v) { return v == idx; };
			shard_of(h).table.erase(h, eq);
			s.key.clear();
		}
	}
	if (ghost) { policy_ghost(h64); }
	if (reopen) {
		if (delay != 0) {
			after_or_post(delay, [this, idx] { run_open(idx); });
		} else {
			post([this, idx] { run_open(idx); });
		}
	} else {
		free_slot(idx);
		maybe_complete_shutdown();
	}
}

template <class Index, class H>
bool catalog<Index, H>::erase_failed(std::uint32_t idx, std::uint64_t now, bool force) {
	slot& s = at(idx);
	const std::uint32_t h = s.key_hash.load(std::memory_order_acquire);
	hook("erase.pre", false);
	{
		std::lock_guard lock(shard_of(h).mu);
		if (s.key_hash.load(std::memory_order_acquire) != h) { return false; }  // the slot went to a key of another shard
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		for (;;) {
			if (word::state(cur) != slot_state::failed || word::pins(cur) != 0) { return false; }
			if (!force && now < s.retry_at + h_.negative_ttl_ns) { return false; }
			hook("sweep.erase", true);
			if (s.word.compare_exchange_weak(cur, word::next_gen(word::make(0, slot_state::free, 0, word::gen(cur))), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(slot_state::failed, slot_state::free);
		erased_.fetch_add(1, std::memory_order_relaxed);
		const auto eq = [&](std::uint32_t v) { return v == idx; };
		shard_of(h).table.erase(h, eq);
		s.key.clear();
		s.error = nullptr;
	}
	free_slot(idx);
	maybe_complete_shutdown();
	return true;
}

template <class Index, class H>
void catalog<Index, H>::kick(std::uint32_t idx) noexcept {
	if (!shutting_.load(std::memory_order_acquire)) { return; }
	try {
		shutdown_slot(idx);
	} catch (...) {
	}
}

template <class Index, class H>
void catalog<Index, H>::shutdown_slot(std::uint32_t idx) {
	slot& s = at(idx);
	std::uint64_t w = s.word.load(std::memory_order_acquire);
	slot_state prior;
	for (;;) {
		prior = word::state(w);
		if (prior == slot_state::closing && s.stuck.load(std::memory_order_acquire)) {  // the sweep may be stopped
			arm_stuck_retry(idx);
			return;
		}
		if (word::pins(w) != 0) {  // its last unpin begins the close
			if (w & word::shut) { return; }
			hook("shutdown.mark", false);
			if (s.word.compare_exchange_weak(w, w | word::shut, std::memory_order_acq_rel, std::memory_order_acquire)) { return; }
			continue;  // the word moved: look again
		}
		if (prior == slot_state::failed) {
			(void)erase_failed(idx, 0, true);
			return;
		}
		if (prior != slot_state::open && prior != slot_state::owed) { return; }
		hook("shutdown.close", false);
		if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
	}
	begin_shutdown_close(s, idx, prior);
}

template <class Index, class H>
void catalog<Index, H>::begin_shutdown_close(slot& s, std::uint32_t idx, slot_state prior) {
	release_due(s);  // before note(), as in reopen
	note(prior, slot_state::closing);
	post([this, idx, prior] { run_close(idx, prior, close_mode::shutdown); });
}

template <class Index, class H>
bool catalog<Index, H>::join_shutdown(waiter* w) {
	w->sched = task::current();
	{
		std::lock_guard l(shut_mu_);
		if (shut_done_.load(std::memory_order_acquire)) { return true; }
		w->mu = &shut_mu_;
		w->retain();
		shut_list_.push(w);
	}
	if (!shutting_.exchange(true, std::memory_order_seq_cst)) {  // before the count is read (B1)
		const std::uint32_t n = slots_made();
		for (std::uint32_t i = 0; i < n; ++i) { kick(i); }
	}
	maybe_complete_shutdown();
	return false;
}

template <class Index, class H>
shutdown_result catalog<Index, H>::shutdown_outcome() {
	std::lock_guard l(shut_mu_);
	return result_;
}

template <class Index, class H>
void catalog<Index, H>::maybe_complete_shutdown() {
	if (!shutting_.load(std::memory_order_acquire) || occupied_.load(std::memory_order_seq_cst) != 0) { return; }
	waiter* list;
	{
		std::lock_guard l(shut_mu_);
		if (shut_done_.load(std::memory_order_acquire)) { return; }
		shut_done_.store(true, std::memory_order_release);
		list = shut_list_.take();
	}
	deliver(list, 0, nullptr);
}

}  // namespace strata
