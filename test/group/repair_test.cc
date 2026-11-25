// strata::group's repair (quest group-heal A3; docs/mechanisms/group-install.md, Repair) on the
// voters' world (world.hh): a replica whose bytes fail their checks marks itself damaged, refuses
// reads, writes and checkpoints, acknowledges delivered ranges unapplied, and reinstalls its
// leader's image through install's path (consensus's Driver::reinstall).
//   segment, manifest, file: a stopped follower's segment, manifest log or sealed set file has a
//     flipped byte; it reopens damaged (a damaged index quarantined, a deferred one in its
//     place), reinstalls once at or past its own base, and its digest equals the replay. A
//     segment maps at its first read (cold-tier D2) unless eager_segments maps it at open
//     (STRATA_EAGER_SEGMENTS=1, ctest's group_repair_eager): lazy, the first read or apply that
//     maps it marks the replica damaged, and a read answers replica_damaged.
//   apply, read: strata's damage met at apply or at a read on a running follower; the read
//     answers replica_damaged, never the bytes.
//   refusal: while damaged, with the third voter cut off so commits count the damaged replica's
//     log (until its install request is admitted), it answers every read, checkpoint and write
//     replica_damaged, serves no install, keeps its applied index while consensus's moves past
//     it, the damaging write's range acknowledged unapplied; then the only healthy replica that
//     applied those commits is lost, and they survive: the damaged one leads, hands off, and
//     reinstalls from the replica that caught up from its log.
//   staged: a staged live set file flipped between the stage and the activation: the activation
//     refuses it, and a later image repairs the replica.
//   leader: a damaged replica made leader hands leadership off, then reinstalls.
//   source: a leader's segments damaged on disk and never read since its reboot (lazy maps: the
//     pin's checkpoint maps them), or rotted on disk after it mapped them (the pin's footer
//     check reads them); a learner's request makes it pin them, the pin meets the damage, and the leader marks
//     itself damaged and hands off to the sound follower instead of shipping the bytes; the
//     learner installs from the next leader and the source reinstalls too.
//   body: as `source` at rest, a byte inside each segment's body behind a valid footer: the pin
//     reads each segment whole and finds it.
//   rot: the source's segment bodies rot after its pin was made and served: the next learner's
//     stage fails the pin's CRC32C and refuses; the source reads its pin again, finds the
//     damage, hands off, and both reinstall.
//   two-damaged: two damaged replicas, one leading, with no host view: the hand-offs go round
//     the ring to the sound one; handoff-down: the ring's next voter is down and is passed by.
//   lone: a lone voter's damaged index is set aside under `iq.`, byte for byte, and stays.
//   deposed: the leader installing a damaged replica loses its leadership mid-flight; it names
//     no install from then on, and once the new leader repairs the replica no node names one.
//   crash: a power cut at every mutation of the damaged follower's directory disk, then of its
//     host disk, from its reboot through the settled repair, for a damaged manifest log (a
//     deferred index) and a damaged set file: every reboot is damaged or repaired, never unfenced over
//     damaged bytes, and settles with equal digests.
// `group_repair_test [case]` runs one case; none runs them all.
#include "world.hh"

#include "strata/group/repair.hh"

#include <cerrno>
#include <cstdio>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace group_test;

bool deferred(node& x) {
	fl::io io(x.dir_disk);
	return !strata::index_detail::log_holding(io);
}
std::string staging_left(node& x) {
	std::string out;
	for (const auto& n : names_in(x)) {
		if (n.starts_with("is.") || n.starts_with("ia.") || n.starts_with("iq.") || n == "ip" || n == "ip.tmp" || n == "rd.tmp") { out += " " + n; }
	}
	return out;
}
node& follower(world& w) {
	auto* l = w.leader();
	for (auto& [id, x] : w.nodes) {
		if (x.get() != l) { return *x; }
	}
	return *w.nodes.begin()->second;
}
node& third(world& w, const node& a, const node& b) {
	for (auto& [id, x] : w.nodes) {
		if (x.get() != &a && x.get() != &b) { return *x; }
	}
	return *w.nodes.begin()->second;
}

// Settled, written and checkpointed on every node: strata segments and a manifest log, sealed
// volumes and a live one.
bool prepare(world& w, std::mt19937_64& gen) {
	const auto before = failures;
	if (!check(w.settle(), "the cluster settles")) { return false; }
	write_and_compact(w, gen, 30);
	return failures == before;
}
// A boot whose host or index open fails on the powered-off disk: the process goes as at a
// power cut. False then.
bool boot(world& w, node& x) {
	try {
		w.boot(x);
		return true;
	} catch (...) {}
	if (x.p) {
		x.host_disk.power_off_after(0);
		x.dir_disk.power_off_after(0);
		auto& p = *x.p;
		p.stop.request(task::cancel_reason::shutdown);
		for (int round = 0; round < 100000 && !(p.turns_done && p.driver_done); ++round) {
			p.loop_s.run();
			p.chore_s.run();
			if (p.held.held()) { p.held.fail(p.held.tickets().front(), std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "power cut"))); }
		}
		x.p.reset();
	}
	const auto s = w.seed * 7 + x.id + w.crashes * 1000;
	x.host_disk.crash(s);
	x.dir_disk.crash(s * 3 + 1);
	return false;
}

