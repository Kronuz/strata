// Repair on live groups (quest placement A3, A4): the root group on three of six nodes holds the
// records; shard groups run test/group/blob_app.hh's application under writes; a repair pass at
// the root leader plans from a linearizable read of the records, the shard leaders' reports and
// the keepalive table, and runs each step on its group (consensus's change_membership,
// transfer_leadership) or on the root (record_shard). After each repair converges: every
// acknowledged write on every data member, their digests equal, the root's records equal on
// every root voter, every shard at its shape and recorded as it is, the loads a recount.
// Learners catch up from the log: no checkpoint truncates it (install waits on group-heal).
//   `repair_group_test [case [seeds]]`: replace, drain, replicas, waits, defer.
#include "repair_world.hh"

#include <cstdio>
#include <string>

namespace {

using namespace repair_test;

using records_map = std::map<std::string, std::pair<std::uint64_t, std::string>>;

// --- the root's operations -----------------------------------------------------------------

std::shared_ptr<started<sg::submitted>> submit(root_replica& at, root_test::operation op) {
	auto prepare = [op = std::move(op)](sp::index_type& ix, const std::vector<std::string>& pending) { return op(sp::view(ix, pending)); };
	auto forward = [](unsigned placements) -> task::task<sg::submitted> { co_return sg::submitted{sg::next_step::unavailable, std::nullopt, std::nullopt, placements, false}; };
	auto s = start(at.p->loop_ref(), at.writer->submit(std::move(prepare), std::move(forward)));
	at.run();
	return s;
}
template <class T>
bool run_until(world& w, const std::shared_ptr<started<T>>& s, int bound = 50000) {
	for (int i = 0; i < bound && !s->done; ++i) { w.step(); }
	return s->done;
}
template <class F>
auto read(world& w, root_replica& at, F fn) {
	using R = std::invoke_result_t<F&, const sp::view&>;
	auto s = start(at.p->loop_ref(), at.g->read_applied([fn](sp::index_type& ix) -> R { return fn(sp::view(ix)); }));
	at.run();
	run_until(w, s);
	return s;
}
// An operation at the root's leader, the world stepped until it answers; whether it was accepted.
bool root_op(world& w, root_test::operation op, const std::string& what_for) {
	for (int attempt = 0; attempt < 20; ++attempt) {
		if (!w.leader(root_gid)) { w.settle(false); }
		auto* l = w.leader_as<root_replica>(root_gid);
		if (!l) { continue; }
		auto s = submit(*l, op);
		run_until(w, s);
		if (root_test::accepted(s)) { return true; }
		if (s->done && !s->error && s->value->refused) { return check(false, what_for + ": refused"); }
	}
	return check(false, what_for + ": no answer");
}
records_map records(sp::index_type& ix) {
	records_map out;
	ix.scan("", [&](std::string_view k, std::string_view v, std::uint64_t version) { out[std::string(k)] = {version, std::string(v)}; });
	return out;
}

// --- a shard replica's digest --------------------------------------------------------------

struct digest {
	records_map documents;  // key: version, the bytes its locator names
	std::map<std::string, std::string> files;
	std::string state;
	bool operator==(const digest&) const = default;
};
digest of(shard_replica& r) {
	digest out;
	r.g->strata().scan("", [&](std::string_view k, std::string_view loc, std::uint64_t version) {
		const std::string l(loc);
		const auto a = l.find(':'), b = l.find(':', a + 1);
		const auto volume = std::stoull(l.substr(0, a)), offset = std::stoull(l.substr(a + 1, b - a - 1)), length = std::stoull(l.substr(b + 1));
		out.documents[std::string(k)] = {version, r.g->files().read(blob::volume_name(volume), offset, std::size_t(length))};
	});
	for (const auto& [name, f] : r.g->files().capture()) { out.files[name] = r.g->files().read(name, 0, std::size_t(f.length)); }
	out.state = r.app.encode(r.g->state());
	return out;
}

// --- the repair driver at the root leader --------------------------------------------------

struct driver {
	world& w;
	std::uint32_t root_rf = 3;
	std::map<cs::GroupId, std::function<bool()>> busy;  // a step in flight per group
	std::uint64_t changes = 0, transfers = 0, recorded = 0, deferred = 0, waits = 0;
	std::map<cs::GroupId, std::map<std::string, std::string>> acked;  // writes answered accepted
	std::map<cs::GroupId, std::vector<std::pair<std::string, std::shared_ptr<started<sg::answer>>>>> writing;
	std::uint64_t written = 0;
	std::map<cs::GroupId, sp::step::kind_type> last;  // each group's last planned step
	std::map<cs::GroupId, std::string> planned_text;  // and as text
	std::map<cs::GroupId, std::function<std::string()>> outcome;  // its last change or transfer's answer

