// strata::group's witness (cluster-release Q3 (a), consensus D9; docs/mechanisms/replication-group.md):
// a voting member that stores its group's log and no application state, on a loop beside the
// loop's groups. consensus acknowledges a witness's own ranges itself, refuses its proposals
// and reads, and hands off any leadership it wins to a data voter. The witness answers what
// reaches its Sink before it is a witness (a range below its promotion, an install while it
// joins) without applying any of it, and pins nothing for a peer's install.
#pragma once

#include "strata/group/group.hh"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace strata::group {

class witness final : public member {
public:
	struct config {
		cs::GroupId id = 0;
		cs::GroupOptions options;
		jn::Identity identity{};            // the journal group's, at create
		std::optional<std::string> closed;  // the last close's closed state, if the host kept it
	};

	witness(loop& l, config c) : loop_(&l), id_(c.id), opening_(std::make_unique<config>(std::move(c))) { loop_->attach(id_, *this); }
	witness(const witness&) = delete;
	witness& operator=(const witness&) = delete;
	~witness() override { loop_->detach(id_); }

	cs::GroupId id() const noexcept { return id_; }

	// Opens the witness's consensus group: recovers it when the host's table holds it, else
	// creates it. On the loop. A failed open fences it and closes its row.
	task::task<void> open() {
		if (task::current() != loop_->scheduler()) { co_await task::schedule_on{loop_->scheduler()}; }
		if (!opening_) { throw std::logic_error("strata::group: the witness was opened"); }
		auto& driver = loop_->driver();
		driver.open(id_, std::move(opening_->options));
		std::exception_ptr failed;
		try {
			// A restored image is a data member's bundle: the witness keeps none of it.
			if (const auto status = loop_->host().status(id_); status == jn::GroupStatus::stored || status == jn::GroupStatus::closed) {
				co_await driver.recover(id_, [](jn::ArtifactReader&) {}, std::move(opening_->closed));
			} else {
				co_await driver.create(id_, opening_->identity);
			}
		} catch (...) {
			failed = std::current_exception();
		}
		opening_.reset();
		if (!failed) { co_return; }
		fence_local(failed);
		co_await task::schedule_on{loop_->scheduler()};
		try {
			co_await loop_->driver().close(id_);
		} catch (...) {}
		std::rethrow_exception(failed);
	}

	// Closes the consensus group: its records stay in the host's log for a reopen.
	task::task<void> close() {
		if (task::current() != loop_->scheduler()) { co_await task::schedule_on{loop_->scheduler()}; }
		co_await loop_->driver().close(id_);
	}

	bool fenced() const noexcept { return fenced_.load(); }
	std::string fence_reason() const {
		std::lock_guard lock(mu_);
		return fence_reason_;
	}
	// What its Sink answered unapplied: ranges below its promotion, and images while it joins.
	std::uint64_t ranges() const noexcept { return ranges_.load(); }
	std::uint64_t images() const noexcept { return images_.load(); }

	// --- the loop's notices ---------------------------------------------------------------

	void committed(cs::Committed range) override {
		if (range.entries.empty()) { return; }
		ranges_.fetch_add(1);
		loop_->driver().applied(id_, cs::Applied{range.entries.back().index});
	}
	void fenced(std::string reason) override {
		std::lock_guard lock(mu_);
		if (fenced_.exchange(true)) { return; }
		fence_reason_ = std::move(reason);
	}
	// It leads only to hand off and holds no files: a learner's fetch of a pin finds none and
	// its validation fails until a data voter leads.
	void install_needed(cs::NodeId, cs::SnapshotKey) override {}
	void install_released(cs::NodeId, cs::SnapshotKey, cs::SnapshotReleaseReason) override {}
	// The image (a data member's bundle) becomes consensus's checkpoint, the base of the log
	// it stores; nothing else is kept.
	void validate(cs::Token token, cs::LogBoundary, cs::NodeId, jn::ArtifactReader&) override {
		images_.fetch_add(1);
		loop_->driver().validated(id_, token);
	}
	void activate(cs::Token token, cs::LogBoundary, jn::ArtifactReader&) override { loop_->driver().activated(id_, token); }

private:
	void fence_local(std::exception_ptr e) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception& x) {
			fenced(x.what());
		} catch (...) {
			fenced("unknown");
		}
	}

	loop* loop_;
	cs::GroupId id_;
	std::unique_ptr<config> opening_;
	mutable std::mutex mu_;
	std::atomic<bool> fenced_{false};
	std::string fence_reason_;
	std::atomic<std::uint64_t> ranges_{0}, images_{0};
};

}  // namespace strata::group
