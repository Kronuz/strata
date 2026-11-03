// The cold tier's step 2, item 4 (docs/design/cold-tier.md D4, D5; S5, S6; CT-R23, CT-R28, CT-R33, CT-R38.5,
// CT-R38.6, CT-R40, I5-N2): the wake, chore pins and the bound. Everything runs on inline_executor with the
// host's timer injected (world::timers): single-threaded and deterministic, except where a second thread
// holds an index's lease, which it does by handshake. Each case names its criterion; the mutants each
// must kill are run by .scratch/mutants4.py (docs/status/cold-tier-step2.md).
#include "catalog_world.hh"

#include <condition_variable>
#include <thread>

namespace {

using W = world<inline_executor>;
using strata::pin_kind;
using strata::slot_state;
using strata::wake_failed;
using S = slot_state;
constexpr std::uint64_t ms = 1'000'000;

std::string kn(int i) { return "k" + std::to_string(i); }

template <class F>
void run_task(W& w, F&& body) {
	task::run(w.exec.scheduler(), body());
}
std::uint64_t pins(W& w, std::uint32_t i) { return cat_t::pins_of(w.cat->word_of(i)); }
S st(W& w, std::uint32_t i) { return cat_t::state_of(w.cat->word_of(i)); }

struct actor {
	std::function<task::task<void>()> fn;  // the coroutine's closure lives as long as it runs
	task::task<void> t;
};
void start(W& w, actor& a) {
	const task::running_on on(task::scheduler_ref::of(w.exec.scheduler()));
	a.t.handle().resume();
}
// Starts it on no scheduler: the delivery that hands it its pin resumes it inline.
void start_inline(actor& a) { a.t.handle().resume(); }

// A pin and release, a demotion: the slot is `owed` (defer_io).
void to_owed(W& w) {
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0), false); co_return; });
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed && pins(w, 0) == 0 && w.cat->index_of(0)->promote_owed());
}
// An applied, flushed delete the index has not committed.
void write_erase(W& w) {
	run_task(w, [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(0));
		auto& ix = c.index();
		CHECK(ix.apply(tx({erase(key_of(0))}), {}).result == verdict::accepted);
		ix.refresh();
	});
	CHECK(w.cat->index_of(0)->uncommitted());
}
std::uint64_t backoff(unsigned failures) { return (10 * ms) << (failures - 1); }  // 10 ms doubling (below its cap)

// The error a failed try_pin carries: the wake_failed and its cause's errno, 0 for anything else.
struct failure {
	bool wake = false;
	int code = 0;
};
failure why(const std::exception_ptr& e) {
	failure f;
	try {
		std::rethrow_exception(e);
	} catch (const wake_failed& x) {
		f.wake = true;
		try {
			std::rethrow_exception(x.cause());
		} catch (const std::system_error& c) {
			f.code = c.code().value();
		} catch (...) {
		}
	} catch (const std::system_error& x) {
		f.code = x.code().value();
	} catch (...) {
	}
	return f;
}

