// Repair (cluster-release D6, D7; contract strata:placement-repair;
// docs/mechanisms/placement-repair.md): the root keeps every shard at its rf. A pass plans one step
// per group from the root's records, its leader's report and the keepalive table: record the group
// as it is, add a learner, promote one (beside the member it replaces), remove a member, or move
// leadership to the preferred leader. Planning is pure; a binding runs each step on its group
// (consensus's change_membership, transfer_leadership) or on the root (record_shard).
#pragma once

#include "strata/placement/root.hh"

#include "consensus/membership.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace strata::placement {

namespace cs = kronuz::consensus;

// --- the root's side: settings, rf, drain, the record of a group ------------------------

inline settings_record settings(const view& v) {
	const auto s = v.get(settings_key());
	return s ? decode_settings(s->bytes) : settings_record{};
}

// The cluster's replace_after, changed at runtime: the next pass reads it.
inline group::prepared set_replace_after(const view& v, std::uint64_t ms) {
	const auto cur = v.get(settings_key());
	settings_record s = cur ? decode_settings(cur->bytes) : settings_record{};
	s.replace_after_ms = ms;
	command c;
	c.writes.push_back(detail::put(settings_key(), cur ? cur->version : 0, encode(s)));
	return detail::chosen(c);
}

// A namespace's rf (Xapiand's number_of_replicas + 1): repair grows or shrinks its shards to it.
inline group::prepared set_rf(const view& v, std::string_view path, std::uint32_t rf) {
	if (!valid_path(path) || rf == 0) { return detail::refuse(verdict::invalid); }
	const auto cur = v.namespace_at(path);
	if (!cur) { return detail::refuse(verdict::version_conflict, 0); }
	auto next = cur->record;
	next.rf = rf;
	return replace_namespace(v, path, next);
}

// A node drained: `leaving`, so repair moves its members off and placement skips it. It is
// drained once its load holds no group; remove_node then removes it.
inline group::prepared drain(const view& v, node_id id) {
	const auto n = v.node(id);
	if (!n) { return detail::refuse(verdict::version_conflict, 0); }
	auto r = n->record;
	r.state = node_state::leaving;
	return update_node(v, id, r);
}
inline bool drained(const view& v, node_id id) {
	const auto n = v.node(id);
	const auto l = v.load(id);
	return n && l && n->record.state != node_state::active && l->record.groups == 0;
}

// One shard's record replaced by `s` (matched by group id), the loads following.
inline group::prepared record_shard(const view& v, std::string_view path, const shard& s) {
	if (!valid_path(path)) { return detail::refuse(verdict::invalid); }
	const auto cur = v.namespace_at(path);
	if (!cur) { return detail::refuse(verdict::version_conflict, 0); }
	auto next = cur->record;
	auto it = std::find_if(next.shards.begin(), next.shards.end(), [&](const shard& x) { return x.group == s.group; });
	if (it == next.shards.end()) { return detail::refuse(verdict::version_conflict, cur->version); }
	it->members = s.members;
	return replace_namespace(v, path, next);
}

// --- what a pass reads --------------------------------------------------------------------

// The keepalive table (D6), volatile at the root leader: a node absent is up.
struct liveness {
	std::map<node_id, std::uint64_t> down_since;  // ms, the root leader's clock
};

// A node as repair sees it: its record's state and load, and its liveness.
struct node_status {
	node_state state = node_state::active;
	std::uint64_t groups = 0, leaders = 0;
	bool down = false;  // missing from the keepalive table
	bool lost = false;  // down for replace_after, or not active: its members are replaced
};
using node_table = std::map<node_id, node_status>;

