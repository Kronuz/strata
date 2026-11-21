// Repair's test world: physical nodes, each hosting replicas of the root group (strata::placement's
// root application) and of shard groups (test/group/blob_app.hh's application), the network in
// seeded order, power cuts per node, and the keepalive table. Each replica has its own journal
// host, loop and chore on inline schedulers (root_world.hh's process), so a replica removed from
// a group is discarded and a later incarnation on the same node starts fresh. Messages route by
// group and incarnation.
#pragma once

#include "../group/blob_app.hh"
#include "root_world.hh"

#include "strata/placement/repair.hh"

#include <set>
#include <tuple>

namespace repair_test {

using root_test::check;
using root_test::context;
using root_test::ms;
using root_test::start;
using root_test::started;
using root_test::what;
namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace sg = strata::group;
namespace sp = strata::placement;

constexpr cs::GroupId root_gid = 1000;  // apart from the root's group counter

// incarnation:node:role (V, L, W) per member, and the leaving one.
inline std::string text(const cs::Membership& m) {
	std::string out;
	for (const auto& x : m.members) {
		if (!out.empty()) { out += ' '; }
		out += std::to_string(x.incarnation) + ':' + std::to_string(x.node) + ':' + "VLW"[int(x.role)];
	}
	if (m.leaving) { out += " leaving " + std::to_string(m.leaving); }
	return out;
}
inline std::string text(const sp::step& s) {
	switch (s.kind) {
	case sp::step::none: return std::string("none: ") + s.why;
	case sp::step::wait: return std::string("wait: ") + s.why;
	case sp::step::record: return "record";
	case sp::step::change: return "change to " + text(s.next);
	case sp::step::transfer: return "transfer to " + std::to_string(s.target);
	}
	return "?";
}

// A replica's shared surface, whichever its application.
struct replica {
	cs::GroupId gid = 0;
	cs::NodeId inc = 0;
	sp::node_id node = 0;
	bool founder = false;
	std::vector<cs::NodeId> voters;     // a founder's configured voters
	std::optional<cs::Membership> roles;  // a founder's founding roles, with a witness
	fl::filesystem host_disk, dir_disk;
	strata::byte_budget storage{std::uint64_t(1) << 30};

	struct process {
		task::inline_scheduler loop_s, chore_s;
		j::HeldCompletion held;
		j::InlineCompletion inline_io;
		std::unique_ptr<j::Host> host;
		std::unique_ptr<sg::loop> lp;
		task::cancel_source stop;
		bool turns_done = false, driver_done = false;
		std::shared_ptr<started<bool>> opening;
		task::scheduler_ref loop_ref() { return task::scheduler_ref::of(loop_s); }
	};
	std::unique_ptr<process> p;

	virtual ~replica() = default;
	bool up() const { return p != nullptr; }
	bool open() const { return p && p->opening->done && !p->opening->error; }
	void run() {
		if (p) { p->loop_s.run(); }
	}
	std::optional<sg::loop::driver_type::GroupState> state() const {
		if (!open()) { return std::nullopt; }
		return p->lp->driver().state(gid);
	}
	bool leads() const {
		auto s = state();
		return s && s->role == cs::Role::Leader;
	}
	// A witness acknowledges its ranges without applying them (D9).
	bool witness() const {
		auto s = state();
		const auto* self = s ? cs::find_member(s->membership, inc) : nullptr;
		return self && self->role == cs::MemberRole::Witness;
	}

	template <class Config>
	void fill(Config& c) {
		c.id = gid;
		c.options.configuration.local = inc;
		if (founder) { c.options.configuration.voters = voters; }
		c.options.founding = roles;
		c.options.configuration.cluster[0] = 'C';
		c.options.configuration.configuration[0] = char('A' + gid % 26);
		c.options.admission = root_test::admission;
		c.options.limits.command_bytes = 4096;
		c.options.limits.rpc_bytes = 8192;
		c.options.limits.log_bytes = 1 << 20;
		c.options.limits.log_entries = 1024;
		c.options.limits.control_entries = 4;
		c.options.limits.rpc_entries = 8;
		c.options.timing = cs::Timing{10, 40, 60, 120, 60000};
		c.options.scheduling = cs::WorkerLimits{16, 128};
		c.identity = j::Identity{'G', char('0' + gid), char('0' + inc)};
		c.uuid = {'g', char('0' + gid), char('0' + inc)};
		c.strata.keyed = true;
		c.strata.storage = &storage;
	}