// A second thread that holds an index's lease on request: call() runs a function on it and waits.
class runner {
public:
	runner() : t_([this] { loop(); }) {}
	~runner() {
		{
			std::lock_guard l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		t_.join();
	}
	void call(std::function<void()> f) {
		std::unique_lock l(m_);
		job_ = std::move(f);
		busy_ = true;
		cv_.notify_all();
		cv_.wait(l, [&] { return !busy_; });
	}

private:
	void loop() {
		std::unique_lock l(m_);
		for (;;) {
			cv_.wait(l, [&] { return busy_ || stop_; });
			if (stop_) { return; }
			auto f = std::move(job_);
			l.unlock();
			f();
			l.lock();
			busy_ = false;
			cv_.notify_all();
		}
	}
	std::mutex m_;
	std::condition_variable cv_;
	std::function<void()> job_;
	bool busy_ = false, stop_ = false;
	std::thread t_;
};

// --- S5: no request pin before promote() completes; no apply answers map_owed (CT-R28, I9) -----------------

// Demoted between pins, over and over: each request pin gets its pin at `{p, open}` with the flag clear
// and no unleased map. The waiter has no scheduler, so the wake's own thread resumes it, and the
// final compare-exchange must follow promote().
void pin_after_promote() {
	W w(1, 2, true);
	to_owed(w);
	for (int round = 0; round < 3; ++round) {
		bool ran = false, flag = true;
		S seen = S::free;
		std::uint64_t unleased = 9;
		strata::verdict::outcome v = strata::verdict::invalid;
		actor a;
		a.fn = [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(0));
			seen = st(w, 0), flag = p.index().promote_owed(), unleased = p.index().unleased_maps();
			v = p.index().apply(tx({add(key_of(9 + round), "x")}), {}).result;
			ran = true;
		};
		a.t = a.fn();
		start_inline(a);
		CHECK(st(w, 0) == S::waking && pins(w, 0) == 1 && !ran);
		w.exec.run();
		CHECK(ran && a.t.done());
		CHECK(seen == S::open && !flag && unleased == 0);
		CHECK(v == verdict::accepted);  // not map_owed (I8)
		{
			auto q = w.cat->try_pin(kn(0), pin_kind::chore, false);  // the host's commit, under a pin
			CHECK(q.status == cat_t::try_status::pinned);
			q.p.index().commit();
		}
		CHECK(pins(w, 0) == 0 && w.cat->index_of(0)->unleased_maps() == 0);
		CHECK(w.cat->begin_demote(0));  // demoted between pins
		w.exec.run();
		CHECK(st(w, 0) == S::owed);
	}
	CHECK(w.cat->stats().wakes_posted == 3);
}

// --- S5: a chore pin on `owed` commits without a wake (CT-R33) -------------------------------------------

void chore_pin_commits() {
	W w(1, 2, true);
	write_erase(w);
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed);
	bool done = false;
	actor a;
	a.fn = [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(0));
		CHECK(st(w, 0) == S::owed);
		c.index().commit();  // the host's commit chore, with the bodies still owed
		done = true;
	};
	a.t = a.fn();
	start(w, a);
	CHECK(done && a.t.done());  // no wait: nothing suspended
	CHECK(!w.cat->index_of(0)->uncommitted() && st(w, 0) == S::owed && w.cat->stats().wakes_posted == 0);
	CHECK(w.exec.queued() == 0 && w.timers_pending() == 0);
}

// --- S5 case 1: a stream of chore pins does not starve the wake -----------------------------------------

void stream_of_chore_pins() {
	W w(1, 2, true);
	to_owed(w);
	runner other;
	std::optional<cat_t::pin> held;
	std::optional<strata::lease_scope> hold;
	other.call([&] {
		auto r = w.cat->try_pin(kn(0), pin_kind::chore, false);
		CHECK(r.status == cat_t::try_status::pinned);
		held.emplace(std::move(r.p));
		hold.emplace(held->index().lease());  // a hold that began before the wake
	});
	actor req, later;
	bool req_done = false, later_done = false;
	S req_state = S::free, later_state = S::free;
	std::uint64_t later_pins = 0;
	req.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		req_state = st(w, 0), req_done = true;
	};
	req.t = req.fn();
	start(w, req);
	CHECK(st(w, 0) == S::waking && pins(w, 0) == 2);
	later.fn = [&]() -> task::task<void> {  // a chore pin taken after `waking`, on a thread without the lease
		auto c = co_await w.cat->pin_chore(kn(0));
		later_state = st(w, 0), later_pins = pins(w, 0), later_done = true;
	};
	later.t = later.fn();
	start(w, later);
	CHECK(!later_done && pins(w, 0) == 3);
	w.exec.run();  // the wake meets the hold and reposts through the timer
	CHECK(!req_done && !later_done && st(w, 0) == S::waking);
	CHECK(w.timers_pending() == 1 && w.timer_delays.size() == 1 && w.timer_delays[0] == 1 * ms);
	CHECK(w.cat->stats().wake_reposts == 1 && w.exec.queued() == 0);
	cat_t::try_status second = cat_t::try_status::pinned;
	other.call([&] {  // the stream's next iteration: the hold ends, then it pins again
		hold.reset();
		held.reset();
		second = w.cat->try_pin(kn(0), pin_kind::chore, false).status;
	});
	CHECK(second == cat_t::try_status::would_wait);  // it waits: no hold begins after the wake
	CHECK(pins(w, 0) == 2);
	CHECK(w.fire_next());
	w.exec.run();
	CHECK(req_done && later_done && req_state == S::open && later_state == S::open);
	CHECK(later_pins >= 1 && pins(w, 0) == 0 && w.cat->waiters_of(0) == 0);
	CHECK(w.cat->stats().wake_reposts == 1 && w.timers_pending() == 0);
}