inline node_table nodes_of(const view& v, const liveness& l, std::uint64_t now) {
	const auto after = settings(v).replace_after_ms;
	node_table out;
	v.scan(nodes_begin(), nodes_end(), [&](std::string_view k, std::uint64_t, std::string_view bytes) {
		const auto id = *node_of(k);
		node_status s;
		s.state = decode_node(bytes).state;
		if (const auto ld = v.load(id)) {
			s.groups = ld->record.groups;
			s.leaders = ld->record.leaders;
		}
		if (const auto d = l.down_since.find(id); d != l.down_since.end()) {
			s.down = true;
			s.lost = now >= d->second && now - d->second >= after;
		}
		s.lost = s.lost || s.state != node_state::active;
		out.emplace(id, s);
	});
	return out;
}

// A group as its leader reports it: no report, no leader (an rf = 1 group whose node is down).
struct report {
	cs::Membership membership;  // in force at the leader
	cs::NodeId leader = 0;      // the leader's incarnation
	bool open_command = false;  // the application holds an open multi-entry command (D12)
};

// --- the plan -----------------------------------------------------------------------------

// The shape a shard keeps on `usable` nodes (placement.hh's): rf data voters, never a lasting
// two-voter group (D7): two take a witness on a third node, else the second is a learner.
struct shape {
	std::size_t data = 0, witnesses = 0, learners = 0;
	bool operator==(const shape&) const = default;
};
inline shape target(std::uint32_t rf, std::size_t usable) {
	const auto data = std::min<std::size_t>(rf, usable);
	if (data == 2) { return usable >= 3 ? shape{2, 1, 0} : shape{1, 0, 1}; }
	return shape{data, 0, 0};
}

struct step {
	enum kind_type : std::uint8_t { none, wait, record, change, transfer };
	kind_type kind = none;
	const char* why = "";    // none and wait
	shard recorded;          // record: the shard as the root records it next
	cs::Membership next;     // change: the membership proposed next
	node_id joining = 0;     // change adding a learner: the node that opens it as a joiner
	cs::NodeId target = 0;   // transfer: the incarnation leadership moves to
};

inline role role_of(cs::MemberRole r) {
	return r == cs::MemberRole::Voter ? role::voter : r == cs::MemberRole::Witness ? role::witness : role::learner;
}

namespace detail {

struct planner {
	const shard* recorded;
	std::uint32_t rf;
	const report& r;
	const node_table& nodes;
	const cs::Membership& m;

	const node_status* status(node_id n) const {
		const auto it = nodes.find(n);
		return it == nodes.end() ? nullptr : &it->second;
	}
	bool healthy(const cs::Member& x) const {
		const auto* s = status(x.node);
		return s && !s->lost;
	}
	bool up(const cs::Member& x) const {
		const auto* s = status(x.node);
		return s && !s->down;
	}
	const cs::Member* on_node(node_id n) const {
		for (const auto& x : m.members) {
			if (x.node == n) { return &x; }
		}
		return nullptr;
	}

	// The preferred leader: the recorded one while a healthy voter, unless another healthy voter
	// has two fewer leaderships; else the healthy voter with the fewest.
	node_id preferred() const {
		const cs::Member* best = nullptr;
		for (const auto& x : m.members) {
			if (x.role != cs::MemberRole::Voter || !healthy(x)) { continue; }
			const auto *a = status(x.node), *b = best ? status(best->node) : nullptr;
			if (!best || a->leaders < b->leaders || (a->leaders == b->leaders && (a->groups < b->groups || (a->groups == b->groups && x.node < best->node)))) { best = &x; }
		}
		if (recorded && !recorded->members.empty()) {
			const auto* p = on_node(recorded->members.front().node);
			if (p && p->role == cs::MemberRole::Voter && healthy(*p)) {
				if (best && best != p && status(p->node)->leaders > status(best->node)->leaders + 1) { return best->node; }
				return p->node;
			}
		}
		if (best) { return best->node; }
		for (const auto& x : m.members) {
			if (x.role == cs::MemberRole::Voter) { return x.node; }
		}
		return 0;
	}

