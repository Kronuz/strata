// strata::group at three and five voters in one process (quest group-core A2). Each node has
// its own journal host, strata index and file set over faultfs, a loop and a chore on inline
// schedulers, and its turn IO held; the network delivers in seeded order. A schedule writes
// puts, conditional writes, deletes and uploads through the leader with `quorum`, takes
// checkpoints, and cuts one node's power at a seeded event (unsynced sectors and directory
// entries lost), restarting it later. Storage schedules make one replica's budget refuse its
// strata writes and another's file-set push fail partway with ENOSPC, then free both.
// Oracle, after the cluster settles: every `quorum`-acknowledged command is at its placement in
// every node's log, every node applied the same entries, and every node's digest equals the
// others' and a replay from empty of the log.
// `group_voters_test [schedules [first]]`; a failure names its voters, seed and crash point;
// `0` schedules runs the named cases alone.
#include "blob_app.hh"
#include "host_io.hh"
#include "oracle.hh"

#include "strata/core/faultfs.hh"

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

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace ff = strata::faultfs;
namespace sg = strata::group;
using group_test::digest;
using group_test::group_type;
using group_test::of;
using group_test::replay;

int failures = 0;
std::string context;
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 30) { std::cerr << "FAIL [" << context << "]: " << what << '\n'; }
	if (failures > 300) { std::exit(1); }
	return ok;
}
std::string what(std::exception_ptr e) {
	if (!e) { return "no error"; }
	try {
		std::rethrow_exception(e);
	} catch (const kronuz::consensus::rejected& x) {
		return std::string(x.what()) + " (reason " + std::to_string(int(x.reason)) + ")";
	} catch (const std::exception& x) {
		return x.what();
	} catch (...) {}
	return "unknown";
}

constexpr std::uint64_t ms = 1'000'000;
constexpr cs::GroupId gid = 5;
const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits admission{{256u << 20, 8192}, {256 * 1024, 3}, {64u << 20, 4}, 32, 3};

group_type::config config(cs::NodeId self, unsigned voters, strata::byte_budget& storage) {
	group_type::config c;
	c.id = gid;
	c.options.configuration.local = self;
	for (cs::NodeId v = 1; v <= voters; ++v) { c.options.configuration.voters.push_back(v); }
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 16384;
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{10, 40, 60, 120, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'V', char('0' + self)};
	c.uuid = {'g', 'r', 'o', 'u', 'p', char('0' + self)};
	c.strata.keyed = true;
	c.strata.storage = &storage;
	return c;
}

template <class T>
struct started {
	bool done = false;
	std::exception_ptr error;
	std::optional<T> value;
};
template <class T>
std::shared_ptr<started<T>> start(task::scheduler_ref on, task::task<T> work) {
	auto s = std::make_shared<started<T>>();
	task::spawn(on, std::move(work), [s](std::exception_ptr e, std::optional<T> v) {
		s->done = true;
		s->error = e;
		s->value = std::move(v);
	});
	return s;
}
std::shared_ptr<started<bool>> start(task::scheduler_ref on, task::task<void> work) {
	auto wrap = [](task::task<void> w) -> task::task<bool> {
		co_await std::move(w);
		co_return true;
	};
	return start(on, wrap(std::move(work)));
}

struct envelope {
	cs::NodeId from, to;
	cs::Message message;
};

// One node: what survives its power cut (disks, the app's record of what it applied), and the
// process that runs while it is up.
struct node {
	cs::NodeId id = 0;
	ff::filesystem host_disk, strata_disk, files_disk;
	strata::byte_budget storage{std::uint64_t(1) << 30};
	std::map<std::uint64_t, blob::app::seen_entry> log;
	blob::app app;

	struct process {
		task::inline_scheduler loop_s, chore_s;
		j::HeldCompletion held;
		j::InlineCompletion inline_io;
		std::unique_ptr<j::Host> host;
		std::unique_ptr<sg::loop> lp;
		ff::io strata_io, files_io;
		std::unique_ptr<group_type> g;
		task::cancel_source stop;
		bool turns_done = false, driver_done = false;
		std::shared_ptr<started<bool>> opening;
		process(node& n) : strata_io(n.strata_disk), files_io(n.files_disk) {}
		task::scheduler_ref loop_ref() { return task::scheduler_ref::of(loop_s); }
	};
	std::unique_ptr<process> p;