	std::optional<sp::report> report_of(cs::GroupId g) {
		auto* l = w.leader(g);
		if (!l) { return std::nullopt; }
		sp::report r{l->state()->membership, l->inc, false};
		if (g != root_gid) { r.open_command = !static_cast<shard_replica*>(l)->g->state().uploads.empty(); }
		return r;
	}

	// One pass: plans from a read at the root leader, each step started unless its group's last
	// one is still in flight. Rested: every step none or wait, nothing in flight.
	bool pass() {
		auto* root = w.leader_as<root_replica>(root_gid);
		if (!root) { return false; }
		std::map<cs::GroupId, std::optional<sp::report>> reports;
		for (auto g : w.groups()) { reports[g] = report_of(g); }
		const auto keepalive = w.keepalive;
		const auto now = w.now;
		const auto rf = root_rf;
		auto planned = read(w, *root, [reports, keepalive, now, rf](const sp::view& v) {
			auto all = sp::plan_all(v, keepalive, now, [&](sp::group_id g) { return reports.contains(g) ? reports.at(g) : std::nullopt; });
			all.push_back({"", root_gid, sp::plan(nullptr, rf, reports.at(root_gid), sp::nodes_of(v, keepalive, now))});
			return all;
		});
		if (!planned->value) { return false; }
		bool rested = true;
		for (auto& p : *planned->value) {
			if (auto b = busy.find(p.group); b != busy.end()) {
				if (!b->second()) {
					rested = false;
					continue;
				}
				busy.erase(b);
			}
			last[p.group] = p.next.kind;
			planned_text[p.group] = text(p.next);
			switch (p.next.kind) {
			case sp::step::none: break;
			case sp::step::wait:
				++waits;
				deferred += std::string(p.next.why).find("deferred") != std::string::npos;
				break;
			case sp::step::record: {
				rested = false;
				++recorded;
				auto s = submit(*root, [path = p.path, shard = p.next.recorded](const sp::view& v) { return sp::record_shard(v, path, shard); });
				busy[p.group] = [s] { return s->done; };
				break;
			}
			case sp::step::change: {
				rested = false;
				auto* l = w.find(p.group, reports.at(p.group)->leader);
				if (!l || !l->leads()) { break; }
				for (const auto& m : p.next.next.members) {
					if (cs::find_member(reports.at(p.group)->membership, m.incarnation)) { continue; }
					// A joiner of a refused change, planned again on another node, starts over there.
					if (auto* old = w.find(p.group, m.incarnation); old && old->node != m.node) { w.discard(p.group, m.incarnation); }
					if (w.find(p.group, m.incarnation)) { continue; }
					if (p.group == root_gid) {
						w.join<root_replica>(p.group, m.incarnation, m.node);
					} else {
						w.join<shard_replica>(p.group, m.incarnation, m.node);
					}
				}
				++changes;
				auto s = start(l->p->loop_ref(), l->p->lp->driver().change_membership(p.group, p.next.next));
				l->run();
				busy[p.group] = [s] { return s->done; };
				outcome[p.group] = [s, t = planned_text[p.group]] { return t + ": " + (s->done ? what(s->error) : "in flight"); };
				break;
			}
			case sp::step::transfer: {
				rested = false;
				auto* l = w.find(p.group, reports.at(p.group)->leader);
				if (!l || !l->leads()) { break; }
				++transfers;
				auto s = start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(p.group, p.next.target));
				l->run();
				busy[p.group] = [s] { return s->done; };
				outcome[p.group] = [s, t = planned_text[p.group]] { return t + ": " + (s->done ? what(s->error) : "in flight"); };
				break;
			}
			}
		}
		return rested;
	}

