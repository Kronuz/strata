// Repair's plan (quest placement A3, A4; cluster-release D7): each step checked against a model
// group that applies it as consensus would (D3's single step, D9's witness shapes, the leaving
// member removed next) and records it as the root would. Values: D7's growth and shrink 1, 2, 3,
// 2, 1; a replacement past replace_after and not before; the setting read from the root's records
// at each pass; a drain that moves leadership first; an rf = 1 group that waits; a shard with no
// node to grow on; leader balance deferred while a multi-entry command is open; a new member only
// on a node known up; a learner whose node is removed before its record; one pass's learners
// spread by the loads counted within it. A property test over seeded clusters: every change
// valid, and every shard at its rf wherever a node exists.
#include "strata/placement/repair.hh"

#include "faultlab/faultfs.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

namespace sp = strata::placement;
namespace sg = strata::group;
namespace cs = kronuz::consensus;
namespace fl = kronuz::faultlab;

int failures = 0;
std::string context;
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 40) { std::cerr << "FAIL [" << context << "]: " << what << '\n'; }
	return ok;
}

std::string text(const cs::Membership& m) {
	std::string out;
	for (const auto& x : m.members) {
		if (!out.empty()) { out += ' '; }
		out += std::to_string(x.incarnation) + ':' + std::to_string(x.node) + ':' + "VLW"[int(x.role)];
	}
	if (m.leaving) { out += " leaving " + std::to_string(m.leaving); }
	return out;
}
std::string text(const sp::step& s) {
	switch (s.kind) {
	case sp::step::none: return std::string("none: ") + s.why;
	case sp::step::wait: return std::string("wait: ") + s.why;
	case sp::step::record: return "record";
	case sp::step::change: return "change " + text(s.next);
	case sp::step::transfer: return "transfer " + std::to_string(s.target);
	}
	return "?";
}

// A group as consensus runs it and its shard as the root records it.
struct model {
	sp::node_table nodes;
	cs::Membership m;
	cs::NodeId leader = 0;
	bool open = false;  // a multi-entry command is open
	bool aborted = false;  // a transfer cut an open command (D12)
	sp::shard rec;
	std::uint32_t rf = 1;
	std::vector<std::string> trail;  // the membership after each change, and after consensus removes a leaving member
	std::vector<sp::step::kind_type> kinds;

	const cs::Member* member(cs::NodeId inc) const { return cs::find_member(m, inc); }
	bool up(const cs::Member& x) const { return !nodes.at(x.node).down; }

	// A leader needs a quorum of up voting members; it stays while it is an up voter.
	std::optional<sp::report> report() {
		std::size_t live = 0;
		for (const auto& x : m.members) { live += cs::voting(x.role) && up(x); }
		if (live < cs::voter_count(m) / 2 + 1) { return std::nullopt; }
		const auto* l = member(leader);
		if (!l || l->role != cs::MemberRole::Voter || !up(*l)) {
			leader = 0;
			for (const auto& x : m.members) {
				if (x.role == cs::MemberRole::Voter && up(x)) {
					leader = x.incarnation;
					break;
				}
			}
		}
		if (!leader) { return std::nullopt; }
		return sp::report{m, leader, open};
	}

	// Applies a step as consensus and the root would; an error names what they refuse.
	std::string apply(const sp::step& s) {
		kinds.push_back(s.kind);
		if (s.kind == sp::step::record) {
			sp::namespace_record ns{rf, 0, {s.recorded}};
			if (!sp::valid_record(sp::namespace_key("x"), sp::encode(ns))) { return "a record the root refuses"; }
			const auto before = rec;
			rec = s.recorded;
			sp::count(nodes, before, rec);
			return {};
		}
		if (s.kind == sp::step::transfer) {
			const auto* t = member(s.target);
			if (!t || t->role != cs::MemberRole::Voter || !up(*t) || s.target == leader) { return "an invalid transfer target"; }
			if (open) { aborted = true; }
			leader = s.target;
			return {};
		}
		if (s.kind != sp::step::change) { return {}; }
		try {
			cs::validate_change(m, s.next, cs::ceilings);
		} catch (const std::invalid_argument& e) {
			return std::string("consensus refuses the change: ") + e.what();
		}
		std::size_t live = 0;
		for (const auto& x : s.next.members) {
			live += cs::voting(x.role) && up(x);
			const auto* old = member(x.incarnation);
			if (!old) {
				if (s.joining != x.node || nodes.at(x.node).down || nodes.at(x.node).lost) { return "a joiner on a node that cannot open it"; }
				for (const auto& y : m.members) {
					if (y.node == x.node) { return "a node twice in a group"; }
				}
			} else if (!cs::voting(old->role) && cs::voting(x.role) && !up(x)) {
				return "a promotion of a learner that has not caught up";
			}
		}
		if (live < cs::voter_count(s.next) / 2 + 1) { return "a change that loses the quorum"; }
		m = s.next;
		trail.push_back(text(m));
		if (m.leaving) {  // D3: the leaving member goes next, whichever leader holds the membership
			std::erase_if(m.members, [&](const cs::Member& x) { return x.incarnation == m.leaving; });
			m.leaving = 0;
			trail.push_back(text(m));
		}
		return {};
	}

