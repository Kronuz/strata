// catalog's table: construction and teardown, slots and their chunks, chores and timers, and the helpers every mechanism shares
#pragma once

#include "strata/core/catalog.hh"

namespace strata {

template <class Index, class H>
catalog<Index, H>::catalog(host h) : h_(std::move(h)), life_(std::make_shared<life>(this)) {
	if (!h_.timer) { throw std::invalid_argument("strata: the catalog's host supplies a timer"); }
	if (!h_.open_payload) { throw std::logic_error("strata: an H that is not default-constructible needs host::open_payload"); }
	if (h_.wake_bound > 255) { throw std::invalid_argument("strata: host::wake_bound is at most 255 (the slot counts failures in a byte)"); }
	group_ = h_.exec->open();
	for (auto& c : chunks_) { c.store(nullptr, std::memory_order_relaxed); }
	if (h_.soft_open) { policy_.cap(h_.soft_open); }
}

template <class Index, class H>
catalog<Index, H>::~catalog() {
	stop_sweep();
	{
		std::lock_guard g(life_->mu);
		life_->c = nullptr;
	}
	for (;;) {  // a timer's callback posts before it is counted done, and a chore may arm another
		{
			std::unique_lock l(timers_mu_);
			timers_cv_.wait(l, [this] { return timers_ == 0; });
		}
		group_->wait();
		std::lock_guard l(timers_mu_);
		if (timers_ == 0) { break; }
	}
	orphan_waiters();
	release_chunks();
}

template <class Index, class H>
std::function<void(const typename catalog<Index, H>::opening&, Index&, std::optional<H>&)> catalog<Index, H>::default_payload() {
	if constexpr (std::is_default_constructible_v<H>) {
		return [](const opening&, Index&, std::optional<H>& h) { h.emplace(); };
	} else {
		return {};
	}
}

template <class Index, class H>
open_intent catalog<Index, H>::checked(pin_kind kind, open_intent intent) {
	if (intent == open_intent::resident && kind != pin_kind::chore) { throw std::logic_error("strata: a resident pin is a chore pin"); }
	return intent;
}

template <class Index, class H>
std::array<char, 16> catalog<Index, H>::fresh_uuid() {
	std::random_device rd;
	std::array<char, 16> u;
	for (auto& c : u) { c = char(rd()); }
	u[6] = char((u[6] & 0x0f) | 0x40), u[8] = char((u[8] & 0x3f) | 0x80);
	return u;
}

template <class Index, class H>
bool catalog<Index, H>::pin_held(std::uint64_t w) noexcept {
	switch (word::state(w)) {
	case slot_state::opening: case slot_state::waking: case slot_state::demoting: case slot_state::closing: return true;
	default: return word::pins(w) != 0 || catalog_detail::payload_depth() != 0;
	}
}

template <class Index, class H>
void catalog<Index, H>::hook(const char* site, bool locked) const {
	if (h_.before_cas) { h_.before_cas(site, locked); }
}

template <class Index, class H>
void catalog<Index, H>::hook_noexcept(const char* site) const noexcept {
	try {
		hook(site, false);
	} catch (...) {
	}
}

template <class Index, class H>
void catalog<Index, H>::post(std::function<void()> f) {
	group_->post([f = std::move(f)](const task::cancel_token&) { f(); });
}

template <class Index, class H>
std::uint32_t catalog<Index, H>::alloc_slot() {
	std::lock_guard l(free_mu_);
	if (free_head_ != none) {
		const std::uint32_t i = free_head_;
		free_head_ = at(i).q_next;
		at(i).q_next = none;
		return i;
	}
	const std::uint32_t i = next_new_;
	if (i / chunk_slots >= max_chunks) { throw too_many_open("strata: the catalog has no slot left"); }
	if (i % chunk_slots == 0) { chunks_[i / chunk_slots].store(new slot[chunk_slots], std::memory_order_release); }
	++next_new_;
	return i;
}

template <class Index, class H>
void catalog<Index, H>::free_slot(std::uint32_t i) {
	std::lock_guard l(free_mu_);
	at(i).q_next = free_head_;
	free_head_ = i;
}

template <class Index, class H>
void catalog<Index, H>::after(std::uint64_t delay_ns, std::function<void()> fn, std::function<void(std::exception_ptr)> failed) {
	{
		std::lock_guard l(timers_mu_);
		++timers_;
	}
	try {
		h_.timer(delay_ns, [this, fn = std::move(fn), failed = std::move(failed)]() mutable {
			try {
				hook("timer.post", false);
				post(std::move(fn));
			} catch (...) {
				if (!failed) {
					timer_end();
					throw;
				}
				failed(std::current_exception());
			}
			timer_end();
		});
	} catch (...) {
		timer_end();
		throw;
	}
}

template <class Index, class H>
void catalog<Index, H>::after_or_post(std::uint64_t delay_ns, std::function<void()> fn) {
	try {
		after(delay_ns, fn);
	} catch (...) {
		post(std::move(fn));
	}
}

template <class Index, class H>
void catalog<Index, H>::timer_end() {
	std::lock_guard l(timers_mu_);
	--timers_;
	hook("timer.end", true);
	timers_cv_.notify_all();
}

template <class Index, class H>
void catalog<Index, H>::orphan_waiters() noexcept {
	const auto orphan = [](catalog_detail::wait_list& l) {
		for (waiter* w = l.take(); w;) {
			waiter* next = w->next;
			w->mu = nullptr;  // the awaiter's destructor then only drops its reference
			w->release();     // the list's
			w = next;
		}
	};
	{
		std::lock_guard l(shut_mu_);
		orphan(shut_list_);
	}
	const std::uint32_t n = slots_made();
	for (std::uint32_t i = 0; i < n; ++i) {
		slot& s = at(i);
		std::lock_guard l(shard_of(s.key_hash).mu);
		orphan(s.waiters);
	}
}

template <class Index, class H>
void catalog<Index, H>::release_chunks() noexcept {
	struct leaked_slot {  // H stays in its slot: the chunk is kept for good, and H need not move
		std::unique_ptr<kronuz::journal::IO> io;
		std::unique_ptr<Index> index;
		reader_type reader;
	};
	const std::uint32_t n = next_new_;
	bool pinned = false, open = false;
	std::vector<bool> keep;
	try {
		keep.assign((n + chunk_slots - 1) / chunk_slots, false);
		for (std::uint32_t i = 0; i < n; ++i) {
			slot& s = at(i);
			const std::uint64_t w = s.word.load(std::memory_order_acquire);
			if (word::state(w) == slot_state::free) { continue; }
			pinned |= word::pins(w) != 0;
			if (word::state(w) == slot_state::failed) { continue; }
			open = true;
			keep[i / chunk_slots] = true;
			auto k = std::make_shared<leaked_slot>();
			k->io = std::move(s.io);
			k->index = std::move(s.index);
			k->reader = std::move(s.reader);
			detail::keep_forever(std::shared_ptr<const void>(std::move(k)));
			for (auto& c : s.counts.n) { c.store(0, std::memory_order_relaxed); }
			for (auto& b : s.counts.booked) { b.store(0, std::memory_order_relaxed); }
		}
	} catch (...) {
		open = true;  // out of memory: keep every chunk
		keep.assign(max_chunks, true);
	}
	if (pinned) { detail::owner_break("strata: a catalog destroyed with a pin held"); }
	if (open) {
		detail::owner_break("strata: a catalog destroyed without shutdown(): its open indexes are leaked");
		for (auto& c : counts_.n) { c.store(0, std::memory_order_relaxed); }
		for (auto& b : counts_.booked) { b.store(0, std::memory_order_relaxed); }
		counts_.holders_mapped.store(0, std::memory_order_relaxed);
	}
	for (std::size_t i = 0; i < chunks_.size(); ++i) {
		slot* p = chunks_[i].load(std::memory_order_relaxed);
		if (!p) { continue; }
		if (i < keep.size() && keep[i]) {
			try { detail::keep_forever(std::shared_ptr<const void>(static_cast<const void*>(p), [](const void*) {})); } catch (...) {}
		} else {
			delete[] p;
		}
	}
}

}  // namespace strata
