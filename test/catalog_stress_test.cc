// The cold tier's step 2, items 3 to 5: pins, the sweep and shutdown, and reopen under threads
// (docs/design/cold-tier.md D4, D5, D6; S4, S7; T5, T6). Pinner threads take request and chore
// pins by key and by handle on a few dozen keys and read through them; a sweeper thread demotes and
// closes slots as fast as it can find them idle, and flips the kernel's map and the resolver's
// failures; the catalog's own sweep runs on a timer thread with idle times of microseconds, a soft cap
// below the key count and negative entries erased at once; chores run on a WorkPool. A last phase
// shuts the catalog down while the pinners still run. Checked against the slot model: only its state changes
// happen (catalog_test.cc allowed_edge), every pin is released and counted in the word, no waiter is
// left suspended, a request pin is never handed out on a slot that still owes its bodies, and the
// counts of the kernel model are zero once everything is closed. Run under TSan and ASan.
#include "catalog_world.hh"
#include "strata/core/work_pool_executor.hh"

#include <condition_variable>
#include <random>

namespace {

using strata::pin_kind;
using strata::slot_state;
using S = slot_state;

// Chores of the catalog's one group that may run at once (argv[3]): 2 or more, so its transitions and
// shutdown's closes overlap as a host's may (C21).
unsigned g_per_group = 2;
// At least `n` chores at once for the scope (the policy's runs wake 3).
struct per_group_scope {
	unsigned saved = g_per_group;
	explicit per_group_scope(unsigned n) { g_per_group = std::max(n, saved); }
	~per_group_scope() { g_per_group = saved; }
};

// An executor over a WorkPool of its own, so `world` can build it by default.
class pool_exec final : public strata::executor {
public:
	pool_exec() : pool_(opts()), ex_(pool_, g_per_group) {}
	std::unique_ptr<group> open() override { return ex_.open(); }
	std::uint64_t now() override { return ex_.now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override { return ex_.sleep_until(t, stop); }

private:
	static WorkPool::Options opts() {
		WorkPool::Options o;
		o.threads = 3;
		o.name_format = "ctstress{:02}";
		return o;
	}
	WorkPool pool_;
	strata::work_pool_executor ex_;
};

using PW = world<pool_exec>;

// A thread-safe scheduler a pinner thread drains: the catalog posts a resumed pin here, from the
// thread that made the transition's final store.
class mt_sched {
public:
	void post(std::coroutine_handle<> h) {
		{
			std::lock_guard l(mu_);
			q_.push_back(h);
		}
		cv_.notify_one();
	}
	std::uint64_t now() const noexcept { return 0; }
	// Runs posted handles until `done`; false if `limit` passed first (a stuck waiter).
	template <class Done>
	bool run_until(Done&& done, std::chrono::seconds limit) {
		const auto end = std::chrono::steady_clock::now() + limit;
		const task::running_on on(task::scheduler_ref::of(*this));
		for (;;) {
			std::coroutine_handle<> h;
			{
				std::unique_lock l(mu_);
				if (!cv_.wait_for(l, std::chrono::milliseconds(20), [&] { return !q_.empty() || done(); })) {
					if (std::chrono::steady_clock::now() > end) { return false; }
					continue;
				}
				if (q_.empty()) { return true; }
				h = q_.front();
				q_.pop_front();
			}
			h.resume();
		}
	}

private:
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<std::coroutine_handle<>> q_;
};

constexpr int keys = 24;
std::string kn(int i) { return "k" + std::to_string(i); }

struct counters {
	std::atomic<std::uint64_t> pins{0}, failed{0}, reads{0}, chores{0}, handle_tries{0}, stuck{0}, closed{0};
};

// The sweep at the pace of the test: idle times of microseconds, a cap under the key count, a short period.
void fast_sweep(cat_t::host& h) {
	h.demote_idle_ns = 300'000;
	h.close_idle_ns = 2'000'000;
	h.negative_ttl_ns = 1'000'000;
	h.sweep_period_ns = 1'000'000;
	h.soft_open = 10;
	h.pass_posts = 8;
}

strata::shutdown_result shut_down(PW& w, mt_sched& sched) {
	std::atomic<bool> done{false};
	strata::shutdown_result res;
	std::function<task::task<void>()> body = [&]() -> task::task<void> { res = co_await w.cat->shutdown(); };
	task::spawn(task::scheduler_ref::of(sched), body(), [&](std::exception_ptr e) {
		if (e) { std::terminate(); }
		done = true;
	});
	CHECK(sched.run_until([&] { return done.load(); }, std::chrono::seconds(60)));
	return res;
}

// One writer per index at a time: the host's turn (shared-reader SR-D20).
std::mutex turns[keys];

task::task<void> one_op(PW& w, std::mt19937_64& rng, cat_t::handle (&handles)[keys], counters& c, bool defer) {
	const int key = int(rng() % 4 ? rng() % 6 : rng() % keys);
	const bool chore = rng() % 7 == 0;
	const bool use = !chore && rng() % 2;
	try {
		cat_t::pin p;
		if (handles[key] && rng() % 2) {
			++c.handle_tries;
			p = co_await w.cat->repin(handles[key], kn(key), chore ? pin_kind::chore : pin_kind::request, use);
		} else if (chore) {
			p = co_await w.cat->pin_chore(kn(key), use);
		} else {
			p = co_await w.cat->pin_request(kn(key), use);
		}
		++c.pins;
		const std::uint64_t word = w.cat->word_of(p.where().slot);
		CHECK(cat_t::pins_of(word) >= 1);
		handles[key] = p.where();
		if (!chore) {
			CHECK(cat_t::state_of(word) == S::open);
			CHECK(!p.index().promote_owed());  // handed out only after promote() completed
			const auto g = p.reader().acquire();
			CHECK(g->get(key_of(0)) == std::optional<std::string>("v0"));
			CHECK(g->get(key_of(3)) == std::optional<std::string>("v3"));
			++c.reads;
			if (defer) { CHECK(p.index().unleased_maps() == 0); }
		} else {
			++c.chores;
			const std::lock_guard turn(turns[key]);
			const strata::lease_scope hold{p.index().lease()};  // a commit's hold: a wake behind it reposts
			if (rng() % 3 == 0 && cat_t::state_of(w.cat->word_of(p.where().slot)) == S::open) {  // a write the close must refuse
				if (p.index().apply(tx({erase(key_of(int(rng() % 2) + 1))}), {}).result == verdict::accepted) {
					p.index().refresh();
					if (rng() % 2) { p.index().commit(); }
				}
			}
			CHECK(p.index().lease().held_here());
			for (int i = int(rng() % 200); i > 0; --i) { std::atomic_signal_fence(std::memory_order_seq_cst); }
			if (rng() % 6 == 0) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }  // a wake behind this hold reposts
		}
		if (rng() % 5 == 0) { co_await task::yield(); }
	} catch (const std::system_error&) {
		++c.failed;
	} catch (const strata::catalog_closed&) {
		++c.closed;
	}
}

void stress(bool defer, int ops, int pinners, bool policy = false) {
	const per_group_scope chores(policy ? 3 : 0);
	// The schedulers outlive the world: a chore's deliver is still inside post() when the pinner it
	// resumed finishes, so a pinner's scheduler must not die with its thread. One per pinner, and one
	// for the final committer.
	std::deque<mt_sched> scheds(std::size_t(pinners) + 1);
	PW w(keys, 2, defer, true, {}, 2);  // the bound: 2 failed wakes
	w.tweak = [policy](cat_t::host& h) {
		fast_sweep(h);
		if (policy) { h.soft_open = 8, h.soft_hot = 6; }  // the closing policy and the hot cap run against the pins (D6, S8.8)
	};
	w.make_catalog({});
	w.cat->start();
	counters c;
	std::atomic<bool> stop{false};
	std::atomic<int> finished{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < pinners; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(1000 + t + (defer ? 100 : 0));
			cat_t::handle handles[keys];
			mt_sched& sched = scheds[std::size_t(t)];
			for (int i = 0; i < ops; ++i) {
				std::atomic<bool> done{false};
				task::spawn(task::scheduler_ref::of(sched), one_op(w, rng, handles, c, defer), [&](std::exception_ptr e) {
					if (e) { std::terminate(); }
					done = true;
				});
				if (!sched.run_until([&] { return done.load(); }, std::chrono::seconds(60))) {
					++c.stuck;  // a waiter nobody resumed
					std::fprintf(stderr, "stuck pin (thread %d, op %d)\n", t, i);
					for (std::uint32_t s = 0; s < w.cat->slots_made(); ++s) {
						std::fprintf(stderr, "  slot %u: pins %llu state %u waiters %zu\n", s, (unsigned long long)cat_t::pins_of(w.cat->word_of(s)),
							unsigned(cat_t::state_of(w.cat->word_of(s))), w.cat->waiters_of(s));
					}
					std::abort();
				}
			}
			++finished;
		});
	}
	std::thread sweeper2;  // a second poster of passes: they coalesce, one runs at a time
	if (policy) {
		sweeper2 = std::thread([&] {
			std::mt19937_64 rng(11);
			while (finished.load() < pinners) {
				w.cat->post_pass();
				std::this_thread::sleep_for(std::chrono::microseconds(rng() % 20));
			}
		});
	}
	std::atomic<std::uint64_t> bound_reads{0};
	std::thread sweeper([&] {
		std::mt19937_64 rng(7);
		bool faulty = false;  // a kernel fault lasts 40 ms, long enough for failures to pile up past the bound
		auto fault_end = std::chrono::steady_clock::now();
		while (finished.load() < pinners) {
			if (faulty && std::chrono::steady_clock::now() > fault_end) { w.kern.fail_map = 0, faulty = false; }
			if (policy && rng() % 4 != 0) {  // a pass, with the direct closers below mixed in
				w.cat->post_pass();
				std::this_thread::sleep_for(std::chrono::microseconds(rng() % 30));
				continue;
			}
			const std::uint32_t n = w.cat->slots_made();
			if (n) {
				const std::uint32_t i = std::uint32_t(rng() % n);
				switch (rng() % 12) {
				case 11: w.cat->post_pass(); break;
				case 0: case 1: w.cat->begin_demote(i); break;
				case 2: case 3: w.cat->begin_close(i); break;
				case 4: w.cat->decay(i); break;
				case 5: case 6: bound_reads += w.cat->wake_failures_of(i) >= 2 && w.cat->begin_bound_close(i); break;
				case 7:
					if (!faulty && c.pins.load() > 2000 && rng() % 6 == 0) {
						w.kern.fail_map = rng() % 2 ? ENOMEM : EMFILE, faulty = true;
						fault_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
					}
					break;
				case 8: w.fail_commit = rng() % 4 == 0 ? EIO : 0; break;
				default: w.fail_resolve = EIO, w.fail_left = rng() % 48 == 0 ? 1 : 0; break;
				}
			}
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 60));
		}
	});
	// Two hosts' commit chores: a chore pin on a warm slot with the lease held for a few ms, so the request pins that
	// meet `owed` start a wake that finds the lease busy and reposts through the timer, over and over (I4-N6).
	std::atomic<std::uint64_t> holds{0};
	const auto hold_loop = [&](unsigned seed) {
		std::mt19937_64 rng(seed);
		while (finished.load() < pinners) {
			const std::uint32_t n = w.cat->slots_made();
			for (std::uint32_t k = 0; k < n && finished.load() < pinners; ++k) {
				const std::uint32_t i = std::uint32_t((k + rng()) % n);
				S now = cat_t::state_of(w.cat->word_of(i));
				if (now == S::open && w.cat->begin_demote(i)) {  // a warm slot to hold, taken before a request pin wakes it
					while ((now = cat_t::state_of(w.cat->word_of(i))) == S::demoting) { std::this_thread::yield(); }
				}
				if (now != S::owed) { continue; }
				const std::string key = w.cat->key_at(i);
				if (key.empty()) { continue; }
				const std::lock_guard turn(turns[std::atoi(key.c_str() + 1)]);  // the host's turn: one writer per index
				auto r = w.cat->try_pin(key, pin_kind::chore, false);
				if (r.status != cat_t::try_status::pinned) { continue; }
				const strata::lease_scope hold{r.p.index().lease()};
				++holds;
				(void)w.cat->try_pin(key, pin_kind::request, false);  // starts the wake, which meets the hold
				std::this_thread::sleep_for(std::chrono::microseconds(2000 + rng() % 38000));
				break;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
		}
	};
	std::thread holder1(hold_loop, 11u), holder2(hold_loop, 12u);
	for (auto& t : ts) { t.join(); }
	sweeper.join();
	if (sweeper2.joinable()) { sweeper2.join(); }
	holder1.join();
	holder2.join();
	w.kern.fail_map = 0;
	w.fail_left = 0;
	w.fail_commit = 0;
	w.cat->stop_sweep();  // no pass begins a transition from here on (B4)
	w.quiesce();          // a wake behind the holder's last hold reposts through the timer

	// Quiescent: every pin gone, nothing waiting, only stable states.
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
		const std::uint64_t word = w.cat->word_of(i);
		const S s = cat_t::state_of(word);
		CHECK(cat_t::pins_of(word) == 0 && w.cat->waiters_of(i) == 0);
		CHECK(s == S::free || s == S::open || s == S::owed || s == S::failed);
		if (s == S::open || s == S::owed) {
			CHECK((s == S::owed) == w.cat->index_of(i)->promote_owed());
			if (defer) { CHECK(w.cat->index_of(i)->unleased_maps() == 0); }
		}
	}
	if (policy) {  // every open or warm slot is in a queue and no other is; no demotion is left in flight
		std::uint64_t resident = 0;
		for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
			const S s = cat_t::state_of(w.cat->word_of(i));
			const bool in = s == S::open || s == S::owed;
			resident += in;
			CHECK((w.cat->queue_of(i) != 0) == in);
		}
		const auto q = w.cat->queue_sizes();
		CHECK(q.small + q.main == resident && w.cat->pending_maps() == 0);
		CHECK(w.cat->stats().passes > 0 && w.cat->stats().evictions > 0);
	}
	// Only the model's state changes were made; each of its changes was made at least once.
	const auto st = w.cat->stats();
	const std::pair<S, S> ok[] = {{S::free, S::opening}, {S::opening, S::open}, {S::opening, S::failed}, {S::failed, S::opening}, {S::open, S::demoting},
		{S::demoting, S::open}, {S::demoting, S::owed}, {S::demoting, S::waking}, {S::owed, S::waking}, {S::waking, S::open}, {S::waking, S::owed},
		{S::open, S::closing}, {S::owed, S::closing}, {S::closing, S::free}, {S::closing, S::opening}, {S::closing, S::open}, {S::closing, S::owed}, {S::closing, S::waking}, {S::failed, S::free}};
	for (unsigned i = 0; i < strata::slot_states; ++i) {
		for (unsigned j = 0; j < strata::slot_states; ++j) {
			bool allowed = false;
			for (const auto& e : ok) { allowed |= unsigned(e.first) == i && unsigned(e.second) == j; }
			if (st.edges[i][j]) { CHECK(allowed); }
		}
	}
	// Required: the changes every run makes. The demotions and wakes depend on the sweeper's timing on a loaded
	// runner, so they are reported here and made by catalog_test's schedules, as are the refused closes, the
	// wake's failure and the retries.
	std::vector<std::pair<S, S>> need = {{S::free, S::opening}, {S::opening, S::open}, {S::open, S::closing}};
	std::vector<std::pair<S, S>> report;
	if (defer) {
		report = {{S::demoting, S::owed}, {S::owed, S::waking}, {S::waking, S::open}};
	} else {
		report = {{S::open, S::demoting}, {S::demoting, S::open}};
	}
	for (const auto& e : need) {
		if (!st.edges[unsigned(e.first)][unsigned(e.second)]) { std::fprintf(stderr, "edge %u>%u never made\n", unsigned(e.first), unsigned(e.second)); }
		CHECK(st.edges[unsigned(e.first)][unsigned(e.second)] > 0);
	}
	for (const auto& e : report) {
		if (!st.edges[unsigned(e.first)][unsigned(e.second)]) { std::fprintf(stderr, "edge %u>%u not made this run\n", unsigned(e.first), unsigned(e.second)); }
	}
	// Whether a pin happens to meet a transition depends on the host's timing, so the stress reports it;
	// catalog_test's schedules make a pin wait at each transitional state deterministically.
	for (S m : {S::opening, S::demoting, S::closing, S::waking}) {
		if (!st.waited[unsigned(m)]) { std::fprintf(stderr, "no pin met state %u this run\n", unsigned(m)); }
	}
	if (defer && !policy) { CHECK(st.wakes_posted > 0 && st.wake_reposts > 0); }
	if (defer && st.wakes_posted == 0) { std::fprintf(stderr, "no wake this run\n"); }
	CHECK(st.handle_pins > 0);

	std::printf("stress %s: bound closes %llu (commits %llu, commit failures %llu), %llu wake failures, %llu gated, %llu overloads, %llu bound tries\n", defer ? "defer_io" : "plain",
		(unsigned long long)st.bound_closes, (unsigned long long)st.bound_commits, (unsigned long long)st.commit_failures, (unsigned long long)st.wake_failures,
		(unsigned long long)st.wake_gated, (unsigned long long)st.wake_overloads, (unsigned long long)bound_reads.load());
	std::printf("stress %s%s: %llu pins (%llu failed, %llu reads, %llu chore holds), %llu handle tries, %llu handle pins, %llu stale, %llu wakes, %llu wake reposts\n",
		defer ? "defer_io" : "plain", policy ? " + policy" : "", (unsigned long long)c.pins.load(), (unsigned long long)c.failed.load(), (unsigned long long)c.reads.load(),
		(unsigned long long)c.chores.load(), (unsigned long long)c.handle_tries.load(), (unsigned long long)st.handle_pins, (unsigned long long)st.stale_handles,
		(unsigned long long)st.wakes_posted, (unsigned long long)st.wake_reposts);
	std::printf("  %llu chore holds on a warm slot, %llu wake reposts\n", (unsigned long long)holds.load(), (unsigned long long)st.wake_reposts);
	std::printf("  state changes:");
	for (unsigned i = 0; i < strata::slot_states; ++i) {
		for (unsigned j = 0; j < strata::slot_states; ++j) {
			if (st.edges[i][j]) { std::printf(" %u>%u:%llu", i, j, (unsigned long long)st.edges[i][j]); }
		}
	}
	std::printf("\n  met transition:");
	for (unsigned i = 0; i < strata::slot_states; ++i) {
		if (st.waited[i]) { std::printf(" %u:%llu", i, (unsigned long long)st.waited[i]); }
	}
	std::printf("\n");

	// Shut down: the host's commit chore commits what the chore pins wrote, every slot closes, and the kernel
	// model and the catalog's counts are zero at rest (I6).
	std::printf("  sweep: %llu passes (%llu reposts), %llu demotes, %llu closes, %llu erased, %llu reclaims, %llu stuck\n", (unsigned long long)st.passes,
		(unsigned long long)st.pass_reposts, (unsigned long long)st.pass_demotes, (unsigned long long)st.pass_closes, (unsigned long long)st.erased,
		(unsigned long long)st.reclaims, (unsigned long long)st.stuck_closes);
	CHECK(st.passes > 0 && st.stuck_closes == 0);
	const auto res = shut_down(w, scheds.back());
	CHECK(res.uncommitted.empty() && w.cat->shut_down() && w.cat->occupied() == 0 && w.cat->table_size() == 0);
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) { CHECK(cat_t::state_of(w.cat->word_of(i)) == S::free); }
	CHECK(w.kern.fds.load() == 0 && w.kern.maps.load() == 0);
	CHECK(w.cat->counts().get(strata::resource::descriptors) == 0 && w.cat->counts().get(strata::resource::maps) == 0);
	CHECK(payload::destroyed_in_close == 0);
}