// --- S5 case 2: a chore pin whose thread holds the lease enters `waking` -------------------------------------

void nested_pin_enters() {
	W w(1, 2, true);
	to_owed(w);
	bool nested = false, req_done = false;
	S at_nested = S::free;
	actor holder, req;
	holder.fn = [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(0));  // enters `owed`
		{
			const strata::lease_scope hold{c.index().lease()};  // a commit's call
			co_await task::yield();  // the request pins, then the wake meets the hold
			co_await task::yield();
			auto n = co_await w.cat->pin_chore(kn(0));  // a nested pin inside the call
			at_nested = st(w, 0), nested = true;
		}
	};
	holder.t = holder.fn();
	req.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		req_done = true;
	};
	req.t = req.fn();
	start(w, holder);
	start(w, req);
	w.exec.run();
	CHECK(nested && at_nested == S::waking && holder.t.done());  // the nested pin did not wait for the wake
	CHECK(!req_done && w.timers_pending() == 1 && w.cat->stats().wake_reposts >= 1);
	CHECK(w.fire_next());
	w.exec.run();  // the hold has ended: the wake completes
	CHECK(req_done && st(w, 0) == S::open && pins(w, 0) == 0);
}

// --- S5 case 3: a repost goes through the timer, 1 ms doubling to 64 ms, outside the bound ---------------

void repost_through_timer() {
	W w(1, 2, true);
	to_owed(w);
	std::optional<strata::lease_scope> hold;
	hold.emplace(w.cat->index_of(0)->lease());  // a chore call's hold, from before the wake
	auto q = w.cat->try_pin(kn(0));
	CHECK(q.status == cat_t::try_status::would_wait && st(w, 0) == S::waking);
	const std::uint64_t want[] = {1, 2, 4, 8, 16, 32, 64, 64, 64, 64, 64, 64};
	for (std::uint64_t d : want) {
		w.exec.run();  // returns: the repost waits for the timer
		CHECK(w.timers_pending() == 1 && w.exec.queued() == 0);
		CHECK(w.timer_delays.back() == d * ms);
		CHECK(w.fire_next());
	}
	w.exec.run();
	CHECK(w.timer_delays.size() == 13);
	const auto s = w.cat->stats();
	CHECK(s.wake_reposts == 13 && s.wakes_posted == 1);
	CHECK(w.cat->wake_failures_of(0) == 0 && s.wake_failures == 0 && st(w, 0) == S::waking);  // not a failure
	hold.reset();
	CHECK(w.fire_next());
	w.exec.run();
	CHECK(st(w, 0) == S::open && pins(w, 0) == 0 && w.cat->wake_failures_of(0) == 0);
	CHECK(!w.cat->index_of(0)->promote_owed());
}

