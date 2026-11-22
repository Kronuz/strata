// strata::group at three and five voters in one process (quest group-core A2). Each node has
// its own journal host, strata index and file set over faultfs, a loop and a chore on inline
// schedulers, and its turn IO held; the network delivers in seeded order. A schedule writes
// puts, conditional writes, deletes and uploads through the leader with `quorum`, takes
// checkpoints, and cuts one node's power at a seeded event (unsynced sectors and directory
// entries lost), restarting it later. Storage schedules make one replica's budget refuse its
// strata writes and another's file-set push fail partway with ENOSPC, then free both.
// A replica behind the leader's checkpoint installs it (group-install): transfer frames and
// fetches ride the same seeded network, and a node cut off or down answers neither.
// Oracle, after the cluster settles: every `quorum`-acknowledged command is at its placement in
// the log, every node applied the same entries (an installed one those past its install), and
// every node's digest equals the others' and a replay from empty of the log.
// `group_voters_test [schedules [first]]`; a failure names its voters, seed and crash point;
// `0` schedules runs the named cases alone.
#include "world.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace group_test;

std::uint64_t checkpoints_started = 0;  // by the gated schedules
struct outcome {
	std::uint64_t acked = 0, applied = 0, stalls = 0, crashes = 0, installs = 0;
};

// One schedule. `storage`: a replica's budget refuses its strata writes and another's
// file-set push fails partway with ENOSPC between two events, then both free. `gated`: its
// checkpoints leave no log behind (D109); ungated, a replica left behind must be named in
// `installs_needed()` (the seeds that do, until group-install).
outcome schedule(unsigned voters, std::uint64_t seed, bool storage, bool gated = true) {
	world w(voters, seed);
	std::mt19937_64 gen(seed ^ 0x9e3779b97f4a7c15ull);
	const std::uint64_t events = 1500;
	const std::uint64_t crash_at = 100 + gen() % 1200, restart_at = crash_at + 50 + gen() % 300;
	const cs::NodeId victim = cs::NodeId(1 + gen() % voters);
	const std::uint64_t space_from = 200 + gen() % 600, space_to = space_from + 150 + gen() % 200;
	const cs::NodeId budget_node = cs::NodeId(1 + gen() % voters), push_node = cs::NodeId(1 + (budget_node % voters));
	context = std::to_string(voters) + " voters, seed " + std::to_string(seed) + ", node " + std::to_string(victim) + " cut at event " + std::to_string(crash_at) + ", up at " + std::to_string(restart_at) +
		(storage ? ", space short on nodes " + std::to_string(budget_node) + " and " + std::to_string(push_node) + " in events " + std::to_string(space_from) + "-" + std::to_string(space_to) : "");
	std::vector<request> in_flight;
	std::optional<strata::byte_budget::permit> hog;
	std::shared_ptr<started<cs::CheckpointPublished>> checkpoint;
	std::uint64_t upload = 0, uploading = 0, upload_at = 0;
	outcome out;
	for (std::uint64_t e = 0; e < events; ++e) {
		if (e == crash_at && w.nodes.at(victim)->up()) {
			++w.crashes;
			w.shut(*w.nodes.at(victim), true);
		}
		if (e == restart_at && !w.nodes.at(victim)->up()) { w.boot(*w.nodes.at(victim)); }
		if (storage && e == space_from) {
			auto& b = *w.nodes.at(budget_node);
			hog = b.storage.open();
			check(hog->try_grow(b.storage.capacity() - b.storage.used() - 16), "the budget is nearly spent");
			auto& f = *w.nodes.at(push_node);
			f.dir_disk.short_writes(4);
			f.dir_disk.set_capacity(f.dir_disk.used() + 6);
		}
		if (storage && e == space_to) {
			hog.reset();
			auto& f = *w.nodes.at(push_node);
			f.dir_disk.set_capacity(~std::uint64_t(0));
			f.dir_disk.short_writes(0);
			for (auto id : {budget_node, push_node}) {
				if (auto& x = *w.nodes.at(id); x.open()) { x.p->g->resume(); }
			}
		}
		// Answers in, then maybe a new request at the leader.
		std::erase_if(in_flight, [&](const request& r) {
			if (!r.s->done) { return false; }
			if (!r.s->error && r.s->value) {
				w.acks.push_back({r.s->value->placement, r.command});
				++out.acked;
			}
			return true;
		});
		if (in_flight.size() < 3 && gen() % 4 == 0) {
			if (auto* l = w.leader()) {
				std::string c;
				const auto key = group_test::keys[gen() % group_test::keys.size()];
				switch (gen() % 8) {
				case 0: {
					// Drawn in order: a + chain's operands are unsequenced (gcc on x86_64 evaluates the right first).
					const auto body = text(gen, 1 + gen() % 12);
					c = "C|" + key + "|" + body + "|" + std::to_string(1 + gen() % 3);
					break;
				}
				case 1: c = "D|" + key; break;
				case 2:
					if (!uploading) {
						uploading = ++upload;
						upload_at = 0;
					}
					{
						auto chunk = text(gen, 1 + gen() % 8);
						c = "U|" + std::to_string(uploading) + "|" + std::to_string(upload_at) + "|" + chunk;
						upload_at += chunk.size();
					}
					break;
				case 3:
					if (uploading) {
						c = (gen() % 4 ? "F|" + std::to_string(uploading) + "|" + key : "A|" + std::to_string(uploading));
						uploading = 0;
						break;
					}
					[[fallthrough]];
				default: c = "P|" + key + "|" + text(gen, 1 + gen() % 20); break;
				}
				in_flight.push_back({c, start(l->p->loop_ref(), l->p->g->propose(c))});
				l->run();
			}
		}
		if (!checkpoint && gen() % 150 == 0) {
			auto& x = *w.nodes.at(cs::NodeId(1 + gen() % voters));
			const bool window = storage && e >= space_from && e < space_to;
			if (x.open() && (!gated || w.leaves_none_behind(x, window ? std::set<cs::NodeId>{budget_node, push_node} : std::set<cs::NodeId>{}))) {
				checkpoint = start(x.p->loop_ref(), x.p->g->checkpoint());
				x.run();
				if (gated) {
					w.capture(x);
					++checkpoints_started;
				}
			}
		}
		if (checkpoint && checkpoint->done) { checkpoint.reset(); }
		w.step();
	}
	if (storage) {
		hog.reset();
		auto& f = *w.nodes.at(push_node);
		f.dir_disk.set_capacity(~std::uint64_t(0));
		f.dir_disk.short_writes(0);
	}
	if (!check(w.settle(), "the cluster settles")) { return out; }
	for (auto& [id, x] : w.nodes) {
		if (x->p->g->stalled()) { x->p->g->resume(); }
	}
	check(w.settle(), "the cluster settles after a resume");
	for (const auto& r : in_flight) {
		if (r.s->done && !r.s->error && r.s->value) { w.acks.push_back({r.s->value->placement, r.command}); }
	}
	// The oracle.
	const auto& first = *w.nodes.begin()->second;
	const auto through = first.p->g->applied();
	out.applied = through;
	out.stalls = w.stalls;
	out.crashes = w.crashes;
	const auto log = merged_log(w, through);
	if (!log) { return out; }
	for (const auto& a : w.acks) {
		auto it = log->find(a.placement.index);
		check(a.placement.index <= through && it != log->end() && it->second.term == a.placement.term && it->second.command == a.command,
			"acknowledged " + a.command + " at " + std::to_string(a.placement.index) + " is in the log");
	}
	const auto reference = replay(*log, through);
	for (auto& [id, x] : w.nodes) {
		const auto who = "node " + std::to_string(id);
		out.installs += x->installs + x->p->g->installs();
		check(!x->app.diverged, who + " applied an index twice with different entries");
		check(!x->p->g->fenced(), who + " is not fenced: " + x->p->g->fence_reason());
		check(x->p->g->applied() == through, who + " applied " + std::to_string(x->p->g->applied()) + ", node 1 " + std::to_string(through));
		try {
			check(of(*x->p->g, x->app) == reference, who + "'s digest equals a replay from empty through " + std::to_string(through));
		} catch (const std::exception& e) {
			check(false, who + "'s digest reads: " + e.what());
		}
	}
	return out;
}