	bool up() const { return p != nullptr; }
	bool open() const { return p && p->opening->done && !p->opening->error; }
	void run() {
		if (p) { p->loop_s.run(); }
	}
};

struct request {
	std::string command;
	std::shared_ptr<started<sg::answer>> s;
};

struct world {
	unsigned voters;
	std::uint64_t seed;
	std::mt19937_64 rng;
	std::uint64_t now = 0;  // ms
	std::map<cs::NodeId, std::unique_ptr<node>> nodes;
	std::deque<envelope> net;
	std::set<cs::NodeId> isolated;  // their messages, both ways, are dropped
	struct acknowledged {
		cs::Placement placement;
		std::string command;
	};
	std::vector<acknowledged> acks;
	std::uint64_t stalls = 0, crashes = 0;

	world(unsigned n, std::uint64_t s) : voters(n), seed(s), rng(s) {
		for (cs::NodeId id = 1; id <= n; ++id) {
			auto x = std::make_unique<node>();
			x->id = id;
			x->app.seen = &x->log;
			nodes.emplace(id, std::move(x));
		}
		for (auto& [id, x] : nodes) { boot(*x); }
	}
	~world() {
		for (auto& [id, x] : nodes) {
			if (x->up()) { shut(*x, false); }
		}
	}
	std::uint64_t pick(std::uint64_t n) { return n ? rng() % n : 0; }

