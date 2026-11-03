// The cold tier's step 2, item 3 (docs/design/cold-tier.md D4, D5; S4; CT-R13, CT-R15, CT-R29, CT-R30,
// CT-R31, CT-R34, CT-R35, CT-R41): slots, the table, pins and the word's protocol. The wake and the
// sweep are item 4's and item 5's; the catalog's stubs stand in for them (catalog.hh). Each case
// names its criterion; the mutants each must kill are run by .scratch/mutants.py
// (docs/status/cold-tier-step2.md). Everything runs on inline_executor: single-threaded and
// deterministic, with `before_cas` placing other actors' compare-exchanges between a load and its own.
#include "catalog_world.hh"

#include <random>
#include <set>

namespace {

using W = world<inline_executor>;
using strata::pin_kind;
using strata::slot_state;
using S = slot_state;
using strata::shutdown_result;

std::string kn(int i) { return "k" + std::to_string(i); }

template <class F>
void run_task(W& w, F&& body) {
	task::run(w.exec.scheduler(), body());
}
std::uint64_t pins(W& w, std::uint32_t i) { return cat_t::pins_of(w.cat->word_of(i)); }
S st(W& w, std::uint32_t i) { return cat_t::state_of(w.cat->word_of(i)); }
unsigned freq(W& w, std::uint32_t i) { return cat_t::freq_of(w.cat->word_of(i)); }
std::uint64_t gen(W& w, std::uint32_t i) { return cat_t::gen_of(w.cat->word_of(i)); }

// --- a miss opens, single-flighted; a waiter's pin is in the word from registration (S4) ------------

// Eight requests for one cold key: one resolve, one open, one payload. Each resumes with its pin
// already in the word: when the first resumes, all eight are counted (CT-R29).
void single_flight() {
	W w(1, 2, false);
	int resumed = 0;
	std::uint64_t pins_seen = 0;
	auto one = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		if (resumed++ == 0) { pins_seen = pins(w, p.where().slot); }
		CHECK(st(w, p.where().slot) == S::open);
		CHECK(p.payload().serial == 1);
		CHECK(p.index().get(key_of(0)) == std::optional<std::string>("v0"));
	};
	std::vector<std::function<task::task<void>()>> fs(8, one);
	for (auto& f : fs) { task::spawn(task::scheduler_ref::of(w.exec.scheduler()), f()); }
	w.exec.run();
	CHECK(w.cat->table_size() == 1);
	CHECK(resumed == 8 && pins_seen == 8);
	CHECK(w.resolves == 1 && w.made == 1);
	CHECK(pins(w, 0) == 0 && st(w, 0) == S::open);
	const auto s = w.cat->stats();
	CHECK(s.misses == 1 && s.opens == 1 && s.waited[unsigned(S::opening)] == 8);
}

// The miss's own pin is not a use; a use by key sets the frequency, saturating at 3; a re-pin by
// handle with use = false does not (CT-R31). The unpin stamps the time (S4).
void use_bit() {
	W w(1, 1, false);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0), true);
		CHECK(freq(w, 0) == 0);  // a miss's pin
		auto h = p.where();
		p.release();
		for (unsigned i = 1; i <= 5; ++i) {
			auto q = co_await w.cat->pin_request(kn(0), true);
			CHECK(freq(w, 0) == std::min(i, 3u));
		}
		auto q = co_await w.cat->repin(h, kn(0));
		CHECK(freq(w, 0) == 3);
		w.cat->decay(0);
		CHECK(freq(w, 0) == 2);
		auto r = co_await w.cat->repin(h, kn(0), pin_kind::request, false);
		CHECK(freq(w, 0) == 2 && w.cat->stats().handle_pins == 2);
		auto c = co_await w.cat->pin_chore(kn(0), false);
		CHECK(freq(w, 0) == 2 && pins(w, 0) == 3);
	});
	CHECK(pins(w, 0) == 0);
	w.exec.scheduler().advance(777);
	const std::uint64_t before = w.cat->last_use_of(0);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
	CHECK(w.cat->last_use_of(0) == before + 777);
}


// A coroutine started on the test thread at once, so that it registers before the chores queued
// ahead of it run. Held by the test until it finishes.
struct actor {
	std::function<task::task<void>()> fn;  // the coroutine's closure lives as long as it runs
	task::task<void> t;
};
void start(W& w, actor& a) {
	const task::running_on on(task::scheduler_ref::of(w.exec.scheduler()));
	a.t.handle().resume();
}

// --- try_pin starts the open or the wake and answers would_wait (CT-R35) ----------------------------

void try_pin_starts() {
	W w(1, 2, true);  // defer_io: demotion ends at owed
	auto r = w.cat->try_pin(kn(0));
	CHECK(r.status == cat_t::try_status::would_wait && !r.p);
	CHECK(st(w, 0) == S::opening && pins(w, 0) == 0 && w.resolves == 0);  // started, posted, not yet run
	w.exec.run();
	CHECK(st(w, 0) == S::open && w.resolves == 1);
	r = w.cat->try_pin(kn(0));
	CHECK(r.status == cat_t::try_status::pinned && pins(w, 0) == 1 && freq(w, 0) == 1);
	r.p.release();
	CHECK(w.cat->begin_demote(0) && st(w, 0) == S::demoting);
	auto d = w.cat->try_pin(kn(0));  // on a transition: nothing counted, nothing changed
	CHECK(d.status == cat_t::try_status::would_wait && pins(w, 0) == 0 && st(w, 0) == S::demoting);
	w.exec.run();
	CHECK(st(w, 0) == S::owed && w.cat->index_of(0)->promote_owed());
	auto c = w.cat->try_pin(kn(0), pin_kind::chore, false);  // a chore pin enters `owed`, no wake
	CHECK(c.status == cat_t::try_status::pinned && st(w, 0) == S::owed && w.cat->stats().wakes_posted == 0);
	c.p.release();
	auto q = w.cat->try_pin(kn(0));  // a request pin starts the wake, holds nothing
	CHECK(q.status == cat_t::try_status::would_wait && st(w, 0) == S::waking && pins(w, 0) == 0 && w.cat->stats().wakes_posted == 1);
	auto c2 = w.cat->try_pin(kn(0), pin_kind::chore, false);  // a chore pin waits for the wake's end
	CHECK(c2.status == cat_t::try_status::would_wait && st(w, 0) == S::waking);
	w.exec.run();
	CHECK(st(w, 0) == S::open && !w.cat->index_of(0)->promote_owed() && w.cat->index_of(0)->unleased_maps() == 0);
}