// Steps until the node's open ends; false (and a failure) when it failed.
bool opened(world& w, node& x) {
	for (int i = 0; i < 40000 && !x.p->opening->done; ++i) { w.step(); }
	return check(x.p->opening->done && !x.p->opening->error, "node " + std::to_string(x.id) + " opens: " + what(x.p->opening->error));
}
bool damaged_error(const std::exception_ptr& e) {
	if (!e) { return false; }
	try {
		std::rethrow_exception(e);
	} catch (const sg::replica_damaged&) {
		return true;
	} catch (...) {}
	return false;
}
template <class T>
std::shared_ptr<started<T>> run_until_done(world& w, node& at, task::task<T> t, bool starve = false) {
	auto s = start(at.p->loop_ref(), std::move(t));
	for (int i = 0; i < 40000 && !s->done; ++i) {
		w.step();
		if (starve) { w.fail_fetches(at.id); }
	}
	return s;
}
bool write_at(world& w, node& l, const std::string& c, cs::Placement* placed = nullptr, node* starved = nullptr) {
	auto s = start(l.p->loop_ref(), l.p->g->propose(c));
	for (int i = 0; i < 40000 && !s->done; ++i) {
		w.step();
		if (starved) { w.fail_fetches(starved->id); }
	}
	if (s->done && !s->error && placed) { *placed = s->value->placement; }
	return s->done && !s->error;
}
// Steps until no open replica is damaged: settle() waits for applied indexes only, which a
// damaged replica at its leader's base already matches. False when one stays damaged.
bool healed(world& w) {
	auto damaged = [&] {
		for (auto& [id, n] : w.nodes) {
			if (n->open() && !n->witness && n->p->g->damaged()) { return true; }
		}
		return false;
	};
	for (int i = 0; i < 400000 && damaged(); ++i) { w.step(); }
	return !damaged();
}
// Leadership moved to `x`: consensus answers a read NotLeader anywhere else.
bool lead(world& w, node& x) {
	for (int attempt = 0; attempt < 5 && w.leader() != &x; ++attempt) {
		if (auto* l = w.leader()) {
			auto t = start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(gid, x.id));
			for (int i = 0; i < 4000 && !t->done; ++i) { w.step(); }
		}
		for (int i = 0; i < 4000 && w.leader() != &x; ++i) { w.step(); }
	}
	for (int i = 0; i < 500 && w.leader() == &x; ++i) { w.step(); }  // its term's first entry commits
	return w.leader() == &x;
}
// Every digest equal to the replay; a read that meets damage fails the check, not the process.
void digests(world& w) {
	try {
		same_digests(w);
	} catch (const std::exception& e) {
		check(false, std::string("the digests read: ") + e.what());
	}
}
// The repaired replica: settled, unmarked, one image at or past `base`, no staging name left,
// every digest equal to the replay.
void repaired(world& w, node& x, cs::Index base) {
	if (!check(healed(w), "every replica reinstalls") || !check(w.settle(), "the cluster settles by the reinstall")) { return; }
	auto& g = *x.p->g;
	check(!g.damaged() && !marked(x), "the replica is no longer damaged");
	check(g.installs() == 1 && g.installed_at() >= base, "one image at or past the replica's base " + std::to_string(base) + ": " + std::to_string(g.installs()) + " at " + std::to_string(g.installed_at()));
	const auto left = staging_left(x);
	check(left.empty(), "no staging name stays:" + left);
	digests(w);
}

// A read of every key, present or not, through every segment that may hold it: each maps.
bool read_all(blob::index_type& ix) {
	bool any = false;
	for (const auto& key : keys) {
		if (ix.find_key(key)) {
			any = true;
			(void)ix.get(key);
		}
	}
	return ix.find_key("~never-written").has_value() || any;
}

std::uint64_t cases = 0;