	// One write at each shard leader, answered later; at most `cap` per group in a run.
	void writes(std::size_t cap = 250) {
		for (auto g : w.groups()) {
			if (g == root_gid || acked[g].size() + writing[g].size() >= cap) { continue; }
			auto* l = static_cast<shard_replica*>(w.leader(g));
			if (!l) { continue; }
			const auto key = "w" + std::to_string(++written);
			const auto value = "v" + std::to_string(written * 7919 % 100000);
			auto s = start(l->p->loop_ref(), l->g->propose("P|" + key + "|" + value, sg::ack::quorum));
			l->run();
			writing[g].push_back({key + "|" + value, s});
		}
	}
	void collect() {
		for (auto& [g, list] : writing) {
			std::erase_if(list, [&](auto& e) {
				if (!e.second->done) { return false; }
				const auto& s = e.second;
				if (!s->error && s->value->applied && s->value->applied->result == strata::verdict::accepted) {
					const auto bar = e.first.find('|');
					acked[g][e.first.substr(0, bar)] = e.first.substr(bar + 1);
				}
				return true;
			});
		}
	}

	// Each group's last plan and answer, and every replica.
	void dump() {
		for (auto g : w.groups()) {
			auto* l = w.leader(g);
			std::cerr << "  group " << g << ": leader " << (l ? std::to_string(l->inc) + " on node " + std::to_string(l->node) : std::string("none")) << ", planned "
					  << planned_text[g] << (busy.contains(g) ? ", busy" : "") << (outcome.contains(g) ? ", last answer " + outcome[g]() : std::string()) << '\n';
		}
		w.print();
	}

	// Writes, steps and passes until two passes in a row rest on a calm world.
	bool converge(int bound = 300) {
		int quiet = 0;
		for (int i = 0; i < bound; ++i) {
			writes();
			for (int k = 0; k < 300; ++k) { w.step(); }
			collect();
			const bool calm = w.settle(false);
			if (calm) { w.discard_removed(); }
			const bool rested = pass();
			quiet = calm && rested ? quiet + 1 : 0;
			if (quiet >= 2) {
				// The writes in flight answer before the checks.
				for (int k = 0; k < 2000; ++k) { w.step(); }
				w.settle(false);
				collect();
				return true;
			}
		}
		dump();
		return false;
	}
};

// --- the cluster ---------------------------------------------------------------------------

sp::node_record named(sp::node_id id) {
	sp::node_record n;
	n.name = "node-" + std::to_string(id);
	n.addresses = {n.name + ":9880"};
	n.pin = std::string(32, 'k');
	return n;
}

struct cluster {
	world w;
	driver d{w};
	std::map<std::string, sp::namespace_record> placed;

	explicit cluster(std::uint64_t seed, std::size_t nodes = 6) : w(seed) {
		for (sp::node_id n = 1; n <= nodes; ++n) { w.nodes.insert(n); }
		w.found<root_replica>(root_gid, {{1, sp::role::voter}, {2, sp::role::voter}, {3, sp::role::voter}});
		check(w.settle(), "the root settles");
		for (sp::node_id n = 1; n <= nodes; ++n) {
			sp::node_id id = 0;
			root_op(w, [n, &id](const sp::view& v) { return sp::add_node(v, named(n), &id); }, "add node " + std::to_string(n));
			check(id == n, "node ids follow the physical nodes");
		}
	}
	// A namespace created and its groups founded on their placed voters and witnesses (founding
	// members vote: repair adds a short group's learner).
	sp::namespace_record& create(const std::string& path, std::uint32_t rf, std::size_t shards = 1) {
		auto ns = std::make_shared<sp::namespace_record>();
		root_op(w, [path, rf, shards, ns](const sp::view& v) { return sp::create_namespace(v, path, shards, rf, 0, ns.get()); }, "create " + path);
		for (const auto& s : ns->shards) {
			std::vector<sp::member> founders;
			for (const auto& m : s.members) {
				if (m.role != sp::role::learner) { founders.push_back(m); }
			}
			w.found<shard_replica>(s.group, founders);
		}
		w.settle(false);
		return placed[path] = *ns;
	}
	std::optional<sp::namespace_record> record(const std::string& path) {
		auto* l = w.leader_as<root_replica>(root_gid);
		if (!l) { return std::nullopt; }
		auto r = read(w, *l, [path](const sp::view& v) { return v.namespace_at(path); });
		if (!r->value || !*r->value) { return std::nullopt; }
		return (*r->value)->record;
	}

