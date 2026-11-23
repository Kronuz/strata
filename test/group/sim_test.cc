// strata::group's simulator rows (quest group-heal A5; docs/gates/phase-3r.md) at a short budget,
// on the voters' world (world.hh): each node's journal host, strata index and file set over
// faultfs, the network and every IO in seeded order, one thread, simulated time.
//
// Clients record a D9 history (sim/include/simulation/history.hh): puts, conditional puts and
// erases at the leader (`quorum`), and `linearizable` gets by id through read_applied at any
// data member; the simulator's checker must find it linearizable. `prefix` gets read a replica's
// applied state directly and must equal the key's state after some prefix of the log, monotone
// per process. An upload (chunks, then a final that writes a key) cut by a leader change writes
// nothing; a whole one writes its value. After each run the cluster settles, and every
// acknowledged command is at its placement and every data member's digest equals a replay.
//
// `group_sim_test ROW [VOTERS]`:
//   faults      crashes, power cuts (faultfs's torn and reordered sectors), power cuts at the data
//               device's next mutations, two-sided partitions, an isolated leader, clock skew,
//               checkpoints with installs, and uploads cut by a leader change
//   barriers    the victim's power cut at every crash point of its data device across a window
//               (an upload whose final retires its staging, then a checkpoint that unlinks it)
//   membership  add, remove and replace (a follower and the leader) under traffic
//   growth      D7's path from a lone voter: 0, 1, 2, 1, 0 replicas, a witness in each window
//   device      a log device that persisted a non-prefix subset of its writes since its last
//               sync, answered by the journal's checksums
//   corruption  A3's simulator half: damaged segment, manifest and file-set files; skipped (77)
//               until task `repair` merges
#include "world.hh"

#include "simulation/checker.hh"
#include "simulation/history.hh"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace group_test;
namespace sc = kronuz::simulation::check;

std::uint64_t key_id(const std::string& k) { return std::uint64_t(std::find(keys.begin(), keys.end(), k) - keys.begin()) + 1; }

// A value that is not a number this test wrote reads as ~0, which no put writes.
std::uint64_t parse_value(std::string_view s) {
	std::uint64_t v = 0;
	const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	return !s.empty() && ec == std::errc() && end == s.data() + s.size() ? v : ~std::uint64_t(0);
}

struct reading {
	bool present = false;
	std::uint64_t version = 0, value = 0;
	bool operator==(const reading&) const = default;
};
// A key's document through its locator into the file set, as the oracle reads it.
template <class Files>
reading read_key(const blob::index_type& ix, Files& files, const std::string& key) {
	reading r;
	auto found = ix.find_key(key);
	if (!found) { return r; }
	auto stored = ix.get(key);
	if (!stored) { return r; }
	const std::string loc(*stored);
	const auto a = loc.find(':'), b = loc.find(':', a + 1);
	const auto volume = std::stoull(loc.substr(0, a)), offset = std::stoull(loc.substr(a + 1, b - a - 1)), length = std::stoull(loc.substr(b + 1));
	r.present = true;
	r.version = found->second;
	r.value = parse_value(files.read(blob::volume_name(volume), offset, std::size_t(length)));
	return r;
}

// Each key's states along the log, replayed from empty through the application alone: (index,
// state) at index 0 and at every index that changed it.
using timeline = std::map<std::uint64_t, std::vector<std::pair<std::uint64_t, reading>>>;
timeline states_of(const std::map<std::uint64_t, blob::app::seen_entry>& log, std::uint64_t through) {
	fl::filesystem fs;
	fl::io io(fs);
	strata::basic_index_options<strata::document_layout> o;
	o.keyed = true;
	auto ix = blob::index_type::create(io, o, {'s', 'i', 'm'});
	blob::app app;
	blob::state s;
	mem_files files;
	timeline out;
	for (const auto& k : keys) { out[key_id(k)].push_back({0, reading{}}); }
	for (std::uint64_t i = 1; i <= through; ++i) {
		const auto& e = log.at(i);
		const sg::entry view{i, e.term, e.kind, e.command};
		app.effects(s, view, files);
		if (auto t = app.transaction(s, view)) { ix.apply(*t, strata::files::position{i, 1}); }
		app.advance(s, view);
		if (e.kind != cs::EntryKind::Command) { continue; }
		const auto f = blob::fields(e.command);
		std::string key;
		if ((f[0] == "P" || f[0] == "C" || f[0] == "D") && f.size() > 1) { key = std::string(f[1]); }
		if (f[0] == "F" && f.size() > 2) { key = std::string(f[2]); }
		if (key.empty()) { continue; }
		auto& states = out[key_id(key)];
		const auto now = read_key(ix, files, key);
		if (!(states.back().second == now)) { states.push_back({i, now}); }
	}
	return out;
}

struct totals {
	std::uint64_t runs = 0, events = 0, crashes = 0, power_cuts = 0, device_cuts = 0, partitions = 0, isolations = 0, skews = 0, checkpoints = 0, installs = 0;
	std::uint64_t writes = 0, conflicts = 0, missing = 0, unknown = 0, gets = 0, prefix_reads = 0, uploads_whole = 0, uploads_cut = 0, ops = 0;
};
totals sum;