// A checkpoint whose admission can never fit fails loudly instead of waiting (hunt row: at
// consensus's default limits the reservation exceeds a 64 MiB replacement pool).
void default_limits() {
	context = "default limits";
	node x;
	x.id = 1;
	x.app.seen = &x.log;
	task::inline_scheduler loop_s, chore_s;
	j::InlineCompletion io;
	j::Host host(std::make_shared<group_test::host_io>(x.host_disk), j::Identity{'D', 'L'}, j::HostOptions{1, turn_options});
	sg::loop lp(task::scheduler_ref::of(loop_s), j::completion_ref::of(io), host, 0, 1);
	task::cancel_source stop;
	bool turns_done = false, driver_done = false;
	task::spawn(task::scheduler_ref::of(loop_s), host.driver(0).run(j::completion_ref::of(io), stop.token()), [&](std::exception_ptr) { turns_done = true; });
	task::spawn(task::scheduler_ref::of(loop_s), lp.run(stop.token()), [&](std::exception_ptr) { driver_done = true; });
	auto c = config(1, 1, x.storage);
	// consensus's default log, a 64 MiB one; the wire limits stay the test's, so a batch fits one
	// turn-log segment (D132).
	c.options.limits.log_bytes = cs::Limits{}.log_bytes;
	c.options.limits.log_entries = cs::Limits{}.log_entries;
	group_type g(lp, c, std::make_unique<fl::io>(x.dir_disk), x.app, task::scheduler_ref::of(chore_s));
	auto run_all = [&] {
		for (int i = 0; i < 100000; ++i) {
			if (!loop_s.run() && !chore_s.run()) { return; }
		}
	};
	auto o = start(task::scheduler_ref::of(loop_s), g.open());
	run_all();
	check(o->done && !o->error, "open: " + what(o->error));
	auto p = start(task::scheduler_ref::of(loop_s), g.propose("P|a|alpha"));
	run_all();
	check(p->done && !p->error, "propose: " + what(p->error));
	auto k = start(task::scheduler_ref::of(loop_s), g.checkpoint());
	run_all();
	check(k->done, "the checkpoint answers, never waits on an admission it cannot get");
	check(k->done && k->error, "the checkpoint fails: its reservation exceeds the replacement pool");
	stop.request(task::cancel_reason::shutdown);
	run_all();
	g.fenced("the process stopped");
	run_all();
	check(turns_done && driver_done, "the default-limits loop stops");
}