	// The checks after a repair converges.
	void verify(const std::string& when) {
		auto* root = w.leader_as<root_replica>(root_gid);
		if (!check(root != nullptr, when + ": the root has a leader")) { return; }
		// The root's records are equal on every root member; a new root voter holds them too.
		const auto m = root->state()->membership;
		std::optional<records_map> ref;
		for (const auto& x : m.members) {
			auto* r = static_cast<root_replica*>(w.find(root_gid, x.incarnation));
			if (!r || !r->up() || x.role == cs::MemberRole::Witness) { continue; }
			auto mine = records(r->g->strata());
			if (!ref) {
				ref = mine;
			} else {
				check(mine == *ref, when + ": root member " + std::to_string(x.incarnation) + " on node " + std::to_string(x.node) + " holds the leader's records");
			}
		}
		auto all = read(w, *root, [](const sp::view& v) {
			std::vector<std::pair<std::string, sp::namespace_record>> out;
			v.scan(sp::namespaces_begin(), sp::namespaces_end(), [&](std::string_view k, std::uint64_t, std::string_view b) { out.push_back({*sp::path_of(k), sp::decode_namespace(b)}); });
			std::map<sp::node_id, sp::load_record> loads;
			v.scan(sp::loads_begin(), sp::loads_end(), [&](std::string_view k, std::uint64_t, std::string_view b) { loads[*sp::load_of(k)] = sp::decode_load(b); });
			return std::pair{out, loads};
		});
		if (!check(all->value.has_value(), when + ": the records read")) { return; }
		std::map<sp::node_id, sp::load_record> recount;
		for (const auto& [path, ns] : all->value->first) {
			for (const auto& s : ns.shards) {
				for (std::size_t i = 0; i < s.members.size(); ++i) {
					++recount[s.members[i].node].groups;
					recount[s.members[i].node].leaders += i == 0;
				}
				verify_shard(when + ", " + path + " group " + std::to_string(s.group), ns.rf, s);
			}
		}
		for (const auto& [id, l] : all->value->second) {
			check(l == recount[id], when + ": node " + std::to_string(id) + "'s load is a recount of the records (" + std::to_string(l.groups) + " groups, " +
										   std::to_string(recount[id].groups) + " counted)");
		}
	}
	void verify_shard(const std::string& where, std::uint32_t rf, const sp::shard& s) {
		auto* l = static_cast<shard_replica*>(w.leader(s.group));
		if (!l) {
			check(s.members.size() == 1 && w.down.contains(s.members.front().node), where + ": only a group whose lone voter is down has no leader");
			return;
		}
		const auto m = l->state()->membership;
		// Recorded as it is.
		std::set<std::pair<sp::node_id, sp::role>> group_side, record_side;
		for (const auto& x : m.members) { group_side.insert({x.node, sp::role_of(x.role)}); }
		for (const auto& x : s.members) { record_side.insert({x.node, x.role}); }
		check(group_side == record_side, where + ": the record names the group's members");
		// At its shape on the usable nodes, no member on a down or leaving node.
		std::size_t usable = 0;
		for (auto n : w.nodes) { usable += !w.down.contains(n) && !draining.contains(n); }
		sp::shape now;
		for (const auto& x : m.members) {
			now.data += x.role == cs::MemberRole::Voter;
			now.witnesses += x.role == cs::MemberRole::Witness;
			now.learners += x.role == cs::MemberRole::Learner;
			check(!w.down.contains(x.node) && !draining.contains(x.node), where + ": no member on a down or draining node (" + std::to_string(x.node) + ")");
		}
		const auto t = sp::target(rf, usable);
		check(now == t, where + ": at its shape for rf " + std::to_string(rf) + ": " + std::to_string(now.data) + " data, " + std::to_string(now.witnesses) + " witnesses, " +
							 std::to_string(now.learners) + " learners; want " + std::to_string(t.data) + ", " + std::to_string(t.witnesses) + ", " + std::to_string(t.learners));
		// Every data member holds every acknowledged write; their digests are equal.
		std::optional<digest> ref;
		for (const auto& x : m.members) {
			if (x.role == cs::MemberRole::Witness) { continue; }
			auto* r = static_cast<shard_replica*>(w.find(s.group, x.incarnation));
			if (!check(r && r->open(), where + ": member " + std::to_string(x.incarnation) + " runs")) { continue; }
			const auto mine = of(*r);
			std::size_t missing = 0;
			for (const auto& [k, v] : d.acked[s.group]) {
				auto it = mine.documents.find(k);
				missing += it == mine.documents.end() || it->second.second != v;
			}
			check(missing == 0, where + ": member " + std::to_string(x.incarnation) + " on node " + std::to_string(x.node) + " holds every acknowledged write (" +
									std::to_string(missing) + " of " + std::to_string(d.acked[s.group].size()) + " missing)");
			if (!ref) {
				ref = mine;
			} else {
				check(mine == *ref, where + ": member " + std::to_string(x.incarnation) + "'s digest equals the leader's");
			}
		}
	}
	std::set<sp::node_id> draining;
};