	// Steps until the plan rests (none or wait); returns the resting step.
	sp::step run(int bound = 200) {
		for (int i = 0; i < bound; ++i) {
			auto r = report();
			auto s = sp::plan(&rec, rf, r, nodes);
			if (s.kind == sp::step::none || s.kind == sp::step::wait) { return s; }
			const auto error = apply(s);
			if (!check(error.empty(), text(s) + " from " + text(m) + ": " + error)) { return sp::step{sp::step::wait, "refused"}; }
		}
		check(false, "the plan rests within " + std::to_string(bound) + " steps");
		return sp::step{sp::step::wait, "no rest"};
	}
};

// `n` active nodes, a group founded on `members` (incarnation = node) and recorded in that order.
model founded(std::size_t n, std::vector<sp::member> members, std::uint32_t rf) {
	model x;
	for (sp::node_id id = 1; id <= n; ++id) { x.nodes[id] = {}; }
	for (const auto& y : members) {
		x.m.members.push_back({y.node, y.node, y.role == sp::role::witness ? cs::MemberRole::Witness : cs::MemberRole::Voter});
		x.m.next_incarnation = std::max(x.m.next_incarnation, y.node + 1);
	}
	std::sort(x.m.members.begin(), x.m.members.end(), [](const cs::Member& a, const cs::Member& b) { return a.incarnation < b.incarnation; });
	x.rec = sp::shard{1, 1, std::move(members)};
	x.leader = x.rec.members.front().node;
	x.rf = rf;
	sp::count(x.nodes, sp::shard{}, x.rec);
	return x;
}
constexpr auto V = sp::role::voter;
constexpr auto W = sp::role::witness;

bool trail_is(model& x, const std::vector<std::string>& want, const std::string& what) {
	bool ok = x.trail == want;
	if (!check(ok, what)) {
		for (const auto& t : x.trail) { std::cerr << "    got " << t << '\n'; }
		for (const auto& t : want) { std::cerr << "   want " << t << '\n'; }
	}
	x.trail.clear();
	return ok;
}

// D7's steps: 0 to 1 promotes a learner (two data voters for the window), then adds the witness;
// 1 to 2 promotes a third beside the leaving witness; 2 to 1 adds the witness beside a leaving
// data voter; 1 to 0 removes the witness, then a data voter.
void growth_and_shrink() {
	context = "rf 1, 2, 3, 2, 1 on five nodes";
	auto x = founded(5, {{1, V}}, 1);
	check(x.run().kind == sp::step::none, "rf 1 rests at rf");
	x.rf = 2;
	auto s = x.run();
	check(s.kind == sp::step::none, "rf 2 rests at rf: " + text(s));
	trail_is(x, {"1:1:V 2:2:L", "1:1:V 2:2:V", "1:1:V 2:2:V 3:3:L", "1:1:V 2:2:V 3:3:W"}, "0 to 1: a learner promoted, then the witness");
	x.rf = 3;
	x.run();
	trail_is(x, {"1:1:V 2:2:V 3:3:W 4:4:L", "1:1:V 2:2:V 3:3:W 4:4:V leaving 3", "1:1:V 2:2:V 4:4:V"}, "1 to 2: a third data voter beside the leaving witness");
	x.rf = 2;
	x.run();
	trail_is(x, {"1:1:V 2:2:V 4:4:V 5:3:L", "1:1:V 2:2:V 4:4:V 5:3:W leaving 4", "1:1:V 2:2:V 5:3:W"}, "2 to 1: the witness beside a leaving data voter");
	x.rf = 1;
	x.run();
	trail_is(x, {"1:1:V 2:2:V", "1:1:V"}, "1 to 0: the witness, then a data voter");
	check(x.rec.members == std::vector<sp::member>{{1, V}}, "the record follows: node 1 alone");
	check(x.nodes.at(1).groups == 1 && x.nodes.at(1).leaders == 1 && x.nodes.at(2).groups == 0 && x.nodes.at(3).groups == 0, "the loads follow the record");
}