	// The record of the group as it is: the preferred leader first, the rest by node.
	shard as_recorded(node_id lead) const {
		shard s = *recorded;
		s.members.clear();
		for (const auto& x : m.members) { s.members.push_back({x.node, role_of(x.role)}); }
		std::sort(s.members.begin(), s.members.end(), [&](const member& a, const member& b) {
			if ((a.node == lead) != (b.node == lead)) { return a.node == lead; }
			return a.node < b.node;
		});
		return s;
	}
	static bool same_record(const shard& a, const shard& b) {
		if (a.members.empty() || b.members.empty() || a.members.front() != b.members.front()) { return a.members == b.members; }
		auto x = a.members, y = b.members;
		std::sort(x.begin() + 1, x.end(), [](const member& p, const member& q) { return p.node < q.node; });
		std::sort(y.begin() + 1, y.end(), [](const member& p, const member& q) { return p.node < q.node; });
		return x == y;
	}

	// A change from `from` consensus accepts (D3's single step, D9's witness shapes) that keeps a
	// quorum of up voters.
	bool valid(const cs::Membership& n, const cs::Membership* from = nullptr) const {
		const auto& f = from ? *from : m;
		try {
			cs::validate_change(f, n, f.limits ? *f.limits : cs::ceilings);
		} catch (const std::invalid_argument&) {
			return false;
		}
		std::size_t live = 0;
		for (const auto& x : n.members) { live += cs::voting(x.role) && up(x); }
		return live >= cs::voter_count(n) / 2 + 1;
	}

	static step wait(const char* why) { return step{step::wait, why}; }
	static step done(const char* why) { return step{step::none, why}; }
	step change(cs::Membership n, node_id joining = 0) const {
		step s;
		s.kind = step::change;
		s.next = std::move(n);
		s.joining = joining;
		return s;
	}
	// Leadership moves to `inc` unless a multi-entry command is open: a new leader's term
	// retires it (D12).
	step transfer(cs::NodeId inc) const {
		if (r.open_command) { return wait("leadership deferred: a multi-entry command is open"); }
		step s;
		s.kind = step::transfer;
		s.target = inc;
		return s;
	}
	// A change that removes or retires the leader moves leadership first, to an up healthy voter.
	std::optional<step> leader_first(cs::NodeId leaving) const {
		if (leaving != r.leader) { return std::nullopt; }
		const auto lead = preferred();
		const cs::Member* to = nullptr;
		for (const auto& x : m.members) {
			if (x.incarnation == leaving || x.role != cs::MemberRole::Voter || !healthy(x) || !up(x)) { continue; }
			if (!to || x.node == lead) { to = &x; }
		}
		if (!to) { return std::nullopt; }  // none: consensus hands off as the leader leaves (D4)
		return transfer(to->incarnation);
	}
	step leaving_change(cs::Membership n, cs::NodeId leaving) const {
		if (auto t = leader_first(leaving)) { return *t; }
		return change(std::move(n));
	}

	// The data voter a shrink drops: neither leader, then the most groups, then the highest id.
	const cs::Member* drop(const std::vector<const cs::Member*>& data, node_id lead) const {
		const cs::Member* out = nullptr;
		auto rank = [&](const cs::Member* x) {
			return std::tuple{x->incarnation != r.leader, x->node != lead, status(x->node)->groups, x->node};
		};
		for (const auto* x : data) {
			if (!out || rank(x) > rank(out)) { out = x; }
		}
		return out;
	}