// --- the wake's hand-off (the stub): the pin is handed in the final store (CT-R29) ----------------------

void wake_hands_pins() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed);
	std::uint64_t pins_at_resume = 0;
	S state_at_resume = S::free;
	bool promoted = false;
	actor chore, req;
	chore.fn = [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(0));  // enters `owed` at once
		CHECK(st(w, 0) == S::owed);
		co_await task::yield();  // holds its pin while the request pins and the wake runs
		co_await task::yield();
	};
	chore.t = chore.fn();
	req.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		pins_at_resume = pins(w, 0), state_at_resume = st(w, 0);
		promoted = !p.index().promote_owed();
		CHECK(p.index().unleased_maps() == 0);
	};
	req.t = req.fn();
	start(w, chore);
	CHECK(pins(w, 0) == 1 && st(w, 0) == S::owed);
	start(w, req);
	CHECK(pins(w, 0) == 2 && st(w, 0) == S::waking);  // the waiter's pin is counted, not handed out
	w.exec.run();
	CHECK(state_at_resume == S::open && promoted);
	CHECK(pins_at_resume >= 1 && pins(w, 0) == 0 && w.cat->waiters_of(0) == 0);
}

// A failed wake fails its request waiters and removes their pins, hands a chore waiter its pin, and
// ends at `owed`; the next request pin wakes again (CT-R29).
void wake_fails() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed);
	w.kern.fail_map = ENOMEM;
	int failed = 0;
	bool chore_got = false;
	actor a, b, c;
	auto reqf = [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->pin_request(kn(0));
		} catch (const std::system_error&) {
			++failed;
		}
	};
	a.fn = reqf, b.fn = reqf;
	a.t = a.fn();
	b.t = b.fn();
	c.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));  // meets `waking` after a: waits for the wake's end
		chore_got = true;
		CHECK(st(w, 0) == S::owed && pins(w, 0) == 1);
	};
	c.t = c.fn();
	start(w, a);
	start(w, b);
	start(w, c);
	CHECK(pins(w, 0) == 3 && st(w, 0) == S::waking);
	w.exec.run();
	CHECK(failed == 2 && chore_got);
	CHECK(pins(w, 0) == 0 && st(w, 0) == S::owed);
	w.kern.fail_map = 0;
	w.exec.scheduler().advance(10'000'000);  // the failed wake's backoff (10 ms) is over
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(st(w, 0) == S::open && p.index().unleased_maps() == 0);
	});
}

// --- demotion's end with waiters ---------------------------------------------------------------

// A request waiter at the end of a demotion to `owed` takes the slot straight to `waking`; a chore
// waiter alone is handed its pin on `owed`; a demotion that leaves the slot open hands every waiter
// its pin on `open`.
void demotion_end() {
	{
		W w(1, 2, true);
		run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
		CHECK(w.cat->begin_demote(0));
		actor r;
		r.fn = [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(0));
			CHECK(st(w, 0) == S::open && !p.index().promote_owed());
		};
		r.t = r.fn();
		start(w, r);
		CHECK(st(w, 0) == S::demoting && pins(w, 0) == 1);
		w.exec.run();
		CHECK(r.t.done() && st(w, 0) == S::open && pins(w, 0) == 0);
		CHECK(freq(w, 0) == 1);  // a waiter's use is set when it registers (CT-R41)
		const auto s = w.cat->stats();
		CHECK(s.edges[unsigned(S::demoting)][unsigned(S::waking)] == 1 && s.edges[unsigned(S::demoting)][unsigned(S::owed)] == 0);
	}
	{
		W w(1, 2, true);
		run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
		CHECK(w.cat->begin_demote(0));
		actor c;
		bool got = false;
		c.fn = [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_chore(kn(0));
			got = true;
			CHECK(st(w, 0) == S::owed && pins(w, 0) == 1);
		};
		c.t = c.fn();
		start(w, c);
		w.exec.run();
		CHECK(got && st(w, 0) == S::owed && w.cat->stats().edges[unsigned(S::demoting)][unsigned(S::owed)] == 1);
	}
	{
		W w(1, 2, false);  // no defer_io: demotion ends at open
		run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
		CHECK(w.cat->begin_demote(0));
		actor r;
		r.fn = [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(0));
			CHECK(st(w, 0) == S::open);
		};
		r.t = r.fn();
		start(w, r);
		w.exec.run();
		CHECK(r.t.done() && st(w, 0) == S::open);
	}
}

// --- every close bumps gen; a reopen in place keeps the waiters (CT-R30) -----------------------------

void close_and_reopen() {
	W w(1, 2, false);
	cat_t::handle h;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		h = p.where();
	});
	const std::uint64_t g0 = gen(w, 0);
	CHECK(h.gen == g0 && w.made == 1);
	CHECK(w.cat->begin_close(0) && st(w, 0) == S::closing);  // hot: its segments are mapped; the chore refuses
	w.exec.run();
	CHECK(st(w, 0) == S::open && gen(w, 0) == g0 && w.made == 1 && w.cat->stats().edges[unsigned(S::closing)][unsigned(S::open)] == 1);
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(w.cat->begin_close(0) && st(w, 0) == S::closing);
	CHECK(!w.cat->begin_close(0) && !w.cat->begin_demote(0));  // one performer per transition
	actor a;
	std::uint64_t seen_gen = 0;
	a.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		seen_gen = p.where().gen;
		CHECK(p.payload().serial == 2);  // a new instance
		CHECK(p.index().get(key_of(1)) == std::optional<std::string>("v1"));
	};
	a.t = a.fn();
	start(w, a);
	CHECK(pins(w, 0) == 1 && st(w, 0) == S::closing);
	w.exec.run();
	CHECK(a.t.done() && w.made == 2 && w.resolves == 2 && w.cat->table_size() == 1);
	CHECK(seen_gen == g0 + 1 && gen(w, 0) == g0 + 1);  // reopened in place: one bump
	// the stale handle falls back to the key and succeeds
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->repin(h, kn(0));
		CHECK(p.where().gen == g0 + 1 && p.where().slot == 0);
	});
	CHECK(w.cat->stats().stale_handles >= 1 && w.cat->stats().handle_pins == 0);
	// closed with no pinner: the slot is freed, erased, its gen bumped; the free list reuses it
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(w.cat->begin_close(0));
	w.exec.run();
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && gen(w, 0) == g0 + 2);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(p.where().slot == 0 && p.where().gen == g0 + 3);  // the miss bumps again
	});
	CHECK(w.cat->slots_made() == 1);
}

