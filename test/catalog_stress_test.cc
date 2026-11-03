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

// An executor over a WorkPool of its own, so `world` can build it by default.
class pool_exec final : public strata::executor {
public:
	pool_exec() : pool_(opts()), ex_(pool_) {}
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

strata::shutdown_result shut_down(PW& w) {
	mt_sched sched;
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

void stress(bool defer, int ops, int pinners) {
	PW w(keys, 2, defer, true, {}, 2);  // the bound: 2 failed wakes
	w.tweak = fast_sweep;
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
			mt_sched sched;
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
	std::atomic<std::uint64_t> bound_reads{0};
	std::thread sweeper([&] {
		std::mt19937_64 rng(7);
		bool faulty = false;  // a kernel fault lasts 40 ms, long enough for failures to pile up past the bound
		auto fault_end = std::chrono::steady_clock::now();
		while (finished.load() < pinners) {
			if (faulty && std::chrono::steady_clock::now() > fault_end) { w.kern.fail_map = 0, faulty = false; }
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
	for (auto& t : ts) { t.join(); }
	sweeper.join();
	w.kern.fail_map = 0;
	w.fail_left = 0;
	w.fail_commit = 0;
	w.cat->wait_chores();

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
	// Required: the changes every run makes; the refused closes, the wake's failure and the retries
	// are reported here and made by catalog_test's schedules.
	std::vector<std::pair<S, S>> need = {{S::free, S::opening}, {S::opening, S::open}, {S::open, S::closing}};
	if (defer) {
		need.insert(need.end(), {{S::demoting, S::owed}, {S::owed, S::waking}, {S::waking, S::open}});
	} else {
		need.insert(need.end(), {{S::open, S::demoting}, {S::demoting, S::open}});
	}
	for (const auto& e : need) {
		if (!st.edges[unsigned(e.first)][unsigned(e.second)]) { std::fprintf(stderr, "edge %u>%u never made\n", unsigned(e.first), unsigned(e.second)); }
		CHECK(st.edges[unsigned(e.first)][unsigned(e.second)] > 0);
	}
	for (S m : {S::opening, S::demoting, S::closing}) { CHECK(st.waited[unsigned(m)] > 0); }
	if (defer) { CHECK(st.waited[unsigned(S::waking)] > 0 && st.wakes_posted > 0); }
	CHECK(st.handle_pins > 0);

	std::printf("stress %s: bound closes %llu (commits %llu, commit failures %llu), %llu wake failures, %llu gated, %llu overloads, %llu bound tries\n", defer ? "defer_io" : "plain",
		(unsigned long long)st.bound_closes, (unsigned long long)st.bound_commits, (unsigned long long)st.commit_failures, (unsigned long long)st.wake_failures,
		(unsigned long long)st.wake_gated, (unsigned long long)st.wake_overloads, (unsigned long long)bound_reads.load());
	std::printf("stress %s: %llu pins (%llu failed, %llu reads, %llu chore holds), %llu handle tries, %llu handle pins, %llu stale, %llu wakes, %llu wake reposts\n",
		defer ? "defer_io" : "plain", (unsigned long long)c.pins.load(), (unsigned long long)c.failed.load(), (unsigned long long)c.reads.load(),
		(unsigned long long)c.chores.load(), (unsigned long long)c.handle_tries.load(), (unsigned long long)st.handle_pins, (unsigned long long)st.stale_handles,
		(unsigned long long)st.wakes_posted, (unsigned long long)st.wake_reposts);
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
	const auto res = shut_down(w);
	CHECK(res.uncommitted.empty() && w.cat->shut_down() && w.cat->occupied() == 0 && w.cat->table_size() == 0);
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) { CHECK(cat_t::state_of(w.cat->word_of(i)) == S::free); }
	CHECK(w.kern.fds.load() == 0 && w.kern.maps.load() == 0);
	CHECK(w.cat->counts().get(strata::resource::descriptors) == 0 && w.cat->counts().get(strata::resource::maps) == 0);
	CHECK(payload::destroyed_in_close == 0);
}

// Shutdown while the pinners run: new pins fail with catalog_closed, held ones finish, and every slot closes.
void stress_shutdown(bool defer, int pinners) {
	PW w(keys, 2, defer, true, {}, 2);
	w.tweak = fast_sweep;
	w.make_catalog({});
	w.cat->start();
	counters c;
	std::vector<std::thread> ts;
	for (int t = 0; t < pinners; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(2000 + t + (defer ? 100 : 0));
			cat_t::handle handles[keys];
			mt_sched sched;
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
	const auto res = shut_down(w);
	for (auto& t : ts) { t.join(); }
	CHECK(res.uncommitted.empty() && w.cat->shut_down() && w.cat->occupied() == 0 && w.cat->table_size() == 0);
	CHECK(c.closed.load() >= std::uint64_t(pinners));
	const auto st = w.cat->stats();
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
		const std::uint64_t word = w.cat->word_of(i);
		CHECK(cat_t::state_of(word) == S::free && cat_t::pins_of(word) == 0);
	}
	CHECK(w.kern.fds.load() == 0 && w.kern.maps.load() == 0);
	CHECK(w.cat->counts().get(strata::resource::descriptors) == 0 && w.cat->counts().get(strata::resource::maps) == 0);
	std::printf("stress shutdown %s: %llu pins, %llu refused after shutdown, %llu shutdown commits, %llu passes\n", defer ? "defer_io" : "plain", (unsigned long long)c.pins.load(),
		(unsigned long long)c.closed.load(), (unsigned long long)st.shutdown_commits, (unsigned long long)st.passes);
}

}  // namespace

int main(int argc, char** argv) {
	const int ops = argc > 1 ? std::atoi(argv[1]) : 3000;
	const int pinners = argc > 2 ? std::atoi(argv[2]) : 4;
	stress(false, ops, pinners);
	stress(true, ops, pinners);
	stress_shutdown(false, pinners);
	stress_shutdown(true, pinners);
	if (failures) {
		std::fprintf(stderr, "catalog_stress: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_stress: ok");
	return 0;
}