// Shutdown while the pinners run: new pins fail with catalog_closed, held ones finish, and every slot closes.
void stress_shutdown(bool defer, int pinners) {
	std::deque<mt_sched> scheds(std::size_t(pinners) + 3);  // outlive the pinners', the joiners' threads and the world
	std::atomic<int> inside{0}, named{0}, overlapped{0};
	std::atomic<bool> pair_up{false};
	PW w(keys, 2, defer, true, {}, 2);
	w.tweak = fast_sweep;
	// Where a failed commit's key is named, a close waits up to 200 ms for another to be there at once: with two of the
	// group's chores running together the names race if unguarded. `overlapped` counts the closes that met another.
	w.make_catalog([&](const char* site, bool) {
		if (pair_up.load() && std::strcmp(site, "shutdown.name") == 0) {
			++named;
			bool met = ++inside > 1;
			for (const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200); !met && std::chrono::steady_clock::now() < end;) {
				std::this_thread::yield();
				met = inside.load() > 1;
			}
			overlapped += met;
			--inside;
		}
	});
	w.cat->start();
	counters c;
	std::vector<std::thread> ts;
	for (int t = 0; t < pinners; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(2000 + t + (defer ? 100 : 0));
			cat_t::handle handles[keys];
			mt_sched& sched = scheds[std::size_t(t)];
			while (c.closed.load() == 0) {
				std::atomic<bool> done{false};
				task::spawn(task::scheduler_ref::of(sched), one_op(w, rng, handles, c, defer), [&](std::exception_ptr e) {
					if (e) { std::terminate(); }
					done = true;
				});
				if (!sched.run_until([&] { return done.load(); }, std::chrono::seconds(60))) {
					std::fprintf(stderr, "stuck pin (shutdown phase, thread %d)\n", t);
					std::abort();
				}
			}
		});
	}
	while (c.pins.load() < 3000) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	// Tasks join from two other threads throughout, the last ones as it completes; each is resumed on its joiner's
	// scheduler, which runs until all of its tasks have been (a host acts on the resumption, never on shut_down()).
	pair_up = true;
	w.fail_commit = EIO;
	std::atomic<bool> go{false};
	std::atomic<int> joins{0}, resumed{0};
	std::function<task::task<void>()> join_body = [&]() -> task::task<void> {
		(void)co_await w.cat->shutdown();
		++resumed;
	};
	const auto join_loop = [&](mt_sched& js) {
		while (!go.load()) { std::this_thread::yield(); }
		int mine = 0;
		std::atomic<int> done{0};
		while (!w.cat->shut_down() && mine < 5000) {
			++mine, ++joins;
			task::spawn(task::scheduler_ref::of(js), join_body(), [&](std::exception_ptr e) {
				if (e) { std::terminate(); }
				++done;
			});
			CHECK(js.run_until([] { return true; }, std::chrono::seconds(60)));  // runs the join itself
		}
		CHECK(js.run_until([&] { return done.load() == mine; }, std::chrono::seconds(60)));
	};
	std::thread joiner1(join_loop, std::ref(scheds[std::size_t(pinners) + 1])), joiner2(join_loop, std::ref(scheds[std::size_t(pinners) + 2]));
	go = true;
	const auto res = shut_down(w, scheds[std::size_t(pinners)]);
	joiner1.join();
	joiner2.join();
	CHECK(resumed.load() == joins.load());
	for (auto& t : ts) { t.join(); }
	CHECK(res.uncommitted.size() == w.cat->stats().shutdown_commit_failures && w.cat->shut_down() && w.cat->occupied() == 0 && w.cat->table_size() == 0);
	CHECK(c.closed.load() > 0);
	const auto st = w.cat->stats();
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
		const std::uint64_t word = w.cat->word_of(i);
		CHECK(cat_t::state_of(word) == S::free && cat_t::pins_of(word) == 0);
	}
	CHECK(w.kern.fds.load() == 0 && w.kern.maps.load() == 0);
	CHECK(w.cat->counts().get(strata::resource::descriptors) == 0 && w.cat->counts().get(strata::resource::maps) == 0);
	CHECK(named.load() < 2 || overlapped.load() > 0);  // namings in parallel: the race T2 would make is reachable
	std::printf("stress shutdown %s (per_group %u): %llu pins, %llu refused after shutdown, %llu shutdown commits (%d failed and named, %d of them while another was), %d joins, %llu passes\n",
		defer ? "defer_io" : "plain", g_per_group, (unsigned long long)c.pins.load(), (unsigned long long)c.closed.load(), (unsigned long long)st.shutdown_commits, named.load(),
		overlapped.load(), joins.load(), (unsigned long long)st.passes);
}