	virtual void open_group(std::uint64_t seed) = 0;
	virtual void drop_group() = 0;

	void boot(std::uint64_t now, std::uint64_t seed, std::function<void(cs::GroupId, cs::NodeId, cs::Send)> send) {
		p = std::make_unique<process>();
		p->loop_s.advance(now * ms);
		p->host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(host_disk), j::Identity{'H', char('0' + gid), char('0' + inc)}, j::HostOptions{1, root_test::turn_options});
		p->lp = std::make_unique<sg::loop>(p->loop_ref(), j::completion_ref::of(p->inline_io), *p->host, 0, seed);
		const auto g = gid, from = inc;
		p->lp->send = [send, g, from](cs::GroupId, cs::Send s) { send(g, from, std::move(s)); };
		auto* raw = p.get();
		task::spawn(p->loop_ref(), p->host->driver(0).run(j::completion_ref::of(p->held), p->stop.token()), [raw](std::exception_ptr) { raw->turns_done = true; });
		task::spawn(p->loop_ref(), p->lp->run(p->stop.token()), [raw](std::exception_ptr) { raw->driver_done = true; });
		open_group(seed);
		run();
	}
	// Stops the process; a power cut fails its IO from here and loses what was not synced.
	void shut(bool power_cut, std::uint64_t seed) {
		auto& q = *p;
		if (power_cut) {
			host_disk.power_off_after(0);
			dir_disk.power_off_after(0);
		}
		q.stop.request(task::cancel_reason::shutdown);
		for (int round = 0; round < 100000 && !(q.turns_done && q.driver_done); ++round) {
			q.loop_s.run();
			q.chore_s.run();
			if (q.held.held()) {
				const auto t = q.held.tickets().front();
				if (power_cut) {
					q.held.fail(t, std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "power cut")));
				} else {
					q.held.complete(t);
				}
			}
		}
		check(q.turns_done && q.driver_done, "replica " + std::to_string(gid) + "/" + std::to_string(inc) + "'s loop stops");
		fence_group();
		for (int round = 0; round < 100000; ++round) {
			if (!q.loop_s.run() && !q.chore_s.run() && !q.held.held()) { break; }
			if (q.held.held()) { q.held.fail(q.held.tickets().front(), std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "stopped"))); }
		}
		drop_group();
		q.lp.reset();
		q.host.reset();
		p.reset();
		if (power_cut) {
			host_disk.crash(seed);
			dir_disk.crash(seed * 3 + 1);
		}
	}
	virtual void fence_group() = 0;
	virtual cs::Index applied() const = 0;
	virtual bool fenced() const = 0;
	virtual std::string fence_reason() const = 0;
};

template <class App>
struct replica_of final : replica {
	using group_type = sg::group<App>;
	App app;
	std::unique_ptr<group_type> g;
	std::unique_ptr<sg::writer<group_type>> writer;

	void open_group(std::uint64_t) override {
		typename group_type::config c;
		fill(c);
		g = std::make_unique<group_type>(*p->lp, std::move(c), std::make_unique<fl::io>(dir_disk), app, task::scheduler_ref::of(p->chore_s));
		writer = std::make_unique<sg::writer<group_type>>(*g);
		p->opening = start(p->loop_ref(), g->open());
	}
	void fence_group() override { g->fenced("the process stopped"); }
	void drop_group() override {
		writer.reset();
		g.reset();
	}
	cs::Index applied() const override { return g ? g->applied() : 0; }
	bool fenced() const override { return g && g->fenced(); }
	std::string fence_reason() const override { return g ? g->fence_reason() : std::string(); }
};
using root_replica = replica_of<sp::root>;
using shard_replica = replica_of<blob::app>;

struct envelope {
	cs::GroupId gid;
	cs::NodeId from, to;
	cs::Message message;
};

struct world {
	std::uint64_t seed;
	std::mt19937_64 rng;
	std::uint64_t now = 0;  // ms
	std::set<sp::node_id> nodes, down;
	sp::liveness keepalive;
	std::map<std::pair<cs::GroupId, cs::NodeId>, std::unique_ptr<replica>> replicas;
	std::deque<envelope> net;
	std::uint64_t crashes = 0;

	explicit world(std::uint64_t s) : seed(s), rng(s) {}
	~world() {
		for (auto& [k, r] : replicas) {
			if (r->up()) { r->shut(false, 0); }
		}
	}
	std::uint64_t pick(std::uint64_t n) { return n ? rng() % n : 0; }

