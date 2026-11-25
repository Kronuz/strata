// catalog's pin protocol: pins by key and by handle, waiters and their delivery, the awaiter, unpin and reopen
#pragma once

#include "strata/core/catalog.hh"

namespace strata {

template <class Index, class H>
void catalog<Index, H>::cancel_pin::operator()() const noexcept {
	if (w->mu) { c->cancel_wait(w); }  // null: ~catalog orphaned it, and c is gone
}

template <class Index, class H>
auto catalog<Index, H>::pin::operator=(pin&& o) noexcept -> pin& {
	if (this != &o) {
		release();
		c_ = std::exchange(o.c_, nullptr), s_ = o.s_, idx_ = o.idx_, gen_ = o.gen_;
	}
	return *this;
}

template <class Index, class H>
void catalog<Index, H>::pin::release() noexcept {
	if (c_) { std::exchange(c_, nullptr)->unpin(*s_, idx_); }
}

template <class Index, class H>
catalog<Index, H>::pin_awaiter::~pin_awaiter() {
	cb_.reset();  // waits for a cancel running elsewhere, which uses w_
	if (w_) { catalog::abandon(c_, std::exchange(w_, nullptr)); }
}

template <class Index, class H>
bool catalog<Index, H>::pin_awaiter::await_ready() {
	const bool may_wait = !stop_.requested();
	const acquired a = c_->acquire_impl(req_, hint_, may_wait ? &w_ : nullptr);
	r_ = a.out;
	if (r_ == outcome::waiting) { return false; }
	if (w_) { std::exchange(w_, nullptr)->release(); }  // made for a join that did not happen
	idx_ = a.idx, gen_ = a.gen, error_ = a.error;
	if (r_ == outcome::would_wait) { error_ = std::make_exception_ptr(task::cancelled{}); }
	return true;
}

template <class Index, class H>
bool catalog<Index, H>::pin_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
	w_->h = h;
	cb_.emplace(stop_, cancel_pin{c_, w_});  // a request already made runs it here: then st is delivered
	int expect = waiter::registered;
	return w_->st.compare_exchange_strong(expect, waiter::suspended, std::memory_order_acq_rel, std::memory_order_acquire);
}

template <class Index, class H>
auto catalog<Index, H>::pin_awaiter::await_resume() -> pin {
	cb_.reset();
	if (waiter* w = std::exchange(w_, nullptr)) {
		idx_ = w->idx, gen_ = w->gen, error_ = std::move(w->error);
		w->release();
	}
	if (error_) { std::rethrow_exception(error_); }
	if (r_ == outcome::none) { return pin(); }
	return pin(c_, idx_, gen_);
}

template <class Index, class H>
catalog<Index, H>::pin_awaiter::pin_awaiter(catalog& c, std::string_view key, pin_kind kind, bool use, handle hint, open_intent intent, task::cancel_token stop)
	: c_(&c), req_{key, kind, use, intent}, hint_(hint), stop_(std::move(stop)) {}

template <class Index, class H>
[[nodiscard]] auto catalog<Index, H>::pin_request(std::string_view key, bool use, open_intent intent, task::cancel_token stop) -> pin_awaiter {
	return pin_awaiter(*this, key, pin_kind::request, use, {}, checked(pin_kind::request, intent), std::move(stop));
}

template <class Index, class H>
[[nodiscard]] auto catalog<Index, H>::pin_chore(std::string_view key, bool use, open_intent intent, task::cancel_token stop) -> pin_awaiter {
	return pin_awaiter(*this, key, pin_kind::chore, use, {}, intent, std::move(stop));
}

template <class Index, class H>
[[nodiscard]] auto catalog<Index, H>::repin(handle hint, std::string_view key, pin_kind kind, bool use, open_intent intent, task::cancel_token stop) -> pin_awaiter {
	return pin_awaiter(*this, key, kind, use, hint, checked(kind, intent), std::move(stop));
}

template <class Index, class H>
auto catalog<Index, H>::try_pin(std::string_view key, pin_kind kind, bool use, open_intent intent) -> try_result {
	const acquired a = acquire_impl({key, kind, use, checked(kind, intent)}, {}, nullptr);
	try_result r;
	switch (a.out) {
	case outcome::granted: r.status = try_status::pinned, r.p = pin(this, a.idx, a.gen); break;
	case outcome::failed: r.status = try_status::failed, r.error = a.error; break;
	case outcome::none: r.status = try_status::absent; break;
	default: r.status = try_status::would_wait; break;
	}
	return r;
}