// The timer's callback only posts: the wake's device work runs on the executor's thread, never on the
// thread that fires the timer.
void timer_thread_does_no_device_work() {
	W w(1, 2, true);
	to_owed(w);
	std::optional<strata::lease_scope> hold;
	hold.emplace(w.cat->index_of(0)->lease());
	auto q = w.cat->try_pin(kn(0));
	w.exec.run();
	CHECK(w.timers_pending() == 1);
	hold.reset();
	std::thread::id mapped_on{};
	w.kern.map_hook = [&] { mapped_on = std::this_thread::get_id(); };
	std::thread::id fired_on{};
	std::thread t([&] {
		fired_on = std::this_thread::get_id();
		CHECK(w.fire_next());
	});
	t.join();
	CHECK(mapped_on == std::thread::id{} && w.exec.queued() == 1);  // posted, not run
	w.exec.run();
	CHECK(mapped_on == std::this_thread::get_id() && mapped_on != fired_on);
	w.kern.map_hook = nullptr;
	CHECK(st(w, 0) == S::open);
}

// --- the destructor waits for a timer that is still armed ----------------------------------------------------

// The host's timer fires after the catalog's destructor has begun; the destructor waits for it (its callback
// posts into the catalog's group), so the callback never meets a destroyed catalog.
void destructor_waits_for_timers() {
	W w(1, 2, true);
	to_owed(w);
	std::optional<strata::lease_scope> hold;
	hold.emplace(w.cat->index_of(0)->lease());
	auto q = w.cat->try_pin(kn(0));
	w.exec.run();
	CHECK(w.timers_pending() == 1);
	hold.reset();
	std::thread fire([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		CHECK(w.fire_next());
	});
	strata::detail::abort_on_owner_break().store(false);  // the slot is open at the destruction: item 5's leak, not under test here
	w.cat.reset();  // returns once the timer has fired and its chore has run
	strata::detail::abort_on_owner_break().store(true);
	fire.join();
	CHECK(w.timers_pending() == 0);
}

// --- S5 case 4: the wake ends chore_scope before its final compare-exchange ---------------------------------

void scope_ends_first() {
	W w(1, 2, true);
	to_owed(w);
	bool ran = false, outer = false;
	actor a;
	a.fn = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		strata::lease_scope ls{p.index().lease()};
		outer = ls.release();  // true: acquired from free, not nested in the wake's hold
		ran = true;
	};
	a.t = a.fn();
	start_inline(a);  // resumed by the thread that makes the wake's final store
	w.exec.run();
	CHECK(ran && outer);
}

// --- S5 case 5 and S6: too_many_open takes the backoff without counting ----------------------------------------

void overload_backs_off() {
	W w(1, 2, true);
	to_owed(w);
	auto fill = w.budget.reserve(strata::resource::maps, w.budget.hard(strata::resource::maps) - w.budget.used(strata::resource::maps));
	for (unsigned k = 1; k <= 3; ++k) {
		int refused = 0;
		actor a;
		a.fn = [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (const strata::too_many_open&) {
				++refused;  // the wake's own error, not wake_failed
			}
		};
		a.t = a.fn();
		start(w, a);
		w.exec.run();
		CHECK(refused == 1 && a.t.done() && st(w, 0) == S::owed && pins(w, 0) == 0);
		CHECK(w.cat->stats().wakes_posted == k);
		// In its backoff a request pin fails at once with wake_failed and posts nothing.
		const std::uint64_t until = w.exec.now() + backoff(k);
		for (int i = 0; i < 2; ++i) {
			auto r = w.cat->try_pin(kn(0));
			CHECK(r.status == cat_t::try_status::failed);
			const failure f = why(r.error);
			CHECK(f.wake);
			CHECK(w.cat->stats().wakes_posted == k && w.exec.queued() == 0 && pins(w, 0) == 0 && st(w, 0) == S::owed);
		}
		w.exec.scheduler().advance(until - 1 - w.exec.now());
		CHECK(w.cat->try_pin(kn(0)).status == cat_t::try_status::failed);
		w.exec.scheduler().advance(1);
	}
	const auto s = w.cat->stats();
	CHECK(s.wake_overloads == 3 && s.wake_failures == 0 && w.cat->wake_failures_of(0) == 0);
	CHECK(!w.cat->begin_bound_close(0));  // overload never reaches the bound
	// The cap lifts: the pin whose backoff is over posts the wake, which completes and resets the backoff.
	fill = {};
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(st(w, 0) == S::open);
	});
	CHECK(w.cat->stats().wakes_posted == 4 && w.exec.queued() == 0);
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); co_return; });  // no backoff left
}