// One history client: one operation at a time; an unknown outcome retires it and a new id
// takes its place (D9).
struct client {
	enum class doing { idle, put, cput, erase, get } what = doing::idle;
	std::uint64_t id = 0, key = 0, value = 0, invoked = 0;
	std::optional<std::uint64_t> expected;
	cs::NodeId at = 0;
	std::uint64_t boot = 0;  // the process it waits on
	std::shared_ptr<started<sg::answer>> write;
	std::shared_ptr<started<reading>> read;
};
// An upload: three chunks, then a final that writes its key from the staging file.
struct uploader {
	int stage = -1;  // -1 idle, 0-2 a chunk, 3 the final
	std::uint64_t client = 0, id = 0, value = 0, key = 0, invoked = 0;
	std::string digits;
	std::vector<cs::Term> terms;
	bool cut = false;  // a leader change is forced after the first chunk
	cs::NodeId at = 0;
	std::uint64_t boot = 0;
	std::shared_ptr<started<sg::answer>> s;
};
struct prefix_read {
	cs::NodeId node = 0;
	std::uint64_t boot = 0;
	cs::Index applied = 0;
	std::uint64_t key = 0;
	reading got;
};

struct sim {
	world& w;
	std::mt19937_64 gen;
	std::string row;
	std::uint64_t clock = 0;  // logical time: one tick per event
	sc::history h;
	std::vector<prefix_read> prefixes;
	std::vector<client> clients;
	uploader up;
	std::uint64_t next_client = 1, next_value = 1'000'000, next_upload = 100'000'000'000;
	std::map<cs::NodeId, std::uint64_t> boots;
	std::map<std::uint64_t, std::uint64_t> seen_version;  // the last version a client saw, by key
	totals t;
	// Faults in force.
	std::set<cs::NodeId> side;  // a partition's one side; empty: none
	std::uint64_t side_until = 0, isolated_until = 0, skew_until = 0, down_until = 0;
	cs::NodeId skewed = 0, down = 0, armed = 0;
	std::uint64_t skew_pct = 0;
	std::shared_ptr<started<cs::CheckpointPublished>> ckpt;
	cs::NodeId ckpt_at = 0;
	std::uint64_t ckpt_boot = 0;

	sim(world& in, std::uint64_t seed, std::string name, unsigned n_clients = 4) : w(in), gen(seed * 0x9e3779b97f4a7c15ull + 7), row(std::move(name)) {
		for (unsigned i = 0; i < n_clients; ++i) { clients.push_back(client{client::doing::idle, next_client++}); }
		up.client = next_client++;
	}

	std::uint64_t pick(std::uint64_t n) { return n ? gen() % n : 0; }
	node* at(cs::NodeId id) {
		auto it = w.nodes.find(id);
		return it == w.nodes.end() ? nullptr : it->second.get();
	}
	bool live(cs::NodeId id, std::uint64_t boot) {
		auto* x = at(id);
		return x && x->up() && boots[id] == boot;
	}
	node* data_leader() {
		auto* l = w.leader();
		return l && !l->witness && !w.isolated.count(l->id) ? l : nullptr;
	}
	node* any_data() {
		std::vector<node*> open;
		for (auto& [id, x] : w.nodes) {
			if (x->open() && !x->witness && !x->fenced()) { open.push_back(x.get()); }
		}
		return open.empty() ? nullptr : open[pick(open.size())];
	}

	// --- processes ------------------------------------------------------------------------

	// Stops a node; operations waiting on its process end: a write unknown, a read dropped.
	void shut(node& x, bool power_cut) {
		w.shut(x, power_cut);
		++w.crashes;
		lost(x.id);
	}
	void boot(node& x) {
		if (x.up()) { return; }
		w.boot(x);
		++boots[x.id];
	}
	void retire(cs::NodeId id) {
		if (auto* x = at(id); x && x->up()) { lost(id); }
		w.retire(id);
		lost(id);
	}
	void lost(cs::NodeId id) {
		for (auto& c : clients) {
			if (c.what == client::doing::idle || c.at != id) { continue; }
			if (c.write) { record_write(c, nullptr); }
			c.read.reset();
			c.what = client::doing::idle;
		}
		if (up.stage >= 0 && up.at == id && up.s) {
			if (up.stage == 3) { record_final(nullptr); }
			up.stage = -1;
			up.s.reset();
		}
		if (ckpt && ckpt_at == id) { ckpt.reset(); }
	}

	// --- clients --------------------------------------------------------------------------

