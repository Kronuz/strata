// The catalog's waiters when their frames die or their tokens are requested (Xapiand X-POOL step 2
// review CR1, CR2; task's teardown, D8 phase 3, and lock(token), D4): a frame destroyed while it waits for a pin or for shutdown leaves the catalog consistent. Its
// waiter is unlinked and its pin uncounted while no transition has taken it; a pin delivered to it and
// never resumed is released by its destructor; a delivery that finds it gone releases the pin itself.
// A catalog destroyed with waiters listed orphans them: a token requested later or the frame destroyed
// later leaves the gone catalog alone. Window tests place the destruction at each of those points on
// inline_executor; a seeded property test destroys frames that hold or await pins at random points
// against a model of the counts, and shutdown still completes; a threaded test destroys waiting frames while a WorkPool's opens deliver to them.
// Run under TSan and ASan+UBSan. The mutants each case kills are in docs/status/cold-tier-step2.md.
#include "catalog_world.hh"
#include "strata/core/work_pool_executor.hh"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <set>
#include <thread>

namespace {

using strata::open_intent;
using strata::pin_kind;
using strata::slot_state;
using S = slot_state;
using W = world<inline_executor>;

std::string kn(int i) { return "k" + std::to_string(i); }

// The scheduler the test's frames suspend on, with task's teardown phases: close() drops what is posted
// (phase 2), and a post of a frame the test destroyed while open is a bug, counted in late().
class test_sched {
public:
	void post(std::coroutine_handle<> h) {
		std::lock_guard l(mu_);
		if (closed_) {
			++dropped_;
			return;
		}
		if (dead_.count(h.address())) {
			++late_;
			return;
		}
		q_.push_back(h);
	}
	std::uint64_t now() const noexcept { return 0; }
	// Resumes what is queued until nothing is; how many ran.
	std::size_t run() {
		const task::running_on on(task::scheduler_ref::of(*this));
		std::size_t n = 0;
		for (;;) {
			std::coroutine_handle<> h;
			{
				std::lock_guard l(mu_);
				if (q_.empty()) { return n; }
				h = q_.front();
				q_.pop_front();
			}
			h.resume();
			++n;
		}
	}
	void born(std::coroutine_handle<> h) {
		std::lock_guard l(mu_);
		dead_.erase(h.address());
	}
	// After the frame is destroyed: its queued resume goes, and a later post of it is late.
	void forget(std::coroutine_handle<> h) {
		std::lock_guard l(mu_);
		std::erase(q_, h);
		dead_.insert(h.address());
	}
	void close() {
		std::lock_guard l(mu_);
		closed_ = true;
	}
	// A new round once every delivery has ended: nothing queued, no frame dead.
	void reopen() {
		std::lock_guard l(mu_);
		closed_ = false;
		q_.clear();
		dead_.clear();
	}
	std::size_t queued() {
		std::lock_guard l(mu_);
		return q_.size();
	}
	std::size_t late() {
		std::lock_guard l(mu_);
		return late_;
	}

private:
	std::mutex mu_;
	std::deque<std::coroutine_handle<>> q_;
	std::set<void*> dead_;
	bool closed_ = false;
	std::size_t late_ = 0, dropped_ = 0;
};

// What one frame did with its pin. `released`: the test took the pin out of the frame.
struct actor_state {
	enum kind_t { waiting, holding, done, failed, cancelled, released } st = waiting;
	cat_t::pin* held = nullptr;  // the frame's pin while holding
	std::string key;
	task::cancel_source src;  // the frame's token, for the cases that cancel
	unsigned resumed = 0;     // the order its pin arrived in, among all frames
};
unsigned resumes = 0;

// Pins `key`; with `hold`, keeps the pin and suspends until the frame is destroyed.
task::task<void> pinner(cat_t* c, std::string key, pin_kind kind, open_intent intent, bool hold, actor_state* out, task::cancel_token stop) {
	try {
		auto p = kind == pin_kind::request ? co_await c->pin_request(key, true, intent, stop) : co_await c->pin_chore(key, false, intent, stop);
		out->resumed = ++resumes;
		if (!p || !hold) {
			out->st = actor_state::done;
			co_return;
		}
		out->st = actor_state::holding;
		out->held = &p;
		co_await std::suspend_always{};
	} catch (const task::cancelled&) {
		out->st = actor_state::cancelled;
	} catch (...) {
		out->st = actor_state::failed;
	}
}

struct actor {
	std::unique_ptr<actor_state> s = std::make_unique<actor_state>();
	task::task<void> t;
	bool live() const { return t.valid(); }
};

// Starts the frame on the test thread, suspended on `ts` when it waits.
// `stop`: the frame's own source's token unless given.
actor spawn(cat_t* c, test_sched& ts, std::string key, bool hold = true, pin_kind kind = pin_kind::request, open_intent intent = open_intent::open, std::optional<task::cancel_token> stop = {}) {
	actor a;
	a.s->key = key;
	a.t = pinner(c, std::move(key), kind, intent, hold, a.s.get(), stop ? *stop : a.s->src.token());
	ts.born(a.t.handle());
	const task::running_on on(task::scheduler_ref::of(ts));
	a.t.handle().resume();
	return a;
}

// task's phase 3 for one root: the frame goes with whatever it awaited or held.
void destroy(test_sched& ts, actor& a) {
	if (!a.live()) { return; }
	const std::coroutine_handle<> h = a.t.handle();
	a.s->held = nullptr;
	a.t = task::task<void>{};
	ts.forget(h);
}

template <class WW>
std::uint64_t pins(WW& w, std::uint32_t i) { return WW::cat_t::pins_of(w.cat->word_of(i)); }
template <class WW>
S st(WW& w, std::uint32_t i) { return WW::cat_t::state_of(w.cat->word_of(i)); }
template <class WW>
std::uint32_t sl(WW& w, const std::string& key) {
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
		if (w.cat->key_at(i) == key) { return i; }
	}
	return ~std::uint32_t(0);
}

