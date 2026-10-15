// The node's merge scheduler (Phase 3 design D11, D18): worker threads that run every
// index's background merges, and one token bucket that limits their writes to a number of
// bytes per second across the node. An index draws on it through a queue, whose end
// cancels its queued merges and waits for its running ones. The host owns it, like the
// budgets, and it must outlive every index that uses it.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace strata {

struct merge_scheduler_options {
	std::size_t threads = 1;             // worker threads (at least one)
	std::uint64_t bytes_per_second = 0;  // merge writes across the node; 0: unlimited
};

class merge_scheduler {
	struct lane {
		std::size_t outstanding = 0;  // queued and running tasks, under mu_
		std::atomic<bool> stop{false};
		std::condition_variable idle;
	};

public:
	using options = merge_scheduler_options;
	// A task runs on a worker thread; `stop` turns true when its queue cancels. It reports
	// through its own state: what it throws is dropped.
	using task = std::function<void(const std::atomic<bool>& stop)>;

	explicit merge_scheduler(const options& o = {}) : rate_(o.bytes_per_second) {
		try {
			for (std::size_t i = 0; i < std::max<std::size_t>(1, o.threads); ++i) { workers_.emplace_back([this] { work(); }); }
		} catch (...) {
			shut_down();
			throw;
		}
	}
	merge_scheduler(const merge_scheduler&) = delete;
	merge_scheduler& operator=(const merge_scheduler&) = delete;
	~merge_scheduler() { shut_down(); }

	// One index's tasks: move-only; cancel() or its end drops the queued ones and waits for
	// the running ones, which see `stop`.
	class queue {
	public:
		queue() = default;
		queue(queue&& o) noexcept : s_(std::exchange(o.s_, nullptr)), lane_(std::move(o.lane_)) {}
		queue& operator=(queue&& o) noexcept {
			if (this != &o) {
				cancel();
				s_ = std::exchange(o.s_, nullptr);
				lane_ = std::move(o.lane_);
			}
			return *this;
		}
		~queue() { cancel(); }

		void submit(task t) { s_->submit(lane_, std::move(t)); }
		// Waits until none of its tasks is queued or running.
		void wait() {
			if (!s_) { return; }
			std::unique_lock l(s_->mu_);
			lane_->idle.wait(l, [&] { return lane_->outstanding == 0; });
		}
		void cancel() noexcept {
			if (s_) { s_->cancel(*lane_); }
		}
		explicit operator bool() const noexcept { return s_ != nullptr; }

	private:
		friend class merge_scheduler;
		explicit queue(merge_scheduler* s) : s_(s), lane_(std::make_shared<lane>()) {}
		merge_scheduler* s_ = nullptr;
		std::shared_ptr<lane> lane_;
	};
	queue open() { return queue(this); }

	// Waits for the bucket to admit `bytes` of writes, at the configured rate across every
	// task; false, admitting nothing, once `stop` turns true.
	bool throttle(std::uint64_t bytes, const std::atomic<bool>& stop) {
		if (stop.load(std::memory_order_acquire)) { return false; }
		if (rate_) {
			std::unique_lock l(rate_mu_);
			const auto cost = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(bytes) / double(rate_)));
			next_ = std::max(next_, clock::now()) + cost;
			const auto until = next_;
			if (rate_cv_.wait_until(l, until, [&] { return stop.load(std::memory_order_acquire); })) { return false; }
		}
		written_.fetch_add(bytes, std::memory_order_relaxed);
		return true;
	}
	// The bytes a task writes between throttle() calls: a tenth of a second's worth.
	std::size_t chunk() const noexcept { return rate_ ? std::size_t(std::clamp<std::uint64_t>(rate_ / 10, 4096, 1 << 20)) : std::size_t(1) << 20; }
	std::uint64_t bytes_per_second() const noexcept { return rate_; }
	std::uint64_t bytes_written() const noexcept { return written_.load(std::memory_order_relaxed); }

private:
	using clock = std::chrono::steady_clock;
	struct entry {
		std::shared_ptr<lane> owner;
		task run;
	};

	void submit(const std::shared_ptr<lane>& ln, task t) {
		{
			std::lock_guard l(mu_);
			tasks_.push_back({ln, std::move(t)});
			++ln->outstanding;
		}
		work_cv_.notify_one();
	}
	void cancel(lane& ln) noexcept {
		std::unique_lock l(mu_);
		for (auto it = tasks_.begin(); it != tasks_.end();) {
			if (it->owner.get() == &ln) {
				it = tasks_.erase(it);
				--ln.outstanding;
			} else {
				++it;
			}
		}
		{
			// Under the bucket's lock, so a task about to wait in throttle() sees it.
			std::lock_guard g(rate_mu_);
			ln.stop.store(true, std::memory_order_release);
		}
		rate_cv_.notify_all();
		ln.idle.wait(l, [&] { return ln.outstanding == 0; });
		ln.stop.store(false, std::memory_order_release);
	}
	void work() {
		std::unique_lock l(mu_);
		for (;;) {
			work_cv_.wait(l, [&] { return stopping_ || !tasks_.empty(); });
			if (stopping_) { return; }
			entry e = std::move(tasks_.front());
			tasks_.pop_front();
			l.unlock();
			try {
				e.run(e.owner->stop);
			} catch (...) {
			}
			e.run = nullptr;  // its captures go before the queue sees it finished
			l.lock();
			if (--e.owner->outstanding == 0) { e.owner->idle.notify_all(); }
		}
	}
	void shut_down() noexcept {
		{
			std::lock_guard l(mu_);
			stopping_ = true;
		}
		work_cv_.notify_all();
		for (auto& t : workers_) { t.join(); }
		workers_.clear();
	}

	std::mutex mu_;  // before rate_mu_ when both are held
	std::condition_variable work_cv_;
	std::deque<entry> tasks_;
	bool stopping_ = false;
	std::vector<std::thread> workers_;
	const std::uint64_t rate_;
	std::mutex rate_mu_;
	std::condition_variable rate_cv_;
	clock::time_point next_{};  // when the bucket has admitted every write so far
	std::atomic<std::uint64_t> written_{0};
};

}  // namespace strata