	sc::operation op(const client& c, sc::kind k) const {
		sc::operation o;
		o.client = c.id;
		o.key = c.key;
		o.op = k;
		o.invoked = c.invoked;
		o.returned = clock;
		return o;
	}
	// A write's answer, or none: the outcome unknown and the client retired.
	void record_write(client& c, const sg::answer* a) {
		auto o = op(c, c.what == client::doing::erase ? sc::kind::erase : sc::kind::put);
		if (c.what != client::doing::erase) { o.value = c.value; }
		o.expected = c.expected;
		if (!a || !a->applied) {
			o.result = sc::outcome::unknown;
			o.returned = 0;
			++t.unknown;
			c.id = next_client++;
		} else if (const auto& v = *a->applied; v.result == strata::verdict::accepted) {
			if (c.what == client::doing::erase) {
				o.result = !v.docids.empty() && v.docids[0] ? sc::outcome::ok : sc::outcome::missing;
				t.missing += o.result == sc::outcome::missing;
			} else {
				o.result = sc::outcome::ok;
				o.version = v.versions.empty() ? 0 : v.versions[0];
				check(o.version > 0, "an accepted put names its version");
				seen_version[c.key] = o.version;
			}
			++t.writes;
			w.acks.push_back({a->placement, command(c)});
		} else if (v.result == strata::verdict::version_conflict) {
			o.result = sc::outcome::conflict;
			o.version = v.current_version;
			seen_version[c.key] = v.current_version;
			++t.conflicts;
			w.acks.push_back({a->placement, command(c)});
		} else {
			check(false, "a write's verdict is accepted or a version conflict: " + std::to_string(int(v.result)));
			o.result = sc::outcome::unknown;
			o.returned = 0;
			c.id = next_client++;
		}
		h.ops.push_back(o);
		c.write.reset();
	}
	std::string command(const client& c) const {
		const auto& k = keys[c.key - 1];
		switch (c.what) {
		case client::doing::put: return "P|" + k + "|" + std::to_string(c.value);
		case client::doing::cput: return "C|" + k + "|" + std::to_string(c.value) + "|" + std::to_string(*c.expected);
		case client::doing::erase: return "D|" + k;
		default: return {};
		}
	}
	void poll(client& c) {
		if (c.what == client::doing::idle) { return; }
		if (c.write) {
			if (!c.write->done) { return; }
			record_write(c, c.write->error || !c.write->value ? nullptr : &*c.write->value);
		} else if (c.read) {
			if (!c.read->done) { return; }
			if (!c.read->error && c.read->value) {
				auto o = op(c, sc::kind::get);
				const auto& r = *c.read->value;
				o.result = r.present ? sc::outcome::ok : sc::outcome::missing;
				o.value = r.present ? r.value : 0;
				o.version = r.present ? r.version : 0;
				h.ops.push_back(o);
				++t.gets;
			}
			c.read.reset();
		}
		c.what = client::doing::idle;
	}
	void begin(client& c) {
		c.key = 1 + pick(keys.size());
		const auto& k = keys[c.key - 1];
		const auto r = pick(10);
		if (r < 5) {
			auto* x = r < 3 ? any_data() : data_leader();
			if (!x) { return; }
			auto* g = &*x->p->g;
			c.what = client::doing::get;
			c.at = x->id;
			c.boot = boots[x->id];
			c.invoked = clock;
			c.read = start(x->p->loop_ref(), g->read_applied([g, k](blob::index_type& ix) { return read_key(ix, g->files(), k); }));
			x->run();
			return;
		}
		auto* l = data_leader();
		if (!l) { return; }
		c.expected.reset();
		c.value = 0;
		if (r < 7) {
			c.what = client::doing::put;
			c.value = next_value++;
		} else if (r < 9) {
			c.what = client::doing::cput;
			c.value = next_value++;
			const auto known = seen_version.find(c.key);
			c.expected = pick(3) == 0 || known == seen_version.end() ? pick(3) : known->second;
		} else {
			c.what = client::doing::erase;
		}
		c.at = l->id;
		c.boot = boots[l->id];
		c.invoked = clock;
		c.write = start(l->p->loop_ref(), l->p->g->propose(command(c)));
		l->run();
	}

	// --- uploads --------------------------------------------------------------------------

	void record_final(const sg::answer* a) {
		sc::operation o;
		o.client = up.client;
		o.key = up.key;
		o.op = sc::kind::put;
		o.value = up.value;
		o.invoked = up.invoked;
		if (!a || !a->applied) {
			o.result = sc::outcome::unknown;
			++t.unknown;
			h.ops.push_back(o);
			up.client = next_client++;
			return;
		}
		const bool cut = std::any_of(up.terms.begin(), up.terms.end(), [&](cs::Term term) { return term != a->placement.term; });
		const bool wrote = !a->applied->versions.empty();
		w.acks.push_back({a->placement, "F|" + std::to_string(up.id) + "|" + keys[up.key - 1]});
		if (cut) {
			++t.uploads_cut;
			check(!wrote, "upload " + std::to_string(up.id) + " cut by a leader change (chunks in terms " + std::to_string(up.terms.front()) + "-" + std::to_string(up.terms.back()) + ", final in " +
				std::to_string(a->placement.term) + ") writes nothing");
		} else {
			++t.uploads_whole;
			check(wrote, "upload " + std::to_string(up.id) + " in one term writes its value");
		}
		if (!wrote) { return; }
		o.result = sc::outcome::ok;
		o.returned = clock;
		o.version = a->applied->versions[0];
		seen_version[up.key] = o.version;
		h.ops.push_back(o);
	}
	void begin_upload(bool cut) {
		up.stage = 0;
		up.id = w.uploads++;
		up.value = next_upload++;
		up.digits = std::to_string(up.value);
		up.key = 1 + pick(keys.size());
		up.terms.clear();
		up.cut = cut;
		up.s.reset();
	}
	// The next step of the upload: its answer in, or its next command out.
	void poll_upload() {
		if (up.stage < 0) { return; }
		if (up.s) {
			if (!up.s->done) { return; }
			const sg::answer* a = up.s->error || !up.s->value ? nullptr : &*up.s->value;
			up.s.reset();
			if (up.stage == 3) {
				record_final(a);
				up.stage = -1;
				return;
			}
			if (!a) {  // a chunk not acknowledged: the client starts over
				up.stage = -1;
				return;
			}
			up.terms.push_back(a->placement.term);
			if (up.stage == 0 && up.cut) { isolate_leader(); }
			++up.stage;
		}
		auto* l = data_leader();
		if (!l) { return; }
		if (up.cut && up.stage == 1 && !w.isolated.empty()) {
			const auto st = l->p->lp->driver().state(gid);
			if (!st || st->term <= up.terms.front()) { return; }  // the next chunk goes to a later term's leader
		}
		std::string c;
		if (up.stage < 3) {
			c = "U|" + std::to_string(up.id) + "|" + std::to_string(4 * up.stage) + "|" + up.digits.substr(std::size_t(4 * up.stage), 4);
		} else {
			c = "F|" + std::to_string(up.id) + "|" + keys[up.key - 1];
			up.invoked = clock;
		}
		up.at = l->id;
		up.boot = boots[l->id];
		up.s = start(l->p->loop_ref(), l->p->g->propose(c));
		l->run();
	}