template <class Index, class H>
void catalog<Index, H>::reopen(pin&& p) {
	if (!p.c_) { return; }
	slot& s = *p.s_;
	const std::uint32_t idx = p.idx_;
	p.c_ = nullptr;
	bool post_close = false, replaced = false;
	{
		std::lock_guard lock(shard_of(s.key_hash).mu);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		for (;;) {
			const slot_state st = word::state(cur);
			if (st != slot_state::open && st != slot_state::owed) { break; }
			const std::uint64_t holders = word::pins(cur) - 1;
			s.drain.store(std::uint32_t(holders), std::memory_order_relaxed);
			hook("reopen", true);
			if (s.word.compare_exchange_weak(cur, word::with_pins(word::with_state(cur, slot_state::closing), holders), std::memory_order_acq_rel, std::memory_order_acquire)) {
				hook("reopen.exchanged", true);  // a holder's unpin from here on is a drain unpin: `drain` is already stored
				release_due(s);  // before note(): closing_over() never counts the close twice (booked and closing)
				note(st, slot_state::closing);
				book(s, resource::descriptors);
				post_close = holders == 0;
				replaced = true;
				break;
			}
		}
	}
	if (!replaced) {
		unpin(s, idx);
	} else if (post_close) {
		post([this, idx] { run_replace(idx); });
	}
}

template <class Index, class H>
void catalog<Index, H>::join(slot& s, waiter* w, const request& r, std::uint32_t idx, slot_state met) {
	w->kind = r.kind;
	w->idx = idx;
	w->pins = true;
	w->mu = &shard_of(s.key_hash).mu;
	w->sched = task::current();
	w->retain();
	s.waiters.push(w);
	waited_[unsigned(met)].fetch_add(1, std::memory_order_relaxed);
}

template <class Index, class H>
void catalog<Index, H>::deliver(waiter* list, std::uint64_t gen, const std::exception_ptr& error) noexcept {
	for (waiter* w = list; w;) {
		waiter* next = w->next;
		w->gen = gen;
		w->error = error;
		int was = w->st.load(std::memory_order_acquire);
		while (was != waiter::abandoned && !w->st.compare_exchange_weak(was, was == waiter::suspended ? waiter::posting : waiter::delivered, std::memory_order_acq_rel, std::memory_order_acquire)) {}
		if (was == waiter::suspended) {
			const std::coroutine_handle<> h = w->h;
			const task::scheduler_ref sched = w->sched;
			if (sched) {
				sched.post(h);
			} else {
				h.resume();
			}
			if (w->st.exchange(waiter::delivered, std::memory_order_acq_rel) == waiter::abandoned) { w->st.notify_all(); }
		} else if (was == waiter::abandoned && w->pins && !error) {
			gone_pins_.fetch_add(1, std::memory_order_relaxed);
			unpin(at(w->idx), w->idx);
		}
		w->release();
		w = next;
	}
}

template <class Index, class H>
bool catalog<Index, H>::unlink_waiter(waiter* w, bool cancel) noexcept {
	{
		std::lock_guard l(*w->mu);
		if (!w->linked) { return false; }
		unlinked_.fetch_add(1, std::memory_order_relaxed);
		if (cancel) { cancels_.fetch_add(1, std::memory_order_relaxed); }
		if (w->pins) {
			slot& s = at(w->idx);
			s.waiters.unlink(w);
			[[maybe_unused]] const std::uint64_t before = s.word.fetch_sub(word::one_pin, std::memory_order_acq_rel);
			assert(word::pins(before) > 0);
		} else {
			shut_list_.unlink(w);
		}
	}
	return true;
}

template <class Index, class H>
void catalog<Index, H>::cancel_wait(waiter* w) noexcept {
	hook_noexcept("cancel");
	if (!unlink_waiter(w, true)) { return; }
	w->next = nullptr;
	deliver(w, 0, std::make_exception_ptr(task::cancelled{}));  // drops the list's reference
}

template <class Index, class H>
void catalog<Index, H>::abandon(catalog* c, waiter* w) noexcept {
	if (!w->mu) {
	} else if (c->unlink_waiter(w)) {
		w->release();  // the list's
	} else {
		int was = w->st.exchange(waiter::abandoned, std::memory_order_acq_rel);
		if (was == waiter::posting) {  // bounded by one post; the delivery's exchange wakes it
			c->hook_noexcept("abandon.posting");
			w->st.wait(waiter::abandoned, std::memory_order_acquire);
			was = waiter::delivered;
		}
		if (was == waiter::delivered && w->pins && !w->error) {
			c->gone_pins_.fetch_add(1, std::memory_order_relaxed);
			c->unpin(c->at(w->idx), w->idx);
		}
	}
	w->release();
}