void damaged_at_open(kind k, std::uint64_t seed) {
	context = std::string(name_of(k)) + ", seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	const auto sealed = sealed_file(x);
	const auto base = x.p->g->recovered().position;
	w.shut(x, false);
	if (!damage(x, k, sealed)) { return; }
	w.boot(x);
	if (!opened(w, x)) { return; }
	// A lazy segment maps at its first read, unless the recovery's apply mapped it already.
	const bool lazy = k == kind::segment && !strata::detail::eager_segments_default();
	const bool at_read = lazy && !x.p->g->damaged();
	if (at_read) {
		if (!check(lead(w, x), "the replica leads, for its read")) { return; }
		auto r = run_until_done(w, x, x.p->g->read_applied(read_all));
		check(r->done && damaged_error(r->error), "the first read of the damaged segment answers replica_damaged: " + what(r->error));
	}
	const auto reason = x.p->g->damage_reason();
	const bool fresh = deferred(x);
	if (!check(x.p->g->damaged() && marked(x), at_read ? "the first read marks the replica damaged" : "the replica reopens damaged")) { return; }
	check(fresh == (k != kind::file && !lazy), "a damaged index at open is quarantined for a deferred one; one damaged at a read, or a damaged set file, keeps it");
	check(!x.p->g->readable(), "a damaged replica is not readable");
	repaired(w, x, base);
	std::printf("repair: %s%s, seed %llu: damaged (%s), %s index, one image at %llu (base %llu)\n", name_of(k), at_read ? " at its first read" : lazy ? " at the recovery's apply" : " at open", (unsigned long long)seed, reason.c_str(), fresh ? "deferred" : "its own",
		(unsigned long long)x.p->g->installed_at(), (unsigned long long)base);
	++cases;
}

void damaged_at_apply(std::uint64_t seed) {
	context = "apply, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	const auto base = x.p->g->recovered().position;
	bool fired = false;
	x.app.before_write = [&](const sg::entry&) {
		if (!fired) {
			fired = true;
			throw strata::corrupt_index("test: a segment read at apply fails its checksum");
		}
	};
	check(write_at(w, *w.leader(), "P|a|after"), "a write commits");
	for (int i = 0; i < 40000 && !x.p->g->damaged(); ++i) { w.step(); }
	check(fired && x.p->g->damaged() && marked(x), "the apply's damage marks the replica damaged");
	check(!x.p->g->fenced(), "damage fences the replica locally only");
	const auto reason = x.p->g->damage_reason();
	repaired(w, x, base);
	x.app.before_write = {};
	std::printf("repair: apply, seed %llu: damaged (%s), one image at %llu\n", (unsigned long long)seed, reason.c_str(), (unsigned long long)x.p->g->installed_at());
	++cases;
}

void damaged_at_read(std::uint64_t seed) {
	context = "read, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	const auto base = x.p->g->recovered().position;
	if (!check(lead(w, x), "the replica leads, for its read")) { return; }
	auto r = run_until_done(w, x, x.p->g->read_applied([](blob::index_type&) -> bool { throw strata::corrupt_index("test: a segment read fails its checksum"); }));
	check(r->done && damaged_error(r->error), "the read answers replica_damaged, never bytes: " + what(r->error));
	if (!check(x.p->g->damaged() && marked(x), "the read's damage marks the replica damaged")) { return; }
	repaired(w, x, base);
	auto* l = w.leader();
	if (!check(l != nullptr, "a leader")) { return; }
	auto theirs = run_until_done(w, *l, l->p->g->read_applied([](blob::index_type& ix) { return ix.get("a").has_value(); }));
	if (!check(lead(w, x), "the repaired replica leads, for its read")) { return; }
	auto mine = run_until_done(w, x, x.p->g->read_applied([](blob::index_type& ix) { return ix.get("a").has_value(); }));
	check(mine->done && !mine->error && theirs->done && !theirs->error && *mine->value == *theirs->value, "the repaired replica reads as the leader: " + what(mine->error));
	std::printf("repair: read, seed %llu: the read answered replica_damaged, one image at %llu\n", (unsigned long long)seed, (unsigned long long)x.p->g->installed_at());
	++cases;
}

