// The multi-node world the group tests share (quest group-core A2, group-heal): nodes with
// their own journal host, strata index and file set over faultfs, a loop and a chore on inline
// schedulers, and their turn IO held; a seeded network carries messages, transfer frames and
// fetches, and a node cut off or down answers none. A node cut by power loses what it did not
// sync. `settle` runs everything through until every node applied the commit index.
#pragma once

#include "blob_app.hh"
#include "group_catalog_borrow.hh"
#include "host_io.hh"
#include "oracle.hh"

#include "strata/group/witness.hh"

#include "faultlab/faultfs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace group_test {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace sg = strata::group;

inline int failures = 0;
inline std::string context;
inline bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 30) { std::cerr << "FAIL [" << context << "]: " << what << '\n'; }
	if (failures > 300) { std::exit(1); }
	return ok;
}
inline std::string what(std::exception_ptr e) {
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

// `voters` 0: a joiner, which a membership change adds as a learner (consensus D5). `founding`:
// the founders' roles, when not every one is a voter (a witness, D9).
inline group_type::config config(cs::NodeId self, unsigned voters, strata::byte_budget& storage, const std::optional<cs::Membership>& founding = std::nullopt) {
	group_type::config c;
	c.id = gid;
	c.options.configuration.local = self;
	for (cs::NodeId v = 1; v <= voters; ++v) { c.options.configuration.voters.push_back(v); }
	c.options.founding = founding;
	c.options.snapshots = cs::SnapshotLimits{1, 1 << 20, 2};
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 8192;  // a batch fits one 16 KiB turn-log segment (DRIVER.md)
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	// A lost transfer frame stalls the install one snapshot_timeout; 30 election timeouts, as at
	// consensus's defaults, so a settle (20 s) outlasts a few.
	c.options.timing = cs::Timing{10, 40, 60, 120, 3600};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'V', char('0' + self)};
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
inline std::shared_ptr<started<bool>> start(task::scheduler_ref on, task::task<void> work) {
	auto wrap = [](task::task<void> w) -> task::task<bool> {
		co_await std::move(w);
		co_return true;
	};
	return start(on, wrap(std::move(work)));
}

struct envelope {
	cs::NodeId from, to;
	cs::Message message;
	std::optional<std::string> frame;  // a transfer frame (RFTXFR01) instead of a message
};
// A fetch on the fetch lane, answered from the source's serve when both are up and connected.
struct fetch_request {
	cs::NodeId from, to;
	std::string name;
	sg::fetch_done done;
};

// One node: what survives its power cut (disks, the app's record of what it applied), and the
// process that runs while it is up.
struct node {
	cs::NodeId id = 0;
	bool joiner = false;  // created with no voters: a learner once added
	bool witness = false;  // a witness member: the log and no application state
	std::uint64_t installs = 0;  // activated by its processes that stopped
	std::uint64_t witness_ranges = 0, witness_images = 0;  // a witness's Sink answered, by its processes that stopped
	fl::filesystem host_disk, dir_disk;  // dir_disk: the group's, strata's names and the file set's under its prefix
	strata::byte_budget storage{std::uint64_t(1) << 30};
	std::map<std::uint64_t, blob::app::seen_entry> log;
	blob::app app;

	struct process {
		task::inline_scheduler loop_s, chore_s;
		j::HeldCompletion held;
		j::InlineCompletion inline_io;
		std::unique_ptr<j::Host> host;
		std::unique_ptr<sg::loop> lp;
		borrowed<group_type> g;  // a data member's
		std::unique_ptr<sg::witness> w;  // a witness's
		task::cancel_source stop;
		bool turns_done = false, driver_done = false;
		std::shared_ptr<started<bool>> opening;
		task::scheduler_ref loop_ref() { return task::scheduler_ref::of(loop_s); }
	};
	std::unique_ptr<process> p;

	bool up() const { return p != nullptr; }
	// A data member's group or a witness: a witness applies nothing, and consensus moves its
	// applied index with its commit index.
	cs::Index applied() const {
		if (!witness) { return p->g->applied(); }
		auto s = p->lp->driver().state(gid);
		return s ? s->applied : 0;
	}
	bool stalled() const { return !witness && p->g->stalled(); }
	bool fenced() const { return witness ? p->w->fenced() : p->g->fenced(); }
	std::string fence_reason() const { return witness ? p->w->fence_reason() : p->g->fence_reason(); }
	std::set<cs::NodeId> installs_needed() const { return witness ? std::set<cs::NodeId>{} : p->g->installs_needed(); }
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
	std::deque<fetch_request> fetches;
	std::set<cs::NodeId> isolated;  // their messages and fetches, both ways, are dropped
	std::uint64_t uploads = 100;    // the next upload id: an app never reuses a retired name
	struct acknowledged {
		cs::Placement placement;
		std::string command;
	};
	std::vector<acknowledged> acks;
	std::uint64_t stalls = 0, crashes = 0;
	bool hand_off;  // the hosts know their peers' applied positions: a stalled leader hands off
	std::optional<cs::Membership> founding;  // with witnesses: the founders' roles

	// `witnesses`: founders that are witnesses (rf = 2 + witness, D9), the rest data voters.
	world(unsigned n, std::uint64_t s, bool handing_off = false, const std::set<cs::NodeId>& witnesses = {}) : voters(n), seed(s), rng(s), hand_off(handing_off) {
		if (!witnesses.empty()) {
			cs::Membership m;
			for (cs::NodeId id = 1; id <= n; ++id) { m.members.push_back({id, id, witnesses.count(id) ? cs::MemberRole::Witness : cs::MemberRole::Voter}); }
			m.next_incarnation = n + 1;
			founding = m;
		}
		for (cs::NodeId id = 1; id <= n; ++id) {
			auto x = std::make_unique<node>();
			x->id = id;
			x->witness = witnesses.count(id) > 0;
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
	// A joiner on a node of its own, its incarnation its id, booted with no voters.
	node& add_joiner(cs::NodeId id, bool as_witness = false) {
		auto x = std::make_unique<node>();
		x->id = id;
		x->joiner = true;
		x->witness = as_witness;
		x->app.seen = &x->log;
		auto& out = *x;
		nodes.emplace(id, std::move(x));
		boot(out);
		return out;
	}
	// A member removed from the group: its process stops and the node leaves the world.
	void retire(cs::NodeId id) {
		auto& x = *nodes.at(id);
		if (x.up()) { shut(x, false); }
		nodes.erase(id);
	}

	void boot(node& x) {
		auto p = std::make_unique<node::process>();
		p->loop_s.advance(now * ms);
		p->host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(x.host_disk), j::Identity{'N', char('0' + x.id)}, j::HostOptions{1, turn_options});
		p->lp = std::make_unique<sg::loop>(p->loop_ref(), j::completion_ref::of(p->inline_io), *p->host, 0, seed * 31 + x.id);
		const auto from = x.id;
		p->lp->send = [this, from](cs::GroupId, cs::Send s) { net.push_back({from, s.peer, std::move(s.message), std::nullopt}); };
		p->lp->send_transfer = [this, from](cs::GroupId, cs::SendTransfer t) { net.push_back({from, t.peer, cs::Message{}, std::move(t.frame)}); };
		p->lp->fetch = [this, from](cs::GroupId, cs::NodeId peer, std::string name, sg::fetch_done done) { fetches.push_back({from, peer, std::move(name), std::move(done)}); };
		if (hand_off) {
			p->lp->applied_past = [this, from](cs::GroupId, cs::Index index) -> std::optional<cs::NodeId> {
				for (auto& [id, y] : nodes) {
					if (id != from && y->open() && !y->witness && !y->stalled() && y->applied() >= index) { return id; }
				}
				return std::nullopt;
			};
		}
		auto* raw = p.get();
		task::spawn(p->loop_ref(), p->host->driver(0).run(j::completion_ref::of(p->held), p->stop.token()), [raw](std::exception_ptr) { raw->turns_done = true; });
		task::spawn(p->loop_ref(), p->lp->run(p->stop.token()), [raw](std::exception_ptr) { raw->driver_done = true; });
		x.p = std::move(p);
		attach(x);
	}
	// A new group object on the node's loop, opened: at boot, or after a close.
	void attach(node& x) {
		auto& p = *x.p;
		auto c = config(x.id, x.joiner ? 0 : voters, x.storage, x.joiner ? std::optional<cs::Membership>{} : founding);
		if (x.witness) {
			p.w = std::make_unique<sg::witness>(*p.lp, sg::witness::config{c.id, std::move(c.options), c.identity, std::nullopt});
			p.opening = start(p.loop_ref(), p.w->open());
		} else {
			p.g.emplace(*p.lp, std::move(c), std::make_unique<fl::io>(x.dir_disk), x.storage, {'g', 'r', 'o', 'u', 'p', char('0' + x.id)}, x.app, task::scheduler_ref::of(p.chore_s));
			p.opening = start(p.loop_ref(), p.g->open());
		}
		x.run();
	}
	void deliver(envelope e) {
		auto it = nodes.find(e.to);
		if (it == nodes.end() || !it->second->up() || isolated.count(e.from) || isolated.count(e.to)) { return; }
		auto& to = *it->second;
		if (e.frame) {
			to.p->lp->driver().transfer(gid, e.from, std::move(*e.frame));
		} else {
			to.p->lp->driver().receive(gid, cs::Receive{e.from, std::move(e.message)});
		}
		to.run();
	}
	// The source answers from its pins; one down, cut off or not open fails the fetch as a lost
	// connection would.
	void answer(fetch_request f) {
		auto it = nodes.find(f.to);
		std::optional<std::string> bytes;
		const bool reached = it != nodes.end() && it->second->open() && !isolated.count(f.from) && !isolated.count(f.to);
		if (reached && !it->second->witness) { bytes = it->second->p->g->serve(f.name); }
		f.done(reached ? std::error_code{} : std::make_error_code(std::errc::connection_aborted), std::move(bytes));
		if (auto at = nodes.find(f.from); at != nodes.end()) { at->second->run(); }
	}
	// A stopping node's fetches end: its stage finishes before its group goes.
	bool fail_fetches(cs::NodeId from) {
		bool any = false;
		for (auto it = fetches.begin(); it != fetches.end();) {
			if (it->from != from) {
				++it;
				continue;
			}
			auto f = std::move(*it);
			it = fetches.erase(it);
			f.done(std::make_error_code(std::errc::connection_aborted), std::nullopt);
			any = true;
		}
		return any;
	}
	// Stops the node; a power cut fails its IO from here and loses what was not synced.
	void shut(node& x, bool power_cut) {
		auto& p = *x.p;
		if (power_cut) {
			x.host_disk.power_off_after(0);
			x.dir_disk.power_off_after(0);
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
		if (p.w) {
			p.w->fenced("the process stopped");
		} else {
			p.g->fenced("the process stopped");
		}
		for (int round = 0; round < 100000; ++round) {
			const bool failed = fail_fetches(x.id);
			if (!failed && !p.loop_s.run() && !p.chore_s.run() && !p.held.held()) { break; }
			if (p.held.held()) { p.held.fail(p.held.tickets().front(), std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "stopped"))); }
		}
		if (p.g) { x.installs += p.g->installs(); }
		if (p.w) {
			x.witness_ranges += p.w->ranges();
			x.witness_images += p.w->images();
		}
		p.g.reset();
		p.w.reset();
		p.lp.reset();
		p.host.reset();
		x.p.reset();
		if (power_cut) {
			const auto s = seed * 7 + x.id + crashes * 1000;
			x.host_disk.crash(s);
			x.dir_disk.crash(s * 3 + 1);
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

	// Transfer frames ride the pair's Bulk connection, in order (consensus drops a transfer whose
	// frames arrive out of sequence): a frame picked goes after the pair's earlier ones.
	std::size_t in_order(std::size_t at) const {
		if (!net[at].frame) { return at; }
		for (std::size_t i = 0; i < at; ++i) {
			if (net[i].frame && net[i].from == net[at].from && net[i].to == net[at].to) { return i; }
		}
		return at;
	}
	// One step: deliver a message, complete an IO, run a chore item, or let time pass.
	void step() {
		const auto k = pick(10);
		if (k < 4 && (!net.empty() || !fetches.empty())) {
			if (fetches.empty() || (!net.empty() && pick(2))) {
				const auto at = in_order(pick(std::min<std::size_t>(net.size(), 4)));  // reordered among the first few
				auto e = std::move(net[at]);
				net.erase(net.begin() + std::ptrdiff_t(at));
				deliver(std::move(e));
			} else {
				const auto at = pick(std::min<std::size_t>(fetches.size(), 4));
				auto f = std::move(fetches[at]);
				fetches.erase(fetches.begin() + std::ptrdiff_t(at));
				answer(std::move(f));
			}
			return;
		}
		auto& x = *std::next(nodes.begin(), std::ptrdiff_t(pick(nodes.size())))->second;
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
			if (x.stalled()) { ++stalls; }
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
	// D109, until group-install: a checkpoint at `x` leaves no log behind when every node is up,
	// none stalled or short of space, and x's commit is at or below every node's (each log's end).
	bool leaves_none_behind(node& x, const std::set<cs::NodeId>& short_of_space) {
		auto mine = x.p->lp->driver().state(gid);
		if (!mine) { return false; }
		for (auto& [id, y] : nodes) {
			if (!y->open() || y->stalled() || short_of_space.count(id)) { return false; }
			auto st = y->p->lp->driver().state(gid);
			if (!st || st->committed < mine->committed) { return false; }
		}
		return true;
	}
	// Runs x's loop and chore alone, IO held and no message in, so its capture lands at or below
	// the commit `leaves_none_behind` saw.
	void capture(node& x) {
		for (int round = 0; round < 100000; ++round) {
			if (!x.p->loop_s.run() && !x.p->chore_s.run()) { return; }
		}
	}
	// Every node up, every message and IO through, every chore idle, every node applied the
	// commit index: false if it does not settle (`report`: the nodes' state on stderr).
	bool settle(bool report = true) {
		for (auto& [id, x] : nodes) {
			if (!x->up()) { boot(*x); }
		}
		int calm = 0;
		for (int round = 0; round < 4000; ++round) {
			advance(5);
			// Everything the tick started, through: messages, IO, chores, until none is left.
			for (int quiet = 0; quiet < 1000; ++quiet) {
				bool any = !net.empty() || !fetches.empty();
				while (!net.empty()) {
					auto e = std::move(net.front());
					net.pop_front();
					deliver(std::move(e));
				}
				while (!fetches.empty()) {
					auto f = std::move(fetches.front());
					fetches.pop_front();
					answer(std::move(f));
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
			for (auto& [id, x] : nodes) { same = same && x->applied() == committed && !x->stalled(); }
			calm = same && leader() ? calm + 1 : 0;
			if (calm >= 30) { return true; }
		}
		for (auto& [id, x] : nodes) {
			if (!report) { break; }
			auto st = x->p->lp->driver().state(gid);
			std::string installs;
			for (auto peer : x->installs_needed()) { installs += " " + std::to_string(peer); }
			if (!installs.empty()) { std::cerr << "  node " << id << " has no log for peers" << installs << ": they install\n"; }
			std::cerr << "  node " << id << (x->open() ? "" : " not open: " + what(x->p->opening->error)) << " applied " << x->applied() << (x->stalled() ? " stalled" : "")
					  << (x->fenced() ? " fenced: " + x->fence_reason() : "") << ", role " << (st ? int(st->role) : -1) << " term " << (st ? st->term : 0) << " committed " << (st ? st->committed : 0) << '\n';
		}
		return false;
	}
};

inline std::string text(std::mt19937_64& rng, std::size_t n) {
	std::string s;
	for (std::size_t i = 0; i < n; ++i) { s.push_back(char('a' + rng() % 26)); }
	return s;
}

// The log through `through`, from every node's record of what it applied: an installed replica
// applied only what lies past its install, so no one node holds it all. Where two hold an index
// they must agree; an index no node applied fails the check.
inline std::optional<std::map<std::uint64_t, blob::app::seen_entry>> merged_log(world& w, std::uint64_t through) {
	std::map<std::uint64_t, blob::app::seen_entry> log;
	for (auto& [id, x] : w.nodes) {
		for (const auto& [i, e] : x->log) {
			if (i > through) { break; }
			auto [it, fresh] = log.try_emplace(i, e);
			if (!check(fresh || it->second == e, "node " + std::to_string(id) + " differs from another at " + std::to_string(i))) { return std::nullopt; }
		}
	}
	for (std::uint64_t i = 1; i <= through; ++i) {
		if (!check(log.count(i), "no node applied index " + std::to_string(i))) { return std::nullopt; }
	}
	return log;
}

// Every data member's digest equals a replay of the merged log; a witness holds none.
inline void same_digests(world& w) {
	const node* first = nullptr;
	for (auto& [id, x] : w.nodes) {
		if (!first && !x->witness) { first = x.get(); }
	}
	if (!check(first, "a data member")) { return; }
	const auto through = first->applied();
	const auto log = merged_log(w, through);
	if (!log) { return; }
	const auto reference = replay(*log, through);
	for (auto& [id, x] : w.nodes) {
		const auto who = "node " + std::to_string(id);
		check(!x->fenced(), who + " is not fenced: " + x->fence_reason());
		check(x->applied() == through, who + " applied what node " + std::to_string(first->id) + " did");
		if (!x->witness) { check(of(*x->p->g, x->app) == reference, who + "'s digest equals the replay"); }
	}
}

inline node* leader_above(world& w, cs::Term term, const node* not_this) {
	for (auto& [id, x] : w.nodes) {
		if (x.get() == not_this || !x->open()) { continue; }
		if (auto st = x->p->lp->driver().state(gid); st && st->role == cs::Role::Leader && st->term > term) { return x.get(); }
	}
	return nullptr;
}

// The membership in force at the leader: the fixed voters' when no change was made.
inline cs::Membership membership_at(node& l, unsigned voters) {
	auto m = l.p->lp->driver().state(gid)->membership;
	if (m.members.empty()) {
		for (cs::NodeId v = 1; v <= voters; ++v) { m.members.push_back({v, v, cs::MemberRole::Voter}); }
		m.next_incarnation = voters + 1;
	}
	return m;
}
// One membership step at whichever replica leads, committed (the caller settles); a leader that
// must leave hands off to `heir` first and the step goes to the new leader.
inline bool change(world& w, unsigned voters, const std::function<void(cs::Membership&)>& edit, cs::NodeId heir) {
	for (int attempt = 0; attempt < 4; ++attempt) {
		auto* l = w.leader();
		if (!l) {
			w.settle(false);
			continue;
		}
		auto next = membership_at(*l, voters);
		edit(next);
		auto placed = start(l->p->loop_ref(), l->p->lp->driver().change_membership(gid, next));
		l->run();
		for (int i = 0; i < 40000 && !placed->done; ++i) { w.step(); }
		if (placed->done && !placed->error) {
			auto done = start(l->p->loop_ref(), l->p->lp->driver().committed(gid, *placed->value));
			for (int i = 0; i < 40000 && !done->done; ++i) { w.step(); }
			if (done->done && !done->error) { return true; }
		}
		try {
			if (placed->error) { std::rethrow_exception(placed->error); }
		} catch (const cs::rejected& r) {
			if (r.reason == cs::RejectReason::LeaderMustTransfer) {
				auto t = start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(gid, heir));
				for (int i = 0; i < 40000 && !t->done; ++i) { w.step(); }
			}
		} catch (...) {}
		w.settle(false);
	}
	return check(false, "the membership step commits");
}
// The leader is cut off until another replica leads in a later term, then healed.
inline void cut_leader(world& w) {
	auto* l = w.leader();
	if (!l) { return; }
	const auto term = l->p->lp->driver().state(gid)->term;
	w.isolated.insert(l->id);
	node* next = nullptr;
	for (int i = 0; i < 40000 && !next; ++i) {
		w.step();
		next = leader_above(w, term, l);
	}
	check(next, "another replica leads once the leader is cut off");
	w.isolated.clear();
}
inline void depose(world& w) {
	cut_leader(w);
	check(w.settle(), "the cluster settles after the leader change");
}
inline bool holds(const cs::Membership& m, cs::NodeId incarnation) {
	return std::any_of(m.members.begin(), m.members.end(), [&](const cs::Member& x) { return x.incarnation == incarnation; });
}
// Once a promotion beside `leaving` commits, the leader in force removes it by itself (consensus
// D3): true when a leader other than `leaving` holds a membership without it.
inline bool removed_by_core(world& w, cs::NodeId leaving) {
	for (int i = 0; i < 40000; ++i) {
		if (auto* l = w.leader(); l && l->id != leaving) {
			const auto m = l->p->lp->driver().state(gid)->membership;
			if (!m.members.empty() && !m.leaving && !holds(m, leaving)) { return true; }
		}
		w.step();
	}
	return false;
}
// Commands at the leader and a checkpoint at every replica: no log reaches a new member.
inline void write_and_compact(world& w, std::mt19937_64& gen, int writes) {
	for (int k = 0; k < writes; ++k) {
		auto* l = w.leader();
		if (!l || l->witness) {  // a witness leads only to hand off
			w.settle(false);
			continue;
		}
		const auto key = group_test::keys[gen() % group_test::keys.size()];
		const std::string c = k % 5 == 4 ? "U|" + std::to_string(w.uploads++) + "|0|" + text(gen, 40) : "P|" + key + "|" + text(gen, 1 + gen() % 20);
		auto s = start(l->p->loop_ref(), l->p->g->propose(c));
		for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
	}
	check(w.settle(), "the cluster settles after the writes");
	for (auto& [id, x] : w.nodes) {
		if (x->witness) { continue; }
		auto k = start(x->p->loop_ref(), x->p->g->checkpoint());
		for (int i = 0; i < 40000 && !k->done; ++i) { w.step(); }
		check(k->done && !k->error, "node " + std::to_string(id) + "'s checkpoint: " + what(k->error));
	}
	check(w.settle(), "the cluster settles after the checkpoints");
}

}  // namespace group_test
