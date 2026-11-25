// The write path at a group's leader (cluster-release Q10 (b), PLAN.md D106), beside the rule
// in answer.hh so a different ruling edits these two files: a request is prepared from the
// leader's applied state at the ready point plus its own commands not applied yet, proposed at
// `quorum`, and prepared again while `after` says so; a replica that is not the leader forwards
// it once, carrying its placement count.
#pragma once

#include "strata/group/group.hh"

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace strata::group {

// What a request becomes at the leader: the command and whether its expectations are the
// leader's (a merge's chosen version) rather than the client's own condition. `refused`: the
// client's condition already fails at prepare (409), nothing is placed.
struct prepared {
	std::string command;
	bool leader_chose = false;
	std::optional<verdict> refused;
};

// A request's end: `reply` with the last placement's answer (or the refusal at prepare), or
// `unavailable` once the bound is spent or a forwarded request finds no leader (a retryable 503).
struct submitted {
	next_step outcome = next_step::reply;
	std::optional<answer> reply;
	std::optional<verdict> refused;
	unsigned placements = 0;  // this request's, forwards included
	bool forwarded = false;
};

template <class G>
class writer {
public:
	explicit writer(G& g) : g_(&g) {}
	writer(const writer&) = delete;
	writer& operator=(const writer&) = delete;

	// `prepare(index, pending) -> prepared` runs on the chore once this replica applied the read
	// index (the ready point: past its term's first entry); `pending` holds this leader's commands
	// of its term prepared and not applied, in prepare order. `forward(placements) ->
	// task<submitted>` sends the original request to the leader, which submits it with
	// `forwarded` set. superseded, closed and fenced propagate as they are.
	template <class Prepare, class Forward>
	task::task<submitted> submit(Prepare prepare, Forward forward, unsigned placements = 0, bool forwarded = false) {
		for (;;) {
			std::optional<prepared> p;
			typename std::list<own>::iterator mine;
			bool deposed = false, unready = false;
			try {
				const auto term = current_term();
				p.emplace(co_await g_->read_applied([&](typename G::index_type& ix) {
					auto out = prepare(ix, pending(term));
					if (!out.refused) { mine = add(std::string(out.command), term); }
					return out;
				}));
			} catch (const cs::rejected& r) {
				if (r.reason == cs::RejectReason::NotReady || r.reason == cs::RejectReason::Busy) {
					unready = true;
				} else if (r.reason == cs::RejectReason::NotLeader) {
					deposed = true;
				} else {
					throw;
				}
			}
			// Before the ready point nothing is prepared: retryable, nothing placed.
			if (unready) { co_return submitted{next_step::unavailable, std::nullopt, std::nullopt, placements, forwarded}; }
			if (!deposed && p->refused) { co_return submitted{next_step::reply, std::nullopt, p->refused, placements, forwarded}; }
			std::optional<answer> a;
			std::exception_ptr failed;
			if (!deposed) {
				try {
					a.emplace(co_await g_->propose(std::move(p->command), ack::quorum, {}, [this, mine](cs::Placement at) { placed(mine, at); }));
				} catch (const cs::rejected& r) {
					if (r.reason == cs::RejectReason::NotLeader) {
						deposed = true;
					} else {
						failed = std::current_exception();
					}
				} catch (...) {
					failed = std::current_exception();
				}
				drop(mine);
			}
			if (failed) { std::rethrow_exception(failed); }
			if (deposed) {
				if (forwarded) { co_return submitted{next_step::unavailable, std::nullopt, std::nullopt, placements, true}; }
				auto s = co_await forward(placements);
				s.forwarded = true;
				co_return s;
			}
			++placements;
			const auto n = after(*a->applied, p->leader_chose, placements);
			if (n != next_step::resubmit) { co_return submitted{n, std::move(a), std::nullopt, placements, forwarded}; }
		}
	}

private:
	struct own {
		std::string command;
		cs::Term term = 0;
		cs::Index index = 0;  // 0 until placed
	};

	cs::Term current_term() const {
		auto s = g_->owner().driver().state(g_->id());
		return s ? s->term : 0;
	}
	// On the chore: this term's commands not applied, the placed ones above applied.
	std::vector<std::string> pending(cs::Term term) {
		std::lock_guard lock(mu_);
		std::vector<std::string> out;
		const auto applied = g_->applied();
		for (const auto& o : pending_) {
			if (o.term == term && (o.index == 0 || o.index > applied)) { out.push_back(o.command); }
		}
		return out;
	}
	typename std::list<own>::iterator add(std::string command, cs::Term term) {
		std::lock_guard lock(mu_);
		return pending_.insert(pending_.end(), own{std::move(command), term, 0});
	}
	void placed(typename std::list<own>::iterator it, cs::Placement at) {
		std::lock_guard lock(mu_);
		it->term = at.term;
		it->index = at.index;
	}
	void drop(typename std::list<own>::iterator it) {
		std::lock_guard lock(mu_);
		pending_.erase(it);
	}

	G* g_;
	std::mutex mu_;
	std::list<own> pending_;
};

}  // namespace strata::group