// A hook the test sets after the world is built.
struct hooked {
	std::function<void(const char*, bool)> fn;
	std::function<void(const char*, bool)> hook() {
		return [this](const char* site, bool locked) {
			if (fn) { fn(site, locked); }
		};
	}
};

// --- the windows ------------------------------------------------------------------------------------

// Linked, untaken: the destructor unlinks the waiter and uncounts its pin at once; the open ends with
// no pin and resumes nobody.
void destroyed_while_linked() {
	W w(2, 1, false);
	test_sched ts;
	actor a = spawn(w.cat.get(), ts, kn(0));
	const std::uint32_t i = sl(w, kn(0));
	CHECK(st(w, i) == S::opening && pins(w, i) == 1 && w.cat->waiters_of(i) == 1);
	destroy(ts, a);
	CHECK(pins(w, i) == 0 && w.cat->waiters_of(i) == 0);
	w.settle();
	CHECK(st(w, i) == S::open && pins(w, i) == 0);
	CHECK(ts.queued() == 0 && ts.late() == 0);
	const auto s = w.cat->stats();
	CHECK(s.unlinked == 1 && s.gone_pins == 0);
	w.finish();
	CHECK(w.cat->shut_down());
}

// Taken, not yet delivered (the open's "open.unlocked"): the delivery finds the frame gone, resumes
// nothing and releases the pin it carried.
void destroyed_in_delivery(bool fail) {
	hooked hk;
	W w(2, 1, false, true, hk.hook());
	test_sched ts;
	if (fail) {
		w.fail_resolve = EIO;
		w.fail_left = 1;
	}
	actor a = spawn(w.cat.get(), ts, kn(0));
	const std::uint32_t i = sl(w, kn(0));
	bool fired = false;
	hk.fn = [&](const char* site, bool) {
		if (!fired && std::strcmp(site, "open.unlocked") == 0) {
			fired = true;
			destroy(ts, a);
		}
	};
	w.settle();
	hk.fn = nullptr;
	CHECK(fired);
	CHECK(st(w, i) == (fail ? S::failed : S::open) && pins(w, i) == 0);
	CHECK(ts.queued() == 0 && ts.late() == 0);
	const auto s = w.cat->stats();
	CHECK(s.unlinked == 0 && s.gone_pins == (fail ? 0u : 1u));
	w.finish();
	CHECK(w.cat->shut_down());
}

// Delivered and posted, never resumed (task's phase 2 drops the post): the destructor releases the pin.
void destroyed_after_delivery(bool fail) {
	W w(2, 1, false);
	test_sched ts;
	if (fail) {
		w.fail_resolve = EIO;
		w.fail_left = 1;
	}
	actor a = spawn(w.cat.get(), ts, kn(0));
	const std::uint32_t i = sl(w, kn(0));
	w.settle();
	CHECK(ts.queued() == 1 && a.s->st == actor_state::waiting);
	CHECK(pins(w, i) == (fail ? 0u : 1u));
	destroy(ts, a);
	CHECK(st(w, i) == (fail ? S::failed : S::open) && pins(w, i) == 0);
	const auto s = w.cat->stats();
	CHECK(s.unlinked == 0 && s.gone_pins == (fail ? 0u : 1u));
	w.finish();
	CHECK(w.cat->shut_down());
}

// The middle of three waiters goes: the list stays whole and the other two get their pins, in order.
void middle_waiter() {
	W w(2, 1, false);
	test_sched ts;
	actor a = spawn(w.cat.get(), ts, kn(0)), b = spawn(w.cat.get(), ts, kn(0)), c = spawn(w.cat.get(), ts, kn(0));
	const std::uint32_t i = sl(w, kn(0));
	CHECK(pins(w, i) == 3 && w.cat->waiters_of(i) == 3);
	destroy(ts, b);
	CHECK(pins(w, i) == 2 && w.cat->waiters_of(i) == 2);
	w.settle();
	ts.run();
	CHECK(a.s->st == actor_state::holding && c.s->st == actor_state::holding && pins(w, i) == 2);
	CHECK(a.s->resumed < c.s->resumed);  // the list is FIFO
	destroy(ts, c);
	CHECK(pins(w, i) == 1);
	destroy(ts, a);
	CHECK(pins(w, i) == 0 && ts.late() == 0);
}