	// `prefix`: a replica's applied state, read between two events.
	void read_prefix() {
		auto* x = any_data();
		if (!x || !x->p->g->readable()) { return; }
		const auto key = 1 + pick(keys.size());
		prefix_read p{x->id, boots[x->id], x->p->g->applied(), key, {}};
		try {
			p.got = read_key(x->p->g->strata(), x->p->g->files(), keys[key - 1]);
		} catch (const std::exception& e) {
			check(false, "node " + std::to_string(x->id) + "'s prefix read: " + e.what());
			return;
		}
		prefixes.push_back(p);
		++t.prefix_reads;
	}

	// --- faults ---------------------------------------------------------------------------

	void isolate_leader() {
		auto* l = w.leader();
		if (!l || !w.isolated.empty()) { return; }
		w.isolated.insert(l->id);
		isolated_until = clock + 150 + pick(200);
		++t.isolations;
	}
	bool crosses(cs::NodeId a, cs::NodeId b) const { return !side.empty() && side.count(a) != side.count(b); }
	// A partition drops what crosses it; a fetch across it fails as a lost connection.
	void cut_links() {
		if (side.empty()) { return; }
		std::erase_if(w.net, [&](const envelope& e) { return crosses(e.from, e.to); });
		std::vector<fetch_request> failed;
		for (auto it = w.fetches.begin(); it != w.fetches.end();) {
			if (!crosses(it->from, it->to)) {
				++it;
				continue;
			}
			failed.push_back(std::move(*it));
			it = w.fetches.erase(it);
		}
		for (auto& f : failed) {
			f.done(std::make_error_code(std::errc::connection_aborted), std::nullopt);
			if (auto* x = at(f.from); x && x->up()) { x->run(); }
		}
	}
	// A skewed node's clock runs `skew_pct` percent fast.
	void skew_clock(std::uint64_t before) {
		auto* x = skewed ? at(skewed) : nullptr;
		if (!x || !x->up() || w.now == before) { return; }
		x->p->loop_s.advance((w.now - before) * ms * skew_pct / 100);
		x->run();
	}
	void disarm() {
		if (auto* x = armed ? at(armed) : nullptr) { x->dir_disk.power_off_after(~std::uint64_t(0) >> 1); }
		armed = 0;
	}
	void inject() {
		// One node down at a time: a process crash, a power cut, or a power cut at one of its
		// data device's next mutations.
		if (armed) {
			auto* x = at(armed);
			if (x && x->up() && x->dir_disk.powered_off()) {
				shut(*x, true);
				++t.device_cuts;
				armed = 0;
			}
		}
		if (down && clock >= down_until) {
			disarm();
			if (auto* x = at(down)) { boot(*x); }
			down = 0;
		} else if (!down && pick(250) == 0) {
			auto it = std::next(w.nodes.begin(), std::ptrdiff_t(pick(w.nodes.size())));
			auto& x = *it->second;
			if (x.up()) {
				down = x.id;
				down_until = clock + 80 + pick(300);
				switch (pick(3)) {
				case 0:
					shut(x, false);
					++t.crashes;
					break;
				case 1:
					shut(x, true);
					++t.power_cuts;
					break;
				default:
					x.dir_disk.power_off_after(pick(40));
					armed = x.id;
					break;
				}
			}
		}
		if (!side.empty() && clock >= side_until) {
			side.clear();
		} else if (side.empty() && pick(300) == 0) {
			for (auto& [id, x] : w.nodes) {
				if (pick(2)) { side.insert(id); }
			}
			if (side.empty() || side.size() == w.nodes.size()) {
				side.clear();
			} else {
				side_until = clock + 60 + pick(240);
				++t.partitions;
			}
		}
		if (!w.isolated.empty() && clock >= isolated_until) {
			w.isolated.clear();
		} else if (pick(400) == 0) {
			isolate_leader();
		}
		if (skewed && clock >= skew_until) {
			skewed = 0;
		} else if (!skewed && pick(300) == 0) {
			skewed = std::next(w.nodes.begin(), std::ptrdiff_t(pick(w.nodes.size())))->first;
			skew_pct = 25 + pick(100);
			skew_until = clock + 200 + pick(400);
			++t.skews;
		}
	}
	void checkpoints() {
		if (ckpt && ckpt->done) { ckpt.reset(); }
		if (ckpt || pick(150) != 0) { return; }
		auto* x = any_data();
		if (!x) { return; }
		ckpt_at = x->id;
		ckpt_boot = boots[x->id];
		ckpt = start(x->p->loop_ref(), x->p->g->checkpoint());
		x->run();
		++t.checkpoints;
	}

	// --- driving --------------------------------------------------------------------------