std::set<sp::node_id> nodes_of(const sp::namespace_record& ns) {
	std::set<sp::node_id> out;
	for (const auto& s : ns.shards) {
		for (const auto& m : s.members) { out.insert(m.node); }
	}
	return out;
}

// A node down less than replace_after keeps its members; the setting changed at runtime takes
// effect at the next pass; past it, every shard and the root replace its members.
void replace(std::uint64_t seed) {
	context = "replace, seed " + std::to_string(seed);
	cluster c(seed);
	auto one = c.create("one", 1);
	auto idx = c.create("idx", 3);
	auto sub = c.create("idx/sub", 2);
	if (!check(c.d.converge(), "the cluster settles at rf before any loss")) { return; }
	c.verify("before");
	// A root voter holding idx and idx/sub members, not one's node.
	sp::node_id x = 0;
	for (sp::node_id n : {1, 2, 3}) {
		if (nodes_of(idx).contains(n) && nodes_of(sub).contains(n) && !nodes_of(one).contains(n)) { x = n; }
	}
	for (sp::node_id n : {1, 2, 3}) {
		if (!x && nodes_of(idx).contains(n) && !nodes_of(one).contains(n)) { x = n; }
	}
	if (!check(x != 0, "a root voter holds an idx member")) { return; }
	const auto before = c.d.changes;
	c.w.cut(x);
	const auto cut_at = c.w.now;
	for (int i = 0; i < 20; ++i) {
		c.d.writes();
		for (int k = 0; k < 300; ++k) { c.w.step(); }
		c.w.settle(false);
		c.d.collect();
		c.d.pass();
	}
	check(c.d.changes == before, "down " + std::to_string(c.w.now - cut_at) + " ms, under the default 300 s: no member moves");
	check(root_op(c.w, [](const sp::view& v) { return sp::set_replace_after(v, 200); }, "replace_after 200 ms"), "replace_after changes at runtime");
	if (!check(c.d.converge(), "repair converges after replace_after changes")) { return; }
	check(c.d.changes > before, "members were replaced: " + std::to_string(c.d.changes - before) + " membership changes");
	c.verify("after node " + std::to_string(x) + " was replaced");
	auto* root = c.w.leader(root_gid);
	if (root) {
		bool fresh = false, left = true;
		const auto m = root->state()->membership;  // a copy: the state is a temporary
		for (const auto& y : m.members) {
			fresh = fresh || (y.incarnation > 3 && y.role == cs::MemberRole::Voter);
			left = left && y.node != x;
		}
		check(fresh && left, "a new root voter joined and node " + std::to_string(x) + "'s left: " + text(m));
	}
}

