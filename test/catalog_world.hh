// Fixtures for the catalog's tests (docs/design/cold-tier.md D4; S4): directories on faultfs, each a
// committed index; a kernel model and a budget behind counted_io; a catalog over them.
#pragma once

#include "counted_hooks.hh"
#include "cold_hooks.hh"
#include "strata/core/catalog.hh"
#include "strata/core/executor.hh"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// H: the serial says which open made it. Its destructor counts itself and whether a close chore was
// running on its thread (S7: H is destroyed after its close chore returns).
struct payload {
	int serial = 0;
	payload() = default;
	explicit payload(int s) : serial(s) {}
	payload(payload&& o) noexcept : serial(std::exchange(o.serial, 0)) {}
	payload& operator=(payload&& o) noexcept {
		serial = std::exchange(o.serial, 0);
		return *this;
	}
	~payload();
	static inline std::atomic<int> destroyed{0}, destroyed_in_close{0};
};

using cat_t = strata::catalog<kv_index, payload>;

inline payload::~payload() {
	if (serial != 0) {
		++destroyed;
		if (cat_t::in_close_chore()) { ++destroyed_in_close; }
	}
}

// The host's timer over a thread (executors with a real clock): `fn` runs on the timer's thread after
// `delay_ns` of steady time, and only posts a chore.
class timer_thread {
public:
	timer_thread() : t_([this] { loop(); }) {}
	~timer_thread() {
		{
			std::lock_guard l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		t_.join();
	}
	void add(std::uint64_t delay_ns, std::function<void()> fn) {
		{
			std::lock_guard l(m_);
			q_.emplace(std::chrono::steady_clock::now() + std::chrono::nanoseconds(delay_ns), std::move(fn));
		}
		cv_.notify_all();
	}

private:
	void loop() {
		std::unique_lock l(m_);
		for (;;) {
			if (q_.empty()) {
				if (stop_) { return; }
				cv_.wait(l);
			} else if (q_.begin()->first <= std::chrono::steady_clock::now() || stop_) {
				auto fn = std::move(q_.begin()->second);
				q_.erase(q_.begin());
				l.unlock();
				fn();
				l.lock();
			} else {
				cv_.wait_until(l, q_.begin()->first);
			}
		}
	}
	std::mutex m_;
	std::condition_variable cv_;
	std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> q_;
	bool stop_ = false;
	std::thread t_;
};

// `n` directories "k0".."k<n-1>" each holding a committed index of `segments` segments.
template <class Exec>
struct world {
	struct dir {
		faultfs::filesystem fs;
		faultfs::io io{fs};
		dir() { fs.map_files(true); }
	};
	std::deque<dir> dirs;
	kernel kern;
	resource_budget budget;
	Exec exec;
	std::map<std::string, int, std::less<>> alias;  // key -> directory, when it is not "k<N>"
	std::atomic<int> resolves{0}, made{0};
	std::atomic<int> fail_resolve{0};  // errno the next resolves fail with (while > 0)
	std::atomic<int> fail_left{0};     // resolves left that fail
	// The host's timer, injected on inline_executor: callbacks wait here until a test fires them, in
	// due order, advancing the manual clock to the due time (item 4's reposts).
	struct timer_entry {
		std::uint64_t due;
		std::function<void()> fn;
	};
	std::mutex timer_mu;
	std::vector<timer_entry> timers;
	std::vector<std::uint64_t> timer_delays;  // every delay asked for, in order
	std::atomic<int> commits{0};              // the host's commit chore ran
	std::atomic<int> fail_commit{0};          // errno the commit chore fails with (while > 0)
	std::function<void()> commit_gate;        // called at the start of every commit chore (a barrier for threads)
	std::atomic<int> fail_commit_serial{0};   // when > 0, only the index whose H has this serial fails it
	std::function<void(typename cat_t::host&)> tweak;  // applied to the host before the catalog is made
	kv_index::options iopts;
	unsigned bound;  // the host's wake_bound
	std::unique_ptr<timer_thread> tthread;  // before `cat`: it outlives the catalog
	std::unique_ptr<cat_t> cat;