// --- a failed open is a negative entry with a doubling backoff (CT-R34) --------------------------------

void failed_open_backs_off() {
	W w(1, 2, false);
	w.fail_resolve = EIO;
	w.fail_left = 3;
	const std::uint64_t ms = 1'000'000;
	auto pin_fails = [&] {
		bool failed = false;
		int code = 0;
		run_task(w, [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (const std::system_error& e) {
				failed = true;
				code = e.code().value();
			}
		});
		CHECK(!failed || code == EIO);
		return failed;
	};
	CHECK(pin_fails() && w.resolves == 1 && st(w, 0) == S::failed && pins(w, 0) == 0);
	const std::uint64_t g1 = gen(w, 0);
	// until its retry time (10 ms) a pin fails at once and nothing is opened
	CHECK(pin_fails() && pin_fails() && w.resolves == 1 && w.cat->stats().failed_fast == 2);
	w.exec.scheduler().advance(9 * ms);
	CHECK(pin_fails() && w.resolves == 1);
	w.exec.scheduler().advance(1 * ms);  // 10 ms: the first pin after reopens (and fails again: 20 ms)
	CHECK(pin_fails() && w.resolves == 2 && gen(w, 0) == g1 + 2);  // the retry and the failure each bump
	w.exec.scheduler().advance(19 * ms);
	CHECK(pin_fails() && w.resolves == 2);
	w.exec.scheduler().advance(1 * ms);
	CHECK(pin_fails() && w.resolves == 3);  // fails a third time: 40 ms
	w.exec.scheduler().advance(39 * ms);
	CHECK(pin_fails() && w.resolves == 3);
	w.exec.scheduler().advance(1 * ms);
	CHECK(!pin_fails() && w.resolves == 4 && st(w, 0) == S::open);  // the fourth resolve succeeds
	// the count resets on success: close it, fail once more, and the delay is 10 ms again
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(w.cat->begin_close(0));
	w.exec.run();
	w.fail_left = 1;
	CHECK(pin_fails());
	w.exec.scheduler().advance(10 * ms);
	CHECK(!pin_fails());
	const auto s = w.cat->stats();
	CHECK(s.open_failures == 4 && s.retries == 4 && s.edges[unsigned(S::failed)][unsigned(S::opening)] == 4);
}

// A waiter on an open that fails gets the error and its pin is removed in the same store (CT-R29).
void failed_open_fails_waiters() {
	W w(1, 2, false);
	w.fail_resolve = EIO;
	w.fail_left = 1;
	int failed = 0;
	std::vector<actor> as(5);
	for (auto& a : as) {
		a.fn = [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (const std::system_error&) {
				++failed;
			}
		};
		a.t = a.fn();
		start(w, a);
	}
	CHECK(pins(w, 0) == 5);
	w.exec.run();
	CHECK(failed == 5 && pins(w, 0) == 0 && st(w, 0) == S::failed);
}

// --- one slot per key, one owner per directory (CT-R15) ----------------------------------------------

void second_key_same_directory() {
	W w(1, 2, false);
	w.alias["alias"] = 0;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		int code = 0;
		try {
			auto q = co_await w.cat->pin_request("alias");
		} catch (const std::system_error& e) {
			code = e.code().value();
		}
		CHECK(code == EWOULDBLOCK);
		CHECK(w.resolves == 2);
		try {  // not retried: the negative entry answers
			auto q = co_await w.cat->pin_request("alias");
		} catch (const std::system_error& e) {
			code = e.code().value() + 1000;
		}
		CHECK(code == EWOULDBLOCK + 1000 && w.resolves == 2);
		CHECK(p.index().get(key_of(0)) == std::optional<std::string>("v0"));  // the first key is untouched
	});
	CHECK(w.cat->table_size() == 2);
	CHECK(w.kern.fds.load() > 0);
}

// --- the unpin stamps the time before it decrements (S4) ------------------------------------------------

// The hook between the stamp and the decrement sees the stamp already in: a sweep that finds
// {0, open} then reads the use it just ended.
void unpin_order() {
	std::uint64_t stamp_expected = 0;
	int calls = 0;
	W* wp = nullptr;
	W w(1, 1, false, true, [&](const char* site, bool) {
		if (std::strcmp(site, "unpin") != 0 || !wp) { return; }
		++calls;
		CHECK(wp->cat->last_use_of(0) == stamp_expected);
	});
	wp = &w;
	std::optional<cat_t::pin> held;
	run_task(w, [&]() -> task::task<void> { held.emplace(co_await w.cat->pin_request(kn(0))); co_return; });
	w.exec.scheduler().advance(5000);
	stamp_expected = w.exec.now();
	held.reset();
	CHECK(calls == 1 && pins(w, 0) == 0);
	wp = nullptr;
}


// --- a chore pin whose thread holds the lease enters `waking` (CT-R40's exception) ----------------------

void chore_pin_on_waking() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed);
	auto c1 = w.cat->try_pin(kn(0), pin_kind::chore, false);  // enters `owed`
	CHECK(c1.status == cat_t::try_status::pinned);
	auto q = w.cat->try_pin(kn(0));
	CHECK(q.status == cat_t::try_status::would_wait && st(w, 0) == S::waking && pins(w, 0) == 1);
	auto c2 = w.cat->try_pin(kn(0), pin_kind::chore, false);  // no lease held here: waits for the wake's end
	CHECK(c2.status == cat_t::try_status::would_wait && pins(w, 0) == 1);
	{
		const strata::lease_scope hold{c1.p.index().lease()};
		auto c3 = w.cat->try_pin(kn(0), pin_kind::chore, false);  // a nested pin in a commit's call
		CHECK(c3.status == cat_t::try_status::pinned && pins(w, 0) == 2 && st(w, 0) == S::waking);
	}
	CHECK(pins(w, 0) == 1);
	c1.p.release();
	w.exec.run();
	CHECK(st(w, 0) == S::open && pins(w, 0) == 0);
}