// A waiter on a replace's drain (`closing`) is not a holder: unlinking it leaves the drain's count, and
// the last holder's unpin still posts the close, which frees the slot (no pin left to reopen for).
void drain_waiter() {
	W w(2, 1, false);
	test_sched ts;
	actor h1 = spawn(w.cat.get(), ts, kn(0)), h2 = spawn(w.cat.get(), ts, kn(0));
	w.settle();
	ts.run();
	CHECK(h1.s->st == actor_state::holding && h2.s->st == actor_state::holding);
	const std::uint32_t i = sl(w, kn(0));
	w.cat->reopen(std::move(*h2.s->held));
	CHECK(st(w, i) == S::closing && w.cat->drain_of(i) == 1 && pins(w, i) == 1);
	actor x = spawn(w.cat.get(), ts, kn(0));
	CHECK(pins(w, i) == 2 && w.cat->waiters_of(i) == 1);
	destroy(ts, x);
	CHECK(pins(w, i) == 1 && w.cat->waiters_of(i) == 0 && w.cat->drain_of(i) == 1);
	destroy(ts, h1);
	w.settle();
	CHECK(sl(w, kn(0)) == ~std::uint32_t(0));  // closed and freed
	destroy(ts, h2);
	CHECK(ts.late() == 0);
	w.finish();
	CHECK(w.cat->shut_down());
}

// A frame awaiting shutdown is destroyed: shutdown still ends, and resumes nobody.
void shutdown_waiter_destroyed() {
	W w(2, 1, false);
	test_sched ts;
	actor h = spawn(w.cat.get(), ts, kn(0));
	w.settle();
	ts.run();
	CHECK(h.s->st == actor_state::holding);
	bool ended = false;
	std::function<task::task<void>()> body = [&]() -> task::task<void> {
		(void)co_await w.cat->shutdown();
		ended = true;
	};
	task::task<void> sd = body();
	ts.born(sd.handle());
	{
		const task::running_on on(task::scheduler_ref::of(ts));
		sd.handle().resume();
	}
	w.cat->stop_sweep();
	w.settle();
	CHECK(!ended && !w.cat->shut_down());
	const std::coroutine_handle<> sh = sd.handle();
	sd = task::task<void>{};
	ts.forget(sh);
	destroy(ts, h);  // the last pin: the shutdown close follows
	w.settle();
	CHECK(w.cat->shut_down() && !ended && ts.late() == 0 && ts.queued() == 0);
}

// --- cancellation (CR2): the caller's token ends a waiting pin ---------------------------------------

// Requested while it waits: unlinked and uncounted at once, the frame resumes with task::cancelled, and
// the open ends with no pin.
void cancel_while_waiting() {
	W w(2, 1, false);
	test_sched ts;
	task::cancel_source src;
	actor a = spawn(w.cat.get(), ts, kn(0), true, pin_kind::request, open_intent::open, src.token());
	const std::uint32_t i = sl(w, kn(0));
	CHECK(pins(w, i) == 1 && w.cat->waiters_of(i) == 1);
	src.request();
	CHECK(pins(w, i) == 0 && w.cat->waiters_of(i) == 0 && ts.queued() == 1);
	ts.run();
	CHECK(a.s->st == actor_state::cancelled);
	w.settle();
	CHECK(st(w, i) == S::open && pins(w, i) == 0 && ts.queued() == 0);
	CHECK(w.cat->stats().cancels == 1);
	destroy(ts, a);
}

// A requested token never waits: a cold key fails at once (the open still starts, holding no pin, as
// try_pin's); an open key is pinned whatever the token.
void cancel_before() {
	W w(2, 1, false);
	test_sched ts;
	task::cancel_source src;
	src.request();
	actor a = spawn(w.cat.get(), ts, kn(0), true, pin_kind::request, open_intent::open, src.token());
	CHECK(a.s->st == actor_state::cancelled);
	const std::uint32_t i = sl(w, kn(0));
	CHECK(st(w, i) == S::opening && pins(w, i) == 0 && w.cat->waiters_of(i) == 0);
	w.settle();
	CHECK(st(w, i) == S::open);
	actor b = spawn(w.cat.get(), ts, kn(0), true, pin_kind::request, open_intent::open, src.token());
	CHECK(b.s->st == actor_state::holding && pins(w, i) == 1);
	CHECK(w.cat->stats().cancels == 0);
	destroy(ts, a);
	destroy(ts, b);
	CHECK(pins(w, i) == 0);
}

// Requested after the pin joined and before its callback is registered (from inside the miss): the
// registration runs the cancel, and the frame continues in place with task::cancelled.
void cancel_between_join_and_registration() {
	hooked hk;
	W w(2, 1, false, true, hk.hook());
	test_sched ts;
	task::cancel_source src;
	hk.fn = [&](const char* site, bool) {
		if (std::strcmp(site, "pin.miss") == 0) { src.request(); }
	};
	actor a = spawn(w.cat.get(), ts, kn(0), true, pin_kind::request, open_intent::open, src.token());
	hk.fn = nullptr;
	CHECK(a.s->st == actor_state::cancelled && ts.queued() == 0);
	const std::uint32_t i = sl(w, kn(0));
	CHECK(pins(w, i) == 0 && w.cat->waiters_of(i) == 0 && w.cat->stats().cancels == 1);
	w.settle();
	CHECK(st(w, i) == S::open && pins(w, i) == 0);
	destroy(ts, a);
}

// Requested after the open took the list ("open.unlocked"): the cancel loses, and the pin is delivered.
void cancel_loses_race() {
	hooked hk;
	W w(2, 1, false, true, hk.hook());
	test_sched ts;
	task::cancel_source src;
	actor a = spawn(w.cat.get(), ts, kn(0), true, pin_kind::request, open_intent::open, src.token());
	hk.fn = [&](const char* site, bool) {
		if (std::strcmp(site, "open.unlocked") == 0) { src.request(); }
	};
	w.settle();
	hk.fn = nullptr;
	ts.run();
	const std::uint32_t i = sl(w, kn(0));
	CHECK(a.s->st == actor_state::holding && pins(w, i) == 1 && w.cat->stats().cancels == 0);
	destroy(ts, a);
	CHECK(pins(w, i) == 0);
}