// The policy's fetch-ops on the frequency (clear, decrement) against pins and unpins of the same word on other
// threads: none loses a pin or a state (D4: every write is a compare-exchange or a fetch-op).
void freq_hammer(int rounds) {
	const per_group_scope chores(1);
	PW w(1, 1, false);
	auto first = w.cat->try_pin(kn(0));
	CHECK(first.status == cat_t::try_status::would_wait);
	w.cat->wait_chores();
	CHECK(cat_t::state_of(w.cat->word_of(0)) == S::open);
	std::atomic<bool> stop{false};
	std::vector<std::thread> pinners;
	for (int t = 0; t < 3; ++t) {
		pinners.emplace_back([&] {
			for (int i = 0; i < rounds; ++i) {
				auto p = w.cat->try_pin(kn(0), pin_kind::request, i % 2 == 0);
				CHECK(p.status == cat_t::try_status::pinned);
			}
		});
	}
	std::thread policy([&] {
		while (!stop.load()) {
			w.cat->clear_freq(0);
			w.cat->decay(0);
		}
	});
	for (auto& t : pinners) { t.join(); }
	stop = true;
	policy.join();
	const std::uint64_t word = w.cat->word_of(0);
	CHECK(cat_t::pins_of(word) == 0 && cat_t::state_of(word) == S::open && cat_t::freq_of(word) <= 3);
	w.drain();
}