// The invariant (A3, review point 1): acknowledging delivered ranges unapplied never lets the
// damaged replica serve, checkpoint or answer writes for state it does not hold; its log still
// counts toward commits, as it holds the log, and those commits survive the loss of the only
// healthy replica that applied them.
void refusal(std::uint64_t seed) {
	context = "refusal, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& l = *w.leader();
	auto& x = follower(w);
	auto& y = third(w, l, x);
	const auto base = x.p->g->recovered().position;
	w.isolated.insert(y.id);
	bool fired = false;
	x.app.before_write = [&](const sg::entry&) {
		if (!fired) {
			fired = true;
			throw strata::corrupt_index("test: a segment read at apply fails its checksum");
		}
	};
	// Commits count x's log alone beside the leader's: y is cut off. x's fetches fail, so it stays
	// damaged. The first write's apply damages x, which acknowledges its range unapplied; later
	// writes commit on x's log until its install request is admitted, after which its leader
	// sends it the image alone and consensus delivers it nothing (install-request.md), so the
	// first write that cannot commit ends the writes.
	struct written {
		cs::Placement at;
		std::string command;
	};
	std::vector<written> acked;
	written first{{}, "P|" + keys[0] + "|while-damaged-0"};
	if (!check(write_at(w, l, first.command, &first.at, &x), "the write that damages the replica commits on the leader's and its logs")) { return; }
	acked.push_back(first);
	for (int i = 0; i < 40000 && !x.p->g->damaged(); ++i) {
		w.step();
		w.fail_fetches(x.id);
	}
	if (!check(x.p->g->damaged(), "the replica is damaged")) { return; }
	for (int k = 1; k < 6; ++k) {
		written c{{}, "P|" + keys[k] + "|while-damaged-" + std::to_string(k)};
		if (!write_at(w, l, c.command, &c.at, &x)) { break; }
		acked.push_back(c);
	}
	auto& g = *x.p->g;
	const auto held = g.applied();
	const auto core = x.p->lp->driver().state(gid);
	check(core && core->applied > held && core->applied >= first.at.index,
		"consensus's applied index moves past the replica's: the damaging write's range acknowledged unapplied (" + std::to_string(core ? core->applied : 0) + " > " + std::to_string(held) + ", the write at " +
			std::to_string(first.at.index) + ")");
	auto r = run_until_done(w, x, g.read_applied([](blob::index_type& ix) { return ix.get("a").has_value(); }), true);
	check(r->done && damaged_error(r->error), "a read answers replica_damaged: " + what(r->error));
	auto k = run_until_done(w, x, g.checkpoint(), true);
	check(k->done && damaged_error(k->error), "a checkpoint answers replica_damaged: " + what(k->error));
	auto p = run_until_done(w, x, g.propose("P|z|from-damaged"), true);
	check(p->done && damaged_error(p->error), "a write answers replica_damaged: " + what(p->error));
	check(!g.readable() && !g.serve(sg::pin_name(g.recovered().position)) && g.installs_needed().empty(), "it is not readable and serves no install");
	check(g.applied() == held && g.recovered().position == base, "its applied index and bundle stay");
	// The only healthy replica that applied the commits goes; they live in x's log and y catches up from it.
	const auto lost = l.id;
	w.retire(lost);
	w.isolated.clear();
	repaired(w, x, base);
	for (const auto& c : acked) {
		auto it = x.log.find(c.at.index);
		check(it != x.log.end() && it->second.command == c.command, "a write committed while damaged is applied at its placement " + std::to_string(c.at.index));
	}
	x.app.before_write = {};
	std::printf("repair: refusal, seed %llu: %zu writes committed on a damaged replica's log (the first damaged it), consensus applied %llu over the replica's %llu, every waiter refused; node %llu lost, the writes survive\n",
		(unsigned long long)seed, acked.size(), (unsigned long long)(core ? core->applied : 0), (unsigned long long)held, (unsigned long long)lost);
	++cases;
}

void staged_damage(std::uint64_t seed) {
	context = "staged, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	const auto sealed = sealed_file(x);
	const auto base = x.p->g->recovered().position;
	w.shut(x, false);
	if (!damage(x, kind::file, sealed)) { return; }
	w.boot(x);
	if (!opened(w, x)) { return; }
	int flipped = 0;
	bool refused = false;
	for (int i = 0; i < 400000 && !refused; ++i) {
		w.step();
		if (!flipped) {
			fl::io io(x.dir_disk);
			if (auto pin = sg::staged(io)) {
				for (const auto& [name, f] : pin->base.files) {
					if (!f.crc && f.length) {
						flip(x, "ia." + name, 0);
						++flipped;
					}
				}
				if (!flipped) { break; }
			}
		}
		refused = flipped && x.p->g->damage_reason().find("a staged file fails its CRC32C") != std::string::npos;
	}
	check(flipped > 0, "a staged live set file to flip");
	if (!check(refused, "the activation refuses the staged file: " + x.p->g->damage_reason())) { return; }
	// The leader shipped this replica an image at its base: a new base ships another.
	auto* l = w.leader();
	check(l && write_at(w, *l, "P|b|next-base"), "a write commits");
	for (auto& [id, n] : w.nodes) {
		if (n.get() == &x) { continue; }
		auto c = run_until_done(w, *n, n->p->g->checkpoint());
		check(c->done && !c->error, "node " + std::to_string(id) + " checkpoints: " + what(c->error));
	}
	repaired(w, x, base);
	std::printf("repair: staged, seed %llu: %d staged live files flipped, the activation refused them, one image at %llu\n", (unsigned long long)seed, flipped, (unsigned long long)x.p->g->installed_at());
	++cases;
}