// A member down less than replace_after stays; at replace_after it is replaced: a learner on the
// node with the fewest groups, promoted beside it, which consensus then removes.
void replace_after() {
	context = "replace after";
	auto x = founded(5, {{1, V}, {2, V}, {3, V}}, 3);
	x.nodes[4].groups = 2;  // node 5 has fewer
	x.nodes[3].down = true;
	auto s = x.run();
	check(s.kind == sp::step::none && x.trail.empty(), "down less than replace_after: nothing moves (" + text(s) + ")");
	x.nodes[3].lost = true;
	s = x.run();
	check(s.kind == sp::step::none, "replaced: " + text(s));
	trail_is(x, {"1:1:V 2:2:V 3:3:V 4:5:L", "1:1:V 2:2:V 3:3:V 4:5:V leaving 3", "1:1:V 2:2:V 4:5:V"}, "a learner on node 5, promoted beside node 3");

	context = "replace a 2 + witness group's members";
	auto y = founded(5, {{1, V}, {2, V}, {3, W}}, 2);
	y.nodes[2].down = y.nodes[2].lost = true;
	y.run();
	trail_is(y, {"1:1:V 2:2:V 3:3:W 4:4:L", "1:1:V 2:2:V 3:3:W 4:4:V leaving 2", "1:1:V 3:3:W 4:4:V"}, "a data voter replaced through the 3 + 1 window");
	y.nodes[3].down = y.nodes[3].lost = true;
	y.run();
	trail_is(y, {"1:1:V 3:3:W 4:4:V 5:5:L", "1:1:V 3:3:W 4:4:V 5:5:W leaving 3", "1:1:V 4:4:V 5:5:W"}, "the witness replaced through the 2 + 2 window");
}