	auto sender() {
		return [this](cs::GroupId g, cs::NodeId from, cs::Send s) { net.push_back({g, from, s.peer, std::move(s.message)}); };
	}
	template <class R>
	R& add(cs::GroupId g, cs::NodeId inc, sp::node_id node) {
		auto r = std::make_unique<R>();
		r->gid = g;
		r->inc = inc;
		r->node = node;
		auto& out = *r;
		replicas[{g, inc}] = std::move(r);
		return out;
	}
	// A group founded on `members` (node = incarnation; a witness among them gives founding roles).
	template <class R>
	void found(cs::GroupId g, const std::vector<sp::member>& members) {
		std::vector<cs::NodeId> voters;
		cs::Membership roles;
		bool witness = false;
		for (const auto& m : members) {
			voters.push_back(m.node);
			roles.members.push_back({m.node, m.node, m.role == sp::role::witness ? cs::MemberRole::Witness : cs::MemberRole::Voter});
			roles.next_incarnation = std::max(roles.next_incarnation, m.node + 1);
			witness = witness || m.role == sp::role::witness;
		}
		std::sort(voters.begin(), voters.end());
		std::sort(roles.members.begin(), roles.members.end(), [](const cs::Member& a, const cs::Member& b) { return a.incarnation < b.incarnation; });
		for (const auto& m : members) {
			auto& r = add<R>(g, m.node, m.node);
			r.founder = true;
			r.voters = voters;
			if (witness) { r.roles = roles; }
			if (!down.contains(r.node)) { r.boot(now, seed * 31 + g * 7 + r.inc, sender()); }
		}
	}
	// A joiner: no voters, no membership until the leader's log names it (D5).
	template <class R>
	R& join(cs::GroupId g, cs::NodeId inc, sp::node_id node) {
		auto& r = add<R>(g, inc, node);
		r.boot(now, seed * 31 + g * 7 + inc, sender());
		return r;
	}
	replica* find(cs::GroupId g, cs::NodeId inc) {
		auto it = replicas.find({g, inc});
		return it == replicas.end() ? nullptr : it->second.get();
	}
	replica* leader(cs::GroupId g) {
		for (auto& [k, r] : replicas) {
			if (k.first == g && r->leads()) { return r.get(); }
		}
		return nullptr;
	}
	template <class R>
	R* leader_as(cs::GroupId g) {
		return static_cast<R*>(leader(g));
	}
	std::set<cs::GroupId> groups() const {
		std::set<cs::GroupId> out;
		for (const auto& [k, r] : replicas) { out.insert(k.first); }
		return out;
	}

	void deliver(envelope e) {
		auto* to = find(e.gid, e.to);
		auto* from = find(e.gid, e.from);
		if (!to || !from || !to->up() || !from->up()) { return; }
		to->p->lp->driver().receive(e.gid, cs::Receive{e.from, std::move(e.message)});
		to->run();
	}
	// Every replica on the node loses power; the keepalive table notes it.
	void cut(sp::node_id n) {
		down.insert(n);
		keepalive.down_since.emplace(n, now);
		for (auto& [k, r] : replicas) {
			if (r->node == n && r->up()) {
				++crashes;
				r->shut(true, seed * 7 + r->inc + crashes * 1000);
			}
		}
	}
	void restore(sp::node_id n) {
		down.erase(n);
		keepalive.down_since.erase(n);
		for (auto& [k, r] : replicas) {
			if (r->node == n && !r->up()) { r->boot(now, seed * 31 + k.first * 7 + r->inc + crashes, sender()); }
		}
	}
	// A replica its group removed: stopped and gone (a later incarnation starts fresh).
	void discard(cs::GroupId g, cs::NodeId inc) {
		auto it = replicas.find({g, inc});
		if (it == replicas.end()) { return; }
		if (it->second->up()) { it->second->shut(false, 0); }
		replicas.erase(it);
	}