// --- S6: persistent EMFILE: the backoff, the bound and the close at it ---------------------------------------

void bound_and_backoff() {
	W w(1, 2, true);
	to_owed(w);
	w.kern.fail_map = EMFILE;
	for (unsigned k = 1; k <= 8; ++k) {
		CHECK(!w.cat->begin_bound_close(0));  // below the bound
		failure first;
		actor a;
		a.fn = [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (...) {
				first = why(std::current_exception());
			}
		};
		a.t = a.fn();
		start(w, a);
		w.exec.run();
		CHECK(a.t.done() && !first.wake && first.code == EMFILE);  // the wake's own error
		CHECK(w.cat->wake_failures_of(0) == k && st(w, 0) == S::owed && pins(w, 0) == 0);
		CHECK(w.cat->stats().wakes_posted == k);
		// Chore pins still enter; request pins fail at once and post no chore, until the retry time.
		{
			auto c = w.cat->try_pin(kn(0), pin_kind::chore, false);
			CHECK(c.status == cat_t::try_status::pinned && st(w, 0) == S::owed);
		}
		auto r = w.cat->try_pin(kn(0));
		CHECK(r.status == cat_t::try_status::failed);
		const failure f = why(r.error);
		CHECK(f.wake && f.code == EMFILE);
		CHECK(w.cat->stats().wakes_posted == k && w.exec.queued() == 0);
		if (k < 8) {
			w.exec.scheduler().advance(backoff(k) - 1);
			CHECK(w.cat->try_pin(kn(0)).status == cat_t::try_status::failed);
			w.exec.scheduler().advance(1);
		}
	}
	CHECK(backoff(1) + backoff(2) + backoff(3) + backoff(4) + backoff(5) + backoff(6) + backoff(7) == 1270 * ms);
	// At the bound with nothing uncommitted the slot closes without the host's commit chore, and the next
	// pin's reopen reports the lasting error.
	CHECK(w.cat->begin_bound_close(0) && st(w, 0) == S::closing);
	w.exec.run();
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && w.commits == 0);
	CHECK(w.cat->stats().bound_closes == 1 && w.cat->failing() == 0);
	failure again;
	actor a;
	a.fn = [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->pin_request(kn(0));
		} catch (...) {
			again = why(std::current_exception());
		}
	};
	a.t = a.fn();
	start(w, a);
	w.exec.run();
	CHECK(a.t.done() && !again.wake && again.code == EMFILE && st(w, 0) == S::failed);
	w.kern.fail_map = 0;
	// The slot starts over: its open's first failure backs off 10 ms, not as the 9th.
	w.exec.scheduler().advance(10 * ms - 1);
	CHECK(w.cat->try_pin(kn(0)).status == cat_t::try_status::failed);
	w.exec.scheduler().advance(1);
	CHECK(w.cat->try_pin(kn(0)).status == cat_t::try_status::would_wait);  // the retry reopens
	w.exec.run();
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(p.index().get(key_of(0)) == std::optional<std::string>("v0"));
	});
}