	void step(bool faults, bool issue) {
		if (faults) { inject(); }
		for (auto& c : clients) { poll(c); }
		poll_upload();
		if (issue) {
			for (auto& c : clients) {
				if (c.what == client::doing::idle && pick(6) == 0) { begin(c); }
			}
			if (up.stage < 0 && pick(40) == 0) { begin_upload(faults && pick(2) == 0); }
			if (pick(8) == 0) { read_prefix(); }
			if (faults) { checkpoints(); }
		}
		cut_links();
		const auto before = w.now;
		w.step();
		skew_clock(before);
		if (faults && !w.isolated.empty() && clock >= isolated_until) { w.isolated.clear(); }
		++clock;
		++t.events;
	}
	void run(std::uint64_t events, bool faults, bool issue = true) {
		for (std::uint64_t e = 0; e < events; ++e) { step(faults, issue); }
	}
	bool run_until(const std::function<bool()>& done, std::uint64_t events, bool issue = false) {
		for (std::uint64_t e = 0; e < events && !done(); ++e) { step(false, issue); }
		return done();
	}
	bool idle() const {
		return up.stage < 0 && std::all_of(clients.begin(), clients.end(), [](const client& c) { return c.what == client::doing::idle; });
	}
	// Every client's operation answered, none issued; what does not answer in `events` is
	// abandoned: a write unknown, a read unrecorded, an upload's chunks left behind.
	void drain(std::uint64_t events = 3000) {
		run_until([&] { return idle(); }, events);
		for (auto& c : clients) {
			if (c.write) { record_write(c, nullptr); }
			c.read.reset();
			c.what = client::doing::idle;
		}
		if (up.stage == 3 && up.s) { record_final(nullptr); }
		up.stage = -1;
		up.s.reset();
	}

	// A checkpoint at every open data member: no log reaches a joiner, which installs.
	void compact() {
		for (auto& [id, x] : w.nodes) {
			if (!x->open() || x->witness) { continue; }
			auto k = start(x->p->loop_ref(), x->p->g->checkpoint());
			x->run();
			run_until([&] { return k->done; }, 40000);
			check(k->done && !k->error, "node " + std::to_string(id) + "'s checkpoint: " + what(k->error));
			++t.checkpoints;
		}
	}

	// --- the oracle -----------------------------------------------------------------------

	void heal() {
		side.clear();
		w.isolated.clear();
		skewed = 0;
		disarm();
		down = 0;
		for (auto& [id, x] : w.nodes) { boot(*x); }
	}
	// Heals, settles and checks: acknowledged commands in the log, digests equal to a replay,
	// the history linearizable and every prefix read a consistent prefix.
	void finish(std::uint64_t seed) {
		heal();
		if (!check(w.settle(), "the cluster settles")) { return; }
		drain();
		check(w.settle(), "the cluster settles once the clients are answered");
		const node* first = nullptr;
		for (auto& [id, x] : w.nodes) {
			if (!first && !x->witness) { first = x.get(); }
			if (x->p->g) { t.installs += x->installs + x->p->g->installs(); }
		}
		if (!check(first, "a data member")) { return; }
		const auto through = first->applied();
		same_digests(w);
		const auto log = merged_log(w, through);
		if (!log) { return; }
		for (const auto& a : w.acks) {
			auto it = log->find(a.placement.index);
			check(a.placement.index <= through && it != log->end() && it->second.term == a.placement.term && it->second.command == a.command,
				"acknowledged " + a.command + " at " + std::to_string(a.placement.index) + " is in the log");
		}
		// The history, through its file format: every record well formed, then checked.
		const auto text = sc::write_history(h);
		const auto parsed = sc::read_history(text);
		check(parsed.ok(), "the history reads back: line " + std::to_string(parsed.line) + ": " + parsed.error);
		const auto r = sc::check(parsed.h.ops);
		t.ops += h.ops.size();
		if (!check(r.result == sc::verdict::linearizable, "the history (" + std::to_string(h.ops.size()) + " operations) is linearizable: verdict " + std::to_string(int(r.result)) + " at key " +
				std::to_string(r.key) + ": " + r.detail)) {
			std::ofstream("group_sim_" + row + "_" + std::to_string(seed) + ".history") << text;
		}
		// `prefix`: the state after some prefix of the log, at or below the replica's applied index
		// and at or past its process's previous read.
		const auto states = states_of(*log, through);
		std::map<std::pair<cs::NodeId, std::uint64_t>, std::uint64_t> floor;
		for (const auto& p : prefixes) {
			auto& lo = floor[{p.node, p.boot}];
			const auto& seq = states.at(p.key);
			std::optional<std::uint64_t> found;
			for (std::size_t k = 0; k < seq.size() && !found; ++k) {
				const auto begin = std::max(lo, seq[k].first);
				const auto end = k + 1 < seq.size() ? seq[k + 1].first : ~std::uint64_t(0);
				if (begin < end && begin <= p.applied && seq[k].second == p.got) { found = begin; }
			}
			if (!check(found.has_value(), "node " + std::to_string(p.node) + "'s prefix read of key " + std::to_string(p.key) + " at applied " + std::to_string(p.applied) + " (version " +
					std::to_string(p.got.version) + ", value " + std::to_string(p.got.value) + ") is the state after a prefix of the log")) {
				continue;
			}
			lo = *found;
		}
		++t.runs;
	}
};

void add_totals(totals& a, const totals& b) {
	a.runs += b.runs;
	a.events += b.events;
	a.crashes += b.crashes;
	a.power_cuts += b.power_cuts;
	a.device_cuts += b.device_cuts;
	a.partitions += b.partitions;
	a.isolations += b.isolations;
	a.skews += b.skews;
	a.checkpoints += b.checkpoints;
	a.installs += b.installs;
	a.writes += b.writes;
	a.conflicts += b.conflicts;
	a.missing += b.missing;
	a.unknown += b.unknown;
	a.gets += b.gets;
	a.prefix_reads += b.prefix_reads;
	a.uploads_whole += b.uploads_whole;
	a.uploads_cut += b.uploads_cut;
	a.ops += b.ops;
}
// The row's coverage: each named count must be at least one.
void covers(const totals& r, const std::vector<std::pair<const char*, std::uint64_t>>& counts) {
	for (const auto& [name, n] : counts) { check(n > 0, std::string("the row covers ") + name); }
	(void)r;
}