// A token requested on another thread while the frame is destroyed: the destructor waits for the running
// callback before it lets the waiter go, so the callback never meets freed memory (ASan) and the pin is
// uncounted once.
void cancel_during_destroy() {
	hooked hk;
	W w(2, 1, false, true, hk.hook());
	test_sched ts;
	actor a = spawn(w.cat.get(), ts, kn(0));
	const std::uint32_t i = sl(w, kn(0));
	std::atomic<bool> inside{false};
	hk.fn = [&](const char* site, bool) {
		if (std::strcmp(site, "cancel") == 0) {
			inside = true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the destructor runs meanwhile
		}
	};
	task::cancel_source src = a.s->src;
	std::thread t([src]() mutable { (void)src.request(); });
	while (!inside) { std::this_thread::yield(); }
	destroy(ts, a);
	t.join();
	hk.fn = nullptr;
	CHECK(pins(w, i) == 0 && w.cat->waiters_of(i) == 0 && w.cat->stats().cancels == 1 && ts.late() == 0);
	w.settle();
	CHECK(st(w, i) == S::open && pins(w, i) == 0);
}

// The reviewer's self_wait: one task holds two pins of a key and hands one back fenced; its next pin of
// the key waits for the replace, which waits for the task's other pin. The token ends the wait; the task
// then releases its pin and the replace goes on.
task::task<void> self_waiter(cat_t* c, task::cancel_token stop, actor_state* out) {
	auto a = co_await c->pin_request(kn(0));
	auto b = co_await c->pin_request(kn(0));
	c->reopen(std::move(a));
	out->st = actor_state::holding;
	try {
		auto again = co_await c->pin_request(kn(0), true, open_intent::open, std::move(stop));
		out->st = actor_state::done;
	} catch (const task::cancelled&) {
		out->st = actor_state::cancelled;
	}
}
void self_wait_cancelled() {
	W w(2, 1, false);
	test_sched ts;
	w.settle();
	task::cancel_source src;
	actor a;
	a.t = self_waiter(w.cat.get(), src.token(), a.s.get());
	ts.born(a.t.handle());
	{
		const task::running_on on(task::scheduler_ref::of(ts));
		a.t.handle().resume();
	}
	for (int r = 0; r < 4; ++r) {
		w.settle();
		ts.run();
	}
	const std::uint32_t i = sl(w, kn(0));
	CHECK(a.s->st == actor_state::holding && st(w, i) == S::closing && pins(w, i) == 2 && w.cat->drain_of(i) == 1 && w.cat->waiters_of(i) == 1);
	src.request();
	ts.run();
	CHECK(a.s->st == actor_state::cancelled && a.t.done());
	w.settle();
	CHECK(sl(w, kn(0)) == ~std::uint32_t(0));  // the replace closed it: nobody waits to reopen
	destroy(ts, a);
}

// --- waiters a destroyed catalog still lists (an unfinished shutdown given up on) -------------------------

// A view kept past its pin holds the slot `closing` (S7.5) with a request waiting; the host gives up and
// destroys the catalog. Afterwards the frame's token is requested (`cancel`: nothing happens, the frame
// stays suspended) and the frame is destroyed: neither touches the gone catalog (review PF1).
using view_t = decltype(std::declval<cat_t::pin&>().reader().acquire());
task::task<void> viewer(cat_t* c, std::optional<view_t>* g) {
	auto p = co_await c->pin_request(kn(0), false);
	g->emplace(p.reader().acquire());
}
void orphaned_waiter(bool cancel) {
	strata::detail::abort_on_owner_break().store(false);
	auto w = std::make_unique<W>(1, 2, false, false);
	test_sched ts;
	auto* g = new std::optional<view_t>;  // kept with the leaked slot, as the world is
	strata::detail::keep_forever(std::shared_ptr<const void>(g, [](const void*) {}));
	task::spawn(task::scheduler_ref::of(w->exec.scheduler()), viewer(w->cat.get(), g));
	w->exec.run();
	CHECK(w->cat->begin_close(0));
	w->exec.run();
	actor a = spawn(w->cat.get(), ts, kn(0));
	w->exec.run();
	CHECK(st(*w, 0) == S::closing && w->cat->waiters_of(0) == 1 && a.s->st == actor_state::waiting);
	w->cat.reset();
	if (cancel) {
		(void)a.s->src.request();
		CHECK(a.s->st == actor_state::waiting && ts.queued() == 0);
	}
	destroy(ts, a);
	CHECK(ts.late() == 0);
	strata::detail::keep_forever(std::shared_ptr<const void>(w.release(), [](const void*) {}));  // its directories hold the slot's IO
	strata::detail::abort_on_owner_break().store(true);
}