// replace_after is the root's record, read at each pass: changed by a command, the next pass uses it.
struct root_index {
	fl::filesystem fs;
	fl::io io{fs};
	sp::index_type ix = make(io);
	std::uint64_t at = 0;
	static sp::index_type make(fl::io& io) {
		strata::basic_index_options<strata::document_layout> o;
		o.keyed = true;
		return sp::index_type::create(io, o, {'r', 'p', 't'});
	}
	bool run(const sg::prepared& p) {
		if (p.refused) { return false; }
		++at;
		const sg::entry e{at, 1, cs::EntryKind::Command, p.command};
		auto t = sp::root{}.transaction({}, e);
		return t && ix.apply(*t, strata::files::position{at, 1}).result == strata::verdict::accepted;
	}
};
void setting_from_root() {
	context = "replace_after from the root";
	root_index r;
	for (int i = 0; i < 3; ++i) {
		sp::node_record n;
		n.name = "n" + std::to_string(i);
		check(r.run(sp::add_node(sp::view(r.ix), n)), "a node is added");
	}
	sp::liveness l;
	l.down_since[2] = 1000;
	check(sp::settings(sp::view(r.ix)).replace_after_ms == 300'000, "the default is 300 s");
	check(!sp::nodes_of(sp::view(r.ix), l, 1000 + 299'999).at(2).lost && sp::nodes_of(sp::view(r.ix), l, 1000 + 300'000).at(2).lost, "lost at 300 s by default");
	check(r.run(sp::set_replace_after(sp::view(r.ix), 5000)), "the setting changes by a command");
	const auto t = sp::nodes_of(sp::view(r.ix), l, 1000 + 5000);
	check(t.at(2).down && t.at(2).lost && !t.at(1).down && !t.at(1).lost, "the next pass reads 5 s: node 2 lost at 5 s");
	check(!sp::nodes_of(sp::view(r.ix), l, 1000 + 4999).at(2).lost, "and not before");
	check(r.run(sp::set_replace_after(sp::view(r.ix), 60'000)), "the setting changes again");
	check(!sp::nodes_of(sp::view(r.ix), l, 1000 + 5000).at(2).lost, "60 s: node 2 not lost at 5 s");
	check(sp::settings(sp::view(r.ix)) == sp::settings_record{60'000}, "the record holds 60 s");
	check(r.run(sp::drain(sp::view(r.ix), 3)), "node 3 drains");
	check(sp::nodes_of(sp::view(r.ix), l, 0).at(3).lost && !sp::nodes_of(sp::view(r.ix), l, 0).at(3).down, "a leaving node is up and its members are replaced");
	check(sp::drained(sp::view(r.ix), 3), "a leaving node with no group is drained");
}

// A drain moves the leader's leadership first, then promotes beside it.
void drain_leader() {
	context = "drain the leader's node";
	auto x = founded(5, {{1, V}, {2, V}, {3, V}}, 3);
	x.nodes[1].state = sp::node_state::leaving;
	x.nodes[1].lost = true;
	x.open = true;
	auto s = x.run();
	check(s.kind == sp::step::wait && std::string(s.why).find("deferred") != std::string::npos, "with a command open, the transfer waits: " + text(s));
	check(x.leader == 1 && !x.aborted, "and leadership stays");
	x.open = false;
	s = x.run();
	check(s.kind == sp::step::none, "drained: " + text(s));
	const auto t = std::find(x.kinds.begin(), x.kinds.end(), sp::step::transfer);
	check(t != x.kinds.end() && std::find(t, x.kinds.end(), sp::step::change) != x.kinds.end(), "a transfer precedes the change that retires node 1");
	check(text(x.m) == "2:2:V 3:3:V 4:4:V" && x.leader == 2, "node 1 gone, node 2 leads: " + text(x.m));
	check(x.nodes.at(1).groups == 0, "node 1's load is empty: drained");
}

// An rf = 1 group whose node is lost has no leader: it waits, and nothing is placed.
void rf1_waits() {
	context = "rf 1 waits";
	auto x = founded(3, {{1, V}}, 1);
	x.nodes[1].down = x.nodes[1].lost = true;
	auto s = x.run();
	check(s.kind == sp::step::wait && x.trail.empty() && x.rec.members == std::vector<sp::member>{{1, V}}, "the group waits for its node: " + text(s));
	x.nodes[1] = {};
	x.nodes[1].groups = x.nodes[1].leaders = 1;
	check(x.run().kind == sp::step::none && x.trail.empty(), "back, it is at rf with nothing moved");
}

// With no node to grow on, a lost voter leaves and a shard runs short (D7: a voter and a learner,
// never a lasting two-voter group); a 2 + witness group whose data voter is lost waits.
void no_replacement() {
	context = "no node to replace on";
	auto x = founded(3, {{1, V}, {2, V}, {3, V}}, 3);
	x.nodes[3].down = x.nodes[3].lost = true;
	auto s = x.run();
	trail_is(x, {"1:1:V 2:2:V", "1:1:V 2:2:L"}, "node 3 removed, node 2 a learner: short");
	check(s.kind == sp::step::none, "at the short shape: " + text(s));
	auto y = founded(3, {{1, V}, {2, V}, {3, W}}, 2);
	y.nodes[1].down = y.nodes[1].lost = true;
	s = y.run();
	check(s.kind == sp::step::wait, "2 + witness with a lost data voter and no node waits: " + text(s));
	check(y.trail.empty(), "no change loses its quorum");
}

// rf 2 to 1 with a data voter lost: 2 + witness drops neither member directly (D9's shapes, the
// quorum), so the lost voter leaves beside a promoted learner first, then the shard shrinks.
void shrink_past_lost() {
	context = "rf 2 to 1, a data voter lost";
	auto x = founded(5, {{1, V}, {4, V}, {5, W}}, 2);
	x.nodes[1].down = x.nodes[1].lost = true;
	x.rf = 1;
	auto s = x.run();
	check(s.kind == sp::step::none, "rests at rf 1: " + text(s));
	check(x.m.members.size() == 1 && x.m.members.front().role == cs::MemberRole::Voter && x.m.members.front().node != 1, "one data voter, off node 1: " + text(x.m));
	check(!x.trail.empty() && x.trail.front() == "1:1:V 4:4:V 5:5:W 6:2:L", "a learner joins first, on the free node with the fewest groups: " + (x.trail.empty() ? "" : x.trail.front()));
	check(x.trail.size() > 1 && x.trail[1] == "1:1:V 4:4:V 5:5:W 6:2:V leaving 1", "promoted beside the lost voter (3 + 1)");
}

// Leader balance (A4): a transfer to the preferred leader waits while a multi-entry command is
// open, and happens once it ends; a preferred leader two leaderships above another voter moves.
void balance() {
	context = "leader balance";
	auto x = founded(3, {{1, V}, {2, V}, {3, V}}, 3);
	x.leader = 2;
	x.open = true;
	auto s = x.run();
	check(s.kind == sp::step::wait && x.leader == 2 && !x.aborted, "deferred while open: " + text(s));
	x.open = false;
	s = x.run();
	check(s.kind == sp::step::none && x.leader == 1, "made once it ends: leader 1 (" + text(s) + ")");
	x.nodes[1].leaders = 4;  // other groups' leaderships
	x.nodes[3].leaders = 1;
	x.nodes[2].leaders = 2;
	s = x.run();
	check(x.rec.members.front().node == 3 && x.leader == 3, "node 1 at 4 leaderships hands its preference to node 3 at 1");
	check(x.nodes.at(1).leaders == 3 && x.nodes.at(3).leaders == 2, "the loads follow");
	check(x.run().kind == sp::step::none && x.rec.members.front().node == 3, "and stays: 2 is not two above 3's");
}

// A free node down but not yet lost takes no learner: node 4 holds fewer groups than node 5,
// but only node 5 is known up.
void candidate_up() {
	context = "a new member only on a node known up";
	auto x = founded(5, {{1, V}, {2, V}, {3, V}}, 3);
	x.nodes[5].groups = 2;
	x.nodes[4].down = true;
	x.nodes[3].down = x.nodes[3].lost = true;
	auto s = x.run();
	check(s.kind == sp::step::none, "replaced: " + text(s));
	trail_is(x, {"1:1:V 2:2:V 3:3:V 4:5:L", "1:1:V 2:2:V 3:3:V 4:5:V leaving 3", "1:1:V 2:2:V 4:5:V"}, "a learner on node 5, not on node 4 (down)");
}

// A new root leader's keepalive table starts empty, so it knows no node up before full_at: a
// shard short of a lost member waits, then takes its learner on a free node.
void new_root_leader() {
	context = "a new root leader's empty keepalive table";
	root_index r;
	for (int i = 0; i < 5; ++i) {
		sp::node_record n;
		n.name = "n" + std::to_string(i);
		check(r.run(sp::add_node(sp::view(r.ix), n)), "a node is added");
	}
	sp::liveness l;
	l.down_since[3] = 0;
	l.full_at = 301'500;
	const sp::shard rec{1, 1, {{1, V}, {2, V}, {3, V}}};
	sp::report rep;
	for (sp::node_id n = 1; n <= 3; ++n) { rep.membership.members.push_back({n, n, cs::MemberRole::Voter}); }
	rep.membership.next_incarnation = 4;
	rep.leader = 1;
	const auto t = sp::nodes_of(sp::view(r.ix), l, 300'000);
	check(t.at(4).down && t.at(5).down && !t.at(4).lost && t.at(3).lost, "before full_at a node not in down_since is down, not lost");
	const auto before = sp::plan(&rec, 3, rep, t);
	check(before.kind == sp::step::wait, "no node known up: it waits (" + text(before) + ")");
	const auto after = sp::plan(&rec, 3, rep, sp::nodes_of(sp::view(r.ix), l, 301'500));
	check(after.kind == sp::step::change && (after.joining == 4 || after.joining == 5), "from full_at the free nodes are up: a learner joins (" + text(after) + ")");
}

// Shard groups as consensus holds them beside the root's records (incarnation = node when
// founded): a pass plans from both, and apply runs a step as its group and the root would.
struct cluster {
	root_index r;
	sp::liveness l;
	std::uint64_t now = 0;
	std::map<sp::group_id, cs::Membership> groups;
	std::map<sp::group_id, cs::NodeId> leaders;
	std::size_t refused = 0, recorded = 0;

	void add_nodes(std::size_t n) {
		for (std::size_t i = 0; i < n; ++i) {
			sp::node_record x;
			x.name = "n" + std::to_string(i);
			check(r.run(sp::add_node(sp::view(r.ix), x)), "a node is added");
		}
	}
	void found(const sp::shard& s) {
		cs::Membership m;
		for (const auto& y : s.members) {
			const auto kind = y.role == sp::role::witness ? cs::MemberRole::Witness : y.role == sp::role::learner ? cs::MemberRole::Learner : cs::MemberRole::Voter;
			m.members.push_back({y.node, y.node, kind});
			m.next_incarnation = std::max(m.next_incarnation, y.node + 1);
		}
		std::sort(m.members.begin(), m.members.end(), [](const cs::Member& a, const cs::Member& b) { return a.incarnation < b.incarnation; });
		groups[s.group] = m;
		leaders[s.group] = s.members.front().node;
	}
	void create(const std::string& path, std::uint32_t rf) {
		sp::namespace_record ns;
		check(r.run(sp::create_namespace(sp::view(r.ix), path, 1, rf, 0, &ns)), path + " is created");
		for (const auto& s : ns.shards) { found(s); }
	}
	// A shard recorded on `members` and its group founded on them.
	sp::group_id record_as(const std::string& path, std::vector<sp::member> members) {
		auto s = sp::view(r.ix).namespace_at(path)->record.shards.front();
		s.members = std::move(members);
		check(r.run(sp::record_shard(sp::view(r.ix), path, s)), path + " is recorded");
		found(s);
		return s.group;
	}
	bool up(sp::node_id n) const { return !l.down_since.contains(n); }
	std::optional<sp::report> report(sp::group_id g) {
		const auto& m = groups.at(g);
		const auto* lead = cs::find_member(m, leaders[g]);
		if (!lead || lead->role != cs::MemberRole::Voter || !up(lead->node)) {
			leaders[g] = 0;
			for (const auto& x : m.members) {
				if (x.role == cs::MemberRole::Voter && up(x.node)) {
					leaders[g] = x.incarnation;
					break;
				}
			}
		}
		if (!leaders[g]) { return std::nullopt; }
		return sp::report{m, leaders[g], false};
	}
	std::vector<sp::planned> plan() {
		return sp::plan_all(sp::view(r.ix), l, now, [this](sp::group_id g) { return report(g); });
	}
	bool apply(const sp::planned& p) {
		auto& m = groups.at(p.group);
		if (p.next.kind == sp::step::record) {
			const bool ok = r.run(sp::record_shard(sp::view(r.ix), p.path, p.next.recorded));
			++(ok ? recorded : refused);
			return ok;
		}
		if (p.next.kind == sp::step::transfer) { leaders[p.group] = p.next.target; }
		if (p.next.kind != sp::step::change) { return true; }
		try {
			cs::validate_change(m, p.next.next, cs::ceilings);
		} catch (const std::invalid_argument&) {
			return false;
		}
		m = p.next.next;
		if (m.leaving) {  // D3: consensus removes the leaving member next
			const auto leaving = m.leaving;
			std::erase_if(m.members, [&](const cs::Member& x) { return x.incarnation == leaving; });
			m.leaving = 0;
		}
		return true;
	}
	// One pass with every step applied; true when it rests (every step none or wait).
	bool pass() {
		bool rested = true;
		for (const auto& p : plan()) {
			if (p.next.kind == sp::step::none || p.next.kind == sp::step::wait) { continue; }
			rested = false;
			check(apply(p), p.path + ": the step applies: " + text(p.next));
		}
		return rested;
	}
};

// A learner joins on a free node, which is removed (its recorded load is 0) before the next
// pass records the learner. The record names only nodes the root knows: the learner leaves, a
// replacement joins on the other free node, and the root accepts every record.
void orphan_learner() {
	context = "a learner on a node removed before its record";
	cluster c;
	c.add_nodes(5);
	c.create("x", 3);
	const auto placed = sp::view(c.r.ix).namespace_at("x")->record.shards.front();
	const auto g = placed.group;
	const auto lost = placed.members[1].node;
	c.l.down_since[lost] = 0;
	c.now = 300'000;
	sp::node_id f = 0;
	for (const auto& p : c.plan()) {
		if (p.group == g && p.next.kind == sp::step::change && p.next.joining) {
			f = p.next.joining;
			check(c.apply(p), "the learner joins");
		}
	}
	if (!check(f != 0, "a learner joins for the lost member")) { return; }
	check(c.r.run(sp::remove_node(sp::view(c.r.ix), f)), "its node, with no recorded group, is removed");
	bool rested = false;
	for (int pass = 0; pass < 20 && !rested; ++pass) { rested = c.pass(); }
	check(rested, "the shard rests");
	check(c.refused == 0 && c.recorded >= 2, "the root accepts every record (" + std::to_string(c.refused) + " refused, " + std::to_string(c.recorded) + " accepted)");
	const auto rec = sp::view(c.r.ix).namespace_at("x")->record.shards.front();
	std::set<sp::node_id> in_group, in_record;
	for (const auto& x : c.groups.at(g).members) { in_group.insert(x.node); }
	for (const auto& y : rec.members) { in_record.insert(y.node); }
	check(in_group == in_record && in_group.size() == 3 && !in_group.contains(f) && !in_group.contains(lost), "three members off the removed and the lost node, recorded as they are: " + text(c.groups.at(g)));
}

// One pass over 24 shards at rf 3 on 8 nodes, node 1 lost. Each learner counts before the next
// shard plans, so each lands on a node with the fewest groups among its shard's candidates, and no
// node takes more than 2 of the pass's learners.
void pass_spread() {
	context = "one pass spreads its learners";
	cluster c;
	c.add_nodes(8);
	for (int i = 0; i < 24; ++i) { c.create("ns" + std::to_string(i), 3); }
	c.l.down_since[1] = 0;
	c.now = 3'600'000;
	auto t = sp::nodes_of(sp::view(c.r.ix), c.l, c.now);
	const auto affected = t.at(1).groups;
	std::size_t steps = 0, records = 0, learners = 0;
	std::map<sp::node_id, std::size_t> joins;
	for (const auto& p : c.plan()) {
		steps += p.next.kind != sp::step::none;
		if (p.next.kind == sp::step::record) {
			++records;
			sp::count(t, sp::view(c.r.ix).namespace_at(p.path)->record.shards.front(), p.next.recorded);
		}
		if (p.next.kind != sp::step::change || !p.next.joining) { continue; }
		const auto j = p.next.joining;
		const auto& m = c.groups.at(p.group);
		for (const auto& [id, s] : t) {
			const auto n = id;
			const bool member = std::any_of(m.members.begin(), m.members.end(), [n](const cs::Member& x) { return x.node == n; });
			if (s.lost || s.down || member) { continue; }
			check(t.at(j).groups <= s.groups, p.path + ": node " + std::to_string(j) + " at " + std::to_string(t.at(j).groups) + " groups, node " + std::to_string(id) + " at " + std::to_string(s.groups));
		}
		++t.at(j).groups;
		++joins[j];
		++learners;
	}
	std::string spread;
	std::size_t most = 0;
	for (const auto& [id, k] : joins) {
		spread += " n" + std::to_string(id) + "=" + std::to_string(k);
		most = std::max(most, k);
	}
	check(affected >= 6 && steps == affected && records + learners == affected, "each of node 1's " + std::to_string(affected) + " shards plans a record or a learner");
	check(learners >= affected / 2 && most <= 2, std::to_string(learners) + " learners, at most 2 per node:" + spread);
}

// A pending record's learner counts too: shard a records its learner on node 4 in this pass, so
// shard b, short of its lost node 3, takes node 5 rather than node 4 (both hold no recorded group).
void pending_record_counts() {
	context = "a recorded learner counts within the pass";
	cluster c;
	c.add_nodes(5);
	c.create("a", 3);
	c.create("b", 3);
	const auto a = c.record_as("a", {{1, V}, {2, V}, {3, V}});
	c.record_as("b", {{2, V}, {1, V}, {3, V}});
	auto& m = c.groups.at(a);
	m.members.push_back({m.next_incarnation, 4, cs::MemberRole::Learner});
	++m.next_incarnation;
	c.l.down_since[3] = 0;
	c.now = 300'000;
	const auto t = sp::nodes_of(sp::view(c.r.ix), c.l, c.now);
	check(t.at(4).groups == 0 && t.at(5).groups == 0 && t.at(3).lost, "nodes 4 and 5 hold no recorded group, node 3 is lost");
	sp::step sa, sb;
	for (const auto& p : c.plan()) { (p.group == a ? sa : sb) = p.next; }
	check(sa.kind == sp::step::record, "a records its learner: " + text(sa));
	check(sb.kind == sp::step::change && sb.joining == 5, "b's learner goes to node 5: " + text(sb));
}

// Property: seeded clusters and groups placed as the root places them, then nodes lost, drained
// and rf changed. Every step is one consensus and the root accept; the plan rests; at rest every
// shard is at its shape with no member on a lost node and the preferred leader leading, unless no
// node is free to take a member.
void property(std::uint64_t seed) {
	std::mt19937_64 rng(seed);
	const std::size_t n = 1 + rng() % 8;
	std::vector<sp::candidate> c;
	for (sp::node_id id = 1; id <= n; ++id) { c.push_back({id, rng() % 3, 0}); }
	const auto rf0 = std::uint32_t(1 + rng() % 5);
	auto placed = sp::place(c, 1, rf0).front();
	std::erase_if(placed, [](const sp::member& y) { return y.role == sp::role::learner; });  // founders vote (consensus)
	auto x = founded(n, placed, rf0);
	for (const auto& y : c) { x.nodes[y.node].groups += y.groups - (std::find_if(placed.begin(), placed.end(), [&](const sp::member& z) { return z.node == y.node; }) != placed.end()); }
	context = "seed " + std::to_string(seed) + ": " + std::to_string(n) + " nodes, rf " + std::to_string(rf0) + ", " + text(x.m);
	check(x.run().kind == sp::step::none, "a placed group rests at rf");
	for (int round = 0; round < 4; ++round) {
		x.rf = std::uint32_t(1 + rng() % 5);
		for (auto& [id, s] : x.nodes) {
			const auto k = rng() % 10;
			if (k == 0) {
				s.down = s.lost = true;
			} else if (k == 1) {
				s.state = sp::node_state::leaving;
				s.lost = true;
			} else if (k == 2) {
				s = sp::node_status{sp::node_state::active, s.groups, s.leaders, false, false};
			}
		}
		const auto r = x.report();
		const auto rest = x.run();
		if (!r) {
			check(rest.kind == sp::step::wait, "no quorum: it waits");
			continue;
		}
		std::size_t usable = 0, free = 0;
		for (const auto& [id, s] : x.nodes) {
			usable += !s.lost;
			bool member = false;
			for (const auto& y : x.m.members) { member = member || y.node == id; }
			free += !s.lost && !s.down && !member;
		}
		const auto t = sp::target(x.rf, usable);
		sp::shape now;
		bool on_lost = false;
		for (const auto& y : x.m.members) {
			on_lost = on_lost || x.nodes.at(y.node).lost;
			now.data += y.role == cs::MemberRole::Voter;
			now.witnesses += y.role == cs::MemberRole::Witness;
			now.learners += y.role == cs::MemberRole::Learner;
		}
		const auto where = " (round " + std::to_string(round) + ", rf " + std::to_string(x.rf) + ", " + text(x.m) + ", " + text(rest) + ")";
		if (rest.kind == sp::step::none) {
			check(now == t && !on_lost, "at rest: the shape for rf on the usable nodes, no member on a lost node" + where);
			check(x.member(x.leader) && x.member(x.leader)->node == x.rec.members.front().node, "the preferred leader leads" + where);
		} else {
			check(free == 0, "it waits only with no free node" + where);
		}
		check(x.rec.members.size() == x.m.members.size(), "the record follows the group" + where);
	}
}

}  // namespace

int main(int argc, char** argv) {
	growth_and_shrink();
	replace_after();
	setting_from_root();
	drain_leader();
	rf1_waits();
	no_replacement();
	shrink_past_lost();
	balance();
	candidate_up();
	new_root_leader();
	orphan_learner();
	pass_spread();
	pending_record_counts();
	const std::uint64_t seeds = argc > 1 ? std::stoull(argv[1]) : 3000;
	for (std::uint64_t seed = 1; seed <= seeds; ++seed) { property(seed); }
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("repair plan: every case passed, %llu seeds\n", static_cast<unsigned long long>(seeds));
	return 0;
}