// The bound is counted from failures: the 8th, not the 7th; a success resets the count.
void count_resets() {
	W w(1, 2, true);
	to_owed(w);
	for (unsigned round = 0; round < 2; ++round) {
		w.kern.fail_map = ENOMEM;
		for (unsigned k = 1; k <= 7; ++k) {
			run_task(w, [&]() -> task::task<void> {
				try {
					auto p = co_await w.cat->pin_request(kn(0));
				} catch (const std::system_error&) {
				}
			});
			CHECK(w.cat->wake_failures_of(0) == k && !w.cat->begin_bound_close(0));
			w.exec.scheduler().advance(backoff(k));
		}
		w.kern.fail_map = 0;
		run_task(w, [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(0));
			CHECK(w.cat->wake_failures_of(0) == 0);
		});
		CHECK(w.cat->wake_failures_of(0) == 0);
		CHECK(w.cat->begin_demote(0));
		w.exec.run();
		CHECK(st(w, 0) == S::owed);
	}
}

// --- S6: an uncommitted slot at the bound is committed, then closed, and its data reopens ----------------------

void fail_wakes(W& w, unsigned n) {
	w.kern.fail_map = EMFILE;
	for (unsigned k = 1; k <= n; ++k) {
		run_task(w, [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (const std::system_error&) {
			}
		});
		w.exec.scheduler().advance(backoff(k));
	}
	CHECK(w.cat->wake_failures_of(0) == n);
}
// After a bound close: the key's erase survived the close (committed), seen through a reopen.
void expect_erased(W& w) {
	w.kern.fail_map = 0;
	w.exec.scheduler().advance(20'000 * ms);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(!p.index().get(key_of(0)));
		CHECK(p.index().get(key_of(1)) == std::optional<std::string>("v1"));
	});
}

void bound_commits_then_closes() {
	W w(1, 2, true);
	write_erase(w);
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed && w.cat->index_of(0)->uncommitted());
	fail_wakes(w, 8);
	w.kern.fail_map = 0;
	CHECK(w.cat->begin_bound_close(0));
	w.exec.run();
	CHECK(w.commits == 1 && st(w, 0) == S::free && w.cat->table_size() == 0);
	CHECK(w.cat->stats().bound_commits == 1 && w.cat->stats().bound_closes == 1 && w.cat->failing() == 0);
	expect_erased(w);
}