// The same for a frame awaiting shutdown: the view keeps shutdown from finishing, the host's timer fails so no
// retry is armed, and the catalog goes with the shutdown waiter listed.
void orphaned_shutdown_waiter() {
	strata::detail::abort_on_owner_break().store(false);
	auto w = std::make_unique<W>(1, 2, false, false);
	test_sched ts;
	auto* g = new std::optional<view_t>;
	strata::detail::keep_forever(std::shared_ptr<const void>(g, [](const void*) {}));
	task::spawn(task::scheduler_ref::of(w->exec.scheduler()), viewer(w->cat.get(), g));
	w->exec.run();
	w->fail_timer.store(EAGAIN);
	bool ended = false;
	auto body = [&]() -> task::task<void> {
		(void)co_await w->cat->shutdown();
		ended = true;
	};
	task::task<void> sd = body();
	ts.born(sd.handle());
	{
		const task::running_on on(task::scheduler_ref::of(ts));
		sd.handle().resume();
	}
	w->exec.run();
	CHECK(!ended && !w->cat->shut_down() && st(*w, 0) == S::closing && w->timers_pending() == 0);
	w->cat.reset();
	const std::coroutine_handle<> sh = sd.handle();
	sd = task::task<void>{};
	ts.forget(sh);
	CHECK(!ended && ts.late() == 0 && ts.queued() == 0);
	strata::detail::keep_forever(std::shared_ptr<const void>(w.release(), [](const void*) {}));
	strata::detail::abort_on_owner_break().store(true);
}

// --- the property: frames that hold or await pins die anywhere, and the counts stay the model's ----------

// The pins the model expects on `key`: its frames that hold one or wait for one.
std::uint64_t expected(const std::vector<actor>& as, const std::string& key) {
	std::uint64_t n = 0;
	for (const auto& a : as) {
		if (a.live() && a.s->key == key && (a.s->st == actor_state::waiting || a.s->st == actor_state::holding)) { ++n; }
	}
	return n;
}

void quiesce(W& w, test_sched& ts) {
	for (int r = 0; r < 1000; ++r) {
		w.settle();
		if (ts.run() == 0 && w.exec.queued() == 0) { return; }
	}
}

int property(unsigned seed, int steps) {
	constexpr int keys = 4;
	hooked hk;
	W w(keys, 1, seed & 1, true, hk.hook());
	test_sched ts;
	std::mt19937 rng(seed);
	std::vector<actor> as;
	int windows = 0, bad = 0;
	const auto pick_live = [&](bool waiting_only) -> actor* {
		std::vector<actor*> c;
		for (auto& a : as) {
			if (a.live() && (!waiting_only || a.s->st == actor_state::waiting)) { c.push_back(&a); }
		}
		return c.empty() ? nullptr : c[rng() % c.size()];
	};
	bool armed = false;
	hk.fn = [&](const char* site, bool locked) {
		if (!armed || locked || (std::strcmp(site, "open.unlocked") != 0 && std::strcmp(site, "wake.unlocked") != 0)) { return; }
		armed = false;
		if (actor* a = pick_live(true)) {
			++windows;
			destroy(ts, *a);
		}
	};
	const auto check_model = [&] {
		quiesce(w, ts);
		for (int k = 0; k < keys; ++k) {
			const std::uint32_t i = sl(w, kn(k));
			const std::uint64_t have = i == ~std::uint32_t(0) ? 0 : pins(w, i), want = expected(as, kn(k));
			if (have != want) {
				std::fprintf(stderr, "seed %u: %s has %llu pins, the model %llu\n", seed, kn(k).c_str(), (unsigned long long)have, (unsigned long long)want);
				++bad;
			}
		}
	};
	for (int s = 0; s < steps; ++s) {
		switch (rng() % 10) {
		case 0:
		case 1:
		case 2: {
			const pin_kind kind = rng() % 4 == 0 ? pin_kind::chore : pin_kind::request;
			const open_intent intent = kind == pin_kind::chore && rng() % 3 == 0 ? open_intent::resident : rng() % 5 == 0 ? open_intent::create : open_intent::open;
			if (as.size() < 16) { as.push_back(spawn(w.cat.get(), ts, kn(int(rng() % keys)), rng() % 3 != 0, kind, intent)); }
			break;
		}
		case 3:
			if (actor* a = pick_live(false)) { destroy(ts, *a); }
			break;
		case 4: ts.run(); break;
		case 5: w.settle(); break;
		case 6: {
			const std::uint32_t i = sl(w, kn(int(rng() % keys)));
			if (i != ~std::uint32_t(0)) { (void)(rng() & 1 ? w.cat->begin_demote(i) : w.cat->begin_close(i)); }
			break;
		}
		case 7:
			for (auto& a : as) {
				if (a.live() && a.s->st == actor_state::holding && a.s->held && rng() % 2 == 0) {
					w.cat->reopen(std::move(*a.s->held));  // the frame keeps an empty pin
					a.s->st = actor_state::released;
					break;
				}
			}
			break;
		case 8:
			if (rng() & 1) {
				armed = true;
			} else if (actor* a = pick_live(true)) {
				a->s->src.request();  // a waiting frame's token: it resumes cancelled, its pin uncounted
			}
			break;
		case 9:
			if (rng() % 4 == 0) {
				w.fail_resolve = EIO;
				w.fail_left = 1;
			} else {
				check_model();
			}
			break;
		}
		std::erase_if(as, [](const actor& a) { return !a.live() || a.s->st == actor_state::done || a.s->st == actor_state::failed; });
	}
	check_model();
	// Teardown: every frame dies in a random order, some inside a delivery's window.
	armed = true;
	std::shuffle(as.begin(), as.end(), rng);
	for (auto& a : as) {
		destroy(ts, a);
		if (rng() % 3 == 0) { w.settle(); }
	}
	quiesce(w, ts);
	for (int k = 0; k < keys; ++k) {
		const std::uint32_t i = sl(w, kn(k));
		if (i != ~std::uint32_t(0) && pins(w, i) != 0) {
			std::fprintf(stderr, "seed %u: %s keeps %llu pins after every frame died\n", seed, kn(k).c_str(), (unsigned long long)pins(w, i));
			++bad;
		}
	}
	w.finish();
	if (!w.cat->shut_down()) {
		std::fprintf(stderr, "seed %u: shutdown did not end\n", seed);
		++bad;
	}
	if (ts.late() != 0) {
		std::fprintf(stderr, "seed %u: %zu resumes posted to destroyed frames\n", seed, ts.late());
		++bad;
	}
	failures += bad;
	return windows;
}