	static resource_options ropts(std::uint64_t fds = 1 << 20) {
		resource_options o;
		o.system = test_system(fds, 1 << 20, 1 << 20);
		o.descriptor_reserve = 64;
		return o;
	}
	world(std::size_t n, std::size_t segments, bool defer_io, bool eager = true, std::function<void(const char*, bool)> hook = {}, unsigned wake_bound = 8, std::uint64_t fds = 1 << 20) : budget(ropts(fds)), bound(wake_bound) {
		iopts.keyed = true;
		iopts.eager_segments = eager;
		iopts.auto_merge = false;
		iopts.defer_io = defer_io;
		for (std::size_t i = 0; i < n; ++i) {
			dirs.emplace_back();
			auto o = iopts;
			o.readers = nullptr;
			auto ix = kv_index::create(dirs.back().io, o, uuid_of(("cat" + std::to_string(i)).c_str()));
			fill(ix, segments);
			ix.close();
		}
		make_catalog(std::move(hook));
	}
	void make_catalog(std::function<void(const char*, bool)> hook) {
		typename cat_t::host h;
		h.exec = &exec;
		h.budget = &budget;
		h.index_options = iopts;
		h.before_cas = std::move(hook);
		h.wake_bound = bound;
		h.resolve = [this](std::string_view key, io_counts c) -> std::unique_ptr<kronuz::journal::IO> {
			++resolves;
			if (fail_left.load() > 0 && fail_left.fetch_sub(1) > 0) { throw std::system_error(fail_resolve.load(), std::generic_category(), "resolve"); }
			int d;
			if (auto it = alias.find(key); it != alias.end()) {
				d = it->second;
			} else {
				d = std::atoi(std::string(key.substr(1)).c_str());
			}
			return counted_io::make(c, [&] { return probe_io::make(kern, dirs[d].io); });
		};
		h.make_payload = [this](std::string_view) { return payload{++made}; };
		h.commit = [this](payload& p, kv_index& ix) {
			++commits;
			if (commit_gate) { commit_gate(); }
			if (fail_commit.load() > 0 && (fail_commit_serial.load() == 0 || fail_commit_serial.load() == p.serial)) { throw std::system_error(fail_commit.load(), std::generic_category(), "commit"); }
			ix.commit();
		};
		if constexpr (!std::is_same_v<Exec, strata::inline_executor>) {
			if (!tthread) { tthread = std::make_unique<timer_thread>(); }
			h.timer = [this](std::uint64_t delay, std::function<void()> fn) { tthread->add(delay, std::move(fn)); };
		}
		if constexpr (std::is_same_v<Exec, strata::inline_executor>) {
			h.timer = [this](std::uint64_t delay, std::function<void()> fn) {
				std::lock_guard l(timer_mu);
				timer_delays.push_back(delay);
				timers.push_back({exec.now() + delay, std::move(fn)});
			};
		}
		if (tweak) { tweak(h); }
		cat = std::make_unique<cat_t>(std::move(h));
	}
	~world() {
		// Close what is open so the destructors find no live index.
		drain();
		finish();
	}
	// shutdown() as a task on the inline scheduler (its closure lives in the world, so the coroutine can outlive
	// the call); `sd_done` and `sd_res` tell how it went. Only for an inline executor.
	std::function<task::task<void>()> sd_body;
	bool sd_done = false;
	strata::shutdown_result sd_res;
	void start_shutdown() {
		sd_done = false;
		sd_body = [this]() -> task::task<void> {
			sd_res = co_await cat->shutdown();
			sd_done = true;
		};
		task::spawn(task::scheduler_ref::of(exec.scheduler()), sd_body());
	}
	// The host's duty before it destroys the catalog: shutdown(), which commits what is left and closes
	// every slot. A slot a test left pinned keeps it from completing, and the catalog's destructor says so.
	void finish() {
		if constexpr (std::is_same_v<Exec, strata::inline_executor>) {
			if (!cat || cat->shut_down()) { return; }
			cat->stop_sweep();
			start_shutdown();
			settle();
		}
	}
	std::size_t timers_pending() {
		std::lock_guard l(timer_mu);
		return timers.size();
	}
	// Fires the earliest timer (its callback posts a chore), the clock advanced to its due time; false
	// when none is armed. The chore runs at the next exec.run().
	bool fire_next() {
		timer_entry t;
		{
			std::lock_guard l(timer_mu);
			if (timers.empty()) { return false; }
			auto it = std::min_element(timers.begin(), timers.end(), [](const auto& a, const auto& b) { return a.due < b.due; });
			t = std::move(*it);
			timers.erase(it);
		}
		if constexpr (std::is_same_v<Exec, strata::inline_executor>) {
			if (t.due > exec.now()) { exec.scheduler().advance(t.due - exec.now()); }
		}
		t.fn();
		return true;
	}
	// Runs the executor and fires timers until neither has anything left.
	void settle() {
		for (int i = 0; i < 10000; ++i) {
			exec.run();
			if (!fire_next()) { break; }
		}
		exec.run();
	}
	// Closes every idle slot; what is pinned stays (a test bug).
	void drain() {
		if (!cat) { return; }
		for (int round = 0; round < 8; ++round) {
			bool any = false;
			if constexpr (std::is_same_v<Exec, strata::inline_executor>) { settle(); }
			cat->wait_chores();
			for (std::uint32_t i = 0; i < cat->slots_made(); ++i) {
				const auto st = cat_t::state_of(cat->word_of(i));
				if (st == slot_state::open) { any |= cat->begin_demote(i); }
				if (st == slot_state::owed) { any |= cat->begin_close(i); }
			}
			cat->wait_chores();
			for (std::uint32_t i = 0; i < cat->slots_made(); ++i) {
				if (cat_t::state_of(cat->word_of(i)) == slot_state::open) { any |= cat->begin_close(i); }
			}
			cat->wait_chores();
			if (!any) { break; }
		}
	}
};

}  // namespace