// A `quorum` waiter across a close and a reopen (journal group_closed): a leader cut off from
// its followers places a write it cannot commit and closes; the close answers the waiter
// `closed`. The group reopens, its records after the closed one's retired
// sequences once detent:consensus-quiescence closes the journal group (the node restarts until
// then); the cluster heals and settles to equal digests.
void close_with_uncommitted(unsigned voters) {
	context = std::to_string(voters) + " voters, close with an uncommitted write";
	world w(voters, 77);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	for (const auto* c : {"P|a|before", "P|b|before"}) {
		auto s = start(l.p->loop_ref(), l.p->g->propose(c));
		for (int i = 0; i < 20000 && !s->done; ++i) { w.step(); }
		check(s->done && !s->error, std::string(c) + ": " + what(s->error));
	}
	if (!check(w.settle(), "the cluster settles before the cut")) { return; }
	w.isolated.insert(l.id);
	auto s = start(l.p->loop_ref(), l.p->g->propose("P|c|cut-off"));
	for (int i = 0; i < 200; ++i) {
		while (l.p->held.held()) {
			l.p->held.complete(l.p->held.tickets().front());
			l.run();
		}
		l.p->chore_s.run();
		l.run();
	}
	check(!s->done, "the cut-off write waits for a quorum");
	auto k = start(l.p->loop_ref(), l.p->g->close());
	for (int i = 0; i < 20000 && !(k->done && s->done); ++i) {
		while (l.p->held.held()) {
			l.p->held.complete(l.p->held.tickets().front());
			l.run();
		}
		l.p->chore_s.run();
		l.p->loop_s.advance(ms);
		l.run();
	}
	check(k->done && !k->error, "close: " + what(k->error));
	check(s->done && s->error, "the cut-off write's waiter answers at the close, unacknowledged");
	l.p->chore_s.run();
	// In-process reopen waits for detent:consensus-quiescence: the node restarts instead.
	w.shut(l, false);
	w.isolated.clear();
	if (!check(w.settle(), "the cluster settles after the reopen")) { return; }
	auto& now_leader = *w.leader();
	auto t = start(now_leader.p->loop_ref(), now_leader.p->g->propose("P|d|after"));
	if (!check(w.settle(), "the cluster settles after a write")) { return; }
	check(t->done && !t->error, "the write after the reopen: " + what(t->error));
	// The reopened node recovers once more, its log read across the retired sequences.
	w.shut(l, true);
	if (!check(w.settle(), "the cluster settles after the reopened node's crash")) { return; }
	const auto& first = *w.nodes.begin()->second;
	const auto through = first.p->g->applied();
	const auto reference = replay(first.log, through);
	for (auto& [id, x] : w.nodes) {
		check(!x->p->g->fenced(), "node " + std::to_string(id) + " is not fenced: " + x->p->g->fence_reason());
		check(x->p->g->applied() == through, "node " + std::to_string(id) + " applied what node 1 did");
		check(of(*x->p->g, x->app) == reference, "node " + std::to_string(id) + "'s digest equals the replay");
	}
}

// A3's levels: with the followers' appends held, `none` answers at ProposalPlaced and `quorum`
// does not; once they flow, `quorum` answers with apply's verdict.
void levels(unsigned voters) {
	context = std::to_string(voters) + " voters, ack levels";
	world w(voters, 91);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	w.isolated.insert(l.id);
	auto none = start(l.p->loop_ref(), l.p->g->propose("P|a|none", sg::ack::none));
	auto quorum = start(l.p->loop_ref(), l.p->g->propose("C|a|quorum|1", sg::ack::quorum));
	for (int i = 0; i < 400; ++i) {
		while (l.p->held.held()) {
			l.p->held.complete(l.p->held.tickets().front());
			l.run();
		}
		l.p->chore_s.run();
		l.run();
	}
	check(none->done && !none->error && !none->value->applied, "`none` answers at ProposalPlaced, without a verdict: " + what(none->error));
	check(!quorum->done, "`quorum` waits while the followers' appends are held");
	w.net.clear();
	w.isolated.clear();
	if (!check(w.settle(), "the cluster settles once appends flow")) { return; }
	check(quorum->done && !quorum->error && quorum->value && quorum->value->applied, "`quorum` answers with a verdict: " + what(quorum->error));
	if (quorum->value && quorum->value->applied) { check(quorum->value->applied->result == strata::verdict::accepted, "the conditional write at version 1 is accepted"); }
}