	void boot(node& x) {
		auto p = std::make_unique<node::process>(x);
		p->loop_s.advance(now * ms);
		p->host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(x.host_disk), j::Identity{'N', char('0' + x.id)}, j::HostOptions{1, turn_options});
		p->lp = std::make_unique<sg::loop>(p->loop_ref(), j::completion_ref::of(p->inline_io), *p->host, 0, seed * 31 + x.id);
		const auto from = x.id;
		p->lp->send = [this, from](cs::GroupId, cs::Send s) { net.push_back({from, s.peer, std::move(s.message)}); };
		auto* raw = p.get();
		task::spawn(p->loop_ref(), p->host->driver(0).run(j::completion_ref::of(p->held), p->stop.token()), [raw](std::exception_ptr) { raw->turns_done = true; });
		task::spawn(p->loop_ref(), p->lp->run(p->stop.token()), [raw](std::exception_ptr) { raw->driver_done = true; });
		x.p = std::move(p);
		attach(x);
	}
	// A new group object on the node's loop, opened: at boot, or after a close.
	void attach(node& x) {
		auto& p = *x.p;
		p.g = std::make_unique<group_type>(*p.lp, config(x.id, voters, x.storage), p.strata_io, p.files_io, x.app, task::scheduler_ref::of(p.chore_s));
		p.opening = start(p.loop_ref(), p.g->open());
		x.run();
	}
	void deliver(envelope e) {
		auto& to = *nodes.at(e.to);
		if (!to.up() || isolated.count(e.from) || isolated.count(e.to)) { return; }
		to.p->lp->driver().receive(gid, cs::Receive{e.from, std::move(e.message)});
		to.run();
	}
	// Stops the node; a power cut fails its IO from here and loses what was not synced.
	void shut(node& x, bool power_cut) {
		auto& p = *x.p;
		if (power_cut) {
			x.host_disk.power_off_after(0);
			x.strata_disk.power_off_after(0);
			x.files_disk.power_off_after(0);
		}
		p.stop.request(task::cancel_reason::shutdown);
		for (int round = 0; round < 100000 && !(p.turns_done && p.driver_done); ++round) {
			p.loop_s.run();
			p.chore_s.run();
			if (p.held.held()) {
				const auto t = p.held.tickets().front();
				if (power_cut) {
					p.held.fail(t, std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "power cut")));
				} else {
					p.held.complete(t);
				}
			}
		}
		check(p.turns_done && p.driver_done, "node " + std::to_string(x.id) + "'s loop stops");
		p.g->fenced("the process stopped");
		for (int round = 0; round < 100000; ++round) {
			if (!p.loop_s.run() && !p.chore_s.run() && !p.held.held()) { break; }
			if (p.held.held()) { p.held.fail(p.held.tickets().front(), std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "stopped"))); }
		}
		p.g.reset();
		p.lp.reset();
		p.host.reset();
		x.p.reset();
		if (power_cut) {
			const auto s = seed * 7 + x.id + crashes * 1000;
			x.host_disk.crash(s);
			x.strata_disk.crash(s * 3 + 1);
			x.files_disk.crash(s * 5 + 2);
		}
	}

	node* leader() {
		for (auto& [id, x] : nodes) {
			if (!x->open()) { continue; }
			auto s = x->p->lp->driver().state(gid);
			if (s && s->role == cs::Role::Leader) { return x.get(); }
		}
		return nullptr;
	}

	// One step: deliver a message, complete an IO, run a chore item, or let time pass.
	void step() {
		const auto k = pick(10);
		if (k < 4 && !net.empty()) {
			const auto at = pick(std::min<std::size_t>(net.size(), 4));  // reordered among the first few
			auto e = std::move(net[at]);
			net.erase(net.begin() + std::ptrdiff_t(at));
			deliver(std::move(e));
			return;
		}
		auto& x = *nodes.at(cs::NodeId(1 + pick(voters)));
		if (!x.up()) { return; }
		if (k < 7 && x.p->held.held()) {
			auto t = x.p->held.tickets();
			x.p->held.complete(t[pick(t.size())]);
			x.run();
			return;
		}
		if (k < 9) {
			std::size_t n = 0;
			x.p->chore_s.run_until([&] { return n++ == 1; });
			x.run();
			if (x.p->g->stalled()) { ++stalls; }
			return;
		}
		advance(1 + pick(5));
	}
	void advance(std::uint64_t dt) {
		now += dt;
		for (auto& [id, x] : nodes) {
			if (!x->up()) { continue; }
			x->p->loop_s.advance(dt * ms);
			x->run();
		}
	}
	// Every node up, every message and IO through, every chore idle, every node applied the
	// commit index: false if it does not settle.
	bool settle() {
		for (auto& [id, x] : nodes) {
			if (!x->up()) { boot(*x); }
		}
		int calm = 0;
		for (int round = 0; round < 4000; ++round) {
			advance(5);
			// Everything the tick started, through: messages, IO, chores, until none is left.
			for (int quiet = 0; quiet < 1000; ++quiet) {
				bool any = !net.empty();
				while (!net.empty()) {
					auto e = std::move(net.front());
					net.pop_front();
					deliver(std::move(e));
				}
				for (auto& [id, x] : nodes) {
					while (x->p->held.held()) {
						any = true;
						x->p->held.complete(x->p->held.tickets().front());
						x->run();
					}
					any = x->p->chore_s.run() > 0 || any;
					x->run();
				}
				if (!any) { break; }
			}
			cs::Index committed = 0;
			bool same = true;
			for (auto& [id, x] : nodes) {
				if (!x->open()) {
					same = false;
					continue;
				}
				if (auto s = x->p->lp->driver().state(gid)) { committed = std::max(committed, s->committed); }
			}
			for (auto& [id, x] : nodes) { same = same && x->p->g->applied() == committed && !x->p->g->stalled(); }
			calm = same && leader() ? calm + 1 : 0;
			if (calm >= 30) { return true; }
		}
		for (auto& [id, x] : nodes) {
			auto st = x->p->lp->driver().state(gid);
			std::string installs;
			for (auto peer : x->p->g->installs_needed()) { installs += " " + std::to_string(peer); }
			if (!installs.empty()) { std::cerr << "  node " << id << " has no log for peers" << installs << ": they need an install (group-install)\n"; }
			std::cerr << "  node " << id << (x->open() ? "" : " not open: " + what(x->p->opening->error)) << " applied " << x->p->g->applied() << (x->p->g->stalled() ? " stalled" : "")
					  << (x->p->g->fenced() ? " fenced: " + x->p->g->fence_reason() : "") << ", role " << (st ? int(st->role) : -1) << " term " << (st ? st->term : 0) << " committed " << (st ? st->committed : 0) << '\n';
		}
		return false;
	}
};