// --- threads: the opens deliver on a WorkPool while the test thread destroys the frames ------------

class pool_exec final : public strata::executor {
public:
	pool_exec() : pool_(opts()), ex_(pool_, 2) {}
	std::unique_ptr<group> open() override { return ex_.open(); }
	std::uint64_t now() override { return ex_.now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override { return ex_.sleep_until(t, stop); }

private:
	static WorkPool::Options opts() {
		WorkPool::Options o;
		o.threads = 3;
		o.name_format = "ctteard{:02}";
		return o;
	}
	WorkPool pool_;
	strata::work_pool_executor ex_;
};
using PW = world<pool_exec>;

void threaded(unsigned rounds) {
	constexpr int keys = 6;
	PW w(keys, 1, false);
	test_sched ts;
	std::mt19937 rng(7);
	std::uint64_t destroyed_waiting = 0;
	for (unsigned r = 0; r < rounds; ++r) {
		std::vector<actor> as;
		const int m = 2 + int(rng() % 6);
		for (int j = 0; j < m; ++j) { as.push_back(spawn(w.cat.get(), ts, kn(int(rng() % keys)), rng() % 2 == 0, rng() % 4 == 0 ? pin_kind::chore : pin_kind::request)); }
		// A second thread requests some tokens while the pool delivers and the test thread destroys frames.
		std::vector<task::cancel_source> srcs;
		for (auto& a : as) {
			if (rng() % 3 == 0) { srcs.push_back(a.s->src); }
		}
		std::thread canceller([srcs, delay = rng() % 600] {
			std::this_thread::sleep_for(std::chrono::microseconds(delay));
			for (auto src : srcs) { (void)src.request(); }
		});
		const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(rng() % 400);
		while (std::chrono::steady_clock::now() < end) { ts.run(); }
		ts.close();  // phase 2; the frames die while the pool may be delivering to them
		std::shuffle(as.begin(), as.end(), rng);
		for (auto& a : as) {
			destroyed_waiting += a.live() && a.s->st == actor_state::waiting;
			destroy(ts, a);
		}
		canceller.join();
		w.cat->wait_chores();
		ts.reopen();
		for (int k = 0; k < keys; ++k) {
			const std::uint32_t i = sl(w, kn(k));
			if (i != ~std::uint32_t(0) && pins(w, i) != 0) {
				std::fprintf(stderr, "round %u: %s keeps %llu pins\n", r, kn(k).c_str(), (unsigned long long)pins(w, i));
				++failures;
			}
		}
		w.drain();  // the next round's pins meet closed indexes and wait for their opens
	}
	// Shutdown ends (the pool's `join` is true): no destroyed frame kept a pin.
	bool ended = false;
	std::function<task::task<void>()> body = [&]() -> task::task<void> {
		(void)co_await w.cat->shutdown();
		ended = true;
	};
	task::task<void> sd = body();
	{
		const task::running_on on(task::scheduler_ref::of(ts));
		sd.handle().resume();
	}
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!ended && std::chrono::steady_clock::now() < limit) {
		ts.run();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	CHECK(ended && w.cat->shut_down());
	const auto s = w.cat->stats();
	std::printf("catalog_teardown threads: %u rounds, %llu frames destroyed waiting, %llu unlinked, %llu pins of gone frames released, %llu cancelled\n", rounds,
		(unsigned long long)destroyed_waiting, (unsigned long long)s.unlinked, (unsigned long long)s.gone_pins, (unsigned long long)s.cancels);
}

// --- the last unpin under shutdown: the host destroys the catalog once the close it posts has ended ----------

// The thread that sets `stall_post` returns from a post only after the catalog is gone, as an unpinner preempted
// right after it posts the shutdown close: the close runs, shutdown resumes the host, and the host destroys the
// catalog. `gone` is relaxed, so nothing orders the destruction before what the unpinner does next (TSan).
thread_local bool stall_post = false;
std::atomic<bool> gone{false};

class stalling_exec final : public strata::executor {
public:
	std::unique_ptr<group> open() override { return std::make_unique<stalling_group>(inner_.open()); }
	std::uint64_t now() override { return inner_.now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override { return inner_.sleep_until(t, stop); }

private:
	struct stalling_group final : group {
		explicit stalling_group(std::unique_ptr<group> g) : g_(std::move(g)) {}
		void post(work w) override {
			g_->post(std::move(w));
			if (!stall_post) { return; }
			while (!gone.load(std::memory_order_relaxed)) { std::this_thread::yield(); }  // `this` may be gone too
		}
		void cancel() noexcept override { g_->cancel(); }
		void wait() override { g_->wait(); }
		std::unique_ptr<group> g_;
	};
	pool_exec inner_;
};

// The last pin of an open index is released on its own thread after shutdown began: the unpin's
// compare-exchange begins the close, and it touches the catalog no more once the post returns (ASan: a use
// after free; TSan: a race with the destruction).
void last_unpin_under_shutdown() {
	using SW = world<stalling_exec>;
	auto w = std::make_unique<SW>(1, 1, false);
	cat_t::try_result r = w->cat->try_pin(kn(0));
	for (int i = 0; i < 1000 && r.status == cat_t::try_status::would_wait; ++i) {
		w->cat->wait_chores();
		r = w->cat->try_pin(kn(0));
	}
	CHECK(r.status == cat_t::try_status::pinned);
	const std::uint32_t i = sl(*w, kn(0));
	test_sched ts;
	bool ended = false;
	std::function<task::task<void>()> body = [&]() -> task::task<void> {
		(void)co_await w->cat->shutdown();
		ended = true;
	};
	task::task<void> sd = body();
	{
		const task::running_on on(task::scheduler_ref::of(ts));
		sd.handle().resume();
	}
	w->cat->wait_chores();
	ts.run();
	CHECK(!ended && st(*w, i) == S::open && pins(*w, i) == 1);  // the pin holds shutdown back
	gone = false;
	std::thread unpinner([p = std::move(r.p)]() mutable {
		stall_post = true;
		p.release();
	});
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!ended && std::chrono::steady_clock::now() < limit) {
		ts.run();
		std::this_thread::yield();
	}
	CHECK(ended && w->cat->shut_down());
	w->cat.reset();  // the host's ~DatabasePool
	gone.store(true, std::memory_order_relaxed);
	unpinner.join();
	std::puts("catalog_teardown last unpin under shutdown: ok");
}

// A synchronous miss counted before shutdown began (review F1): its open, shutdown's close and the host's
// destruction all finish while its thread is still in the open's post, so the post is its last touch.
void miss_under_shutdown() {
	using SW = world<stalling_exec>;
	test_sched ts;
	bool ended = false;
	std::atomic<bool> armed{false};
	std::unique_ptr<SW> w;
	std::optional<task::task<void>> sd;
	std::function<task::task<void>()> body = [&]() -> task::task<void> {
		(void)co_await w->cat->shutdown();
		ended = true;
	};
	w = std::make_unique<SW>(1, 1, false, true, [&](const char* site, bool) {
		if (std::strcmp(site, "pin.miss") != 0 || !armed.exchange(false)) { return; }
		stall_post = true;  // this thread: the miss's post of the open stalls until the catalog is gone
		const task::running_on on(task::scheduler_ref::of(ts));
		sd->handle().resume();  // shutdown begins after the miss passed its check, before its post
	});
	sd.emplace(body());
	gone = false;
	armed = true;
	std::thread misser([&] {
		const cat_t::try_result r = w->cat->try_pin(kn(0));
		CHECK(r.status == cat_t::try_status::would_wait);
	});
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!ended && std::chrono::steady_clock::now() < limit) {
		ts.run();
		std::this_thread::yield();
	}
	CHECK(ended && w->cat->shut_down());
	w->cat.reset();  // the host's ~DatabasePool
	gone.store(true, std::memory_order_relaxed);
	misser.join();
	std::puts("catalog_teardown miss under shutdown: ok");
}

