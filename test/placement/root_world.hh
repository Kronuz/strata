// The root group's test world: 1, 3 or 5 voters in one process, each with its own journal host
// and strata index over faultfs, a loop and a chore on inline schedulers, its turn IO held, and the
// network in seeded order (test/group/voters_test.cc's world, on the root's application). Writes go
// through submit.hh's writer at the leader with the root's prepared operations.
#pragma once

#include "../group/host_io.hh"

#include "strata/core/faultfs.hh"
#include "strata/group/submit.hh"
#include "strata/placement/root.hh"

#include <algorithm>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace root_test {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace ff = strata::faultfs;
namespace sg = strata::group;
namespace sp = strata::placement;

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
	} catch (const cs::rejected& x) {
		return std::string(x.what()) + " (reason " + std::to_string(int(x.reason)) + ")";
	} catch (const std::exception& x) {
		return x.what();
	} catch (...) {}
	return "unknown";
}

// The root's application, recording every entry it applied for the oracle.
struct traced : sp::root {
	struct seen_entry {
		std::uint64_t term;
		cs::EntryKind kind;
		std::string command;
		bool operator==(const seen_entry&) const = default;
	};
	std::map<std::uint64_t, seen_entry>* seen = nullptr;
	bool diverged = false;
	template <class Files>
	void effects(const state&, const sg::entry& e, Files&) {
		if (!seen) { return; }
		auto [it, fresh] = seen->try_emplace(e.index, seen_entry{e.term, e.kind, std::string(e.command)});
		if (!fresh && !(it->second == seen_entry{e.term, e.kind, std::string(e.command)})) { diverged = true; }
	}
};
static_assert(sg::application<traced>);
using group_type = sg::group<traced>;

constexpr std::uint64_t ms = 1'000'000;
constexpr cs::GroupId root_gid = 1;
inline const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
inline const j::AdmissionLimits admission{{256u << 20, 8192}, {256 * 1024, 3}, {64u << 20, 4}, 32, 3};

inline group_type::config config(cs::NodeId self, unsigned voters, strata::byte_budget& storage) {
	group_type::config c;
	c.id = root_gid;
	c.options.configuration.local = self;
	for (cs::NodeId v = 1; v <= voters; ++v) { c.options.configuration.voters.push_back(v); }
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'R';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 8192;  // a batch fits one 16 KiB turn-log segment (DRIVER.md)
	c.options.limits.log_bytes = 1 << 20;
	c.options.limits.log_entries = 1024;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{10, 40, 60, 120, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'R', char('0' + self)};
	c.uuid = {'r', 'o', 'o', 't', char('0' + self)};
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
};

// One voter: what survives its power cut (disks, the entries it applied), and its process.
struct node {
	cs::NodeId id = 0;
	ff::filesystem host_disk, dir_disk;
	strata::byte_budget storage{std::uint64_t(1) << 30};
	std::map<std::uint64_t, traced::seen_entry> log;
	traced app;
	bool lost = false;  // cut and never booted again

	struct process {
		task::inline_scheduler loop_s, chore_s;
		j::HeldCompletion held;
		j::InlineCompletion inline_io;
		std::unique_ptr<j::Host> host;
		std::unique_ptr<sg::loop> lp;
		std::unique_ptr<group_type> g;
		std::unique_ptr<sg::writer<group_type>> writer;
		task::cancel_source stop;
		bool turns_done = false, driver_done = false;
		std::shared_ptr<started<bool>> opening;
		task::scheduler_ref loop_ref() { return task::scheduler_ref::of(loop_s); }
	};
	std::unique_ptr<process> p;

	bool up() const { return p != nullptr; }
	bool open() const { return p && p->opening->done && !p->opening->error; }
	void run() {
		if (p) { p->loop_s.run(); }
	}
};

// An operation prepared at the leader from its view (root.hh).
using operation = std::function<sg::prepared(const sp::view&)>;