// Many keys over a few slots: three threads pin whatever they meet without waiting, a pass runs at full speed, and
// the pool's workers open and close slots at once, each close adding to the ghost and each open taking from it.
void churn_hammer(int rounds) {  // rounds / 5 milliseconds
	const per_group_scope chores(3);
	PW w(64, 1, false, false);
	w.remake(16, 0);  // a ghost of 8: enough that a hit is not rare under a slow sanitizer
	std::atomic<bool> stop{false}, over{false};
	std::vector<std::thread> pinners;
	for (int t = 0; t < 3; ++t) {
		pinners.emplace_back([&, t] {
			std::mt19937_64 rng(77 + t);
			while (!over.load()) {
				auto p = w.cat->try_pin(kn(int(rng() % 64)), pin_kind::request, rng() % 2);
				if (rng() % 8 == 0) { std::this_thread::yield(); }
			}
		});
	}
	std::thread passes([&] {
		while (!stop.load()) { w.cat->policy_pass(cat_t::policy_limits::lifted()); }
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(rounds / 5));  // pins do not wait, so the run is timed
	over = true;
	for (auto& t : pinners) { t.join(); }
	stop = true;
	passes.join();
	w.cat->wait_chores();
	std::uint64_t resident = 0;
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
		const S s = cat_t::state_of(w.cat->word_of(i));
		const bool in = s == S::open || s == S::owed;
		resident += in;
		CHECK((w.cat->queue_of(i) != 0) == in && cat_t::pins_of(w.cat->word_of(i)) == 0);
	}
	const auto q = w.cat->queue_sizes();
	CHECK(q.small + q.main == resident && w.cat->stats().ghost_hits > 0 && w.cat->stats().ghost_adds > 0);
	w.cat->policy_pass(cat_t::policy_limits::lifted());
	w.cat->wait_chores();
	CHECK(w.cat->queue_sizes().small + w.cat->queue_sizes().main <= 16);
	w.drain();
	std::printf("churn: %llu misses, %llu closes, %llu ghost hits, %llu ghost adds\n", (unsigned long long)w.cat->misses(), (unsigned long long)w.cat->stats().evictions,
		(unsigned long long)w.cat->stats().ghost_hits, (unsigned long long)w.cat->stats().ghost_adds);
}