// Pins k0 on an inline world and returns the pin; the shutdown task `sd` is made, not started.
cat_t::pin pinned(W& w) {
	cat_t::try_result r = w.cat->try_pin(kn(0));
	w.settle();
	r = w.cat->try_pin(kn(0));
	CHECK(r.status == cat_t::try_status::pinned);
	return std::move(r.p);
}
template <class Stats>
std::uint64_t edge(const Stats& s, S from, S to) { return s.edges[unsigned(from)][unsigned(to)]; }

// Review F2(a): shutdown marks the slot between the last unpin's load and its exchange. The exchange fails on
// the mark, and its retry begins the close itself: one open-to-closing, then free, and shutdown ends.
void mark_in_unpin_window() {
	hooked hk;
	W w(1, 1, false, true, hk.hook());
	test_sched ts;
	cat_t::pin p = pinned(w);
	const std::uint32_t i = sl(w, kn(0));
	bool ended = false, fired = false;
	std::function<task::task<void>()> body = [&]() -> task::task<void> {
		(void)co_await w.cat->shutdown();
		ended = true;
	};
	task::task<void> sd = body();
	hk.fn = [&](const char* site, bool) {
		if (fired || std::strcmp(site, "unpin") != 0) { return; }
		fired = true;
		const task::running_on on(task::scheduler_ref::of(ts));
		sd.handle().resume();  // join_shutdown meets the pin and marks the slot
	};
	const auto before = w.cat->stats();
	p.release();
	hk.fn = nullptr;
	CHECK(fired && !ended && st(w, i) == S::closing && pins(w, i) == 0);  // the unpin began the close
	w.settle();
	ts.run();
	const auto after = w.cat->stats();
	CHECK(ended && w.cat->shut_down() && st(w, i) == S::free);
	CHECK(edge(after, S::open, S::closing) - edge(before, S::open, S::closing) == 1);
	CHECK(edge(after, S::closing, S::free) - edge(before, S::closing, S::free) == 1);
	std::puts("catalog_teardown mark in the unpin's window: ok");
}