// A4: a leader partitioned away and deposed never answers `read_applied` with a state older
// than a write the new leader committed: its quorum probe waits or fails until it hears the
// cluster, and once it does it applies through the write first.
void deposed_read(unsigned voters) {
	context = std::to_string(voters) + " voters, a deposed leader's read";
	world w(voters, 123);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto write = [&](const std::string& c) {
		auto* l = w.leader();
		if (!l) { return false; }
		auto s = start(l->p->loop_ref(), l->p->g->propose(c));
		for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
		return s->done && !s->error;
	};
	check(write("P|a|one"), "the first write");
	if (!check(w.settle(), "the cluster settles after it")) { return; }
	auto& old = *w.leader();
	w.isolated.insert(old.id);
	bool deposed = false;
	for (int i = 0; i < 40000 && !deposed; ++i) {
		w.step();
		auto* l = w.leader();
		deposed = l && l != &old;
		if (!deposed) {
			for (auto& [id, x] : w.nodes) {
				if (x.get() == &old || !x->open()) { continue; }
				auto st = x->p->lp->driver().state(gid);
				deposed = deposed || (st && st->role == cs::Role::Leader);
			}
		}
	}
	if (!check(deposed, "the others elect a new leader")) { return; }
	node* fresh = nullptr;
	for (auto& [id, x] : w.nodes) {
		if (x.get() == &old || !x->open()) { continue; }
		if (auto st = x->p->lp->driver().state(gid); st && st->role == cs::Role::Leader) { fresh = x.get(); }
	}
	auto s = start(fresh->p->loop_ref(), fresh->p->g->propose("P|a|two"));
	for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
	if (!check(s->done && !s->error, "the new leader commits the second write: " + what(s->error))) { return; }
	auto r = start(old.p->loop_ref(), old.p->g->read_applied([](blob::index_type& ix) -> std::uint64_t {
		auto found = ix.find_key("a");
		return found ? std::uint64_t(found->second) : 0;
	}));
	for (int i = 0; i < 4000 && !r->done; ++i) { w.step(); }
	check(!r->done || r->error || (r->value && *r->value >= 2), "the cut-off read never answers the first write's state");
	w.isolated.clear();
	check(w.settle(), "the cluster settles once healed");
	check(r->done, "the read answers once healed");
	check(!r->done || r->error || (r->value && *r->value >= 2), "the healed read answers the second write's state or fails");
}

// Every node applied the same entries, and its digest equals a replay from empty of the log.
// A replica behind the leader's checkpoint catches up only by install: the leader names it in
// `installs_needed` until the install ends, and it installs the bundle and applies past it.
void install_gap(unsigned voters) {
	context = std::to_string(voters) + " voters, a replica behind the leader's checkpoint";
	world w(voters, 55);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	auto& behind = *w.nodes.at(l.id % voters + 1);
	w.isolated.insert(behind.id);
	auto write = [&](const std::string& c) {
		auto s = start(l.p->loop_ref(), l.p->g->propose(c));
		for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
		return s->done && !s->error;
	};
	check(write("P|a|one") && write("P|b|two"), "the others commit without it");
	// Every other replica checkpoints past it, so whichever leads has no log for it.
	cs::Index base = ~cs::Index(0);
	for (auto& [id, x] : w.nodes) {
		if (x.get() == &behind) { continue; }
		auto k = start(x->p->loop_ref(), x->p->g->checkpoint());
		for (int i = 0; i < 40000 && !k->done; ++i) { w.step(); }
		if (!check(k->done && !k->error && k->value, "node " + std::to_string(id) + "'s checkpoint: " + what(k->error))) { return; }
		base = std::min(base, k->value->through);
	}
	if (!check(behind.p->g->applied() < base, "the checkpoints pass the isolated replica")) { return; }
	w.isolated.clear();
	std::set<cs::NodeId> named;
	for (int i = 0; i < 20000 && named.empty(); ++i) {
		w.step();
		for (auto& [id, x] : w.nodes) {
			if (x->open()) { named.merge(x->p->g->installs_needed()); }
		}
	}
	check(named == std::set<cs::NodeId>{behind.id}, "the leader names the replica behind its checkpoint");
	if (!check(w.settle(), "the cluster settles by the install")) { return; }
	check(behind.p->g->installs() == 1 && behind.p->g->installed_at() >= base, "the replica installs a bundle at or past the checkpoints: " + std::to_string(behind.p->g->installs()) + " at " + std::to_string(behind.p->g->installed_at()));
	for (auto& [id, x] : w.nodes) { check(x->p->g->installs_needed().empty(), "node " + std::to_string(id) + " names no install once it ended"); }
	same_digests(w);
}

// --- A3: the write path (Q10 (b), PLAN.md D106) --------------------------------------------

using writer_type = sg::writer<group_type>;
struct no_forward {
	task::task<sg::submitted> operator()(unsigned) const { co_return sg::submitted{}; }
};
struct writers_of {
	world* w;
	std::map<cs::NodeId, std::unique_ptr<writer_type>> at;
	std::uint64_t forwards = 0;
};
auto prepare_at(node& x, std::string request) {
	return [&x, request = std::move(request)](blob::index_type& ix, const std::vector<std::string>& pending) { return x.app.prepare(ix, pending, request); };
}
// The host's forward: the original request to whichever replica leads now, once.
task::task<sg::submitted> forward_to_leader(writers_of* ws, std::string request, unsigned placements) {
	++ws->forwards;
	// The leader of the highest term: a cut-off one may still believe it leads.
	node* l = nullptr;
	cs::Term top = 0;
	for (auto& [id, x] : ws->w->nodes) {
		if (!x->open()) { continue; }
		if (auto st = x->p->lp->driver().state(gid); st && st->role == cs::Role::Leader && st->term > top) {
			top = st->term;
			l = x.get();
		}
	}
	if (!l) { co_return sg::submitted{sg::next_step::unavailable, std::nullopt, std::nullopt, placements, true}; }
	co_await task::schedule_on{l->p->loop_ref()};
	co_return co_await ws->at.at(l->id)->submit(prepare_at(*l, request), no_forward{}, placements, true);
}
task::task<sg::submitted> submit_at(writers_of* ws, node* x, std::string request) {
	co_return co_await ws->at.at(x->id)->submit(prepare_at(*x, request), [ws, request](unsigned placements) { return forward_to_leader(ws, request, placements); });
}