// A drain moves a node's members, its leaderships first; the node is then drained and removed.
void drain(std::uint64_t seed) {
	context = "drain, seed " + std::to_string(seed);
	cluster c(seed);
	c.create("idx", 3);
	c.create("idx/sub", 2);
	c.create("other", 3);
	if (!check(c.d.converge(), "the cluster settles")) { return; }
	// A shard's preferred leader on a node not a root voter, else a member off the root.
	sp::node_id x = 0;
	for (const auto& [path, ns] : c.placed) {
		for (const auto& s : ns.shards) {
			if (!x && s.members.front().node > 3) { x = s.members.front().node; }
		}
	}
	for (const auto& [path, ns] : c.placed) {
		for (auto n : nodes_of(ns)) {
			if (!x && n > 3) { x = n; }
		}
	}
	if (!check(x != 0, "a member off the root")) { return; }
	check(root_op(c.w, [x](const sp::view& v) { return sp::drain(v, x); }, "drain"), "node " + std::to_string(x) + " drains");
	c.draining.insert(x);
	if (!check(c.d.converge(), "repair converges after the drain")) { return; }
	c.verify("after node " + std::to_string(x) + " drained");
	auto* root = c.w.leader_as<root_replica>(root_gid);
	auto done = read(c.w, *root, [x](const sp::view& v) { return sp::drained(v, x); });
	check(done->value && *done->value, "node " + std::to_string(x) + " holds no group: drained");
	check(root_op(c.w, [x](const sp::view& v) { return sp::remove_node(v, x); }, "remove"), "the drained node is removed");
}

// number_of_replicas 0, 1, 2, 1, 0 (rf 1, 2, 3, 2, 1) under writes, D7's steps.
void replicas(std::uint64_t seed) {
	context = "replicas, seed " + std::to_string(seed);
	cluster c(seed);
	c.create("live", 1);
	if (!check(c.d.converge(), "rf 1 settles")) { return; }
	c.verify("rf 1");
	for (std::uint32_t rf : {2u, 3u, 2u, 1u}) {
		check(root_op(c.w, [rf](const sp::view& v) { return sp::set_rf(v, "live", rf); }, "rf " + std::to_string(rf)), "rf changes to " + std::to_string(rf));
		if (!check(c.d.converge(), "repair converges at rf " + std::to_string(rf))) { return; }
		c.verify("rf " + std::to_string(rf));
	}
	check(c.d.acked[c.placed["live"].shards.front().group].size() > 20, "writes ran throughout: " + std::to_string(c.d.acked[c.placed["live"].shards.front().group].size()) + " acknowledged");
}

// An rf = 1 group whose node is down waits for it: nothing moves, and back up it takes writes.
void waits(std::uint64_t seed) {
	context = "rf 1 waits, seed " + std::to_string(seed);
	cluster c(seed);
	auto one = c.create("one", 1);
	c.create("idx", 3);
	if (!check(c.d.converge(), "the cluster settles")) { return; }
	const auto n = one.shards.front().members.front().node;
	const auto g = one.shards.front().group;
	check(root_op(c.w, [](const sp::view& v) { return sp::set_replace_after(v, 100); }, "replace_after 100 ms"), "replace_after 100 ms");
	c.w.cut(n);
	for (int i = 0; i < 30; ++i) {
		c.d.writes();
		for (int k = 0; k < 300; ++k) { c.w.step(); }
		c.w.settle(false);
		c.d.collect();
		c.d.pass();
	}
	check(c.d.last[g] == sp::step::wait, "the plan for one's group is a wait");
	auto rec = c.record("one");
	if (!check(rec && rec->shards.front().members == one.shards.front().members, std::string("one's record is unchanged") + (rec ? "" : ": no record read"))) { c.d.dump(); }
	check(c.w.replicas.count({g, n}) == 1 && !c.w.leader(g), "its lone voter is down, its group leaderless, no other replica made");
	c.w.restore(n);
	if (!check(c.d.converge(), "back up, the cluster settles")) { return; }
	c.verify("one's node back");
}