void damaged_leader(std::uint64_t seed) {
	context = "leader, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	const auto base = x.p->g->recovered().position;
	w.shut(x, false);
	if (!damage(x, kind::manifest, {})) { return; }
	w.boot(x);
	if (!opened(w, x) || !check(x.p->g->damaged(), "the replica reopens damaged")) { return; }
	// Its fetches fail, so it is still damaged when it leads.
	auto* l = w.leader();
	bool led = false;
	if (l) {
		auto t = start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(gid, x.id));
		for (int i = 0; i < 40000 && !led; ++i) {
			w.step();
			w.fail_fetches(x.id);
			const auto s = x.p->lp->driver().state(gid);
			led = s && s->role == cs::Role::Leader && x.p->g->damaged();
		}
	}
	if (!check(led, "the damaged replica leads")) { return; }
	repaired(w, x, base);
	const auto s = x.p->lp->driver().state(gid);
	std::printf("repair: leader, seed %llu: the damaged replica led, handed off (leader now %llu), one image at %llu\n", (unsigned long long)seed, (unsigned long long)(s ? s->leader : 0),
		(unsigned long long)x.p->g->installed_at());
	++cases;
}

// `rested`: the running leader's segment footers rot on disk after it mapped them; only the
// pin's footer check reads them again. Else they are flipped while it is down and stay unread
// since its reboot: the pin's checkpoint maps them first (its commit learns their facts).
// `body`: a byte in the middle of each segment rots instead, behind a valid footer (rested): the
// pin reads each segment whole.
void damaged_source(std::uint64_t seed, bool rested, bool body = false) {
	context = std::string(body ? "source body" : rested ? "source at rest" : "source unread") + ", seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto* first = w.leader();
	if (!check(first != nullptr, "a leader")) { return; }
	auto& s = *first;
	if (!rested) { w.shut(s, false); }
	int flipped = 0;
	for (const auto& n : names_in(s)) {
		const auto pn = strata::index_detail::parse_name(n);
		if (pn && pn->type == strata::index_detail::parsed_name::segment) {
			flip(s, n, body ? size_of(s, n) / 2 : size_of(s, n) - 8);
			++flipped;
		}
	}
	if (!check(flipped > 0, "segments to damage")) { return; }
	if (!rested) {
		w.boot(s);
		if (!opened(w, s) || !check(w.settle(), "the cluster settles after the reboot")) { return; }
	}
	// It leads, its damaged segment bytes unread.
	if (!check(lead(w, s) && !s.p->g->damaged(), "the source leads with its damaged segments unread")) { return; }
	auto& x = follower(w);
	const auto base = x.p->g->recovered().position;
	w.shut(x, false);
	if (!damage(x, kind::manifest, {})) { return; }
	w.boot(x);
	if (!opened(w, x)) { return; }
	std::string reason;
	for (int i = 0; i < 400000 && reason.empty(); ++i) {
		w.step();
		if (s.p->g->damaged()) { reason = s.p->g->damage_reason(); }
	}
	check(reason.find(rested && !body ? "a pinned segment's footer fails its check" : "the pin met damage") != std::string::npos, "the source's pin finds its damage: " + reason);
	if (!healed(w)) {
		for (auto& [id, n] : w.nodes) {
			const auto st = n->open() ? n->p->lp->driver().state(gid) : std::nullopt;
			std::printf("  %s: node %llu %s, role %d, leader %llu, commit %llu, applied %llu, damaged %d (%s)\n", context.c_str(), (unsigned long long)id, n->open() ? "open" : "down",
				st ? int(st->role) : -1, (unsigned long long)(st ? st->leader : 0), (unsigned long long)(st ? st->committed : 0), (unsigned long long)(n->open() ? n->p->g->applied() : 0),
				n->open() && n->p->g->damaged(), n->open() ? n->p->g->damage_reason().c_str() : "");
		}
	}
	repaired(w, x, base);
	check(!s.p->g->damaged() && !marked(s) && s.p->g->installs() >= 1, "the source reinstalls too");
	std::printf("repair: %s: %d segments damaged on the leader; its pin found them (%s); the learner installed at %llu from node %llu\n", context.c_str(), flipped, reason.c_str(), (unsigned long long)x.p->g->installed_at(), (unsigned long long)(w.leader() ? w.leader()->id : 0));
	++cases;
}