// --- faults ---------------------------------------------------------------------------------

void faults(unsigned voters) {
	totals row;
	for (std::uint64_t seed = 1; seed <= 8; ++seed) {
		context = "faults, " + std::to_string(voters) + " voters, seed " + std::to_string(seed);
		world w(voters, 1000 * voters + seed);
		sim s(w, seed, "faults_" + std::to_string(voters));
		if (!check(w.settle(), "the cluster settles")) { continue; }
		s.run(2000, true);
		s.finish(seed);
		add_totals(row, s.t);
	}
	context = "faults, " + std::to_string(voters) + " voters";
	covers(row, {{"process crashes", row.crashes}, {"power cuts", row.power_cuts}, {"power cuts at the data device", row.device_cuts}, {"partitions", row.partitions},
		{"isolated leaders", row.isolations}, {"clock skew", row.skews}, {"checkpoints", row.checkpoints}, {"acknowledged writes", row.writes}, {"conflicts", row.conflicts},
		{"linearizable gets", row.gets}, {"prefix reads", row.prefix_reads}, {"whole uploads", row.uploads_whole}, {"uploads cut by a leader change", row.uploads_cut}});
	add_totals(sum, row);
}

// --- barriers -------------------------------------------------------------------------------

// One run: traffic, then the window on a follower: an upload whose final retires its staging
// file, then a checkpoint at the follower, which publishes and unlinks it. `cut`: the follower's
// data device loses power before that many more mutations in the window. The window's mutations.
std::uint64_t barrier_run(unsigned voters, std::optional<std::uint64_t> cut, totals& row) {
	world w(voters, 2000 + voters);
	sim s(w, 9, "barriers_" + std::to_string(voters) + (cut ? "_" + std::to_string(*cut) : ""));
	if (!check(w.settle(), "the cluster settles")) { return 0; }
	s.run(300, false);
	s.drain();
	auto* l = w.leader();
	if (!check(l, "a leader")) { return 0; }
	node* victim = nullptr;
	for (auto& [id, x] : w.nodes) {
		if (id != l->id && !victim) { victim = x.get(); }
	}
	auto& x = *victim;
	const auto m0 = x.dir_disk.mutations();
	if (cut) { x.dir_disk.power_off_after(*cut); }
	bool landed = false;
	auto tripped = [&] {
		if (!landed && x.up() && x.dir_disk.powered_off()) {
			s.shut(x, true);
			landed = true;
		}
		return landed;
	};
	s.begin_upload(false);
	s.run_until([&] { return tripped() || s.up.stage < 0; }, 40000);
	if (!landed) {
		auto* now = w.leader();
		const auto target = now ? now->p->lp->driver().state(gid)->committed : 0;
		s.run_until([&] { return tripped() || (x.open() && x.p->g->applied() >= target); }, 40000);
	}
	if (!landed) {
		auto k = start(x.p->loop_ref(), x.p->g->checkpoint());
		x.run();
		s.run_until([&] { return tripped() || k->done; }, 40000);
		if (!cut) { check(k->done && !k->error, "the follower's checkpoint: " + what(k->error)); }
	}
	const auto window = x.dir_disk.mutations() - m0;
	if (cut) {
		check(landed, "the power cut lands before mutation " + std::to_string(*cut) + " of the window");
		if (!landed) { x.dir_disk.power_off_after(~std::uint64_t(0) >> 1); }
		++s.t.power_cuts;
		s.run(100, false);
	}
	s.finish(cut.value_or(0));
	add_totals(row, s.t);
	return window;
}

void barriers(unsigned voters) {
	totals row;
	context = "barriers, " + std::to_string(voters) + " voters, dry run";
	const auto window = barrier_run(voters, std::nullopt, row);
	check(window > 0, "the window mutates the follower's data device");
	for (std::uint64_t k = 0; k < window && failures == 0; ++k) {
		context = "barriers, " + std::to_string(voters) + " voters, power cut before the window's mutation " + std::to_string(k) + " of " + std::to_string(window);
		barrier_run(voters, k, row);
	}
	std::printf("barriers (%u voters): %llu crash points in the window, each cut with faultfs's torn and reordered sectors\n", voters, (unsigned long long)window);
	add_totals(sum, row);
}

// --- membership -----------------------------------------------------------------------------