std::string text(std::mt19937_64& rng, std::size_t n) {
	std::string s;
	for (std::size_t i = 0; i < n; ++i) { s.push_back(char('a' + rng() % 26)); }
	return s;
}

struct outcome {
	std::uint64_t acked = 0, applied = 0, stalls = 0, crashes = 0;
};

// One schedule. `storage`: a replica's budget refuses its strata writes and another's
// file-set push fails partway with ENOSPC between two events, then both free.
outcome schedule(unsigned voters, std::uint64_t seed, bool storage) {
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
			f.files_disk.short_writes(4);
			f.files_disk.set_capacity(f.files_disk.used() + 6);
		}
		if (storage && e == space_to) {
			hog.reset();
			auto& f = *w.nodes.at(push_node);
			f.files_disk.set_capacity(~std::uint64_t(0));
			f.files_disk.short_writes(0);
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
				case 0: c = "C|" + key + "|" + text(gen, 1 + gen() % 12) + "|" + std::to_string(1 + gen() % 3); break;
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
			if (x.open()) {
				checkpoint = start(x.p->loop_ref(), x.p->g->checkpoint());
				x.run();
			}
		}
		if (checkpoint && checkpoint->done) { checkpoint.reset(); }
		w.step();
	}
	if (storage) {
		hog.reset();
		auto& f = *w.nodes.at(push_node);
		f.files_disk.set_capacity(~std::uint64_t(0));
		f.files_disk.short_writes(0);
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
	std::optional<digest> reference;
	for (auto& [id, x] : w.nodes) {
		const auto who = "node " + std::to_string(id);
		check(!x->app.diverged, who + " applied an index twice with different entries");
		check(!x->p->g->fenced(), who + " is not fenced: " + x->p->g->fence_reason());
		check(x->p->g->applied() == through, who + " applied " + std::to_string(x->p->g->applied()) + ", node 1 " + std::to_string(through));
		for (std::uint64_t i = 1; i <= through; ++i) {
			if (!check(x->log.count(i), who + "'s log has a hole at " + std::to_string(i))) { return out; }
			if (!check(x->log.at(i) == first.log.at(i), who + " and node 1 differ at " + std::to_string(i))) { return out; }
		}
		for (const auto& a : w.acks) {
			auto it = x->log.find(a.placement.index);
			check(a.placement.index <= through && it != x->log.end() && it->second.term == a.placement.term && it->second.command == a.command,
				"acknowledged " + a.command + " at " + std::to_string(a.placement.index) + " is in " + who + "'s log");
		}
		try {
			auto d = of(*x->p->g, x->app);
			if (!reference) {
				reference = replay(x->log, through);
			}
			check(d == *reference, who + "'s digest equals a replay from empty through " + std::to_string(through));
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
	ff::io strata_io(x.strata_disk), files_io(x.files_disk);
	auto c = config(1, 1, x.storage);
	c.options.limits = cs::Limits{};  // consensus's defaults: a 64 MiB log
	group_type g(lp, c, strata_io, files_io, x.app, task::scheduler_ref::of(chore_s));
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

// A replica behind the leader's checkpoint catches up only by install, which group-install
// serves: until then the leader names it in `installs_needed` and it stays behind, never silent.
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
	for (int i = 0; i < 20000; ++i) { w.step(); }
	std::set<cs::NodeId> named;
	for (auto& [id, x] : w.nodes) { named.merge(x->p->g->installs_needed()); }
	check(named == std::set<cs::NodeId>{behind.id}, "the leader names the replica behind its checkpoint");
	check(behind.p->g->applied() < base, "the replica stays behind the checkpoints: no install yet");
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
	write_paths(3, 8);
	write_paths(5, 8);
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
	std::printf("voters: %llu schedules (3 and 5 voters, seeds %llu-%llu, half short of space), %llu crashes, %llu acknowledged, %llu entries applied, %llu stalled chore steps: %s\n",
		(unsigned long long)runs, (unsigned long long)first, (unsigned long long)(first + schedules - 1), (unsigned long long)crashes, (unsigned long long)acked, (unsigned long long)applied,
		(unsigned long long)stalls, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
