// An executor over Kronuz/threadpool's WorkPool (Phase 7c design D4). strata starts no threads:
// the host owns one WorkPool per node, and each group the engine opens (an index's merges, its
// reclaim deletes) is a WorkGroup on it, which takes fair turns with the host's other groups.
// All of strata's background work is merge-class work there: it runs behind queries and flushes,
// ages past a flush flood, and never takes the pool's reserve workers.
// The stop flag and the clock are this adapter's, so a cancel or a shutdown also ends sleeps.
// close(), the destructor, drain() and wait_deletes() wait for a group's work, so the pool must
// keep running until its indexes are closed (executor.hh). A wait off the pool's workers reaches
// WorkPool::Options::block_guard first, where a host aborts for an event loop. The engine waits only
// from an index's owner (a host chore) on the index's own groups, never from those groups' work, so
// a wait on a worker serves the awaited group's items inline (inline depth 2 under Xapiand).
#pragma once

#include "strata/core/executor.hh"
#include "strata/core/owner.hh"
#include "work_pool.hh"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace strata {

class work_pool_executor final : public executor {
public:
	// `per_group` caps how many of one index's items run at once; 1 keeps an index to one merge at a
	// time, so a node's merges spread across its indexes. The pool outlives the executor and every group.
	explicit work_pool_executor(WorkPool& pool, unsigned per_group = 1) : pool_(&pool), per_group_(per_group) {}
	work_pool_executor(const work_pool_executor&) = delete;
	work_pool_executor& operator=(const work_pool_executor&) = delete;
	// Every group must have ended; a running item still sees its stop.
	~work_pool_executor() override { stop_all(); }

	std::unique_ptr<group> open() override { return std::make_unique<work_group>(*this); }
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
	// The caller-owned item of one posted work: the pool links it while queued and runs it once; a
	// cancel hands the queued ones back, to be freed here.
	struct item final : WorkItem {
		work fn;
		task::cancel_token stop;
		static void invoke(WorkItem* w) noexcept {
			const std::unique_ptr<item> self(static_cast<item*>(w));
			// Background work holds no lease: the work, and what it holds, end inside the mark (owner.hh).
			const off_owner_scope worker;
			try {
				self->fn(self->stop);
			} catch (...) {
			}
		}
	};

	class work_group final : public group {
	public:
		explicit work_group(work_pool_executor& e) : e_(&e), g_(*e.pool_, e.per_group_) {
			std::lock_guard l(e_->mu_);
			e_->groups_.insert(this);
		}
		~work_group() override {
			cancel();
			std::lock_guard l(e_->mu_);
			e_->groups_.erase(this);
		}
		void post(work w) override {
			auto it = std::make_unique<item>();
			it->run = &item::invoke;
			it->cls = WorkClass::merge;
			it->fn = std::move(w);
			{
				std::lock_guard l(e_->mu_);
				it->stop = source_.token();
			}
			g_.post(*it.release());
		}
		void cancel() noexcept override {
			// Queued items leave first, so none starts once the stop is requested; they never run.
			WorkItem* dropped = g_.cancel();
			{
				// Under the executor's lock, so a sleeper cannot miss it.
				std::lock_guard l(e_->mu_);
				source_.request();
			}
			e_->cv_.notify_all();
			while (dropped) {
				WorkItem* next = dropped->next;
				delete static_cast<item*>(dropped);
				dropped = next;
			}
			// Running work must not outlive its owner's state: no wait, no process.
			g_.wait();
			std::lock_guard l(e_->mu_);
			source_ = task::cancel_source();
		}
		void wait() override { g_.wait(); }

	private:
		friend class work_pool_executor;
		work_pool_executor* e_;
		WorkGroup g_;
		task::cancel_source source_;  // under e_->mu_
	};

	WorkPool* pool_;
	unsigned per_group_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::set<work_group*> groups_;
};

}  // namespace strata