// A source's segment bodies rot after its pin was made and served: the next learner's stage fails
// the pin's CRC32C and refuses the flight; the source reads its pin again, finds its damage and
// hands off; the learner and the source reinstall from the sound follower.
void rot_after_pin(std::uint64_t seed) {
	context = "rot after the pin, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	w.shut(x, false);
	if (!damage(x, kind::manifest, {})) { return; }
	w.boot(x);
	if (!opened(w, x) || !check(healed(w) && w.settle(), "the first learner reinstalls")) { return; }
	auto* l = w.leader();
	if (!check(l && l->p->g->serve(sg::pin_name(l->p->g->recovered().position)).has_value(), "the leader holds the pin it served")) { return; }
	auto& s = *l;
	int flipped = 0;
	for (const auto& n : names_in(s)) {
		const auto pn = strata::index_detail::parse_name(n);
		if (pn && pn->type == strata::index_detail::parsed_name::segment) {
			flip(s, n, size_of(s, n) / 2);
			++flipped;
		}
	}
	if (!check(flipped > 0, "segments to damage")) { return; }
	auto& y = third(w, s, x);
	const auto base = y.p->g->recovered().position;
	w.shut(y, false);
	if (!damage(y, kind::manifest, {})) { return; }
	w.boot(y);
	if (!opened(w, y)) { return; }
	std::string reason;
	for (int i = 0; i < 400000 && reason.empty(); ++i) {
		w.step();
		if (s.p->g->damaged()) { reason = s.p->g->damage_reason(); }
	}
	check(reason.find("a refused pin fails its check") != std::string::npos, "the refused source finds its damage: " + reason);
	repaired(w, y, base);
	check(!s.p->g->damaged() && !marked(s) && s.p->g->installs() >= 1, "the source reinstalls too");
	std::printf("repair: %s: %d segment bodies rotted under node %llu's pin; the refusal found them (%s); the learner installed at %llu\n", context.c_str(), flipped,
		(unsigned long long)s.id, reason.c_str(), (unsigned long long)y.p->g->installed_at());
	++cases;
}

// Damaged node `a` made leader, its fetches failing meanwhile so it stays damaged.
bool lead_damaged(world& w, node& a, const std::function<void()>& starve) {
	bool led = false;
	for (int attempt = 0; attempt < 20 && !led; ++attempt) {
		if (auto* l = w.leader(); l && l != &a) { (void)start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(gid, a.id)); }
		for (int i = 0; i < 4000 && !led; ++i) {
			w.step();
			starve();
			const auto st = a.p->lp->driver().state(gid);
			led = st && st->role == cs::Role::Leader && a.p->g->damaged();
		}
	}
	return led;
}

// Two damaged replicas, one leading, and no host view (no applied_past: every production host):
// the hand-offs go round the ring and reach the sound replica; both reinstall.
void two_damaged(std::uint64_t seed) {
	context = "two damaged, seed " + std::to_string(seed);
	world w(3, seed);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& a = *w.nodes.at(1);
	auto& b = *w.nodes.at(2);
	w.shut(a, false);
	w.shut(b, false);
	if (!damage(a, kind::manifest, {}) || !damage(b, kind::manifest, {})) { return; }
	w.boot(a);
	w.boot(b);
	auto starve = [&] {
		w.fail_fetches(a.id);
		w.fail_fetches(b.id);
	};
	for (int i = 0; i < 40000 && !(a.p->opening->done && b.p->opening->done); ++i) {
		w.step();
		starve();
	}
	if (!check(a.p->g->damaged() && b.p->g->damaged(), "both reopen damaged") || !check(lead_damaged(w, a, starve), "damaged node 1 leads")) { return; }
	if (!check(healed(w) && w.settle(), "both damaged replicas reinstall")) { return; }
	digests(w);
	std::printf("repair: two damaged, seed %llu: leader now %llu, installs %llu and %llu\n", (unsigned long long)seed, (unsigned long long)(w.leader() ? w.leader()->id : 0),
		(unsigned long long)a.p->g->installs(), (unsigned long long)b.p->g->installs());
	++cases;
}

// A damaged leader whose next voter in the ring is down: its next hand-off passes it by.
void handoff_down(std::uint64_t seed) {
	context = "hand-off past a down voter, seed " + std::to_string(seed);
	world w(3, seed);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& a = *w.nodes.at(1);
	auto& b = *w.nodes.at(2);
	w.shut(a, false);
	if (!damage(a, kind::manifest, {})) { return; }
	w.boot(a);
	auto starve = [&] { w.fail_fetches(a.id); };
	if (!opened(w, a) || !check(lead_damaged(w, a, starve), "damaged node 1 leads")) { return; }
	w.shut(b, false);
	if (!check(healed(w), "the damaged leader hands off past the down voter and reinstalls")) { return; }
	w.boot(b);
	if (!opened(w, b) || !check(w.settle(), "the cluster settles once the down voter is back")) { return; }
	digests(w);
	std::printf("repair: hand-off past a down voter, seed %llu: leader now %llu, node 1 installs %llu\n", (unsigned long long)seed, (unsigned long long)(w.leader() ? w.leader()->id : 0),
		(unsigned long long)a.p->g->installs());
	++cases;
}

