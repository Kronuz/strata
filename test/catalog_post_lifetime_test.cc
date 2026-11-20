// A delivery never touches a waiter's scheduler once the waiter's frame is destroyed (task's teardown, D8
// phases 2-4): the open delivers on a WorkPool thread and is held inside the waiter's post while the test
// thread closes the scheduler, destroys the frame and destroys the scheduler. `abandon` waits for the post,
// so the scheduler outlives it. Handshakes only, no sleeps; run under ASan (a post into the freed scheduler)
// and TSan. docs/mechanisms/catalog-delivery.md has the waiter states.
#include "catalog_world.hh"
#include "strata/core/work_pool_executor.hh"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace {

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
		o.name_format = "ctpost{:02}";
		return o;
	}
	WorkPool pool_;
	strata::work_pool_executor ex_;
};
using PW = world<pool_exec>;

// The scheduler the frame suspends on, destroyed by the test. Every post first runs `gate`, which holds the
// poster there; `posted_dead` records a post that came after the destruction.
class gate_sched {
public:
	static inline std::function<void()> gate;
	static inline std::atomic<bool> alive{false}, posted_dead{false};

	gate_sched() { alive = true; }
	~gate_sched() { alive = false; }
	void post(std::coroutine_handle<> h) {
		if (gate) { gate(); }
		if (!alive.load()) {
			posted_dead = true;
			(void)closed_.load();  // reads *this: freed (ASan)
			return;
		}
		std::lock_guard l(mu_);
		if (closed_.load()) { return; }  // task's phase 2 drops it
		q_.push_back(h);
	}
	std::uint64_t now() const noexcept { return 0; }
	void close() { closed_ = true; }

private:
	std::mutex mu_;
	std::deque<std::coroutine_handle<>> q_;
	std::atomic<bool> closed_{false};
};

struct hooked {
	std::function<void(const char*, bool)> fn;
	std::function<void(const char*, bool)> hook() {
		return [this](const char* site, bool locked) {
			if (fn) { fn(site, locked); }
		};
	}
};

task::task<void> pinner(cat_t* c, int* got) {
	try {
		auto p = co_await c->pin_request("k0");
		*got = p ? 1 : 2;
	} catch (...) {
		*got = 3;
	}
}

// `fail`: the open fails, so the delivery carries its error and the waiting abandon releases no pin.
void a_delivery_never_posts_to_a_torn_down_scheduler(bool fail) {
	std::mutex m;
	std::condition_variable cv;
	bool suspended = false, posting = false, released = false;
	const auto signal = [&](bool& flag) {
		{
			std::lock_guard l(m);
			flag = true;
		}
		cv.notify_all();
	};
	const auto until = [&](const bool& flag) {
		std::unique_lock l(m);
		cv.wait(l, [&] { return flag; });
	};
	hooked hk;
	hk.fn = [&](const char* site, bool) {
		if (std::strcmp(site, "abandon.posting") == 0) { signal(released); }  // abandon waits for the post: let it end
	};
	gate_sched::posted_dead = false;
	gate_sched::gate = [&] {
		signal(posting);
		until(released);
	};
	{
		PW w(1, 1, false, true, hk.hook());
		// The open resolves only once the frame suspended, so the waiter is still listed when the test looks.
		w.tweak = [&](cat_t::host& h) {
			h.resolve = [&until, &suspended, inner = std::move(h.resolve)](const cat_t::opening& op) {
				until(suspended);
				return inner(op);
			};
		};
		w.remake(0, 0, hk.hook());
		if (fail) {
			w.fail_resolve = EIO;
			w.fail_left = 1;
		}
		auto ts = std::make_unique<gate_sched>();
		int got = 0;
		task::task<void> t = pinner(w.cat.get(), &got);
		{
			const task::running_on on(task::scheduler_ref::of(*ts));
			t.handle().resume();
		}
		CHECK(got == 0 && w.cat->key_at(0) == "k0" && w.cat->waiters_of(0) == 1);
		signal(suspended);
		until(posting);         // the delivery is inside the waiter's post
		ts->close();            // phase 2
		t = task::task<void>{}; // phase 3: ~pin_awaiter abandons the waiter
		ts.reset();             // phase 4
		signal(released);
		w.cat->wait_chores();
		CHECK(!gate_sched::posted_dead.load());
		CHECK(got == 0 && cat_t::pins_of(w.cat->word_of(0)) == 0);
		CHECK(cat_t::state_of(w.cat->word_of(0)) == (fail ? strata::slot_state::failed : strata::slot_state::open));
		CHECK(w.cat->stats().gone_pins == (fail ? 0u : 1u));
	}
	gate_sched::gate = nullptr;
}

}  // namespace

int main() {
	a_delivery_never_posts_to_a_torn_down_scheduler(false);
	a_delivery_never_posts_to_a_torn_down_scheduler(true);
	if (failures) {
		std::fprintf(stderr, "catalog_post_lifetime: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_post_lifetime: ok");
	return 0;
}