// Review F2(b): the last unpin lands between shutdown_slot's load and its mark. The mark's exchange fails, and
// its retry closes the `{0, open}` slot: one open-to-closing, then free, and shutdown ends.
void unpin_in_mark_window() {
	hooked hk;
	W w(1, 1, false, true, hk.hook());
	test_sched ts;
	cat_t::pin p = pinned(w);
	const std::uint32_t i = sl(w, kn(0));
	bool ended = false, fired = false;
	std::function<task::task<void>()> body = [&]() -> task::task<void> {
		(void)co_await w.cat->shutdown();
		ended = true;
	};
	task::task<void> sd = body();
	hk.fn = [&](const char* site, bool) {
		if (fired || std::strcmp(site, "shutdown.mark") != 0) { return; }
		fired = true;
		p.release();  // the last pin goes before the mark's exchange
	};
	const auto before = w.cat->stats();
	{
		const task::running_on on(task::scheduler_ref::of(ts));
		sd.handle().resume();
	}
	hk.fn = nullptr;
	CHECK(fired && !ended && st(w, i) == S::closing && pins(w, i) == 0);  // the mark's retry began the close
	w.settle();
	ts.run();
	const auto after = w.cat->stats();
	CHECK(ended && w.cat->shut_down() && st(w, i) == S::free);
	CHECK(edge(after, S::open, S::closing) - edge(before, S::open, S::closing) == 1);
	CHECK(edge(after, S::closing, S::free) - edge(before, S::closing, S::free) == 1);
	std::puts("catalog_teardown unpin in the mark's window: ok");
}

// A refused close's end unbooks while the slot is still `closing`: a close a pass begins right after the end's
// exchange books the slot's descriptors afresh, and the booking holds until that close ends: nothing stays booked
// after it. The slot is hot (a map held), so each close is refused and ends at `open`, as catalog_stress's are.
void close_after_refused_end() {
	hooked hk;
	W w(1, 1, false, true, hk.hook());
	const std::uint32_t i = [&] {
		cat_t::pin p = pinned(w);
		return sl(w, kn(0));
	}();
	const std::uint64_t held = w.cat->count_of(i, strata::resource::descriptors);
	CHECK(held != 0 && w.cat->count_of(i, strata::resource::maps) != 0 && st(w, i) == S::open && pins(w, i) == 0);
	bool begun = false, checked = false;
	hk.fn = [&](const char* site, bool) {
		if (!begun && std::strcmp(site, "transition.ended") == 0) {
			begun = true;
			CHECK(w.cat->begin_close(i));  // a pass's close, right after the refused end's exchange
		}
		if (begun && !checked && std::strcmp(site, "refuse.end") == 0) {
			checked = true;
			CHECK(st(w, i) == S::closing && w.cat->pending_descriptors() == held);
		}
	};
	const auto before = w.cat->stats();
	CHECK(w.cat->begin_close(i));
	w.settle();
	hk.fn = nullptr;
	const auto after = w.cat->stats();
	CHECK(begun && checked);
	CHECK(st(w, i) == S::open && w.cat->pending_descriptors() == 0);
	CHECK(edge(after, S::closing, S::open) - edge(before, S::closing, S::open) == 2);
	std::puts("catalog_teardown close after a refused end: ok");
}

}  // namespace

// argv[1]: property seeds (default 200), or one case alone: `threads`, `unpin` (the last unpin under shutdown),
// `miss` (a miss under shutdown), `mark-window` or `unpin-window` (the mark and the last unpin in each other's
// window), `refused-end` (a close begun right after a refused close's end); argv[2]: threaded rounds (default 300).
int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "unpin") == 0) {
		last_unpin_under_shutdown();
		return failures ? 1 : 0;
	}
	if (argc > 1 && std::strcmp(argv[1], "miss") == 0) {
		miss_under_shutdown();
		return failures ? 1 : 0;
	}
	if (argc > 1 && std::strcmp(argv[1], "mark-window") == 0) {
		mark_in_unpin_window();
		return failures ? 1 : 0;
	}
	if (argc > 1 && std::strcmp(argv[1], "unpin-window") == 0) {
		unpin_in_mark_window();
		return failures ? 1 : 0;
	}
	if (argc > 1 && std::strcmp(argv[1], "refused-end") == 0) {
		close_after_refused_end();
		return failures ? 1 : 0;
	}
	const bool only_threads = argc > 1 && std::strcmp(argv[1], "threads") == 0;
	const unsigned seeds = argc > 1 && !only_threads ? unsigned(std::atoi(argv[1])) : 200;
	const unsigned rounds = argc > 2 ? unsigned(std::atoi(argv[2])) : 300;
	if (only_threads) {
		threaded(rounds);
		return failures ? 1 : 0;
	}
	destroyed_while_linked();
	destroyed_in_delivery(false);
	destroyed_in_delivery(true);
	destroyed_after_delivery(false);
	destroyed_after_delivery(true);
	middle_waiter();
	drain_waiter();
	shutdown_waiter_destroyed();
	cancel_while_waiting();
	cancel_before();
	cancel_between_join_and_registration();
	cancel_loses_race();
	cancel_during_destroy();
	self_wait_cancelled();
	orphaned_waiter(false);
	orphaned_waiter(true);
	orphaned_shutdown_waiter();
	long windows = 0;
	for (unsigned s = 1; s <= seeds; ++s) { windows += property(s, 80); }
	std::printf("catalog_teardown property: %u seeds, %ld frames destroyed inside a delivery's window\n", seeds, windows);
	threaded(rounds);
	last_unpin_under_shutdown();
	miss_under_shutdown();
	mark_in_unpin_window();
	unpin_in_mark_window();
	close_after_refused_end();
	if (failures) {
		std::fprintf(stderr, "catalog_teardown: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_teardown: ok");
	return 0;
}