struct a3_totals {
	std::uint64_t requests = 0, forwards = 0, superseded = 0, unavailable = 0, placements = 0;
};

// Two merges to one id, a client's conditional write and an invalid document, submitted at the
// leader in seeded order while it is cut off and another elected; `stale`: the predictor misses
// (as one that misses the fields mark), so the merges spend their placements.
void write_path(unsigned voters, std::uint64_t seed, bool stale, a3_totals& totals) {
	context = std::to_string(voters) + " voters, write path, seed " + std::to_string(seed) + (stale ? ", stale predictor" : "");
	world w(voters, seed);
	if (!check(w.settle(), "the cluster settles")) { return; }
	writers_of ws{&w, {}, 0};
	for (auto& [id, x] : w.nodes) {
		ws.at.emplace(id, std::make_unique<writer_type>(*x->p->g));
		x->app.stale = stale ? 1000 : 0;
	}
	std::mt19937_64 gen(seed * 0x9e3779b97f4a7c15ull + 1);
	std::vector<std::string> requests = {"M|k|one", "M|k|two", "C|k|cond|" + std::to_string(gen() % 3), "I|k"};
	std::shuffle(requests.begin(), requests.end(), gen);
	struct sent {
		std::string request;
		std::shared_ptr<started<sg::submitted>> s;
	};
	std::vector<sent> out;
	auto* first = w.leader();
	const std::uint64_t cut_at = gen() % 300, heal_at = cut_at + 400 + gen() % 400;
	std::vector<std::uint64_t> send_at;
	for (std::size_t i = 0; i < requests.size(); ++i) { send_at.push_back(gen() % 1200); }
	for (std::uint64_t e = 0; e < 3000; ++e) {
		if (e == cut_at) { w.isolated.insert(first->id); }
		if (e == heal_at) { w.isolated.clear(); }
		for (std::size_t i = 0; i < requests.size(); ++i) {
			if (send_at[i] != e) { continue; }
			// The client reaches the first leader while it is up, or whichever leads now.
			node* at = (e < heal_at || !w.leader()) ? first : w.leader();
			out.push_back({requests[i], start(at->p->loop_ref(), submit_at(&ws, at, requests[i]))});
			at->run();
		}
		w.step();
	}
	if (!check(w.settle(), "the cluster settles")) { return; }
	const auto& one = *w.nodes.begin()->second;
	std::map<std::uint64_t, strata::verdict::outcome> verdicts;
	replay(one.log, one.p->g->applied(), &verdicts);
	totals.forwards += ws.forwards;
	for (const auto& r : out) {
		++totals.requests;
		const auto who = r.request;
		if (!check(r.s->done, who + " is answered")) { continue; }
		if (r.s->error) {
			// An election replaced its entry: answered as is, never submitted again.
			try {
				std::rethrow_exception(r.s->error);
			} catch (const cs::superseded&) {
				++totals.superseded;
				continue;
			} catch (...) {}
			check(false, who + " fails only as superseded: " + what(r.s->error));
			continue;
		}
		const auto& s = *r.s->value;
		totals.placements += s.placements;
		check(s.placements <= sg::max_placements, who + " is placed at most three times");
		if (s.outcome == sg::next_step::unavailable) {
			// Past the bound (a merge's third placement), or no leader ready to prepare it.
			++totals.unavailable;
			if (s.placements == sg::max_placements) { check(r.request[0] == 'M' && stale, who + " spends the bound only as a merge under a stale predictor"); }
			continue;
		}
		if (s.refused) {
			check(r.request[0] == 'C' && s.placements == 0, who + " is refused at prepare only as a client's condition");
			continue;
		}
		if (!check(s.reply && s.reply->applied, who + " carries apply's verdict")) { continue; }
		const auto v = s.reply->applied->result;
		auto it = verdicts.find(s.reply->placement.index);
		check(it != verdicts.end() && it->second == v, who + "'s answer matches its effect at " + std::to_string(s.reply->placement.index));
		if (r.request[0] == 'M') { check(v == strata::verdict::accepted, who + " is never answered 409"); }
		if (r.request[0] == 'I') { check(v == strata::verdict::invalid && s.placements == 1, who + " is answered 400 once"); }
		if (!stale) { check(s.placements == 1, who + " needs no second submission: " + std::to_string(s.placements) + " placements"); }
	}
}

// The leader of a term above `term`, other than `not_this`, if one is elected.
bool is_superseded(std::exception_ptr e) {
	try {
		if (e) { std::rethrow_exception(e); }
	} catch (const cs::superseded&) {
		return true;
	} catch (...) {}
	return false;
}

// --- A1: growth and healing by install (group-heal) ---------------------------------------

