// The root authority on strata::group (quest placement A1): node and namespace records created,
// read and deleted through the leader's prepared commands, with `twitter` and `twitter/user`
// distinct; and at 3 and 5 voters, every minority lost at a seeded event while writes run, the
// survivors electing and taking more writes, with no acknowledged record lost.
// `root_group_test [schedules [first]]`; `0` schedules runs the lone-voter cases alone.
#include "root_world.hh"

#include <cstdio>
#include <string>

namespace {

using namespace root_test;

sp::node_record named(std::string name) {
	sp::node_record n;
	n.name = std::move(name);
	n.addresses = {n.name + ":9880"};
	n.pin = std::string(32, 'k');
	return n;
}

// Every record on a node's index, decoded, by key: the digest the voters compare.
std::map<std::string, std::pair<std::uint64_t, std::string>> records(sp::index_type& ix) {
	std::map<std::string, std::pair<std::uint64_t, std::string>> out;
	ix.scan("", [&](std::string_view k, std::string_view v, std::uint64_t version) { out[std::string(k)] = {version, std::string(v)}; });
	return out;
}

// A replay from empty of a log's commands through the application alone, on a fresh index.
std::map<std::string, std::pair<std::uint64_t, std::string>> replay(const std::map<std::uint64_t, traced::seen_entry>& log, std::uint64_t through) {
	ff::filesystem fs;
	ff::io io(fs);
	strata::basic_index_options<strata::document_layout> o;
	o.keyed = true;
	auto ix = sp::index_type::create(io, o, {'r', 'e', 'f'});
	sp::root app;
	sp::root::state s;
	for (std::uint64_t i = 1; i <= through; ++i) {
		const auto& e = log.at(i);
		const sg::entry view{i, e.term, e.kind, e.command};
		if (auto t = app.transaction(s, view)) { ix.apply(*t, strata::files::position{i, 1}); }
	}
	return records(ix);
}

std::shared_ptr<started<sg::submitted>> add(world& w, node& at, std::string name, sp::node_id* id = nullptr) {
	return w.submit(at, [name, id](const sp::view& v) { return sp::add_node(v, named(name), id); });
}
std::shared_ptr<started<sg::submitted>> create(world& w, node& at, std::string path, std::size_t shards, std::uint32_t rf, std::shared_ptr<sp::namespace_record> placed = nullptr) {
	return w.submit(at, [path, shards, rf, placed](const sp::view& v) { return sp::create_namespace(v, path, shards, rf, 0, placed.get()); });
}
bool refused(const std::shared_ptr<started<sg::submitted>>& s, strata::verdict::outcome why) {
	return s->done && !s->error && s->value->refused && s->value->refused->result == why;
}

// One voter: the records' lifecycle with values.
void lone() {
	context = "lone voter";
	world w(1, 1);
	if (!check(w.settle(), "the root settles")) { return; }
	auto& l = *w.leader();
	sp::node_id ids[3] = {};
	for (int i = 0; i < 3; ++i) {
		auto s = add(w, l, "node-" + std::to_string(i), &ids[i]);
		w.run_until(s);
		check(accepted(s), "node " + std::to_string(i) + " is added: " + what(s->error));
		check(ids[i] == sp::node_id(i + 1), "node ids count from 1");
	}
	auto tw = std::make_shared<sp::namespace_record>(), user = std::make_shared<sp::namespace_record>();
	auto a = create(w, l, "twitter", 2, 3, tw);
	w.run_until(a);
	auto b = create(w, l, "twitter/user", 1, 2, user);
	w.run_until(b);
	check(accepted(a) && accepted(b), "twitter and twitter/user are created");
	auto again = create(w, l, "twitter", 1, 1);
	w.run_until(again);
	check(refused(again, strata::verdict::version_conflict), "a second twitter is refused at prepare, nothing placed");
	check(again->value && again->value->placements == 0, "the refusal placed nothing");
	auto bad = create(w, l, "twitter//x", 1, 1);
	w.run_until(bad);
	check(refused(bad, strata::verdict::invalid), "an empty name is invalid");

	auto r = w.read(l, [](const sp::view& v) { return std::pair{v.namespace_at("twitter"), v.namespace_at("twitter/user")}; });
	check(r->done && !r->error, "the read answers");
	if (r->value) {
		const auto& [t, u] = *r->value;
		check(t && t->record == *tw && t->version == 1, "twitter reads as placed");
		check(u && u->record == *user && u->version == 1, "twitter/user reads as placed, a record of its own");
		check(tw->shards.size() == 2 && tw->shards[0].members.size() == 3 && user->shards[0].members.size() == 3, "3 data members at rf 3; 2 and a witness at rf 2");
		check(tw->shards[0].group == 1 && tw->shards[1].group == 2 && user->shards[0].group == 3, "group ids from the counter, never reused");
	}
	// Loads follow placement: 2 x 3 + 3 memberships, 3 leaderships.
	auto loads = w.read(l, [&](const sp::view& v) {
		std::uint64_t g = 0, k = 0;
		for (auto id : ids) {
			g += v.load(id)->record.groups;
			k += v.load(id)->record.leaders;
		}
		return std::pair{g, k};
	});
	check(loads->value && *loads->value == std::pair<std::uint64_t, std::uint64_t>{9, 3}, "loads count every membership and leadership");

	// A node with groups is not removed; a node update moves the directory version.
	auto rm = w.submit(l, [&](const sp::view& v) { return sp::remove_node(v, ids[0]); });
	w.run_until(rm);
	check(refused(rm, strata::verdict::version_conflict), "a node holding groups is not removed");
	auto up = w.submit(l, [&](const sp::view& v) {
		auto n = named("node-0");
		n.state = sp::node_state::leaving;
		return sp::update_node(v, ids[0], n);
	});
	w.run_until(up);
	check(accepted(up), "a node update is accepted");
	auto dir = w.read(l, [&](const sp::view& v) { return std::pair{v.directory()->version, v.node(ids[0])->record}; });
	if (check(dir->value.has_value(), "the directory reads")) {
		check(dir->value->first == 4 && dir->value->second.directory_version == 4 && dir->value->second.state == sp::node_state::leaving,
			"the update wrote directory version 4 (three adds, one update) into the node record");
	}
	// A leaving node takes no new shard.
	auto later = std::make_shared<sp::namespace_record>();
	auto c = create(w, l, "later", 3, 3, later);
	w.run_until(c);
	bool avoided = accepted(c);
	for (const auto& s : later->shards) {
		for (const auto& m : s.members) { avoided = avoided && m.node != ids[0]; }
	}
	check(avoided, "placement skips a node that is not active");

	// Deletes: twitter goes, twitter/user stays; then the node holds nothing and is removed.
	for (const char* p : {"twitter", "later"}) {
		auto d = w.submit(l, [p](const sp::view& v) { return sp::delete_namespace(v, p); });
		w.run_until(d);
		check(accepted(d), std::string("delete ") + p);
	}
	auto gone = w.read(l, [](const sp::view& v) { return std::pair{v.namespace_at("twitter").has_value(), v.namespace_at("twitter/user").has_value()}; });
	check(gone->value && *gone->value == std::pair{false, true}, "deleting twitter leaves twitter/user");
	auto d = w.submit(l, [](const sp::view& v) { return sp::delete_namespace(v, "twitter/user"); });
	w.run_until(d);
	auto rm2 = w.submit(l, [&](const sp::view& v) { return sp::remove_node(v, ids[0]); });
	w.run_until(rm2);
	check(accepted(d) && accepted(rm2), "with its namespaces gone the node is removed");
	auto after = w.read(l, [&](const sp::view& v) {
		std::uint64_t g = 0;
		for (auto id : {ids[1], ids[2]}) { g += v.load(id)->record.groups + v.load(id)->record.leaders; }
		return std::pair{v.node(ids[0]).has_value() || v.load(ids[0]).has_value(), g};
	});
	check(after->value && *after->value == std::pair<bool, std::uint64_t>{false, 0}, "the removed node's records are gone and every load is back to zero");

	// Five creations at once: each prepared from the view with the others pending, one placement each.
	std::vector<std::shared_ptr<started<sg::submitted>>> burst;
	for (int i = 0; i < 5; ++i) { burst.push_back(create(w, l, "burst/" + std::to_string(i), 2, 2)); }
	bool one_each = true;
	for (auto& s : burst) {
		w.run_until(s);
		one_each = one_each && accepted(s) && s->value->placements == 1;
	}
	check(one_each, "concurrent creations each take one placement: the pending ones are in the view");
	auto ids_after = w.read(l, [](const sp::view& v) {
		std::set<sp::group_id> seen;
		std::size_t n = 0;
		for (int i = 0; i < 5; ++i) {
			for (const auto& s : v.namespace_at("burst/" + std::to_string(i))->record.shards) {
				seen.insert(s.group);
				++n;
			}
		}
		return seen.size() == n;
	});
	check(ids_after->value && *ids_after->value, "concurrent creations take distinct group ids");

	// A command prepared from a view another command has since changed loses its race: strata
	// refuses it on the version it expected, and the record stays as the winner wrote it.
	std::string stale;
	auto prep = w.read(l, [&](const sp::view& v) { return sp::create_namespace(v, "race", 1, 1).command; });
	if (check(prep->value.has_value(), "a stale command is prepared")) { stale = *prep->value; }
	auto winner = std::make_shared<sp::namespace_record>();
	auto won = create(w, l, "race", 2, 2, winner);
	w.run_until(won);
	auto lost = start(l.p->loop_ref(), l.p->g->propose(stale));
	w.run_until(lost);
	check(accepted(won) && lost->done && !lost->error && lost->value->applied && lost->value->applied->result == strata::verdict::version_conflict, "the stale command is refused on its expected version");
	auto race = w.read(l, [](const sp::view& v) { return v.namespace_at("race"); });
	check(race->value && *race->value && (*race->value)->record == *winner, "the record is the winner's");

	// A command that fails its decoder is refused `invalid` and writes nothing.
	for (std::string bytes : {std::string("\x01\x01\x02\x01" "D\x00", 6), std::string("garbage")}) {
		auto p = start(l.p->loop_ref(), l.p->g->propose(bytes));
		w.run_until(p);
		check(p->done && !p->error && p->value->applied && p->value->applied->result == strata::verdict::invalid, "a malformed command is answered invalid");
	}
	check(w.settle(), "the root settles");
	check(records(l.p->g->strata()) == replay(l.log, l.p->g->applied()), "the root's records equal a replay of its log");
}

// Voters: a minority (every one, across seeds) lost at a seeded event while writes run.
struct outcome {
	std::uint64_t acked = 0, applied = 0;
};
outcome minority(unsigned voters, std::uint64_t seed) {
	world w(voters, seed);
	std::mt19937_64 gen(seed ^ 0x9e3779b97f4a7c15ull);
	// The minority: the seed's subset of the largest minority size, so the seeds cover every one.
	std::vector<std::set<cs::NodeId>> subsets;
	const unsigned size = (voters - 1) / 2;
	for (unsigned mask = 0; mask < (1u << voters); ++mask) {
		if (unsigned(__builtin_popcount(mask)) != size) { continue; }
		std::set<cs::NodeId> s;
		for (unsigned b = 0; b < voters; ++b) {
			if (mask & (1u << b)) { s.insert(cs::NodeId(b + 1)); }
		}
		subsets.push_back(s);
	}
	const auto& lost = subsets[seed % subsets.size()];
	const std::uint64_t events = 2500, lose_at = 300 + gen() % 1200;
	const bool restart = seed % 2 == 0;  // a survivor's power cut and restart after the loss
	const std::uint64_t restart_at = lose_at + 200 + gen() % 400;
	const cs::NodeId restarted = [&] {
		for (cs::NodeId id = 1; id <= voters; ++id) {
			if (!lost.count(id)) { return id; }
		}
		return cs::NodeId(0);
	}();
	std::string list;
	for (auto id : lost) { list += " " + std::to_string(id); }
	context = std::to_string(voters) + " voters, seed " + std::to_string(seed) + ", lost" + list + " at event " + std::to_string(lose_at) +
		(restart ? ", node " + std::to_string(restarted) + " restarted at " + std::to_string(restart_at) : "");
	outcome out;
	if (!check(w.settle(), "the root settles")) { return out; }

	struct request {
		std::string path;  // empty for a node
		bool erase = false;
		std::shared_ptr<sp::namespace_record> placed;
		std::shared_ptr<started<sg::submitted>> s;
	};
	// What the acknowledged answers say of each path; a path whose last request has no
	// acknowledged answer is in neither, since its outcome is unknown.
	std::map<std::string, sp::namespace_record> created;
	std::set<std::string> deleted;
	const auto settle_path = [&](const request& r) {
		if (r.s->done && !r.s->error && r.s->value->refused) { return; }  // refused at prepare: nothing placed
		created.erase(r.path);
		deleted.erase(r.path);
		if (!accepted(r.s)) { return; }
		if (r.erase) {
			deleted.insert(r.path);
		} else {
			created[r.path] = *r.placed;
		}
	};
	std::uint64_t nodes_acked = 0;
	std::vector<request> in_flight;
	const std::vector<std::string> names = {"a", "a/b", "a/b/c", "ab", "a-b", "b", "b/a", ".x", ".x/y", "twitter", "twitter/user", "z/z"};
	std::uint64_t added = 0;
	for (std::uint64_t e = 0; e < events; ++e) {
		if (e == lose_at) {
			for (auto id : lost) {
				auto& x = *w.nodes.at(id);
				if (x.up()) { w.shut(x, true); }
				x.lost = true;
			}
		}
		if (restart && e == restart_at && w.nodes.at(restarted)->up()) { w.shut(*w.nodes.at(restarted), true); }
		if (restart && e == restart_at + 100 && !w.nodes.at(restarted)->up()) { w.boot(*w.nodes.at(restarted)); }
		std::erase_if(in_flight, [&](const request& r) {
			if (!r.s->done) { return false; }
			if (accepted(r.s)) {
				++out.acked;
				nodes_acked += r.path.empty();
			}
			if (!r.path.empty()) { settle_path(r); }
			return true;
		});
		if (in_flight.size() < 3 && gen() % 4 == 0) {
			if (auto* l = w.leader()) {
				request r;
				if (added < 4 || gen() % 6 == 0) {
					++added;
					r.s = add(w, *l, "n" + std::to_string(added));
				} else {
					r.path = names[gen() % names.size()];
					bool busy = false;
					for (const auto& f : in_flight) { busy = busy || f.path == r.path; }
					if (busy) {
						w.step();
						continue;
					}
					if (created.count(r.path) && gen() % 2 == 0) {
						r.erase = true;
						r.s = w.submit(*l, [p = r.path](const sp::view& v) { return sp::delete_namespace(v, p); });
					} else {
						r.placed = std::make_shared<sp::namespace_record>();
						r.s = create(w, *l, r.path, 1 + gen() % 3, std::uint32_t(1 + gen() % 3), r.placed);
					}
				}
				in_flight.push_back(std::move(r));
			}
		}
		w.step();
	}
	if (!check(w.settle(), "the survivors settle")) { return out; }
	for (const auto& r : in_flight) {
		if (!r.path.empty()) { settle_path(r); }
		if (accepted(r.s)) {
			++out.acked;
			nodes_acked += r.path.empty();
		}
	}
	// The oracle: every survivor holds every acknowledged record, the same records, and a replay.
	const node* first = nullptr;
	for (auto& [id, x] : w.nodes) {
		if (!x->up()) { continue; }
		if (!first) { first = x.get(); }
		const auto who = "node " + std::to_string(id);
		check(!x->app.diverged, who + " applied an index twice with different entries");
		check(!x->p->g->fenced(), who + " is not fenced: " + x->p->g->fence_reason());
		auto& ix = x->p->g->strata();
		const sp::view v(ix);
		for (const auto& [path, ns] : created) {
			const auto got = v.namespace_at(path);
			check(got && got->record == ns, who + " holds acknowledged " + path);
		}
		for (const auto& path : deleted) { check(!v.namespace_at(path), who + " holds no acknowledged delete " + path); }
		std::uint64_t nodes_held = 0;
		ix.scan(sp::nodes_begin(), sp::nodes_end(), [&](std::string_view, std::string_view, std::uint64_t) { ++nodes_held; });
		check(nodes_held >= nodes_acked, who + " holds every acknowledged node");
		const auto through = x->p->g->applied();
		out.applied = through;
		check(records(ix) == replay(x->log, through), who + "'s records equal a replay of its log");
		if (first != x.get()) { check(records(ix) == records(first->p->g->strata()), who + "'s records equal node " + std::to_string(first->id) + "'s"); }
	}
	return out;
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t schedules = argc > 1 ? std::stoull(argv[1]) : 20, first = argc > 2 ? std::stoull(argv[2]) : 1;
	lone();
	std::uint64_t acked = 0, applied = 0, runs = 0;
	for (unsigned voters : {3u, 5u}) {
		for (std::uint64_t seed = first; seed < first + schedules; ++seed) {
			const auto o = minority(voters, seed);
			acked += o.acked;
			applied += o.applied;
			++runs;
		}
	}
	std::printf("root group: lone voter, %llu minority-loss schedules (3 and 5 voters, seeds %llu-%llu), %llu acknowledged, %llu entries applied: %s\n", (unsigned long long)runs,
		(unsigned long long)first, (unsigned long long)(first + schedules - 1), (unsigned long long)acked, (unsigned long long)applied, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