// A pass over 60 slots too many posts 60 closes at once: the pool's workers end them together, each unlinking its
// slot and adding its key to the ghost; then the keys come back together, each open taking from the ghost.
void burst_closes(int iterations) {
	const per_group_scope chores(3);
	PW w(64, 1, false, false);
	w.remake(4, 0);
	for (int it = 0; it < iterations; ++it) {
		for (int k = 0; k < 64; ++k) { (void)w.cat->try_pin(kn(k)); }
		w.cat->wait_chores();
		const auto r = w.cat->policy_pass(cat_t::policy_limits::lifted());
		CHECK(r.closes >= 56);
		w.cat->wait_chores();
		CHECK(w.cat->queue_sizes().small + w.cat->queue_sizes().main <= 4);
	}
	CHECK(w.cat->stats().ghost_hits > 0 && w.cat->stats().ghost_adds >= std::uint64_t(iterations) * 56);
	w.drain();
}

}  // namespace

int main(int argc, char** argv) {
	const int ops = argc > 1 ? std::atoi(argv[1]) : 3000;
	const int pinners = argc > 2 ? std::atoi(argv[2]) : 4;
	if (argc > 3) { g_per_group = unsigned(std::atoi(argv[3])); }
	stress(false, ops, pinners);
	stress(true, ops, pinners);
	stress_shutdown(false, pinners);
	stress_shutdown(true, pinners);
	freq_hammer(ops * 40);
	churn_hammer(ops);
	burst_closes(ops / 10);
	stress(false, ops, pinners, true);
	stress(true, ops, pinners, true);
	if (failures) {
		std::fprintf(stderr, "catalog_stress: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_stress: ok");
	return 0;
}