// A joiner added as a learner past every replica's checkpoint installs the leader's bundle,
// catches up, and is promoted to replace a voter, which is then removed; a leader change
// falls between each step. The second round replaces the leader itself.
void grow_and_heal(unsigned voters) {
	context = std::to_string(voters) + " voters, a learner installs, is promoted and replaces a voter";
	world w(voters, 77);
	std::mt19937_64 gen(77);
	if (!check(w.settle(), "the cluster settles")) { return; }
	cs::NodeId next_id = voters + 1;
	for (int round = 0; round < 2; ++round) {
		write_and_compact(w, gen, 12);
		auto* l = w.leader();
		if (!check(l, "a leader")) { return; }
		// Round 0 replaces a follower; round 1 the leader.
		cs::NodeId leaving = l->id;
		if (round == 0) {
			for (auto& [id, x] : w.nodes) {
				if (id != l->id && !x->joiner) { leaving = id; }
			}
		}
		auto& j = w.add_joiner(next_id++);
		if (!check(change(w, voters, [&](cs::Membership& m) {
				m.members.push_back({j.id, j.id, cs::MemberRole::Learner});
				m.next_incarnation = j.id + 1;
			}, j.id), "the learner joins")) { return; }
		if (!check(w.settle(), "the cluster settles with the learner installed")) { return; }
		check(j.p->g->installs() >= 1, "node " + std::to_string(j.id) + " installs as a learner: " + std::to_string(j.p->g->installs()));
		same_digests(w);
		depose(w);
		write_and_compact(w, gen, 6);
		if (!check(change(w, voters, [&](cs::Membership& m) {
				for (auto& x : m.members) {
					if (x.incarnation == j.id) { x.role = cs::MemberRole::Voter; }
				}
				m.leaving = leaving;
			}, j.id), "the learner is promoted beside the voter it replaces")) { return; }
		if (!check(w.settle(), "the cluster settles after the promotion")) { return; }
		depose(w);
		if (!check(change(w, voters, [&](cs::Membership& m) {
				std::erase_if(m.members, [&](const cs::Member& x) { return x.incarnation == leaving; });
				m.leaving = 0;
			}, j.id), "the replaced voter is removed")) { return; }
		w.retire(leaving);
		check(w.settle(), "the cluster settles without the removed voter");
		same_digests(w);
	}
	// The group still writes and settles with both newcomers voting.
	write_and_compact(w, gen, 6);
	same_digests(w);
}

// An election replaces an uncommitted entry: the leader prepares a request, is cut off as it
// places it, and another node is elected and commits past it. The request answers `superseded`,
// is never forwarded or submitted again, and its command is in no node's log.
void superseded_entry(unsigned voters) {
	context = std::to_string(voters) + " voters, an election replaces an uncommitted entry";
	world w(voters, 131);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	const auto term = l.p->lp->driver().state(gid)->term;
	writers_of ws{&w, {}, 0};
	for (auto& [id, x] : w.nodes) { ws.at.emplace(id, std::make_unique<writer_type>(*x->p->g)); }
	const std::string request = "P|s|lost";
	auto cut_then_prepare = [&w, &l, inner = prepare_at(l, request)](blob::index_type& ix, const std::vector<std::string>& pending) {
		w.isolated.insert(l.id);  // past the ready point: the placement reaches no one
		return inner(ix, pending);
	};
	auto s = start(l.p->loop_ref(), ws.at.at(l.id)->submit(cut_then_prepare, [&ws, request](unsigned n) { return forward_to_leader(&ws, request, n); }));
	l.run();
	for (int i = 0; i < 20000 && !w.isolated.count(l.id); ++i) { w.step(); }
	if (!check(w.isolated.count(l.id), "the leader prepares the request")) { return; }
	// The cut-off leader places it alone: its own IO and chore, no time passing.
	for (int i = 0; i < 200; ++i) {
		while (l.p->held.held()) {
			l.p->held.complete(l.p->held.tickets().front());
			l.run();
		}
		l.p->chore_s.run();
		l.run();
	}
	node* fresh = nullptr;
	for (int i = 0; i < 40000 && !fresh; ++i) {
		w.step();
		fresh = leader_above(w, term, &l);
	}
	if (!check(fresh, "the others elect a new leader")) { return; }
	auto t = start(fresh->p->loop_ref(), fresh->p->g->propose("P|s|won"));
	for (int i = 0; i < 40000 && !t->done; ++i) { w.step(); }
	check(t->done && !t->error, "the new leader commits its write: " + what(t->error));
	check(!s->done, "the cut-off request waits for its placement");
	w.isolated.clear();
	if (!check(w.settle(), "the cluster settles once healed")) { return; }
	check(s->done && is_superseded(s->error), "the request answers superseded: " + (s->done ? what(s->error) : std::string("unanswered")));
	check(ws.forwards == 0, "the superseded request is never forwarded");
	for (auto& [id, x] : w.nodes) {
		for (const auto& [i, e] : x->log) { check(e.command != request, "node " + std::to_string(id) + "'s log never holds the superseded request (at " + std::to_string(i) + ")"); }
	}
	same_digests(w);
}

// A forward to the host's choice of leader, counted; a second one is counted and never sent.
task::task<sg::submitted> forward_to(writers_of* ws, node* to, std::string request, unsigned placements) {
	if (++ws->forwards > 1) { co_return sg::submitted{sg::next_step::unavailable, std::nullopt, std::nullopt, placements, true}; }
	co_await task::schedule_on{to->p->loop_ref()};
	co_return co_await ws->at.at(to->id)->submit(prepare_at(*to, request), [ws, to, request](unsigned n) { return forward_to(ws, to, request, n); }, placements, true);
}