struct world {
	unsigned voters;
	std::uint64_t seed;
	std::mt19937_64 rng;
	std::uint64_t now = 0;  // ms
	std::map<cs::NodeId, std::unique_ptr<node>> nodes;
	std::deque<envelope> net;
	std::uint64_t crashes = 0;

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
		auto p = std::make_unique<node::process>();
		p->loop_s.advance(now * ms);
		p->host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(x.host_disk), j::Identity{'N', char('0' + x.id)}, j::HostOptions{1, turn_options});
		p->lp = std::make_unique<sg::loop>(p->loop_ref(), j::completion_ref::of(p->inline_io), *p->host, 0, seed * 31 + x.id);
		const auto from = x.id;
		p->lp->send = [this, from](cs::GroupId, cs::Send s) { net.push_back({from, s.peer, std::move(s.message)}); };
		auto* raw = p.get();
		task::spawn(p->loop_ref(), p->host->driver(0).run(j::completion_ref::of(p->held), p->stop.token()), [raw](std::exception_ptr) { raw->turns_done = true; });
		task::spawn(p->loop_ref(), p->lp->run(p->stop.token()), [raw](std::exception_ptr) { raw->driver_done = true; });
		p->g = std::make_unique<group_type>(*p->lp, config(x.id, voters, x.storage), std::make_unique<ff::io>(x.dir_disk), x.app, task::scheduler_ref::of(p->chore_s));
		p->writer = std::make_unique<sg::writer<group_type>>(*p->g);
		p->opening = start(p->loop_ref(), p->g->open());
		x.p = std::move(p);
		x.run();
	}
	void deliver(envelope e) {
		auto& to = *nodes.at(e.to);
		if (!to.up() || !nodes.at(e.from)->up()) { return; }
		to.p->lp->driver().receive(root_gid, cs::Receive{e.from, std::move(e.message)});
		to.run();
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
		p.g->fenced("the process stopped");
		for (int round = 0; round < 100000; ++round) {
			if (!p.loop_s.run() && !p.chore_s.run() && !p.held.held()) { break; }
			if (p.held.held()) { p.held.fail(p.held.tickets().front(), std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "stopped"))); }
		}
		p.writer.reset();
		p.g.reset();
		p.lp.reset();
		p.host.reset();
		x.p.reset();
		if (power_cut) {
			++crashes;
			const auto s = seed * 7 + x.id + crashes * 1000;
			x.host_disk.crash(s);
			x.dir_disk.crash(s * 3 + 1);
		}
	}

	node* leader() {
		for (auto& [id, x] : nodes) {
			if (!x->open()) { continue; }
			auto s = x->p->lp->driver().state(root_gid);
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
	// Every node not lost up, every message and IO through, every chore idle, and every up node
	// applied the commit index with a leader among them.
	bool settle(bool report = true) {
		for (auto& [id, x] : nodes) {
			if (!x->up() && !x->lost) { boot(*x); }
		}
		int calm = 0;
		for (int round = 0; round < 4000; ++round) {
			advance(5);
			for (int quiet = 0; quiet < 1000; ++quiet) {
				bool any = !net.empty();
				while (!net.empty()) {
					auto e = std::move(net.front());
					net.pop_front();
					deliver(std::move(e));
				}
				for (auto& [id, x] : nodes) {
					if (!x->up()) { continue; }
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
				if (!x->up()) { continue; }
				if (!x->open()) {
					same = false;
					continue;
				}
				if (auto s = x->p->lp->driver().state(root_gid)) { committed = std::max(committed, s->committed); }
			}
			for (auto& [id, x] : nodes) { same = same && (!x->up() || x->p->g->applied() == committed); }
			calm = same && leader() ? calm + 1 : 0;
			if (calm >= 30) { return true; }
		}
		for (auto& [id, x] : nodes) {
			if (!report || !x->up()) { continue; }
			auto st = x->p->lp->driver().state(root_gid);
			std::cerr << "  node " << id << (x->open() ? "" : " not open: " + what(x->p->opening->error)) << " applied " << x->p->g->applied()
					  << (x->p->g->fenced() ? " fenced: " + x->p->g->fence_reason() : "") << ", role " << (st ? int(st->role) : -1) << " term " << (st ? st->term : 0) << '\n';
		}
		return false;
	}

	// Submits `op` at node x through its writer; the result answers once the world has run.
	std::shared_ptr<started<sg::submitted>> submit(node& x, operation op) {
		auto prepare = [op = std::move(op)](sp::index_type& ix, const std::vector<std::string>& pending) { return op(sp::view(ix, pending)); };
		auto forward = [](unsigned placements) -> task::task<sg::submitted> { co_return sg::submitted{sg::next_step::unavailable, std::nullopt, std::nullopt, placements, false}; };
		auto s = start(x.p->loop_ref(), x.p->writer->submit(std::move(prepare), std::move(forward)));
		x.run();
		return s;
	}
	// Steps until `s` answers, at most `bound` steps.
	template <class T>
	bool run_until(const std::shared_ptr<started<T>>& s, int bound = 50000) {
		for (int i = 0; i < bound && !s->done; ++i) { step(); }
		return s->done;
	}
	// A linearizable read at node x: fn(view) once x applied the read index.
	template <class F>
	auto read(node& x, F fn) {
		using R = std::invoke_result_t<F&, const sp::view&>;
		auto s = start(x.p->loop_ref(), x.p->g->read_applied([fn](sp::index_type& ix) -> R { return fn(sp::view(ix)); }));
		x.run();
		run_until(s);
		return s;
	}
};

// Whether a submit answered with its write accepted.
inline bool accepted(const std::shared_ptr<started<sg::submitted>>& s) {
	return s->done && !s->error && s->value->outcome == sg::next_step::reply && s->value->reply && s->value->reply->applied && s->value->reply->applied->result == strata::verdict::accepted;
}

}  // namespace root_test