	// One step: deliver a message, complete an IO, run a chore item, or let time pass.
	void step() {
		const auto k = pick(10);
		if (k < 4 && !net.empty()) {
			const auto at = pick(std::min<std::size_t>(net.size(), 4));
			auto e = std::move(net[at]);
			net.erase(net.begin() + std::ptrdiff_t(at));
			deliver(std::move(e));
			return;
		}
		auto it = replicas.begin();
		std::advance(it, std::ptrdiff_t(pick(replicas.size())));
		if (it == replicas.end()) { return; }
		auto& r = *it->second;
		if (!r.up()) { return; }
		if (k < 7 && r.p->held.held()) {
			auto t = r.p->held.tickets();
			r.p->held.complete(t[pick(t.size())]);
			r.run();
			return;
		}
		if (k < 9) {
			std::size_t n = 0;
			r.p->chore_s.run_until([&] { return n++ == 1; });
			r.run();
			return;
		}
		advance(1 + pick(5));
	}
	void advance(std::uint64_t dt) {
		now += dt;
		for (auto& [k, r] : replicas) {
			if (!r->up()) { continue; }
			r->p->loop_s.advance(dt * ms);
			r->run();
		}
	}
	// Messages, IO and chores drained; returns whether anything moved.
	bool drain_all() {
		bool moved = false;
		for (int quiet = 0; quiet < 1000; ++quiet) {
			bool any = !net.empty();
			while (!net.empty()) {
				auto e = std::move(net.front());
				net.pop_front();
				deliver(std::move(e));
			}
			for (auto& [k, r] : replicas) {
				if (!r->up()) { continue; }
				while (r->p->held.held()) {
					any = true;
					r->p->held.complete(r->p->held.tickets().front());
					r->run();
				}
				any = r->p->chore_s.run() > 0 || any;
				r->run();
			}
			moved = moved || any;
			if (!any) { break; }
		}
		return moved;
	}
	// The newest membership an up replica holds of group `g` (the highest commit index's).
	std::optional<cs::Membership> newest(cs::GroupId g) {
		std::optional<cs::Membership> out;
		cs::Index at = 0;
		for (auto& [k, r] : replicas) {
			if (k.first != g) { continue; }
			const auto s = r->state();
			if (s && !s->membership.members.empty() && (!out || s->committed >= at)) {
				out = s->membership;
				at = s->committed;
			}
		}
		return out;
	}
	// Every up member open and at its group's commit index (witnesses aside), and a leader in
	// every group whose up voters are a quorum of its newest membership.
	bool calm() {
		for (auto g : groups()) {
			const auto m = newest(g);
			cs::Index committed = 0;
			for (auto& [k, r] : replicas) {
				if (k.first != g || !r->up()) { continue; }
				if (!r->open()) { return false; }
				committed = std::max(committed, r->state()->committed);
			}
			if (!m) { continue; }
			std::size_t live = 0;
			for (const auto& x : m->members) {
				auto* r = find(g, x.incarnation);
				if (!r || !r->up()) { continue; }
				if (!r->witness() && r->applied() != committed) { return false; }
				live += cs::voting(x.role);
			}
			if (live >= cs::voter_count(*m) / 2 + 1 && !leader(g)) { return false; }
		}
		return true;
	}
	// After a settle: the replicas no membership names any more go (joiners not yet named stay).
	void discard_removed() {
		for (auto g : groups()) {
			auto* l = leader(g);
			if (!l) { continue; }
			const auto m = l->state()->membership;
			std::vector<cs::NodeId> gone;
			for (auto& [k, r] : replicas) {
				if (k.first == g && !cs::find_member(m, k.second) && k.second < m.next_incarnation) { gone.push_back(k.second); }
			}
			for (auto inc : gone) { discard(g, inc); }
		}
	}
	bool settle(bool report = true) {
		int still = 0;
		for (int round = 0; round < 4000; ++round) {
			advance(5);
			drain_all();
			still = calm() ? still + 1 : 0;
			if (still >= 30) { return true; }
		}
		if (report) { print(); }
		return false;
	}
	// Every replica: its node, whether open, applied, role, term, commit index and membership.
	void print() {
		for (auto& [k, r] : replicas) {
			if (!r->up()) {
				std::cerr << "  replica " << k.first << "/" << k.second << " on node " << r->node << " down\n";
				continue;
			}
			auto s = r->state();
			std::cerr << "  replica " << k.first << "/" << k.second << " on node " << r->node << (r->open() ? "" : " not open: " + what(r->p->opening->error)) << " applied "
					  << r->applied() << (r->fenced() ? " fenced (" + r->fence_reason() + ")" : "") << ", role " << (s ? int(s->role) : -1) << " term " << (s ? s->term : 0) << " committed "
					  << (s ? s->committed : 0) << (s ? ", " + text(s->membership) : std::string()) << '\n';
		}
	}
};

}  // namespace repair_test