// A lone voter (D7) whose manifest log is damaged: no replica can reinstall it, so the reopen
// sets its bytes aside under `iq.`, byte for byte, and it waits damaged.
void lone(std::uint64_t seed) {
	context = "lone voter, seed " + std::to_string(seed);
	world w(1, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = *w.nodes.at(1);
	w.shut(x, false);
	if (!damage(x, kind::manifest, {})) { return; }
	std::map<std::string, std::string> before;
	{
		fl::io io(x.dir_disk);
		for (const auto& n : names_in(x)) {
			if (strata::index_detail::parse_name(n)) { before[n] = strata::index_detail::read_all(*io.open_existing(n)); }
		}
	}
	w.boot(x);
	if (!opened(w, x)) { return; }
	for (int i = 0; i < 50000; ++i) { w.step(); }
	check(x.p->g->damaged() && marked(x), "the lone voter stays damaged");
	std::size_t kept = 0;
	{
		fl::io io(x.dir_disk);
		for (const auto& [n, bytes] : before) {
			auto f = sg::repair_detail::open_if(io, "iq." + n);
			kept += f && strata::index_detail::read_all(*f) == bytes && !sg::repair_detail::open_if(io, n);
		}
	}
	check(!before.empty() && kept == before.size(), "every strata file is set aside whole: " + std::to_string(kept) + " of " + std::to_string(before.size()));
	std::printf("repair: lone voter, seed %llu: %zu strata files set aside, damaged\n", (unsigned long long)seed, kept);
	++cases;
}

void deposed(std::uint64_t seed) {
	context = "deposed, seed " + std::to_string(seed);
	world w(3, seed, true);
	std::mt19937_64 gen(seed);
	if (!prepare(w, gen)) { return; }
	auto& x = follower(w);
	const auto base = x.p->g->recovered().position;
	w.shut(x, false);
	if (!damage(x, kind::manifest, {})) { return; }
	w.boot(x);
	if (!opened(w, x) || !check(x.p->g->damaged(), "the replica reopens damaged")) { return; }
	// Its fetches fail, so the leader's flights to it stay unfinished.
	auto* l = w.leader();
	bool flying = false;
	for (int i = 0; l && i < 40000 && !flying; ++i) {
		w.step();
		w.fail_fetches(x.id);
		flying = l->p->g->installs_needed().count(x.id) > 0;
	}
	if (!check(flying, "the leader names the damaged replica's install")) { return; }
	auto& y = third(w, *l, x);
	auto t = start(l->p->loop_ref(), l->p->lp->driver().transfer_leadership(gid, y.id));
	bool moved = false;
	for (int i = 0; i < 40000 && !moved; ++i) {
		w.step();
		w.fail_fetches(x.id);
		const auto s = l->p->lp->driver().state(gid);
		moved = s && s->role != cs::Role::Leader;
	}
	if (!check(moved, "the leader loses its leadership mid-flight")) { return; }
	bool forgot = false;
	for (int i = 0; i < 1000 && !forgot; ++i) {
		w.step();
		w.fail_fetches(x.id);
		forgot = l->p->g->installs_needed().empty();
	}
	check(forgot, "the deposed leader names no install");
	const auto old = l->id;
	repaired(w, x, base);
	for (auto& [id, n] : w.nodes) { check(n->installs_needed().empty(), "node " + std::to_string(id) + " names no install once settled"); }
	std::printf("repair: deposed, seed %llu: node %llu lost its leadership mid-flight and names no install; one image at %llu\n", (unsigned long long)seed, (unsigned long long)old,
		(unsigned long long)x.p->g->installed_at());
	++cases;
}

// The damage of `dk` is gone from the open replica: strata's index holds a manifest log again,
// or the sealed file its bundle names reads back its CRC32C. A replica unmarked and clean was
// repaired, whichever process counted the activation.
bool damage_gone(node& x, kind dk, const std::string& sealed) {
	if (dk == kind::manifest) { return !deferred(x); }
	const auto& files = x.p->g->recovered().files;
	const auto it = files.find(sealed);
	if (it == files.end() || !it->second.crc) { return true; }
	fl::io io(x.dir_disk);
	auto f = sg::repair_detail::open_if(io, "app." + sealed);
	return f && sg::install_detail::crc_of(*f, it->second.length) == *it->second.crc;
}
// The names a complete stage left, with the replica's state beside them.
std::string stage_state(node& x) {
	fl::io io(x.dir_disk);
	const auto pin = sg::staged(io);
	auto& g = *x.p->g;
	return " (stage at " + (pin ? std::to_string(pin->base.position) : std::string("none")) + ", recovered " + std::to_string(g.recovered().position) + ", applied " +
		std::to_string(g.applied()) + ", installs " + std::to_string(x.installs) + "+" + std::to_string(g.installs()) + ")";
}

// A power cut on the damaged follower's directory disk (or host disk) after `k` mutations from
// its reboot; ~0 is the dry run, which returns the mutations through the settled repair in
// `used`. False when the repair settles before the cut.
bool cut_at(kind dk, bool host, std::uint64_t k, std::uint64_t* used) {
	context = std::string(name_of(dk)) + ", a power cut on the " + (host ? "host" : "directory") + " disk after " + std::to_string(k) + " mutations";
	world w(3, 55, true);
	std::mt19937_64 gen(55);
	if (!prepare(w, gen)) { return false; }
	auto& x = follower(w);
	const auto sealed = sealed_file(x);
	const auto base = x.p->g->recovered().position;
	w.shut(x, false);
	if (!damage(x, dk, sealed)) { return false; }
	auto& fs = host ? x.host_disk : x.dir_disk;
	const auto m0 = fs.mutations();
	if (k == ~std::uint64_t(0)) {
		boot(w, x);
		check(healed(w) && w.settle() && x.p->g->installs() == 1, "the dry run settles by the reinstall");
		*used = fs.mutations() - m0;
		return false;
	}
	fs.power_off_after(k);
	const bool up = boot(w, x);
	bool cut = !up || fs.powered_off();
	for (int i = 0; i < 400000 && !cut; ++i) {
		w.step();
		cut = fs.powered_off();
		if (!cut && i % 1000 == 999 && x.p->g->installs() >= 1 && fs.mutations() - m0 < k && w.settle(false)) { break; }
	}
	if (!cut) {
		fs.power_on();
		return false;
	}
	++w.crashes;
	if (up) { w.shut(x, true); }
	try {
		w.boot(x);
		if (!opened(w, x)) {
			std::fflush(stdout);
			std::_Exit(1);
		}
		check(x.p->g->damaged() || (!marked(x) && damage_gone(x, dk, sealed)), "the reboot is damaged or repaired, never unfenced over damaged bytes" + stage_state(x));
		check(x.p->g->damaged() == marked(x), "damaged exactly while marked");
		if (!check(healed(w) && w.settle(), "the cluster settles after the power cut")) { return true; }
	} catch (const std::exception& e) {
		check(false, std::string("the reboot throws: ") + e.what());
		std::fflush(stdout);
		std::_Exit(1);
	}
	check(!x.p->g->damaged() && !marked(x), "the replica is repaired");
	check(damage_gone(x, dk, sealed) && x.p->g->recovered().position >= base, "an image at or past its base replaced the damaged bytes" + stage_state(x));
	const auto left = staging_left(x);
	check(left.empty(), "no staging name stays:" + left + stage_state(x));
	digests(w);
	return true;
}
void crash(kind dk, bool host) {
	std::uint64_t used = 0, cut = 0;
	cut_at(dk, host, ~std::uint64_t(0), &used);
	for (std::uint64_t k = 0; k <= used; ++k) { cut += cut_at(dk, host, k, nullptr); }
	context = std::string(name_of(dk)) + ", power cuts on the " + (host ? "host" : "directory") + " disk";
	check(used > 5 && 2 * cut >= used, "the cuts fall inside the repair's " + std::to_string(used) + " mutations: " + std::to_string(cut));
	std::printf("repair: crash, %s on the %s disk: %llu power cuts over %llu mutations\n", name_of(dk), host ? "host" : "directory", (unsigned long long)cut, (unsigned long long)used);
	++cases;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	auto wants = [&](const char* c) { return only.empty() || only == c; };
	for (std::uint64_t seed : {1, 2}) {
		if (wants("segment")) { damaged_at_open(kind::segment, seed); }
		if (wants("manifest")) { damaged_at_open(kind::manifest, seed); }
		if (wants("file")) { damaged_at_open(kind::file, seed); }
		if (wants("apply")) { damaged_at_apply(seed); }
		if (wants("read")) { damaged_at_read(seed); }
		if (wants("refusal")) { refusal(seed); }
		if (wants("staged")) { staged_damage(seed); }
		if (wants("leader")) { damaged_leader(seed); }
		if (wants("source")) {
			damaged_source(seed, false);
			damaged_source(seed, true);
		}
		if (wants("body")) { damaged_source(seed, true, true); }
		if (wants("rot")) { rot_after_pin(seed); }
		if (wants("deposed")) { deposed(seed); }
		if (wants("two-damaged")) { two_damaged(seed); }
		if (wants("handoff-down")) { handoff_down(seed); }
		if (wants("lone")) { lone(seed); }
	}
	if (wants("crash") || only == "crash-manifest-dir") { crash(kind::manifest, false); }
	if (wants("crash") || only == "crash-manifest-host") { crash(kind::manifest, true); }
	if (wants("crash") || only == "crash-file-dir") { crash(kind::file, false); }
	if (wants("crash") || only == "crash-file-host") { crash(kind::file, true); }
	std::printf("repair: %llu cases: %s\n", (unsigned long long)cases, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