template <class Index, class H>
auto catalog<Index, H>::acquire_impl(const request& r, handle hint, waiter** w) -> acquired {
	acquired a;
	if (shutting_.load(std::memory_order_acquire)) { return {outcome::failed, 0, 0, std::make_exception_ptr(catalog_closed())}; }
	if (hint && try_fast(hint, r, a)) { return a; }
	const std::uint32_t h = index_detail::key_hash(r.key);
	shard& sh = shard_of(h);
	const bool sync = w == nullptr;
	bool post_open = false, post_wake = false;
	std::uint32_t idx = none;
	{
		std::unique_lock lock(sh.mu);
		const auto eq = [&](std::uint32_t v) { return at(v).key == r.key; };
		if (const std::uint32_t* f = sh.table.find(h, eq)) {
			idx = *f;
			slot& s = at(idx);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			const bool create = r.intent == open_intent::create, resident = r.intent == open_intent::resident;
			bool sets_create = false;
			for (;;) {
				const slot_state st = word::state(cur);
				if (word::full(cur)) { throw too_many_open("strata: a slot has no pin left"); }
				std::uint64_t next = 0;
				outcome out = outcome::granted;
				bool joins = false;
				sets_create = false;
				switch (st) {
				case slot_state::open:
					next = word::add_pin(cur, r.use);
					break;
				case slot_state::owed:
					if (r.kind == pin_kind::chore) {
						next = word::add_pin(cur, r.use);
						break;
					}
					if (std::exception_ptr gate = wake_gate(s)) {
						return {outcome::failed, idx, 0, std::move(gate)};
					}
					next = word::with_state(sync ? word::add_use(cur, r.use) : word::add_pin(cur, r.use), slot_state::waking);
					out = sync ? outcome::would_wait : outcome::waiting;
					joins = !sync;
					post_wake = true;
					break;
				case slot_state::waking:
					if (r.kind == pin_kind::chore && s.index && s.index->lease().held_here()) {
						next = word::add_pin(cur, r.use);
						break;
					}
					[[fallthrough]];
				case slot_state::opening:
				case slot_state::demoting:
				case slot_state::closing:
					if (resident && st == slot_state::closing) { return {outcome::none, idx, 0, nullptr}; }
					if (sync) {
						waited_[unsigned(st)].fetch_add(1, std::memory_order_relaxed);
						return {outcome::would_wait, idx, 0, nullptr};
					}
					next = word::add_pin(cur, r.use);
					out = outcome::waiting;
					joins = true;
					sets_create = create && (st == slot_state::opening || st == slot_state::closing);
					break;
				case slot_state::failed:
					if (resident) { return {outcome::none, idx, 0, nullptr}; }
					// A create pin passes the negative entry of a missing index (F1); any other error gates it as every pin.
					if (exec_now() < s.retry_at && !(create && s.absent)) {
						failed_fast_.fetch_add(1, std::memory_order_relaxed);
						return {outcome::failed, idx, 0, s.error};
					}
					next = word::next_gen(word::with_state(sync ? word::add_use(cur, r.use) : word::add_pin(cur, r.use), slot_state::opening));
					out = sync ? outcome::would_wait : outcome::waiting;
					joins = !sync;
					post_open = true;
					sets_create = true;
					retries_.fetch_add(1, std::memory_order_relaxed);
					break;
				case slot_state::free:
					assert(false && "a slot in the table is not free");
					return {outcome::failed, idx, 0, nullptr};
				}
				if (joins && !*w) { *w = new waiter; }
				hook("pin.key", true);
				if (!s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
					post_open = post_wake = false;
					continue;
				}
				if (st != word::state(next)) { note(st, word::state(next)); }
				if (sets_create) { s.create = st == slot_state::failed ? create : s.create || create; }
				if ((st == slot_state::open || st == slot_state::owed) && s.due.load(std::memory_order_relaxed)) { release_due(s); }
				a.out = out;
				a.idx = idx;
				a.gen = word::gen(next);
				if (joins) { join(s, *w, r, idx, st); }
				break;
			}
		} else {
			if (r.intent == open_intent::resident) { return {outcome::none, 0, 0, nullptr}; }
			if (!sync && !*w) { *w = new waiter; }
			// Counted before shutdown is checked, both sequentially consistent, as shutdown sets its flag before
			// it reads the count: either this miss sees shutdown, or shutdown sees this slot (B1).
			occupied_.fetch_add(1, std::memory_order_seq_cst);
			hook("pin.counted", true);
			if (shutting_.load(std::memory_order_seq_cst)) {
				lock.unlock();
				uncount_miss();
				return {outcome::failed, 0, 0, std::make_exception_ptr(catalog_closed())};
			}
			try {
				idx = alloc_slot();
			} catch (...) {
				lock.unlock();
				uncount_miss();
				throw;
			}
			slot& s = at(idx);
			try {
				s.key = std::string(r.key);
				s.key_hash = h;
				sh.table.insert(h, idx);
			} catch (...) {
				s.key.clear();
				free_slot(idx);
				lock.unlock();
				uncount_miss();
				throw;
			}
			// A reused slot starts clean: nothing of its last key's backoff or use (I3-N3).
			s.failures.store(0, std::memory_order_relaxed);
			s.streak = 0;
			s.error = nullptr;
			s.retry_at = 0;
			s.probe_at.store(0, std::memory_order_relaxed);
			s.stuck.store(false, std::memory_order_relaxed);
			s.retrying.store(false, std::memory_order_relaxed);
			s.create = r.intent == open_intent::create, s.attempt_create = s.absent = s.replacing = false;
			s.fences = 0, s.replaced_once = false;
			s.holdup.store(0, std::memory_order_relaxed);
			s.last_use.store(exec_now(), std::memory_order_relaxed);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			for (;;) {  // free to opening; the miss's own pin is not a use
				assert(word::state(cur) == slot_state::free && word::pins(cur) == 0);
				const std::uint64_t next = word::next_gen(word::make(sync ? 0 : 1, slot_state::opening, 0, word::gen(cur)));
				hook("pin.miss", true);
				if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
					a.gen = word::gen(next);
					break;
				}
			}
			note(slot_state::free, slot_state::opening);
			a.idx = idx;
			a.out = sync ? outcome::would_wait : outcome::waiting;
			if (!sync) { join(s, *w, r, idx, slot_state::opening); }
			misses_.fetch_add(1, std::memory_order_relaxed);
			post_open = true;
		}
	}
	if (post_open) {  // the post is the last touch: the open, then shutdown's close, may end the catalog
		over_soft();
		post([this, idx] { run_open(idx); });
	}
	if (post_wake) { post_wake_chore(idx); }
	return a;
}