	step run() {
		const auto lead = preferred();
		if (recorded) {
			auto want = as_recorded(lead);
			if (!same_record(want, *recorded)) {
				step s;
				s.kind = step::record;
				s.recorded = std::move(want);
				return s;
			}
		}
		if (m.leaving) { return wait("a replacement completes: consensus removes the leaving member"); }

		std::size_t usable = 0;
		for (const auto& [id, s] : nodes) { usable += !s.lost; }
		const auto t = target(rf, usable);
		std::vector<const cs::Member*> data, wit, learn, bad_data, bad_wit, bad_learn;
		for (const auto& x : m.members) {
			const bool h = healthy(x);
			switch (x.role) {
			case cs::MemberRole::Voter: (h ? data : bad_data).push_back(&x); break;
			case cs::MemberRole::Witness: (h ? wit : bad_wit).push_back(&x); break;
			case cs::MemberRole::Learner: (h ? learn : bad_learn).push_back(&x); break;
			}
		}
		const cs::Member* ready = nullptr;  // the oldest up healthy learner
		for (const auto* x : learn) {
			if (up(*x)) {
				ready = x;
				break;
			}
		}
		// The candidate for a new member: up, active, in no member, fewest groups.
		node_id candidate = 0;
		for (const auto& [id, s] : nodes) {
			if (s.lost || s.down || on_node(id)) { continue; }
			const auto* c = candidate ? status(candidate) : nullptr;
			if (!c || s.groups < c->groups || (s.groups == c->groups && s.leaders < c->leaders)) { candidate = id; }
		}
		auto with_learner = [&](node_id n) {
			auto next = m;
			next.members.push_back({next.next_incarnation, n, cs::MemberRole::Learner});
			++next.next_incarnation;
			return next;
		};
		auto add_learner = [&](node_id n) { return change(with_learner(n), n); };
		auto without = [&](cs::NodeId inc) {
			auto next = m;
			std::erase_if(next.members, [&](const cs::Member& x) { return x.incarnation == inc; });
			return next;
		};
		// `base` with learner `l` promoted to `to` beside `leaving`.
		auto promoted = [](cs::Membership base, cs::NodeId l, cs::MemberRole to, cs::NodeId leaving) {
			for (auto& x : base.members) {
				if (x.incarnation == l) { x.role = to; }
			}
			base.leaving = leaving;
			return base;
		};
		// A learner promoted to `to`, beside the first of `leaving` that makes a valid change.
		auto promote = [&](const cs::Member& l, cs::MemberRole to, const std::vector<cs::NodeId>& leaving) -> std::optional<step> {
			for (const auto inc : leaving) {
				auto next = promoted(m, l.incarnation, to, inc);
				if (valid(next)) { return leaving_change(std::move(next), inc); }
			}
			return std::nullopt;
		};
		auto incarnations = [](const std::vector<const cs::Member*>& xs) {
			std::vector<cs::NodeId> out;
			for (const auto* x : xs) { out.push_back(x->incarnation); }
			return out;
		};

		// 1. A lost learner leaves.
		if (!bad_learn.empty()) { return change(without(bad_learn.front()->incarnation)); }
		// 2. Too few data voters: promote a learner beside a lost member, else add one.
		if (data.size() < t.data) {
			if (ready) {
				auto leaving = incarnations(bad_data);
				for (auto inc : incarnations(bad_wit)) { leaving.push_back(inc); }
				if (wit.size() > t.witnesses) {
					for (auto inc : incarnations(wit)) { leaving.push_back(inc); }
				}
				leaving.push_back(0);
				if (auto s = promote(*ready, cs::MemberRole::Voter, leaving)) { return *s; }
			} else if (!learn.empty()) {
				return wait("a learner's node is down");
			} else if (candidate) {
				return add_learner(candidate);
			}
		} else if (wit.size() < t.witnesses) {
			// 3. Too few witnesses: a learner promoted to one, beside a lost witness or a data voter
			// a shrink drops.
			if (ready) {
				auto leaving = incarnations(bad_wit);
				for (auto inc : incarnations(bad_data)) { leaving.push_back(inc); }
				if (data.size() > t.data) { leaving.push_back(drop(data, lead)->incarnation); }
				leaving.push_back(0);
				if (auto s = promote(*ready, cs::MemberRole::Witness, leaving)) { return *s; }
			} else if (!learn.empty()) {
				return wait("a learner's node is down");
			} else if (candidate) {
				return add_learner(candidate);
			}
		} else if (learn.size() < t.learners && candidate) {
			return add_learner(candidate);
		}
		// 4. Removals: lost voting members, then witnesses, data voters and learners past the shape.
		for (const auto* x : bad_wit) {
			if (auto next = without(x->incarnation); valid(next)) { return leaving_change(std::move(next), x->incarnation); }
		}
		for (const auto* x : bad_data) {
			if (auto next = without(x->incarnation); valid(next)) { return leaving_change(std::move(next), x->incarnation); }
		}
		// A lost member no removal drops leaves beside a promoted learner, whatever the shape
		// (D9: 2 + witness loses either only through a window); a learner joins for it only when
		// that promotion is valid, so none joins to be removed.
		for (const auto* bad : {&bad_wit, &bad_data}) {
			for (const auto* x : *bad) {
				if (ready) {
					if (auto s = promote(*ready, x->role, {x->incarnation})) { return *s; }
				} else if (learn.empty() && candidate) {
					const auto base = with_learner(candidate);
					if (valid(promoted(base, m.next_incarnation, x->role, x->incarnation), &base)) { return add_learner(candidate); }
				}
			}
		}
		if (wit.size() > t.witnesses) {
			if (auto next = without(wit.front()->incarnation); valid(next)) { return change(std::move(next)); }
		}
		if (data.size() > t.data) {
			const auto* x = drop(data, lead);
			auto next = learn.size() < t.learners ? m : without(x->incarnation);
			if (learn.size() < t.learners) {
				for (auto& y : next.members) {
					if (y.incarnation == x->incarnation) { y.role = cs::MemberRole::Learner; }
				}
			}
			if (valid(next)) { return leaving_change(std::move(next), x->incarnation); }
		}
		if (learn.size() > t.learners) { return change(without(learn.back()->incarnation)); }
		// 5. Leadership to the preferred leader, also while the shape waits for a node.
		if (recorded) {
			const auto* p = on_node(lead);
			if (p && p->incarnation != r.leader && up(*p)) { return transfer(p->incarnation); }
		}
		const bool off = !bad_data.empty() || !bad_wit.empty() || shape{data.size(), wit.size(), learn.size()} != t;
		return off ? wait("no node or no valid change reaches rf yet") : done("at rf");
	}
};

}  // namespace detail