// A replica that does not lead forwards a request once: to the leader, which answers it; to a
// replica that does not lead either (a stale view), which answers 503 and forwards no further.
void forward_once(unsigned voters) {
	context = std::to_string(voters) + " voters, a request forwarded once";
	world w(voters, 147);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	std::vector<node*> followers;
	for (auto& [id, x] : w.nodes) {
		if (x.get() != &l) { followers.push_back(x.get()); }
	}
	auto& a = *followers[0];
	auto& b = *followers[1];
	auto submit = [&](node& to, const std::string& request, std::uint64_t& forwards) {
		writers_of ws{&w, {}, 0};
		for (auto& [id, x] : w.nodes) { ws.at.emplace(id, std::make_unique<writer_type>(*x->p->g)); }
		auto s = start(a.p->loop_ref(), ws.at.at(a.id)->submit(prepare_at(a, request), [&ws, to = &to, request](unsigned n) { return forward_to(&ws, to, request, n); }));
		a.run();
		for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
		forwards = ws.forwards;
		return s;
	};
	std::uint64_t forwards = 0;
	auto s = submit(l, "M|k|one", forwards);
	check(s->done && !s->error, "the forwarded merge answers: " + what(s->error));
	if (s->done && s->value) {
		const auto& r = *s->value;
		check(forwards == 1 && r.forwarded, "the follower forwards the merge once");
		check(r.outcome == sg::next_step::reply && r.reply && r.reply->applied && r.reply->applied->result == strata::verdict::accepted, "the leader answers the forwarded merge with its verdict");
	}
	s = submit(b, "M|k|two", forwards);
	check(s->done && !s->error, "the request forwarded to a replica that does not lead answers: " + what(s->error));
	if (s->done && s->value) {
		check(forwards == 1, "a forwarded request is never forwarded again: " + std::to_string(forwards) + " forwards");
		check(s->value->outcome == sg::next_step::unavailable && s->value->placements == 0, "a forwarded request at a replica that does not lead answers 503, nothing placed");
	}
	if (!check(w.settle(), "the cluster settles")) { return; }
	same_digests(w);
}

// A leader stalled on `no_space` holds its own `quorum` answers; one election timeout in, it
// hands leadership to a follower applied past the stalled entry, which answers writes, and the
// old leader answers its write once space frees.
void stalled_leader(unsigned voters) {
	context = std::to_string(voters) + " voters, a leader stalled on no_space";
	world w(voters, 163, true);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	const auto term = l.p->lp->driver().state(gid)->term;
	std::optional<strata::byte_budget::permit> hog = l.storage.open();
	check(hog->try_grow(l.storage.capacity() - l.storage.used() - 16), "the leader's budget is nearly spent");
	auto s = start(l.p->loop_ref(), l.p->g->propose("P|a|" + std::string(64, 'x')));
	l.run();
	bool stalled = false;
	node* next = nullptr;
	for (int i = 0; i < 40000 && !next; ++i) {
		w.step();
		stalled = stalled || l.p->g->stalled();
		next = leader_above(w, term, &l);
	}
	check(stalled, "the leader stalls on no_space");
	if (!check(next, "the stalled leader hands leadership to a follower")) { return; }
	check(l.p->g->stalled(), "the old leader is still stalled");
	check(next->p->g->applied() > l.p->g->applied(), "the new leader applied past the stalled entry: " + std::to_string(next->p->g->applied()) + " against " + std::to_string(l.p->g->applied()));
	check(!s->done, "the stalled leader's `quorum` answer waits with its apply");
	auto t = start(next->p->loop_ref(), next->p->g->propose("P|b|after"));
	for (int i = 0; i < 40000 && !t->done; ++i) { w.step(); }
	check(t->done && !t->error && t->value && t->value->applied && t->value->applied->result == strata::verdict::accepted, "the new leader answers a write: " + what(t->error));
	hog.reset();
	l.p->g->resume();
	if (!check(w.settle(), "the cluster settles once space frees")) { return; }
	check(s->done && !s->error && s->value && s->value->applied && s->value->applied->result == strata::verdict::accepted, "the old leader answers its write with apply's verdict: " + what(s->error));
	same_digests(w);
}