bool step_to(sim& s, unsigned voters, const std::function<void(cs::Membership&)>& edit, cs::NodeId heir, const std::string& what_for) {
	s.drain();
	const bool ok = check(change(s.w, voters, edit, heir), what_for);
	return ok && check(s.w.settle(), "the cluster settles: " + what_for);
}
cs::NodeId other_data(world& w, std::initializer_list<cs::NodeId> not_these) {
	for (auto& [id, x] : w.nodes) {
		if (!x->witness && std::find(not_these.begin(), not_these.end(), id) == not_these.end()) { return id; }
	}
	return 0;
}
// A joiner installs past every checkpoint as a learner.
bool join_learner(sim& s, unsigned voters, cs::NodeId id, bool witness, cs::NodeId heir) {
	s.compact();
	auto& j = s.w.add_joiner(id, witness);
	++s.boots[id];
	return step_to(s, voters, [&](cs::Membership& m) {
		m.members.push_back({j.id, j.id, cs::MemberRole::Learner});
		m.next_incarnation = j.id + 1;
	}, heir, "node " + std::to_string(id) + " joins as a learner");
}
bool promote(sim& s, unsigned voters, cs::NodeId id, cs::MemberRole role, cs::NodeId leaving, cs::NodeId heir) {
	return step_to(s, voters, [&](cs::Membership& m) {
		for (auto& x : m.members) {
			if (x.incarnation == id) { x.role = role; }
		}
		if (leaving) { m.leaving = leaving; }
	}, heir, "node " + std::to_string(id) + " is promoted" + (leaving ? " beside " + std::to_string(leaving) : ""));
}
bool remove_member(sim& s, unsigned voters, cs::NodeId id, cs::NodeId heir) {
	if (!step_to(s, voters, [&](cs::Membership& m) { std::erase_if(m.members, [&](const cs::Member& x) { return x.incarnation == id; }); }, heir, "node " + std::to_string(id) + " is removed")) { return false; }
	s.retire(id);
	return check(s.w.settle(), "the cluster settles without node " + std::to_string(id));
}
bool replace_member(sim& s, unsigned voters, cs::NodeId leaving, cs::NodeId joiner) {
	if (!join_learner(s, voters, joiner, false, joiner)) { return false; }
	if (!promote(s, voters, joiner, cs::MemberRole::Voter, leaving, joiner)) { return false; }
	if (!check(removed_by_core(s.w, leaving), "the core removes the replaced voter " + std::to_string(leaving))) { return false; }
	s.retire(leaving);
	return check(s.w.settle(), "the cluster settles without the replaced voter");
}

void membership(unsigned voters) {
	totals row;
	context = "membership, " + std::to_string(voters) + " voters";
	world w(voters, 3000 + voters);
	sim s(w, 3, "membership_" + std::to_string(voters));
	if (!check(w.settle(), "the cluster settles")) { return; }
	cs::NodeId next = voters + 1;
	s.run(300, false);
	// Replace a follower, then the leader.
	auto* l = w.leader();
	if (!check(l, "a leader") || !replace_member(s, voters, other_data(w, {l->id}), next++)) { return; }
	s.run(200, false);
	l = w.leader();
	if (!check(l, "a leader") || !replace_member(s, voters, l->id, next++)) { return; }
	s.run(200, false);
	// Add a voter, then remove a follower.
	const auto added = next++;
	l = w.leader();
	if (!check(l, "a leader") || !join_learner(s, voters, added, false, l->id) || !promote(s, voters, added, cs::MemberRole::Voter, 0, l->id)) { return; }
	s.run(200, false);
	l = w.leader();
	if (!check(l, "a leader") || !remove_member(s, voters, other_data(w, {l->id, added}), l->id)) { return; }
	s.run(200, false);
	s.finish(3);
	add_totals(row, s.t);
	add_totals(sum, row);
}

// --- growth ---------------------------------------------------------------------------------

// D7: 0 to 1 (a data learner installs and is promoted, two data voters for the window, then a
// witness joins), 1 to 2 (a third data voter, then the witness leaves), 2 to 1 (a witness joins,
// a data voter leaves) and 1 to 0 (the witness leaves, then a data voter).
void growth() {
	totals row;
	context = "growth from a lone voter";
	world w(1, 4001);
	sim s(w, 4, "growth");
	if (!check(w.settle(), "the lone voter settles")) { return; }
	s.run(300, false);
	if (!join_learner(s, 1, 2, false, 1) || !promote(s, 1, 2, cs::MemberRole::Voter, 0, 1)) { return; }
	s.run(150, false);
	if (!join_learner(s, 1, 3, true, 1) || !promote(s, 1, 3, cs::MemberRole::Witness, 0, 1)) { return; }
	s.run(150, false);
	if (!join_learner(s, 1, 4, false, 1) || !promote(s, 1, 4, cs::MemberRole::Voter, 0, 1) || !remove_member(s, 1, 3, 1)) { return; }
	s.run(150, false);
	if (!join_learner(s, 1, 5, true, 1) || !promote(s, 1, 5, cs::MemberRole::Witness, 0, 1) || !remove_member(s, 1, 4, 1)) { return; }
	s.run(150, false);
	if (!remove_member(s, 1, 5, 1) || !remove_member(s, 1, 2, 1)) { return; }
	s.run(150, false);
	for (auto& [id, x] : w.nodes) { check(id == 1, "the lone voter is all that is left"); }
	s.finish(4);
	add_totals(row, s.t);
	add_totals(sum, row);
}

// --- device ---------------------------------------------------------------------------------