template <class Index, class H>
bool catalog<Index, H>::try_fast(handle hint, const request& r, acquired& a) {
	slot& s = at(hint.slot);
	std::uint64_t cur = s.word.load(std::memory_order_acquire);
	for (;;) {
		if (word::gen(cur) != hint.gen) {
			stale_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (word::state(cur) != slot_state::open) { return false; }
		if (word::full(cur)) { throw too_many_open("strata: a slot has no pin left"); }
		hook("pin.handle", false);
		if (s.word.compare_exchange_weak(cur, word::add_pin(cur, r.use), std::memory_order_acq_rel, std::memory_order_acquire)) {
			handle_pins_.fetch_add(1, std::memory_order_relaxed);
			if (s.due.load(std::memory_order_relaxed)) [[unlikely]] { release_due(s); }
			a = {outcome::granted, hint.slot, hint.gen, nullptr};
			return true;
		}
	}
}

template <class Index, class H>
void catalog<Index, H>::unpin(slot& s, std::uint32_t idx) noexcept {
	s.last_use.store(exec_now(), std::memory_order_relaxed);
	std::uint64_t cur = s.word.load(std::memory_order_relaxed);
	hook("unpin", false);
	bool checked = false, close = false;
	slot_state st = slot_state::free;
	for (;;) {
		assert(word::pins(cur) > 0);
		st = word::state(cur);
		// A pin that ends in `closing` is a holder of a replace's drain (waiters hold no granted pin there).
		const bool last = word::pins(cur) == 1 && st != slot_state::closing;
		if (last && !checked) {
			checked = true;
			over_soft();  // the slot is a candidate again, and a request may have mapped it
		}
		close = last && (cur & word::shut) && (st == slot_state::open || st == slot_state::owed);
		const std::uint64_t next = close ? word::with_state(cur - word::one_pin, slot_state::closing) : cur - word::one_pin;
		if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_relaxed)) { break; }
	}
	if (st == slot_state::closing) {
		drain_unpin(s, idx);
	} else if (close) {
		try {
			begin_shutdown_close(s, idx, st);
		} catch (...) {
		}
	}
}

template <class Index, class H>
void catalog<Index, H>::drain_unpin(slot& s, std::uint32_t idx) noexcept {
	(void)s.word.load(std::memory_order_acquire);
	hook("drain.unpin", false);
	drain_unpins_.fetch_add(1, std::memory_order_relaxed);
	if (s.drain.fetch_sub(1, std::memory_order_acq_rel) == 1) { post([this, idx] { run_replace(idx); }); }
}

template <class Index, class H>
void catalog<Index, H>::uncount_miss() noexcept {
	try {
		post([this] {
			occupied_.fetch_sub(1, std::memory_order_relaxed);
			maybe_complete_shutdown();
		});
	} catch (...) {
		occupied_.fetch_sub(1, std::memory_order_relaxed);
		maybe_complete_shutdown();
	}
}

}  // namespace strata
