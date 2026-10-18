// Background executors (ARCHITECTURE, "Scheduler concept"; Phase 3 design D11, D18). The
// engine owns no threads: the host passes an executor that runs posted work for groups of
// tasks, one group per index, and tells time in nanoseconds. Work observes a
// task::cancel_token, and long waits go through sleep_until(), so a cancel ends them.
//
// Two bindings: inline_executor runs work on Kronuz/task's inline_scheduler with its
// manual clock, deterministically, for tests and simulation; pool_executor.hh adapts a
// thread pool's owner-scoped task groups.
#pragma once

#include "task.hh"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace strata {

class executor {
public:
	// Runs once on the executor; `stop` is requested when its group cancels or the
	// executor shuts down. It reports through its own state: what it throws is dropped.
	using work = std::function<void(const task::cancel_token& stop)>;

	// One owner's work. cancel() drops the queued work, requests `stop` for the running
	// work, and waits for it; work posted later runs normally. wait() returns once none of
	// the group's work is queued or running. Ending a group cancels it.
	class group {
	public:
		virtual ~group() = default;
		virtual void post(work w) = 0;
		virtual void cancel() noexcept = 0;
		virtual void wait() = 0;
	};

	virtual ~executor() = default;
	virtual std::unique_ptr<group> open() = 0;
	// A group running at most `width` of its work at once, for work that fans out (bisection's
	// parts); an executor that runs nothing in parallel returns open().
	virtual std::unique_ptr<group> open_bounded(std::size_t width) {
		(void)width;
		return open();
	}
	virtual std::uint64_t now() = 0;  // nanoseconds
	// Waits until now() reaches t; false, early, once `stop` is requested.
	virtual bool sleep_until(std::uint64_t t, const task::cancel_token& stop) = 0;
	// A point where work may yield (a stepping executor resumes it later); false once
	// `stop` is requested.
	virtual bool checkpoint(const task::cancel_token& stop) { return !stop.requested(); }
};

// Work on a task::inline_scheduler: it runs when run() drains the scheduler (a group's
// wait() does), in posting order across groups, and sleep_until() advances the manual
// clock. Single-threaded and deterministic.
class inline_executor final : public executor {
public:
	inline_executor() = default;
	inline_executor(const inline_executor&) = delete;
	inline_executor& operator=(const inline_executor&) = delete;
	// Every group must have ended; their dropped work's frames still queued finish here.
	~inline_executor() override { s_.run(); }

	std::unique_ptr<group> open() override { return std::make_unique<inline_group>(*this); }
	std::uint64_t now() override { return s_.now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		if (!stop.requested() && t > s_.now()) { s_.advance(t - s_.now()); }
		return !stop.requested();
	}
	// Runs every queued work item; returns how many coroutine resumptions ran.
	std::size_t run() { return s_.run(); }
	std::size_t queued() const noexcept { return s_.queued(); }
	task::inline_scheduler& scheduler() noexcept { return s_; }

private:
	struct entry {
		work fn;
		task::cancel_token stop;
	};
	static task::task<void> start(std::shared_ptr<entry> e) {
		if (work fn = std::exchange(e->fn, nullptr)) {
			try {
				fn(e->stop);
			} catch (...) {
			}
		}
		co_return;
	}
	class inline_group final : public group {
	public:
		explicit inline_group(inline_executor& e) : e_(&e) {}
		~inline_group() override { cancel(); }
		void post(work w) override {
			std::erase_if(queued_, [](const auto& q) { return q.expired(); });
			auto en = std::make_shared<entry>(entry{std::move(w), source_.token()});
			queued_.push_back(en);
			task::spawn(task::scheduler_ref::of(e_->s_), start(std::move(en)));
		}
		// Nothing of a group runs while its owner runs, so nothing waits here.
		void cancel() noexcept override {
			source_.request();
			for (const auto& q : queued_) {
				if (auto en = q.lock()) { en->fn = nullptr; }
			}
			queued_.clear();
			source_ = task::cancel_source();
		}
		void wait() override { e_->run(); }

	private:
		inline_executor* e_;
		task::cancel_source source_;
		std::vector<std::weak_ptr<entry>> queued_;
	};

	task::inline_scheduler s_;
};

}  // namespace strata