std::map<std::string, std::string> contents(fl::filesystem& fs) {
	std::map<std::string, std::string> out;
	fl::io io(fs);
	for (auto cursor = io.scan_directory(); auto name = cursor->next();) {
		try {
			auto f = io.open_existing_read_only(*name);
			std::string b(std::size_t(f->size()), '\0');
			for (std::size_t at = 0; at < b.size();) {
				const auto got = f->read_at(at, std::span<char>(b.data() + at, b.size() - at));
				if (!got) { break; }
				at += got;
			}
			out[*name] = std::move(b);
		} catch (...) {}
	}
	return out;
}
std::string sector_of(const std::string& s, std::size_t k) {
	std::string out(512, '\0');
	if (k * 512 < s.size()) { s.copy(out.data(), std::min<std::size_t>(512, s.size() - k * 512), k * 512); }
	return out;
}
// Sectors written since the last sync, by file: those whose current bytes are not durable.
std::size_t unsynced(const std::map<std::string, std::string>& current, const std::map<std::string, std::string>& durable) {
	std::size_t most = 0;
	for (const auto& [name, c] : current) {
		const auto d = durable.count(name) ? durable.at(name) : std::string();
		std::size_t n = 0;
		for (std::size_t k = 0; k * 512 < std::max(c.size(), d.size()); ++k) { n += sector_of(c, k) != sector_of(d, k); }
		most = std::max(most, n);
	}
	return most;
}
// Files where the crash kept a later unsynced sector and lost an earlier one: a non-prefix
// subset of the writes since the last sync.
std::size_t non_prefix(const std::map<std::string, std::string>& current, const std::map<std::string, std::string>& durable, const std::map<std::string, std::string>& after) {
	std::size_t files = 0;
	for (const auto& [name, c] : current) {
		const auto d = durable.count(name) ? durable.at(name) : std::string();
		const auto a = after.count(name) ? after.at(name) : std::string();
		bool lost = false, kept_after_lost = false;
		for (std::size_t k = 0; k * 512 < std::max(c.size(), d.size()); ++k) {
			const auto cs_ = sector_of(c, k), ds = sector_of(d, k), as = sector_of(a, k);
			if (cs_ == ds) { continue; }
			if (as == ds) { lost = true; }
			if (as == cs_ && lost) { kept_after_lost = true; }
		}
		files += kept_after_lost;
	}
	return files;
}

void device(unsigned voters) {
	totals row;
	std::uint64_t found = 0, cuts = 0;
	const fl::crash_policy durable_only{fl::epoch_pick::none, fl::name_pick::durable, fl::size_pick::durable, fl::sector_pick::old};
	for (std::uint64_t seed = 1; seed <= 16 && found < 2; ++seed) {
		context = "device, " + std::to_string(voters) + " voters, seed " + std::to_string(seed);
		world w(voters, 5000 + seed);
		sim s(w, seed, "device_" + std::to_string(voters));
		if (!check(w.settle(), "the cluster settles")) { continue; }
		s.run(150, false);
		s.drain();
		auto* l = w.leader();
		if (!check(l, "a leader")) { continue; }
		auto& x = *w.nodes.at(other_data(w, {l->id}));
		// Large no-op commands at the leader until the follower's log holds two unsynced sectors.
		std::map<std::string, std::string> current, durable;
		bool ready = false;
		for (int i = 0; i < 8 && !ready; ++i) {
			auto p = start(l->p->loop_ref(), l->p->g->propose("X|" + std::string(1800, char('a' + i)), sg::ack::none));
			l->run();
			for (int k = 0; k < 400 && !ready; ++k) {
				s.step(false, false);
				if (!x.up()) { break; }
				auto now = x.host_disk.snapshot();
				auto kept = x.host_disk.snapshot();
				kept.crash(seed, durable_only);
				current = contents(now);
				durable = contents(kept);
				ready = unsynced(current, durable) >= 2;
			}
		}
		if (!check(ready, "the follower's log holds two unsynced sectors")) { continue; }
		s.shut(x, true);
		++cuts;
		const auto after = contents(x.host_disk);
		found += non_prefix(current, durable, after) > 0;
		s.run(150, false);
		s.finish(seed);
		check(x.open() && !x.fenced(), "the follower reopens on its log: " + (x.open() ? x.fence_reason() : what(x.p->opening->error)));
		add_totals(row, s.t);
	}
	context = "device, " + std::to_string(voters) + " voters";
	check(found > 0, "a power cut leaves a log that persisted a non-prefix subset of its unsynced sectors");
	std::printf("device (%u voters): %llu power cuts with unsynced log sectors, %llu left a non-prefix log, each reopened and settled\n", voters, (unsigned long long)cuts, (unsigned long long)found);
	add_totals(sum, row);
}

}  // namespace

int main(int argc, char** argv) {
	const std::string row = argc > 1 ? argv[1] : "";
	const unsigned voters = argc > 2 ? unsigned(std::stoul(argv[2])) : 3;
	if (row == "corruption") {
		std::printf("corruption: A3's simulator half (a damaged segment, manifest or file-set file fences its replica, which reinstalls) waits for task repair: skipped\n");
		return 77;
	}
	if (row.empty() || row == "faults") { faults(voters); }
	if (row.empty() || row == "barriers") { barriers(voters); }
	if (row.empty() || row == "membership") { membership(voters); }
	if (row.empty() || row == "growth") { growth(); }
	if (row.empty() || row == "device") { device(voters); }
	std::printf("group_sim %s (%u voters): %llu runs, %llu events, %llu crashes, %llu power cuts, %llu at the data device, %llu partitions, %llu isolated leaders, %llu skews, %llu checkpoints, "
				"%llu installs; %llu writes (%llu conflicts, %llu missing erases), %llu unknown, %llu linearizable gets, %llu prefix reads, %llu whole and %llu cut uploads, %llu history "
				"operations: %s\n",
		row.empty() ? "all" : row.c_str(), voters, (unsigned long long)sum.runs, (unsigned long long)sum.events, (unsigned long long)sum.crashes, (unsigned long long)sum.power_cuts,
		(unsigned long long)sum.device_cuts, (unsigned long long)sum.partitions, (unsigned long long)sum.isolations, (unsigned long long)sum.skews, (unsigned long long)sum.checkpoints,
		(unsigned long long)sum.installs, (unsigned long long)sum.writes, (unsigned long long)sum.conflicts, (unsigned long long)sum.missing, (unsigned long long)sum.unknown,
		(unsigned long long)sum.gets, (unsigned long long)sum.prefix_reads, (unsigned long long)sum.uploads_whole, (unsigned long long)sum.uploads_cut, (unsigned long long)sum.ops,
		failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