// --- waiters resume after the mutex is released, on their scheduler (CT-R35) ------------------------------

// The resumptions are posted behind work already queued, so none runs inside the transition's chore.
void resume_posted() {
	W w(1, 2, false);
	std::vector<char> order;
	std::vector<actor> as(3);
	for (auto& a : as) {
		a.fn = [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(0));
			order.push_back('W');
		};
		a.t = a.fn();
		start(w, a);
	}
	actor marker;
	marker.fn = [&]() -> task::task<void> {
		order.push_back('M');
		co_return;
	};
	marker.t = marker.fn();
	task::spawn(task::scheduler_ref::of(w.exec.scheduler()), std::move(marker.t));
	w.exec.run();
	CHECK((order == std::vector<char>{'M', 'W', 'W', 'W'}));
}

// A waiter with no scheduler is resumed by the thread that made the final store, after the shard
// mutex is released: its continuation may take any shard's mutex.
void resume_outside_mutex() {
	W w(1, 2, false);
	actor a;
	bool ran = false;
	a.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		ran = true;
		CHECK(w.cat->table_size() == 1);  // takes every shard's mutex
		auto q = w.cat->try_pin(kn(0), pin_kind::request, false);
		CHECK(q.status == cat_t::try_status::pinned);
	};
	a.t = a.fn();
	a.t.handle().resume();  // no scheduler on this thread
	CHECK(pins(w, 0) == 1 && !ran);
	w.exec.run();
	CHECK(ran && a.t.done() && pins(w, 0) == 0);
}

// --- a pin released between a transition's load and its final store is not lost (CT-R29) ---------------

// The wake's final store loads the word, then another thread's chore pin releases (the hook), so the
// store must be a compare-exchange: a plain store of the loaded word would bring the pin back.
void unpin_during_wake_end() {
	std::optional<cat_t::pin> held;
	bool armed = false;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (armed && std::strcmp(site, "wake.end") == 0) {
			armed = false;
			held.reset();
		}
	});
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	auto c = w.cat->try_pin(kn(0), pin_kind::chore, false);
	CHECK(c.status == cat_t::try_status::pinned);
	held.emplace(std::move(c.p));
	auto q = w.cat->try_pin(kn(0));  // starts the wake; the chore pin is still held
	CHECK(q.status == cat_t::try_status::would_wait && pins(w, 0) == 1);
	armed = true;
	w.exec.run();
	CHECK(!armed && !held && st(w, 0) == S::open && pins(w, 0) == 0);
}

// A pin that retries a failed slot in the window after the open's final store, before its waiters are
// delivered, joins the new open's waiters; the old delivery does not take it (CT-R29, CT-R34).
void retry_joins_new_transition() {
	bool armed = false;
	W* wp = nullptr;
	actor first, late;
	int first_failed = 0;
	bool late_ok = false;
	W w(1, 2, false, true, [&](const char* site, bool) {
		if (armed && wp && std::strcmp(site, "open.unlocked") == 0) {
			armed = false;
			wp->exec.scheduler().advance(20'000'000'000ull);  // past the retry time
			late.fn = [&]() -> task::task<void> {
				auto p = co_await wp->cat->pin_request(kn(0));
				late_ok = true;
			};
			late.t = late.fn();
			start(*wp, late);
		}
	});
	wp = &w;
	w.fail_resolve = EIO;
	w.fail_left = 1;
	first.fn = [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->pin_request(kn(0));
		} catch (const std::system_error&) {
			++first_failed;
		}
	};
	first.t = first.fn();
	armed = true;
	start(w, first);
	w.exec.run();
	CHECK(first_failed == 1 && late_ok && late.t.done());
	CHECK(pins(w, 0) == 0 && st(w, 0) == S::open && w.resolves == 2);
	wp = nullptr;
}

// --- T6: seeded schedules over every writer of the word (S4) ----------------------------------------------

// The state changes D4's table allows (and slot_model's: its `open` with the flag set is `owed`).
bool allowed_edge(S a, S b) {
	static const std::pair<S, S> ok[] = {
		{S::free, S::opening},    {S::opening, S::open},   {S::opening, S::failed}, {S::failed, S::opening},
		{S::open, S::demoting},   {S::demoting, S::open},  {S::demoting, S::owed},  {S::demoting, S::waking},
		{S::owed, S::waking},     {S::waking, S::open},    {S::waking, S::owed},    {S::open, S::closing},
		{S::owed, S::closing},    {S::closing, S::free},   {S::closing, S::opening}, {S::closing, S::open},
		{S::closing, S::owed},    {S::closing, S::waking},  {S::failed, S::free},
		{S::demoting, S::closing}, {S::waking, S::closing},  // a fence met by a chore: the index is replaced (F4)
	};
	for (const auto& e : ok) {
		if (e.first == a && e.second == b) { return true; }
	}
	return false;
}

struct totals {
	std::uint64_t edges[slot_states][slot_states] = {};
	std::uint64_t waited[slot_states] = {};
	std::uint64_t stale = 0, failed_fast = 0, retries = 0, handle_pins = 0, wakes = 0, opens = 0, open_failures = 0;
	std::uint64_t reposts = 0, wake_failures = 0, wake_overloads = 0, gated = 0, bound_closes = 0, bound_commits = 0, commit_failures = 0, chore_commits = 0;
	std::uint64_t passes = 0, pass_demotes = 0, pass_closes = 0, pass_reposts = 0, erased = 0, reclaims = 0, shutdown_commits = 0;
	std::map<std::string, std::uint64_t> sites, injected;
	std::uint64_t pins_granted = 0, pins_failed = 0, steps = 0;
	std::uint64_t fences = 0, reopens = 0, replaces = 0, drain_unpins = 0, creates = 0;
};

