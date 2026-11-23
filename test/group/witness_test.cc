// A two-data-voter-plus-witness group (cluster-release Q3 (a), consensus D9; quest group-heal A4)
// on the voters' world (world.hh): each data member has its strata index and file set over
// faultfs, the witness a `strata::group::witness` with no application state.
//   loss: the group writes on through the loss of any one member, a power cut, and the member
//     rejoins: a lost data voter by the survivor's log or its checkpoint's install.
//   carry: a data voter lost while the others commit comes back as the other one is lost; only
//     the witness holds what it lacks, so the witness leads, refuses proposals and reads,
//     replicates, and hands leadership to it.
//   join: a witness joins a three-voter group by install's membership steps (a learner that
//     installs, promoted beside the voter it replaces, which is then removed) and the group
//     survives the loss of each member.
// Oracle, once settled: every acknowledged command at its placement in the log, every data
// member's digest equal to a replay of it, the witness applied through the commit index with
// its Sink handed no range once it is a witness, and a data voter leading.
// `group_witness_test [case]` runs one case (loss, carry, join); none runs them all.
#include "world.hh"

#include <cstdio>
#include <string>

namespace {

using namespace group_test;

node* data_leader(world& w) {
	auto* l = w.leader();
	return l && !l->witness ? l : nullptr;
}

std::string command(std::mt19937_64& gen, int k) {
	const auto key = keys[gen() % keys.size()];
	return k % 4 == 3 ? "U|" + std::to_string(500 + k) + "|0|" + text(gen, 24) : "P|" + key + "|" + text(gen, 1 + gen() % 16);
}

// A write at the data member that leads, placed again across elections; true once
// acknowledged at `quorum`.
bool write(world& w, const std::string& c) {
	for (int attempt = 0; attempt < 6; ++attempt) {
		node* l = nullptr;
		for (int i = 0; i < 40000 && !(l = data_leader(w)); ++i) { w.step(); }
		if (!l) { return false; }
		auto s = start(l->p->loop_ref(), l->p->g->propose(c));
		l->run();
		for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
		if (s->done && !s->error && s->value) {
			w.acks.push_back({s->value->placement, c});
			return true;
		}
	}
	return false;
}

bool checkpoint(world& w, node& x) {
	auto k = start(x.p->loop_ref(), x.p->g->checkpoint());
	x.run();
	for (int i = 0; i < 40000 && !k->done; ++i) { w.step(); }
	return check(k->done && !k->error, "node " + std::to_string(x.id) + "'s checkpoint: " + what(k->error));
}

bool refused_as_follower(const std::shared_ptr<started<bool>>& s) {
	if (!s->done || !s->error) { return false; }
	try {
		std::rethrow_exception(s->error);
	} catch (const cs::rejected& r) {
		return r.reason == cs::RejectReason::NotLeader;
	} catch (...) {}
	return false;
}

// `ranges`: what the witness's Sink may have been handed (none for a founder).
void oracle(world& w, std::uint64_t ranges) {
	same_digests(w);
	const node* first = nullptr;
	for (auto& [id, x] : w.nodes) {
		if (!first && !x->witness) { first = x.get(); }
	}
	const auto through = first->applied();
	if (const auto log = merged_log(w, through)) {
		for (const auto& a : w.acks) {
			auto it = log->find(a.placement.index);
			check(a.placement.index <= through && it != log->end() && it->second.term == a.placement.term && it->second.command == a.command,
				"acknowledged " + a.command + " at " + std::to_string(a.placement.index) + " is in the log");
		}
	}
	for (auto& [id, x] : w.nodes) {
		if (!x->witness) { continue; }
		const auto st = x->p->lp->driver().state(gid);
		check(st && st->applied == st->committed && st->committed == through, "the witness's applied index follows its commit index to " + std::to_string(through));
		check(x->witness_ranges + x->p->w->ranges() == ranges, "the witness's Sink is handed " + std::to_string(x->witness_ranges + x->p->w->ranges()) + " ranges, " + std::to_string(ranges) + " before it was one");
	}
	check(data_leader(w) != nullptr, "a data voter leads once settled");
}

std::uint64_t losses = 0, witness_leads = 0, joins = 0;

// The group writes on through the loss of `victim`, which rejoins.
void loss(cs::NodeId victim, std::uint64_t seed) {
	context = "2 voters + witness, seed " + std::to_string(seed) + ", node " + std::to_string(victim) + " lost";
	world w(3, seed, false, {3});
	std::mt19937_64 gen(seed);
	if (!check(w.settle(), "the group settles")) { return; }
	int k = 0;
	for (int i = 0; i < 4; ++i) { check(write(w, command(gen, k++)), "a write before the loss"); }
	auto& lost = *w.nodes.at(victim);
	++w.crashes;
	w.shut(lost, true);
	for (int i = 0; i < 6; ++i) { check(write(w, command(gen, k++)), "a write commits without node " + std::to_string(victim)); }
	// The surviving data voter checkpoints past a lost one, which may then install.
	for (auto& [id, x] : w.nodes) {
		if (x->up() && !x->witness && !lost.witness) { checkpoint(w, *x); }
	}
	if (!check(w.settle(), "the group settles with node " + std::to_string(victim) + " back")) { return; }
	for (int i = 0; i < 3; ++i) { check(write(w, command(gen, k++)), "a write once it rejoined"); }
	if (!check(w.settle(), "the group settles after the writes")) { return; }
	oracle(w, 0);
	++losses;
}

// Only the witness holds what a returning data voter lacks: it leads to hand off.
void carry(std::uint64_t seed) {
	context = "2 voters + witness, seed " + std::to_string(seed) + ", the witness carries the log";
	world w(3, seed, false, {3});
	std::mt19937_64 gen(seed);
	if (!check(w.settle(), "the group settles")) { return; }
	auto &a = *w.nodes.at(1), &b = *w.nodes.at(2), &wit = *w.nodes.at(3);
	int k = 0;
	for (int i = 0; i < 3; ++i) { check(write(w, command(gen, k++)), "a write before the loss"); }
	++w.crashes;
	w.shut(a, true);
	for (int i = 0; i < 5; ++i) { check(write(w, command(gen, k++)), "a write commits on node 2 and the witness"); }
	const auto target = w.acks.back().placement.index;
	checkpoint(w, b);  // b's log no longer reaches a
	++w.crashes;
	w.shut(b, true);
	w.boot(a);
	bool led = false;
	std::shared_ptr<started<bool>> proposed, read;
	for (int i = 0; i < 400000 && !(data_leader(w) == &a && a.applied() >= target); ++i) {
		w.step();
		if (!led && w.leader() == &wit) {
			led = true;
			auto propose = [](sg::loop* lp) -> task::task<void> { co_await lp->driver().propose(gid, "P|z|at the witness"); };
			auto confirm = [](sg::loop* lp) -> task::task<void> { co_await lp->driver().read(gid); };
			proposed = start(wit.p->loop_ref(), propose(wit.p->lp.get()));
			read = start(wit.p->loop_ref(), confirm(wit.p->lp.get()));
			wit.run();
		}
	}
	if (!check(led, "the witness leads: node 1 lacks what it committed")) { return; }
	++witness_leads;
	check(proposed->done && refused_as_follower(proposed), "the leading witness refuses a proposal: " + what(proposed->error));
	check(read->done && refused_as_follower(read), "the leading witness refuses a read: " + what(read->error));
	if (!check(data_leader(w) == &a, "the witness hands leadership to node 1")) { return; }
	check(a.applied() >= target, "node 1 applies what only the witness held, through " + std::to_string(target));
	for (int i = 0; i < 3; ++i) { check(write(w, command(gen, k++)), "a write commits on node 1 and the witness"); }
	if (!check(w.settle(), "the group settles with node 2 back")) { return; }
	oracle(w, 0);
}

// A witness joins a three-voter group, replacing a voter, and the group survives each loss.
void join(std::uint64_t seed) {
	context = "3 voters, seed " + std::to_string(seed) + ", a witness replaces a voter";
	world w(3, seed);
	std::mt19937_64 gen(seed);
	if (!check(w.settle(), "the group settles")) { return; }
	write_and_compact(w, gen, 8);
	auto* l = w.leader();
	if (!check(l, "a leader")) { return; }
	cs::NodeId leaving = 0, heir = l->id;
	for (auto& [id, x] : w.nodes) {
		if (id != l->id) { leaving = id; }
	}
	auto& jw = w.add_joiner(4, true);
	if (!check(change(w, 3, [&](cs::Membership& m) {
			m.members.push_back({jw.id, jw.id, cs::MemberRole::Learner});
			m.next_incarnation = jw.id + 1;
		}, heir), "the witness joins as a learner")) { return; }
	if (!check(w.settle(), "the group settles with the learner")) { return; }
	check(jw.p->w->images() >= 1, "the joiner installs past every checkpoint: its image validated, nothing staged");
	if (!check(change(w, 3, [&](cs::Membership& m) {
			for (auto& x : m.members) {
				if (x.incarnation == jw.id) { x.role = cs::MemberRole::Witness; }
			}
			m.leaving = leaving;
		}, heir), "the learner is promoted to witness beside the voter it replaces")) { return; }
	// The core removes the replaced voter (D3), which never learns that removal's commit.
	if (!check(removed_by_core(w, leaving), "the core removes the replaced voter " + std::to_string(leaving))) { return; }
	w.retire(leaving);
	if (!check(w.settle(), "the group settles as two voters and a witness")) { return; }
	const auto ranges = jw.p->w->ranges();
	int k = 0;
	std::vector<cs::NodeId> members;
	for (auto& [id, x] : w.nodes) { members.push_back(id); }
	for (auto victim : members) {
		auto& lost = *w.nodes.at(victim);
		++w.crashes;
		w.shut(lost, true);
		for (int i = 0; i < 3; ++i) { check(write(w, command(gen, k++)), "a write commits without node " + std::to_string(victim)); }
		if (!check(w.settle(), "the group settles with node " + std::to_string(victim) + " back")) { return; }
	}
	oracle(w, ranges);
	++joins;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	if (only.empty() || only == "loss") {
		for (std::uint64_t seed : {1, 2, 3}) {
			for (cs::NodeId victim = 1; victim <= 3; ++victim) { loss(victim, seed); }
		}
	}
	if (only.empty() || only == "carry") {
		for (std::uint64_t seed : {1, 2, 3, 4}) { carry(seed); }
	}
	if (only.empty() || only == "join") {
		for (std::uint64_t seed : {1, 2}) { join(seed); }
	}
	std::printf("witness: %llu losses survived, %llu hand-offs from a leading witness, %llu joins: %s\n", (unsigned long long)losses, (unsigned long long)witness_leads, (unsigned long long)joins,
		failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