// A4: leader balance defers a transfer while the group reports an open multi-entry command (an
// upload's chunks applied, its final entry not) and makes it once the command ends.
void defer(std::uint64_t seed) {
	context = "defer, seed " + std::to_string(seed);
	cluster c(seed);
	auto up = c.create("up", 3);
	if (!check(c.d.converge(), "the cluster settles")) { return; }
	const auto g = up.shards.front().group;
	auto rec = c.record("up");
	if (!check(rec.has_value(), "up's record reads")) { return; }
	const auto preferred = rec->shards.front().members.front().node;
	// Leadership away from the preferred leader, by hand.
	auto* l = c.w.leader(g);
	if (!check(l != nullptr, "up has a leader")) { return; }
	if (l->node == preferred) {
		cs::NodeId other = 0;
		const auto members = l->state()->membership.members;  // a copy: the state is a temporary
		for (const auto& m : members) {
			if (m.node != preferred && m.role == cs::MemberRole::Voter) { other = m.incarnation; }
		}
		auto t = start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(g, other));
		l->run();
		run_until(c.w, t);
		c.w.settle(false);
		l = c.w.leader(g);
		if (!check(l && l->node != preferred, "leadership is away from preferred node " + std::to_string(preferred) + " (transfer to " + std::to_string(other) + ": " +
												  (t->done ? what(t->error) : std::string("in flight")) + ")")) {
			c.d.dump();
			return;
		}
	}
	const auto away = l->inc;
	const auto term = l->state()->term;
	// An upload's chunks: the command is open.
	auto* sr = static_cast<shard_replica*>(l);
	for (const char* chunk : {"U|7|0|abc", "U|7|3|def"}) {
		auto s = start(sr->p->loop_ref(), sr->g->propose(chunk, sg::ack::quorum));
		sr->run();
		run_until(c.w, s);
		check(s->done && !s->error, std::string("chunk ") + chunk + " is applied");
	}
	const auto transfers = c.d.transfers;
	for (int i = 0; i < 10; ++i) {
		for (int k = 0; k < 300; ++k) { c.w.step(); }
		c.w.settle(false);
		c.d.pass();
	}
	l = c.w.leader(g);
	check(c.d.transfers == transfers && c.d.deferred > 0, "10 passes with the upload open: " + std::to_string(c.d.transfers - transfers) + " transfers, " + std::to_string(c.d.deferred) + " deferrals");
	check(l && l->inc == away && l->state()->term == term, "leadership stays on incarnation " + std::to_string(away) + " at term " + std::to_string(term));
	// The final entry ends it; the next passes transfer.
	sr = static_cast<shard_replica*>(c.w.leader(g));
	if (!check(sr != nullptr, "up has a leader for the final entry")) { return; }
	auto f = start(sr->p->loop_ref(), sr->g->propose("F|7|upload", sg::ack::quorum));
	sr->run();
	run_until(c.w, f);
	if (!check(c.d.converge(), "repair converges once the upload ends")) { return; }
	l = c.w.leader(g);
	check(c.d.transfers > transfers && l && l->node == preferred, "made once it ends: leader on node " + std::to_string(l ? l->node : 0) + ", preferred " + std::to_string(preferred));
	if (!l) { return; }
	const auto members = l->state()->membership.members;  // a copy: the state is a temporary
	for (const auto& m : members) {
		auto* r = static_cast<shard_replica*>(c.w.find(g, m.incarnation));
		if (!r) { continue; }
		const auto d = of(*r);
		const auto it = d.documents.find("upload");
		check(it != d.documents.end() && it->second.second == "abcdef", "member " + std::to_string(m.incarnation) + " holds the upload's 6 bytes: the command was not aborted");
	}
	c.verify("after the deferred transfer");
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::uint64_t seeds = argc > 2 ? std::stoull(argv[2]) : 3;
	for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
		if (only.empty() || only == "replace") { replace(seed); }
		if (only.empty() || only == "drain") { drain(seed); }
		if (only.empty() || only == "replicas") { replicas(seed); }
		if (only.empty() || only == "waits") { waits(seed); }
		if (only.empty() || only == "defer") { defer(seed); }
	}
	if (root_test::failures) {
		std::fprintf(stderr, "%d failures\n", root_test::failures);
		return 1;
	}
	std::printf("repair on groups: every case passed, %llu seeds\n", static_cast<unsigned long long>(seeds));
	return 0;
}