// One seeded schedule. The `before_cas` hook runs another actor's lock-free compare-exchange (an
// unpin, a sweep's demote or close, a decay) between a load and the compare-exchange it feeds,
// at every site; the test thread drives pins, the sweep and the clock between its steps.
struct sim {
	static constexpr int keys = 5;
	std::mt19937_64 rng;
	bool defer;
	totals& tot;
	bool in_hook = false;
	std::unique_ptr<W> w;
	std::vector<std::unique_ptr<actor>> actors;
	// A pin held by the test, and sometimes a lease hold on its index (a chore call's); the hold ends first.
	struct held_pin {
		std::unique_ptr<cat_t::pin> p;
		std::unique_ptr<strata::lease_scope> hold;
	};
	std::vector<held_pin> held;
	bool settling = false;  // while the schedule settles, a pin granted late takes no lease hold (a wake behind one would never end)
	cat_t::handle handles[keys];
	std::vector<std::uint64_t> last_gen;
	std::set<std::string> fenced_keys;  // keys whose index was ever fenced: shutdown may name them

	sim(std::uint64_t seed, totals& t) : rng(seed * 0x9E3779B97F4A7C15ull + 1), defer(seed % 2 == 0), tot(t) {
		auto hook = [this](const char* site, bool locked) { on_site(site, locked); };
		w = std::make_unique<W>(keys, 2, defer, true, hook, 2);  // the bound: 2 failed wakes
		w->tweak = [](cat_t::host& h) { h.soft_open = 3, h.pass_posts = 2; };  // the sweep: a cap under the key count
		w->make_catalog(hook);
	}

