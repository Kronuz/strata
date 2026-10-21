// A threaded executor over Kronuz/threadpool (Phase 3 design D18). strata starts no
// threads: the host owns one pool per node, like the budgets, and each index's merges go
// to a TaskGroup of their own on it, which serves groups round-robin. The stop flag and
// the clock are this adapter's, so a cancel or a shutdown also ends sleeps.
#pragma once

#include "strata/core/executor.hh"
#include "strata/core/owner.hh"
#include "task_group.hh"
#include "threadpool.hh"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <set>
#include <utility>

namespace strata {

template <typename Pool = ThreadPool<>>
class pool_executor final : public executor {
public:
	// `per_group` caps how many of one index's merges run at once; 1 keeps an index to one
	// merge at a time, so a node's merges spread across its indexes. The pool outlives the
	// executor and every group.
	explicit pool_executor(Pool& pool, std::size_t per_group = 1) : pool_(&pool), per_group_(per_group) {}
	pool_executor(const pool_executor&) = delete;
	pool_executor& operator=(const pool_executor&) = delete;
	// Every group must have ended; a running task still sees its stop.
	~pool_executor() override { stop_all(); }

	std::unique_ptr<group> open() override { return std::make_unique<pool_group>(*this); }
	std::uint64_t now() override {
		return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		const std::chrono::steady_clock::time_point until(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(t)));
		std::unique_lock l(mu_);
		cv_.wait_until(l, until, [&] { return stop.requested(); });
		return !stop.requested();
	}
	// Shutdown: requests every group's stop, so running work ends at its next check.
	void stop_all() noexcept {
		{
			std::lock_guard l(mu_);
			for (auto* g : groups_) { g->source_.request(); }
		}
		cv_.notify_all();
	}

private:
	class pool_group final : public group {
	public:
		explicit pool_group(pool_executor& e) : e_(&e), g_(*e.pool_, e.per_group_) {
			std::lock_guard l(e_->mu_);
			e_->groups_.insert(this);
		}
		~pool_group() override {
			cancel();
			std::lock_guard l(e_->mu_);
			e_->groups_.erase(this);
		}
		void post(work w) override {
			task::cancel_token stop;
			{
				std::lock_guard l(e_->mu_);
				stop = source_.token();
			}
			g_.enqueue([w = std::move(w), stop]() mutable {
				// A worker owns no index: the work, and what it holds, end inside the mark (owner.hh).
				const off_owner_scope worker;
				work run = std::exchange(w, nullptr);
				try {
					run(stop);
				} catch (...) {
				}
			});
		}
		void cancel() noexcept override {
			{
				// Under the executor's lock, so a sleeper cannot miss it.
				std::lock_guard l(e_->mu_);
				source_.request();
			}
			e_->cv_.notify_all();
			// Running work must not outlive its owner's state: no wait, no process.
			try {
				g_.cancel_and_wait();
			} catch (...) {
				std::terminate();
			}
			std::lock_guard l(e_->mu_);
			source_ = task::cancel_source();
		}
		void wait() override { g_.wait(); }

	private:
		friend class pool_executor;
		pool_executor* e_;
		TaskGroup<Pool> g_;
		task::cancel_source source_;  // under e_->mu_
	};

	Pool* pool_;
	std::size_t per_group_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::set<pool_group*> groups_;
};

}  // namespace strata