void bound_commit_fails() {
	W w(1, 2, true);
	write_erase(w);
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	fail_wakes(w, 8);
	w.kern.fail_map = 0;
	w.fail_commit = EIO;
	const std::uint64_t t0 = w.exec.now();
	CHECK(w.cat->begin_bound_close(0));
	w.exec.run();
	// It ends at `owed`, uncommitted, with a 10 s probe time; the failure is counted.
	CHECK(st(w, 0) == S::owed && pins(w, 0) == 0 && w.commits == 1 && w.cat->index_of(0)->uncommitted());
	CHECK(w.cat->failing() == 1 && w.cat->stats().commit_failures == 1 && w.cat->stats().bound_closes == 0);
	CHECK(w.cat->probe_at_of(0) == t0 + 10'000 * ms);
	CHECK(!w.cat->begin_bound_close(0));  // not before the probe
	w.exec.scheduler().advance(10'000 * ms - 1);
	CHECK(!w.cat->begin_bound_close(0));
	w.exec.scheduler().advance(1);
	CHECK(w.cat->begin_bound_close(0));  // the probe: fails again, probe time moves
	w.exec.run();
	CHECK(w.commits == 2 && w.cat->failing() == 1 && st(w, 0) == S::owed);
	CHECK(w.cat->probe_at_of(0) == w.exec.now() + 10'000 * ms);
	w.exec.scheduler().advance(10'000 * ms);
	w.fail_commit = 0;
	CHECK(w.cat->begin_bound_close(0));
	w.exec.run();
	CHECK(w.commits == 3 && st(w, 0) == S::free && w.cat->failing() == 0 && w.cat->stats().bound_closes == 1);
	expect_erased(w);
}

// --- C14 (I3-N1): the wake's gate covers the composed edge `closing` to `waking` -----------------------------

// A failed wake leaves `owed` in its backoff; the sweep closes it; a request pin waits on `closing`; the close is
// refused (a write is uncommitted). The end is `owed` with the waiter failed, not a wake around the backoff.
// After the backoff the same refusal does end at `waking` (the wake runs and the waiter gets its pin).
void refused_close_in_backoff() {
	for (const bool in_backoff : {true, false}) {
		W w(1, 2, true);
		write_erase(w);
		CHECK(w.cat->begin_demote(0));
		w.exec.run();
		CHECK(st(w, 0) == S::owed);
		w.kern.fail_map = ENOMEM;
		run_task(w, [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (const std::system_error&) {
			}
		});
		CHECK(w.cat->stats().wakes_posted == 1 && w.cat->wake_failures_of(0) == 1 && st(w, 0) == S::owed);
		w.kern.fail_map = 0;
		if (!in_backoff) { w.exec.scheduler().advance(backoff(1)); }
		CHECK(w.cat->begin_close(0) && st(w, 0) == S::closing);
		failure got;
		bool granted = false;
		actor a;
		a.fn = [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
				granted = true;
			} catch (...) {
				got = why(std::current_exception());
			}
		};
		a.t = a.fn();
		start(w, a);
		CHECK(pins(w, 0) == 1 && st(w, 0) == S::closing);
		w.exec.run();  // the close chore: refused, the write is uncommitted
		CHECK(a.t.done());
		if (in_backoff) {
			CHECK(!granted && got.wake && st(w, 0) == S::owed && pins(w, 0) == 0);
			CHECK(w.cat->stats().wakes_posted == 1 && w.cat->stats().wake_gated >= 1);
		} else {
			CHECK(granted && st(w, 0) == S::open && pins(w, 0) == 0);
			CHECK(w.cat->stats().wakes_posted == 2);
		}
	}
}

// --- the windows: another actor's step between a transition's load and its compare-exchange ------------------

// A failed wake's final store is a compare-exchange: a chore pin released after the load is not
// brought back.
void unpin_during_wake_fail_end() {
	std::optional<cat_t::pin> held;
	bool armed = false;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (armed && std::strcmp(site, "wake.end") == 0) {
			armed = false;
			held.reset();
		}
	});
	to_owed(w);
	auto c = w.cat->try_pin(kn(0), pin_kind::chore, false);
	held.emplace(std::move(c.p));
	w.kern.fail_map = ENOMEM;
	int failed = 0;
	actor a;
	a.fn = [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->pin_request(kn(0));
		} catch (const std::system_error&) {
			++failed;
		}
	};
	a.t = a.fn();
	armed = true;
	start(w, a);
	CHECK(pins(w, 0) == 2);
	w.exec.run();
	CHECK(!armed && !held && failed == 1);
	CHECK(st(w, 0) == S::owed && pins(w, 0) == 0);
}

// The failure's backoff is in place when the final store is: a request pin in the window after it
// (before the waiters are delivered) fails at once and starts no second wake.
void backoff_visible_at_the_store() {
	bool armed = false;
	W* wp = nullptr;
	cat_t::try_result seen;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (armed && wp && std::strcmp(site, "wake.unlocked") == 0) {
			armed = false;
			seen = wp->cat->try_pin(kn(0));
		}
	});
	wp = &w;
	to_owed(w);
	w.kern.fail_map = ENOMEM;
	actor a;
	int failed = 0;
	a.fn = [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->pin_request(kn(0));
		} catch (const std::system_error&) {
			++failed;
		}
	};
	a.t = a.fn();
	armed = true;
	start(w, a);
	w.exec.run();
	CHECK(!armed && failed == 1);
	CHECK(seen.status == cat_t::try_status::failed && why(seen.error).wake);
	CHECK(w.cat->stats().wakes_posted == 1 && st(w, 0) == S::owed);
	wp = nullptr;
}