	void release_random() {
		if (held.empty()) { return; }
		const std::size_t i = rng() % held.size();
		auto p = std::move(held[i]);
		held[i] = std::move(held.back());
		held.pop_back();
		p.hold.reset();
		p.p.reset();  // unpin: may call the hook
	}
	void release_leases() {
		for (auto& h : held) { h.hold.reset(); }
	}
	~sim() { in_hook = true; }  // the world's destructor drains with the hook still set
	void on_site(const char* site, bool locked) {
		++tot.sites[site];
		if (in_hook || !w || rng() % 3 != 0) { return; }
		in_hook = true;
		++tot.injected[site];
		const std::uint32_t n = w->cat->slots_made();
		switch (rng() % (locked ? 5 : 6)) {
		case 5: {  // where no mutex is held: the retry window opens, and a pin by key joins the new transition
			w->exec.scheduler().advance(20'000'000'000ull);
			actors.push_back(std::make_unique<actor>());
			actors.back()->t = pinner(int(rng() % keys), pin_kind::request, true, false);
			start(*w, *actors.back());
			break;
		}
		case 0: release_random(); break;
		case 3: if (n) { w->cat->begin_bound_close(std::uint32_t(rng() % n)); } break;
		case 1:
			if (n) { w->cat->begin_demote(std::uint32_t(rng() % n)); }
			if (rng() % 4 == 0) { w->cat->post_pass(); }  // a crossing between a load and a compare-exchange
			break;
		case 2: if (n) { w->cat->begin_close(std::uint32_t(rng() % n)); } break;
		default: if (n) { w->cat->decay(std::uint32_t(rng() % n)); } break;  // 4
		}
		in_hook = false;
	}

	// Fences the index behind a held pin that holds no lease (a sync failure under commit), then hands the pin back
	// to reopen() or leaves the fenced index for the wake, the sweep or the next holder to meet.
	void fence_one() {
		std::vector<std::size_t> ok;
		for (std::size_t i = 0; i < held.size(); ++i) {
			const S s = cat_t::state_of(w->cat->word_of(held[i].p->where().slot));
			if (!held[i].hold && (s == S::open || s == S::owed) && !held[i].p->index().fenced()) { ok.push_back(i); }
		}
		if (ok.empty()) { return; }
		const std::size_t i = ok[rng() % ok.size()];
		auto& pin = *held[i].p;
		const std::string key = w->cat->key_at(pin.where().slot);
		const int dir = std::atoi(key.c_str() + 1);
		w->dirs[dir].fs.add_fault({faultfs::op::sync, 1, EIO});
		bool fenced = false;
		try {
			if (pin.index().apply(tx({add(key_of(100 + int(rng() % 50)), "x")}), {}).result == verdict::accepted) { pin.index().commit(); }
		} catch (const index_fenced&) {
			fenced = true;
		} catch (const std::system_error&) {  // a kernel fault the schedule injected: not a fence
		}
		w->dirs[dir].fs.clear_faults();
		if (!fenced) { return; }
		++tot.fences;
		fenced_keys.insert(key);
		if (rng() % 3 != 0) {
			auto p = std::move(held[i]);
			held[i] = std::move(held.back());
			held.pop_back();
			++tot.reopens;
			w->cat->reopen(std::move(*p.p));
		}
	}

	task::task<void> pinner(int key, pin_kind kind, bool use, bool via_handle) {
		try {
			cat_t::pin p;
			const cat_t::handle h = handles[key];
			if (via_handle && h) {
				p = co_await w->cat->repin(h, kn(key), kind, use);
			} else if (kind == pin_kind::request) {
				p = co_await w->cat->pin_request(kn(key), use);
			} else {
				p = co_await w->cat->pin_chore(kn(key), use);
			}
			++tot.pins_granted;
			const std::uint64_t word = w->cat->word_of(p.where().slot);
			CHECK(cat_t::pins_of(word) >= 1);
			if (kind == pin_kind::request) {
				CHECK(cat_t::state_of(word) == S::open);  // I9: a request pin is never handed out on `owed`
				CHECK(!p.index().promote_owed());
				CHECK(p.index().unleased_maps() == 0);
			} else {
				// `waking` only for a pin whose thread holds the lease (a nested pin in a call, CT-R40), or one granted
				// on `owed` before another pin's wake began (this reads the word when the task resumes, not at the grant)
				CHECK(cat_t::state_of(word) == S::open || cat_t::state_of(word) == S::owed || (cat_t::state_of(word) == S::waking && (p.index().lease().held_here() || cat_t::pins_of(word) >= 2)));
				if (cat_t::state_of(word) == S::open && rng() % 2) {  // a write the close must not lose
					auto& ix = p.index();
					const auto v = ix.apply(tx({erase(key_of(int(rng() % 2)))}), {}).result;
					CHECK(v != verdict::map_owed);  // an `open` slot has its bodies (I8)
					if (v == verdict::accepted) {
						ix.refresh();
						if (rng() % 4 == 0) { ix.commit(); }
					}
				}
			}
			handles[key] = p.where();
			held_pin hp;
			hp.p = std::make_unique<cat_t::pin>(std::move(p));
			if (!settling && (kind == pin_kind::chore ? rng() % 5 < 3 : rng() % 8 == 0)) { hp.hold = std::make_unique<strata::lease_scope>(hp.p->index().lease()); }  // a call that holds the lease
			held.push_back(std::move(hp));
		} catch (const std::system_error&) {
			++tot.pins_failed;
		}
	}

	void quiescent() {
		release_leases();  // a wake behind a hold ends once the hold does
		settling = true;
		w->settle();
		for (int round = 0; round < 6; ++round) {  // a replace drains when its holders unpin: the test's holders let go
			bool any = false;
			for (std::size_t i = 0; i < held.size();) {
				if (cat_t::state_of(w->cat->word_of(held[i].p->where().slot)) == S::closing) {
					held_pin gone = std::move(held[i]);  // out of the vector before the unpin: its hook may release another
					held.erase(held.begin() + std::ptrdiff_t(i));
					gone.p.reset();
					any = true;
				} else {
					++i;
				}
			}
			if (!any) { break; }
			w->settle();
		}
		settling = false;
		for (auto& a : actors) { CHECK(a->t.done()); }  // no stuck waiter
		const std::uint32_t n = w->cat->slots_made();
		last_gen.resize(n, 0);
		std::size_t live = 0;
		for (std::uint32_t i = 0; i < n; ++i) {
			const std::uint64_t word = w->cat->word_of(i);
			const S s = cat_t::state_of(word);
			std::uint64_t mine = 0;
			for (const auto& h : held) { mine += h.p->where().slot == i; }
			CHECK(cat_t::pins_of(word) == mine);  // the word counts exactly the pins held
			CHECK(w->cat->waiters_of(i) == 0);
			CHECK(s == S::free || s == S::open || s == S::owed || s == S::failed);
			CHECK(cat_t::gen_of(word) >= last_gen[i]);
			last_gen[i] = cat_t::gen_of(word);
			live += s != S::free;
			Index_checks(i, s);
		}
		CHECK(w->cat->table_size() == live);
	}
	void Index_checks(std::uint32_t i, S s) {
		kv_index* ix = w->cat->index_of(i);
		if (s == S::open || s == S::owed) {
			CHECK(ix != nullptr);
			if (ix) {
				CHECK((s == S::owed) == ix->promote_owed());  // CT-R28: the word's `owed` is the flag
				CHECK(ix->unleased_maps() == 0);
			}
		} else {
			CHECK(ix == nullptr);
		}
	}

	void run(int steps) {
		const std::uint64_t ms = 1'000'000;
		for (int step = 0; step < steps; ++step) {
			++tot.steps;
			const unsigned r = unsigned(rng() % 100);
			const std::uint32_t n = w->cat->slots_made();
			if (rng() % 12 == 0) {
				fence_one();
				continue;
			}
			if (rng() % 14 == 0) {  // the sweep: a pass, the clock past the idle times and the negative entries' time
				switch (rng() % 4) {
				case 0: w->cat->post_pass(); w->exec.run(); break;
				case 1: w->exec.scheduler().advance((11 + rng() % 30) * 1000 * ms); w->cat->post_pass(); w->exec.run(); break;
				case 2: w->exec.scheduler().advance(700'000 * ms); w->cat->post_pass(); break;
				default: w->cat->post_pass(); break;  // left queued: the next run() takes it among the others
				}
				continue;
			}
			if (r < 30) {
				actors.push_back(std::make_unique<actor>());
				const bool chore = rng() % 4 == 0;
				actors.back()->t = pinner(int(rng() % keys), chore ? pin_kind::chore : pin_kind::request, !chore && rng() % 2, rng() % 3 == 0);
				start(*w, *actors.back());
			} else if (r < 42) {
				release_random();
			} else if (r < 50) {
				if (n) { w->cat->begin_demote(std::uint32_t(rng() % n)); }
			} else if (r < 56) {
				if (n) { w->cat->begin_close(std::uint32_t(rng() % n)); }
			} else if (r < 64) {
				if (n) { w->cat->begin_bound_close(std::uint32_t(rng() % n)); }
			} else if (r < 66) {
				if (n) { w->cat->decay(std::uint32_t(rng() % n)); }
			} else if (r < 70) {
				w->exec.scheduler().advance(rng() % (30 * ms));
			} else if (r < 74) {
				w->fire_next();
			} else if (r < 80) {
				w->exec.run();  // the chores run with the holds still in place: a busy lease reposts
			} else if (r < 81) {
				w->fail_resolve = EIO, w->fail_left = int(1 + rng() % 2);
			} else if (r < 85) {
				w->kern.fail_map = rng() % 3 == 0 ? 0 : (rng() % 2 ? ENOMEM : EMFILE);
			} else if (r < 87) {
				w->fail_commit = rng() % 2 ? EIO : 0;
			} else if (r < 89) {  // drive a warm slot past the bound with persistent faults, then the sweep's bound close
				for (std::uint32_t i = 0; i < n; ++i) {
					const std::uint32_t slot = std::uint32_t((i + rng()) % n);
					if (cat_t::state_of(w->cat->word_of(slot)) != S::owed || cat_t::pins_of(w->cat->word_of(slot)) != 0) { continue; }
					const int key = std::atoi(w->cat->key_at(slot).c_str() + 1);
					const bool overload = rng() % 3 == 0;  // the budget's cap instead of the kernel's error: not counted
					strata::resource_reservation fill;
					if (overload) {
						fill = w->budget.reserve(strata::resource::maps, w->budget.hard(strata::resource::maps) - w->budget.used(strata::resource::maps));
					} else {
						w->kern.fail_map = rng() % 2 ? ENOMEM : EMFILE;
					}
					for (int k = 0; k < 2; ++k) {
						actors.push_back(std::make_unique<actor>());
						actors.back()->t = pinner(key, pin_kind::request, false, false);
						start(*w, *actors.back());
						w->exec.run();
						w->exec.scheduler().advance(100 * ms);
					}
					fill = {};
					if (rng() % 2) {  // the host's commit chore on `owed`, with or without the kernel's fault (I4-B1)
						if (rng() % 2) { w->kern.fail_map = 0; }
						auto c = w->cat->try_pin(kn(key), pin_kind::chore, false);
						if (c.status == cat_t::try_status::pinned) {
							const strata::lease_scope hold{c.p.index().lease()};
							try {
								c.p.index().commit();
								++tot.chore_commits;
							} catch (const std::system_error&) {
							}
						}
					}
					w->kern.fail_map = 0;
					w->fail_commit = rng() % 2 ? EIO : 0;
					w->cat->begin_bound_close(slot);
					break;
				}
			} else if (r < 90) {  // a request for a warm slot a chore call holds: the wake meets the hold and reposts
				for (const auto& h : held) {
					const auto slot = h.p->where().slot;
					if (h.hold && cat_t::state_of(w->cat->word_of(slot)) == S::owed) {
						actors.push_back(std::make_unique<actor>());
						actors.back()->t = pinner(std::atoi(w->cat->key_at(slot).c_str() + 1), pin_kind::request, true, false);
						start(*w, *actors.back());
						w->exec.run();
						break;
					}
				}
			} else {
				quiescent();
			}
		}
		w->fail_commit = 0;
		w->kern.fail_map = 0;
		w->fail_left = 0;
		while (!held.empty()) { release_random(); }
		w->exec.scheduler().advance(20'000 * ms);
		quiescent();
		// Shutdown after any history: every slot closes, what a failed commit would lose is named, nothing is left.
		release_leases();
		while (!held.empty()) { release_random(); }  // pins the last settle handed out
		w->cat->stop_sweep();
		w->start_shutdown();
		settling = true;
		for (int round = 0; round < 8 && !w->sd_done; ++round) {  // a waiter resumed during the shutdown holds a pin until released
			w->settle();
			release_leases();
			while (!held.empty()) { release_random(); }
		}
		w->settle();
		settling = false;
		for (const auto& k : w->sd_res.uncommitted) { CHECK(fenced_keys.count(k) == 1); }  // only a fenced index fails its commit
		CHECK(w->sd_done && w->cat->shut_down() && w->cat->occupied() == 0 && w->cat->table_size() == 0);
		CHECK(w->kern.fds.load() == 0 && w->kern.maps.load() == 0);
		quiescent();
		const auto s = w->cat->stats();
		for (unsigned i = 0; i < slot_states; ++i) {
			tot.waited[i] += s.waited[i];
			for (unsigned j = 0; j < slot_states; ++j) { tot.edges[i][j] += s.edges[i][j]; }
		}
		tot.stale += s.stale_handles, tot.failed_fast += s.failed_fast, tot.retries += s.retries, tot.handle_pins += s.handle_pins;
		tot.wakes += s.wakes_posted, tot.opens += s.opens, tot.open_failures += s.open_failures;
		tot.reposts += s.wake_reposts, tot.wake_failures += s.wake_failures, tot.wake_overloads += s.wake_overloads, tot.gated += s.wake_gated;
		tot.bound_closes += s.bound_closes, tot.bound_commits += s.bound_commits, tot.commit_failures += s.commit_failures;
		tot.passes += s.passes, tot.pass_demotes += s.pass_demotes, tot.pass_closes += s.pass_closes, tot.pass_reposts += s.pass_reposts;
		tot.erased += s.erased, tot.reclaims += s.reclaims, tot.shutdown_commits += s.shutdown_commits;
		tot.replaces += s.replaces, tot.drain_unpins += s.drain_unpins;
	}
};

void schedules(int seeds) {
	totals tot;
	for (int seed = 1; seed <= seeds; ++seed) {
		sim s(std::uint64_t(seed), tot);
		s.run(160);
		if (failures) {
			std::fprintf(stderr, "schedule: seed %d failed\n", seed);
			return;
		}
	}
	for (unsigned i = 0; i < slot_states; ++i) {
		for (unsigned j = 0; j < slot_states; ++j) {
			if (tot.edges[i][j]) { CHECK(allowed_edge(S(i), S(j))); }
		}
	}
	// Every state change the table names was made, every transition was met by a pin, and each
	// site had another actor's compare-exchange injected before its own.
	for (unsigned i = 0; i < slot_states; ++i) {
		for (unsigned j = 0; j < slot_states; ++j) {
			if (allowed_edge(S(i), S(j))) {
				if (!tot.edges[i][j]) { std::fprintf(stderr, "edge %u->%u never made\n", i, j); }
				CHECK(tot.edges[i][j] > 0);
			}
		}
	}
	for (S m : {S::opening, S::waking, S::demoting, S::closing}) { CHECK(tot.waited[unsigned(m)] > 0); }
	CHECK(tot.stale > 0 && tot.failed_fast > 0 && tot.retries > 0 && tot.handle_pins > 0 && tot.wakes > 0);
	CHECK(tot.reposts > 0 && tot.wake_failures > 0 && tot.wake_overloads > 0 && tot.gated > 0 && tot.bound_closes > 0 && tot.bound_commits > 0 && tot.commit_failures > 0 && tot.chore_commits > 0);
	for (const char* site : {"pin.key", "pin.miss", "pin.handle", "open.end", "open.unlocked", "transition.end", "wake.end", "close.end", "sweep.demote", "sweep.close", "sweep.bound", "wake.unlocked", "policy.decay", "unpin", "sweep.erase", "shutdown.close", "chore.demote", "chore.close", "close.call", "reopen", "replace.begin", "drain.unpin", "chore.replace"}) {
		if (!tot.injected[site]) { std::fprintf(stderr, "site %s: no injection\n", site); }
		CHECK(tot.injected[site] > 0);
	}
	std::printf("schedules: %d seeds, %llu steps, %llu pins granted, %llu failed\n", seeds, (unsigned long long)tot.steps,
		(unsigned long long)tot.pins_granted, (unsigned long long)tot.pins_failed);
	std::printf("  state changes:");
	for (unsigned i = 0; i < slot_states; ++i) {
		for (unsigned j = 0; j < slot_states; ++j) {
			if (tot.edges[i][j]) { std::printf(" %u>%u:%llu", i, j, (unsigned long long)tot.edges[i][j]); }
		}
	}
	std::printf("\n  met transition:");
	for (unsigned i = 0; i < slot_states; ++i) {
		if (tot.waited[i]) { std::printf(" %u:%llu", i, (unsigned long long)tot.waited[i]); }
	}
	std::printf("\n  injections:");
	for (const auto& [k, v] : tot.injected) { std::printf(" %s:%llu/%llu", k.c_str(), (unsigned long long)v, (unsigned long long)tot.sites[k]); }
	std::printf("\n  wake: reposts %llu failures %llu overloads %llu gated %llu; bound closes %llu (commits %llu, failed %llu)", (unsigned long long)tot.reposts,
		(unsigned long long)tot.wake_failures, (unsigned long long)tot.wake_overloads, (unsigned long long)tot.gated, (unsigned long long)tot.bound_closes,
		(unsigned long long)tot.bound_commits, (unsigned long long)tot.commit_failures);
	std::printf("\n  sweep: %llu passes (%llu reposts), %llu demotes, %llu closes, %llu negative entries erased, %llu reclaims; shutdown commits %llu",
		(unsigned long long)tot.passes, (unsigned long long)tot.pass_reposts, (unsigned long long)tot.pass_demotes, (unsigned long long)tot.pass_closes,
		(unsigned long long)tot.erased, (unsigned long long)tot.reclaims, (unsigned long long)tot.shutdown_commits);
	CHECK(tot.passes > 0 && tot.pass_demotes > 0 && tot.pass_closes > 0 && tot.pass_reposts > 0 && tot.erased > 0);
	CHECK(tot.fences > 0 && tot.reopens > 0 && tot.replaces > 0 && tot.drain_unpins > 0);
	std::printf("\n  fences %llu (%llu handed to reopen), replaces %llu, drain unpins %llu", (unsigned long long)tot.fences, (unsigned long long)tot.reopens,
		(unsigned long long)tot.replaces, (unsigned long long)tot.drain_unpins);
	std::printf("\n  chore commits on owed (drive) %llu", (unsigned long long)tot.chore_commits);
	std::printf("\n  stale %llu failed_fast %llu retries %llu handle_pins %llu wakes %llu\n", (unsigned long long)tot.stale, (unsigned long long)tot.failed_fast,
		(unsigned long long)tot.retries, (unsigned long long)tot.handle_pins, (unsigned long long)tot.wakes);
}

// --- the pin count stops at the word's 24 bits (I3-B1, limits.md "Pins on one slot") ----------------

// At the cap a pin by key, by handle, by try_pin and a chore's all fail with too_many_open and change
// nothing; one below it the pin is granted and fills the field. Without the check the count wraps to
// zero and the slot can be closed under its pins.
void pin_cap() {
	constexpr std::uint64_t cap = strata::catalog_detail::word::max_pins;
	W w(1, 1, false);
	const auto refused = [&](auto&& pin_it) -> bool {
		bool threw = false;
		try {
			pin_it();
		} catch (const strata::too_many_open&) {
			threw = true;
		}
		return threw;
	};
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		const auto h = p.where();
		w.cat->set_pins_for_test(0, cap);
		const std::uint64_t at_cap = w.cat->word_of(0);
		CHECK(pins(w, 0) == cap && st(w, 0) == S::open);
		bool by_key = false, by_handle = false, by_chore = false;
		try {
			auto q = co_await w.cat->pin_request(kn(0));
		} catch (const strata::too_many_open&) {
			by_key = true;
		}
		try {
			auto q = co_await w.cat->repin(h, kn(0));
		} catch (const strata::too_many_open&) {
			by_handle = true;
		}
		try {
			auto q = co_await w.cat->pin_chore(kn(0));
		} catch (const strata::too_many_open&) {
			by_chore = true;
		}
		CHECK(by_key && by_handle && by_chore);
		CHECK(refused([&] { (void)w.cat->try_pin(kn(0)); }));
		CHECK(w.cat->word_of(0) == at_cap);  // not one bit moved
		CHECK(w.cat->stats().handle_pins == 0);
		w.cat->set_pins_for_test(0, cap - 1);
		{
			auto q = co_await w.cat->repin(h, kn(0));  // the last pin the field holds
			CHECK(pins(w, 0) == cap && st(w, 0) == S::open && w.cat->stats().handle_pins == 1);
		}
		CHECK(pins(w, 0) == cap - 1);
		w.cat->set_pins_for_test(0, 1);
	});
	CHECK(pins(w, 0) == 0 && st(w, 0) == S::open);
}

// A slot is about D4's 192 B, not the index's 1.5 KB: the index is allocated by the open and freed by
// the close (C16, I3-N4).
void slot_cost() {
	std::printf("catalog: a slot is %zu B (sizeof(kv_index) %zu B, held by an open slot only)\n", cat_t::slot_bytes(), sizeof(kv_index));
	static_assert(cat_t::slot_bytes() <= 192, "a free or cold slot is D4's ~192 B plus the host's payload");
	W w(1, 1, false);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(w.cat->index_of(0) != nullptr);
		co_return;
	});
	w.drain();
	CHECK(st(w, 0) == S::free && w.cat->index_of(0) == nullptr);
}

}  // namespace

int main(int argc, char** argv) {
	single_flight();
	use_bit();
	try_pin_starts();
	wake_hands_pins();
	wake_fails();
	demotion_end();
	close_and_reopen();
	failed_open_backs_off();
	failed_open_fails_waiters();
	second_key_same_directory();
	unpin_order();
	chore_pin_on_waking();
	resume_posted();
	resume_outside_mutex();
	unpin_during_wake_end();
	retry_joins_new_transition();
	pin_cap();
	slot_cost();
	schedules(argc > 1 ? std::atoi(argv[1]) : 300);
	if (failures) {
		std::fprintf(stderr, "catalog: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog: ok");
	return 0;
}