// The next step for one group: `recorded` is its shard in the root (null for the root group
// itself, which no record names and no leader balance moves).
inline step plan(const shard* recorded, std::uint32_t rf, const std::optional<report>& r, const node_table& nodes) {
	if (!r) { return step{step::wait, "no leader reports: the group waits for its nodes"}; }
	return detail::planner{recorded, rf, *r, nodes, r->membership}.run();
}

// The load changes a record step makes, applied to `nodes` so later plans in a pass see them.
inline void count(node_table& nodes, const shard& before, const shard& after) {
	auto add = [&](const shard& s, std::int64_t sign) {
		for (std::size_t i = 0; i < s.members.size(); ++i) {
			auto it = nodes.find(s.members[i].node);
			if (it == nodes.end()) { continue; }
			it->second.groups = std::uint64_t(std::int64_t(it->second.groups) + sign);
			if (i == 0) { it->second.leaders = std::uint64_t(std::int64_t(it->second.leaders) + sign); }
		}
	};
	add(before, -1);
	add(after, +1);
}

struct planned {
	std::string path;
	group_id group = 0;
	step next;
};

// One pass over every namespace's shards: `reports(group_id) -> std::optional<report>`.
template <class Reports>
std::vector<planned> plan_all(const view& v, const liveness& l, std::uint64_t now, Reports&& reports) {
	auto nodes = nodes_of(v, l, now);
	std::vector<planned> out;
	v.scan(namespaces_begin(), namespaces_end(), [&](std::string_view k, std::uint64_t, std::string_view bytes) {
		const auto path = path_of(k);
		if (!path) { return; }
		const auto ns = decode_namespace(bytes);
		for (const auto& s : ns.shards) {
			auto next = plan(&s, ns.rf, reports(s.group), nodes);
			if (next.kind == step::record) { count(nodes, s, next.recorded); }
			out.push_back({*path, s.group, std::move(next)});
		}
	});
	return out;
}

}  // namespace strata::placement