// A pin that meets another transition in the window after a failed wake's store joins it; the old
// delivery does not take it (CT-R29).
void failed_wake_delivery_leaves_new_waiters() {
	bool armed = false;
	W* wp = nullptr;
	actor late;
	bool late_ok = false;
	int late_failed = 0;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (armed && wp && std::strcmp(site, "wake.unlocked") == 0) {
			armed = false;
			wp->kern.fail_map = 0;
			CHECK(wp->cat->begin_close(0));  // a sweep pass takes the slot: `{0, owed}` to `{0, closing}`
			late.fn = [&]() -> task::task<void> {
				try {
					auto p = co_await wp->cat->pin_request(kn(0));
					late_ok = true;
				} catch (const std::system_error&) {
					++late_failed;
				}
			};
			late.t = late.fn();
			start(*wp, late);
		}
	});
	wp = &w;
	to_owed(w);
	w.kern.fail_map = ENOMEM;
	actor first;
	int first_failed = 0;
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
	w.exec.run();
	CHECK(first_failed == 1 && late_ok && late_failed == 0 && late.t.done());
	CHECK(st(w, 0) == S::open && pins(w, 0) == 0);
	wp = nullptr;
}

// The bound close is a compare-exchange: a request pin that arrives after its load stops it.
void bound_close_loses_to_a_pin() {
	bool armed = false;
	W* wp = nullptr;
	cat_t::try_result late;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (armed && wp && std::strcmp(site, "sweep.bound") == 0) {
			armed = false;
			late = wp->cat->try_pin(kn(0));  // `owed` to `waking`
		}
	}, 2);
	wp = &w;
	to_owed(w);
	fail_wakes(w, 2);
	w.kern.fail_map = 0;
	armed = true;
	CHECK(!w.cat->begin_bound_close(0));
	CHECK(!armed && late.status == cat_t::try_status::would_wait && st(w, 0) == S::waking);
	CHECK(w.cat->stats().bound_closes == 0);
	w.exec.run();
	CHECK(st(w, 0) == S::open);
	wp = nullptr;
}

// The exchange can follow a reset: a wake completes and a demotion brings the slot back to the same
// word (same generation, same frequency) with a count of zero; the chore finds it below the bound and
// ends at `owed`.
void bound_close_after_a_reset() {
	bool armed = false;
	W* wp = nullptr;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (armed && wp && std::strcmp(site, "sweep.bound") == 0) {
			armed = false;
			wp->kern.fail_map = 0;
			wp->exec.scheduler().advance(20'000 * ms);
			task::run(wp->exec.scheduler(), [&]() -> task::task<void> { auto p = co_await wp->cat->pin_request(kn(0), false); co_return; }());
			CHECK(wp->cat->begin_demote(0));
			wp->exec.run();
			CHECK(st(*wp, 0) == S::owed && wp->cat->wake_failures_of(0) == 0);
		}
	}, 2);
	wp = &w;
	to_owed(w);
	fail_wakes(w, 2);
	armed = true;
	CHECK(w.cat->begin_bound_close(0));  // the exchange succeeds on the identical word
	w.exec.run();
	CHECK(!armed);
	CHECK(st(w, 0) == S::owed && pins(w, 0) == 0 && w.cat->stats().bound_closes == 0);
	CHECK(w.cat->table_size() == 1);
	wp = nullptr;
}

}  // namespace

int main() {
	pin_after_promote();
	chore_pin_commits();
	stream_of_chore_pins();
	nested_pin_enters();
	repost_through_timer();
	timer_thread_does_no_device_work();
	destructor_waits_for_timers();
	scope_ends_first();
	overload_backs_off();
	bound_and_backoff();
	count_resets();
	bound_commits_then_closes();
	bound_commit_fails();
	refused_close_in_backoff();
	unpin_during_wake_fail_end();
	backoff_visible_at_the_store();
	failed_wake_delivery_leaves_new_waiters();
	bound_close_loses_to_a_pin();
	bound_close_after_a_reset();
	if (failures) {
		std::fprintf(stderr, "catalog_wake: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_wake: ok");
	return 0;
}
