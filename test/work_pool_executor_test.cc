// strata::work_pool_executor (phase7c design D4): the executor concept over a WorkPool. Work runs
// on a pool worker in the merge class and marked as background; a group's cancel drops its
// queued work, frees it, and stops what runs; wait() sees the group quiet; sleeps end on a stop;
// a pool shared by two groups serves both. Indexes on it: background_merge_test's cases through
// defer_io_test.
#include "strata/core/work_pool_executor.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

using namespace strata;
using namespace std::chrono_literals;

namespace {

WorkPool::Options two_threads() {
	WorkPool::Options o;
	o.threads = 2;
	o.name_format = "wpx{:02}";
	return o;
}

template <class P>
bool spin_until(P&& p, std::chrono::milliseconds limit = 10s) {
	const auto end = std::chrono::steady_clock::now() + limit;
	while (!p()) {
		if (std::chrono::steady_clock::now() > end) { return false; }
		std::this_thread::yield();
	}
	return true;
}

// Counts the copies of a captured value that are alive: a dropped item must destroy its work.
struct tracked {
	std::atomic<int>* live;
	explicit tracked(std::atomic<int>& l) : live(&l) { live->fetch_add(1); }
	tracked(const tracked& o) : live(o.live) { live->fetch_add(1); }
	~tracked() { live->fetch_sub(1); }
};

// Work runs once on a worker, off this thread, with the background mark and an unrequested stop.
void runs_on_a_worker() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	auto g = ex.open();
	std::atomic<int> ran{0};
	std::atomic<bool> background{false}, stopped{true};
	std::thread::id on;
	g->post([&](const task::cancel_token& stop) {
		on = std::this_thread::get_id();
		background = detail::owner_thread().background;
		stopped = stop.requested();
		ran.fetch_add(1);
	});
	g->wait();
	CHECK(ran == 1 && on != std::this_thread::get_id() && background && !stopped);
	std::printf("runs on a worker: ok\n");
}

// cancel(): the queued work never runs and is destroyed; the running work sees its stop and is
// waited for; work posted afterwards runs normally.
void cancel_drops_and_stops() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	auto g = ex.open();
	std::atomic<int> live{0}, ran{0}, saw_stop{0};
	std::atomic<bool> started{false};
	g->post([&](const task::cancel_token& stop) {
		started = true;
		while (!stop.requested()) { std::this_thread::yield(); }
		saw_stop.fetch_add(1);
	});
	CHECK(spin_until([&] { return started.load(); }));
	for (int i = 0; i < 8; ++i) {
		g->post([t = tracked(live), &ran](const task::cancel_token&) { ran.fetch_add(1); });
	}
	CHECK(live == 8);  // width 1: they wait behind the running one
	g->cancel();
	CHECK(saw_stop == 1 && ran == 0 && live == 0);
	g->post([&](const task::cancel_token& stop) { ran.fetch_add(stop.requested() ? 100 : 1); });
	g->wait();
	CHECK(ran == 1);
	std::printf("cancel drops queued work, stops running work: ok\n");
}

// The executor's sleeps end early on a stop, and wait for the clock otherwise.
void sleeps_end_on_stop() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	auto g = ex.open();
	std::atomic<int> result{-1};
	std::atomic<bool> started{false};
	g->post([&](const task::cancel_token& stop) {
		started = true;
		result = ex.sleep_until(ex.now() + 60'000'000'000ull, stop) ? 1 : 0;
	});
	CHECK(spin_until([&] { return started.load(); }));
	const auto t0 = std::chrono::steady_clock::now();
	g->cancel();
	CHECK(result == 0 && std::chrono::steady_clock::now() - t0 < 5s);
	task::cancel_source never;
	const auto t1 = ex.now();
	CHECK(ex.sleep_until(t1 + 20'000'000ull, never.token()) && ex.now() >= t1 + 20'000'000ull);
	std::printf("sleeps end on a stop: ok\n");
}

// Two groups on one pool, each with width 1: both finish, and each runs its own items one at a time.
void groups_share_the_pool() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	auto a = ex.open(), b = ex.open();
	std::atomic<int> in_a{0}, over_a{0}, done{0};
	for (int i = 0; i < 50; ++i) {
		a->post([&](const task::cancel_token&) {
			if (in_a.fetch_add(1) != 0) { over_a.fetch_add(1); }
			std::this_thread::yield();
			in_a.fetch_sub(1);
			done.fetch_add(1);
		});
		b->post([&](const task::cancel_token&) { done.fetch_add(1); });
	}
	a->wait();
	b->wait();
	CHECK(done == 100 && over_a == 0);
	std::printf("two groups on one pool: ok\n");
}

// Work is merge-class: it takes only the workers beyond the pool's reserve. With two threads and a
// reserve of one, two groups' items run one at a time; the second starts once the first ends.
void merge_class_leaves_the_reserve() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	auto a = ex.open(), b = ex.open();
	std::atomic<int> started{0};
	std::atomic<bool> release{false};
	const auto blocker = [&](const task::cancel_token&) {
		started.fetch_add(1);
		while (!release.load()) { std::this_thread::yield(); }
	};
	a->post(blocker);
	b->post(blocker);
	CHECK(spin_until([&] { return started.load() >= 1; }));
	std::this_thread::sleep_for(100ms);
	CHECK(started == 1);  // the other worker is the reserve's
	release = true;
	a->wait();
	b->wait();
	CHECK(started == 2);
	std::printf("merge class leaves the reserve: ok\n");
}

// An item that throws is dropped; the group goes on.
void throwing_work_is_dropped() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	auto g = ex.open();
	std::atomic<int> ran{0};
	g->post([](const task::cancel_token&) { throw 1; });
	g->post([&](const task::cancel_token&) { ran.fetch_add(1); });
	g->wait();
	CHECK(ran == 1);
	std::printf("throwing work: ok\n");
}

// Ending a group with work queued frees the work (the destructor cancels).
void ending_a_group_cancels() {
	WorkPool pool(two_threads());
	work_pool_executor ex(pool);
	std::atomic<int> live{0};
	std::atomic<bool> release{false}, started{false};
	{
		auto g = ex.open();
		g->post([&](const task::cancel_token&) {
			started = true;
			while (!release.load()) { std::this_thread::yield(); }
		});
		CHECK(spin_until([&] { return started.load(); }));
		for (int i = 0; i < 4; ++i) { g->post([t = tracked(live)](const task::cancel_token&) {}); }
		CHECK(live == 4);
		release = true;
	}
	CHECK(live == 0);
	std::printf("ending a group: ok\n");
}

}  // namespace

int main() {
	runs_on_a_worker();
	cancel_drops_and_stops();
	sleeps_end_on_stop();
	groups_share_the_pool();
	merge_class_leaves_the_reserve();
	throwing_work_is_dropped();
	ending_a_group_cancels();
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); }
	return failures ? 1 : 0;
}