// A follower stalls on `no_space`, stays a follower past several election timeouts, then takes
// leadership by transfer while still stalled: within a few timeouts of leading it hands
// leadership to a replica applied past it, which answers a `quorum` write.
void stalled_follower_elected(unsigned voters) {
	context = std::to_string(voters) + " voters, a follower stalled on no_space, then elected";
	world w(voters, 163, true);
	if (!check(w.settle(), "the cluster settles")) { return; }
	auto& l = *w.leader();
	node* f = nullptr;
	for (auto& [id, x] : w.nodes) {
		if (x.get() != &l && x->open()) {
			f = x.get();
			break;
		}
	}
	std::optional<strata::byte_budget::permit> hog = f->storage.open();
	check(hog->try_grow(f->storage.capacity() - f->storage.used() - 16), "the follower's budget is nearly spent");
	auto s = start(l.p->loop_ref(), l.p->g->propose("P|a|" + std::string(64, 'x')));
	l.run();
	for (int i = 0; i < 40000 && !(s->done && f->p->g->stalled()); ++i) { w.step(); }
	check(s->done && !s->error, "the leader answers its write: " + what(s->error));
	if (!check(f->p->g->stalled(), "the follower stalls on no_space")) { return; }
	// Three election timeouts as a follower: its hand-off timer fires and finds no leadership.
	const auto t0 = w.now;
	while (w.now < t0 + 360) { w.step(); }
	check(f->p->g->stalled() && w.leader() == &l, "still stalled, the leader unchanged");
	const auto term = l.p->lp->driver().state(gid)->term;
	start(l.p->loop_ref(), l.p->lp->driver().transfer_leadership(gid, f->id));
	l.run();
	node* fl = nullptr;
	for (int i = 0; i < 40000 && fl != f; ++i) {
		w.step();
		fl = leader_above(w, term, nullptr);
	}
	if (!check(fl == f, "the stalled follower takes leadership by transfer")) { return; }
	const auto fterm = f->p->lp->driver().state(gid)->term;
	node* next = nullptr;
	const auto t1 = w.now;
	for (int i = 0; i < 200000 && !next && w.now < t1 + 2000; ++i) {
		w.step();
		next = leader_above(w, fterm, f);
	}
	if (!check(next, "the replica elected while stalled hands leadership on within 2 s (16 election timeouts)")) { return; }
	check(next->p->g->applied() > f->p->g->applied(), "to a replica applied past its stalled entry");
	auto q = start(next->p->loop_ref(), next->p->g->propose("P|b|after"));
	for (int i = 0; i < 40000 && !q->done; ++i) { w.step(); }
	check(q->done && !q->error && q->value && q->value->applied && q->value->applied->result == strata::verdict::accepted, "the new leader answers a `quorum` write while that replica is stalled: " + what(q->error));
	hog.reset();
	f->p->g->resume();
	if (!check(w.settle(), "the cluster settles once space frees")) { return; }
	same_digests(w);
}

void write_paths(unsigned voters, std::uint64_t schedules) {
	a3_totals plain, stale;
	for (std::uint64_t seed = 1; seed <= schedules; ++seed) {
		write_path(voters, seed, false, plain);
		write_path(voters, seed, true, stale);
	}
	context = std::to_string(voters) + " voters, write paths";
	check(plain.forwards > 0, "a deposed proposer forwards some request");
	check(stale.unavailable > 0, "a stale predictor reaches the bound");
	std::printf("write path, %u voters: %llu requests, %llu placements, %llu forwards, %llu superseded, %llu answered 503; stale predictor: %llu placements, %llu answered 503\n", voters,
		(unsigned long long)plain.requests, (unsigned long long)plain.placements, (unsigned long long)plain.forwards, (unsigned long long)plain.superseded, (unsigned long long)plain.unavailable,
		(unsigned long long)stale.placements, (unsigned long long)stale.unavailable);
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t schedules = argc > 1 ? std::stoull(argv[1]) : 12, first = argc > 2 ? std::stoull(argv[2]) : 1;
	default_limits();
	close_with_uncommitted(3);
	close_with_uncommitted(5);
	levels(3);
	levels(5);
	deposed_read(3);
	deposed_read(5);
	install_gap(3);
	install_gap(5);
	grow_and_heal(3);
	grow_and_heal(5);
	write_paths(3, 8);
	write_paths(5, 8);
	superseded_entry(3);
	superseded_entry(5);
	forward_once(3);
	forward_once(5);
	stalled_leader(3);
	stalled_leader(5);
	stalled_follower_elected(3);
	stalled_follower_elected(5);
	// D109: the schedules whose ungated checkpoints left a replica behind settle by install,
	// and 40 more ungated ones at 3 voters (group-heal A2).
	std::uint64_t installed = 0;
	for (std::uint64_t seed : {8, 12, 16, 18, 23, 24, 27}) {
		const auto o = schedule(3, seed, seed % 2 == 0, false);
		check(o.installs > 0, "seed " + std::to_string(seed) + " settles by an install");
		installed += o.installs;
	}
	if (schedules) {
		for (std::uint64_t seed = 101; seed <= 140; ++seed) { installed += schedule(3, seed, seed % 2 == 0, false).installs; }
	}
	std::printf("voters: ungated schedules settled with %llu installs\n", (unsigned long long)installed);
	std::uint64_t acked = 0, applied = 0, stalls = 0, crashes = 0, runs = 0, short_of_space = 0;
	for (unsigned voters : {3u, 5u}) {
		for (std::uint64_t seed = first; seed < first + schedules; ++seed) {
			short_of_space += seed % 2 == 0;
			const auto o = schedule(voters, seed, seed % 2 == 0);
			acked += o.acked;
			applied += o.applied;
			stalls += o.stalls;
			crashes += o.crashes;
			++runs;
		}
	}
	// `0` schedules runs the named cases alone (the mutant jobs).
	if (short_of_space) { check(stalls > 0, "some replica stalled on space"); }
	if (runs) { check(checkpoints_started > 0, "the gated schedules checkpoint"); }
	std::printf("voters: %llu schedules (3 and 5 voters, seeds %llu-%llu, half short of space), %llu crashes, %llu acknowledged, %llu entries applied, %llu stalled chore steps, %llu checkpoints: %s\n",
		(unsigned long long)runs, (unsigned long long)first, (unsigned long long)(first + schedules - 1), (unsigned long long)crashes, (unsigned long long)acked, (unsigned long long)applied,
		(unsigned long long)stalls, (unsigned long long)checkpoints_started, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
