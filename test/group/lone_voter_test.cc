// strata::group's lone voter over faultfs (quest group-core A1): create, propose, apply,
// checkpoint and recover, crashed at every point of one script (a power cut: every unsynced
// sector and directory entry of strata, the file set and the journal host may be lost), and
// crashed again at every point of the recovery. HeldCompletion holds the journal's IO, so each
// completion and each chore step is a crash point. Oracle: every entry ever applied keeps its
// index, term and command; every acknowledged command is in the recovered log; and the digest
// (documents with versions, resolved through their locators; the file set; the application
// state) equals a replay from empty of the recovered log on a fresh index and an in-memory file
// set. `lone_voter_test [first [stride]]` limits the crash points (ctest runs them all).
#include "blob_app.hh"
#include "host_io.hh"

#include "strata/core/faultfs.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace ff = strata::faultfs;
namespace sg = strata::group;
using group_type = sg::group<blob::app>;

int failures = 0;
std::string context;
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 20) { std::cerr << "FAIL [" << context << "]: " << what << '\n'; }
	if (failures > 200) { std::exit(1); }
	return ok;
}
std::string what(std::exception_ptr e) {
	if (!e) { return "no error"; }
	try {
		std::rethrow_exception(e);
	} catch (const std::exception& x) {
		return x.what();
	} catch (...) {}
	return "unknown";
}

const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits admission{{256u << 20, 8192}, {256 * 1024, 3}, {64u << 20, 4}, 32, 3};
constexpr cs::GroupId gid = 5;

group_type::config config(strata::byte_budget& storage) {
	group_type::config c;
	c.id = gid;
	c.options.configuration.local = 1;
	c.options.configuration.voters = {1};
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	// Small enough that a checkpoint's reservation fits the admission's artifact limit.
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 16384;
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{20, 40, 100, 200, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'L', 'V'};
	c.uuid = {'g', 'r', 'o', 'u', 'p'};
	c.strata.keyed = true;
	c.strata.storage = &storage;
	return c;
}

// What survives a power cut: three directories, and the node's storage budget.
struct disks {
	ff::filesystem host, strata, files;
	strata::byte_budget storage{std::uint64_t(1) << 30};
};

struct crash_now {};
struct stuck {};

// One process: a loop and a chore on inline schedulers, the journal's IO held.
struct process {
	disks& d;
	task::inline_scheduler loop_s, chore_s;
	j::HeldCompletion held;
	j::InlineCompletion inline_io;
	std::unique_ptr<j::Host> host;
	std::unique_ptr<sg::loop> lp;
	ff::io strata_io, files_io;
	std::unique_ptr<group_type> g;
	task::cancel_source stop;
	bool turns_done = false, driver_done = false;

	process(disks& disk, blob::app& app, std::uint64_t seed) : d(disk), strata_io(disk.strata), files_io(disk.files) {
		host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(d.host), j::Identity{'H', 'O', 'S', 'T'}, j::HostOptions{1, turn_options});
		lp = std::make_unique<sg::loop>(loop_ref(), j::completion_ref::of(inline_io), *host, 0, seed);
		task::spawn(loop_ref(), host->driver(0).run(j::completion_ref::of(held), stop.token()), [this](std::exception_ptr) { turns_done = true; });
		task::spawn(loop_ref(), lp->run(stop.token()), [this](std::exception_ptr) { driver_done = true; });
		g = std::make_unique<group_type>(*lp, config(d.storage), strata_io, files_io, app, task::scheduler_ref::of(chore_s));
		loop_s.run();
	}
	task::scheduler_ref loop_ref() { return task::scheduler_ref::of(loop_s); }

	// One crash point: a chore step, else one held IO completion. False when idle.
	bool event() {
		loop_s.run();
		std::size_t n = 0;
		if (chore_s.run_until([&] { return n++ == 1; })) { return true; }
		if (held.held()) {
			held.complete(held.tickets().front());
			return true;
		}
		return false;
	}
	// Stops the process; a power cut fails every IO from here, held or not.
	void shut(bool power_cut) {
		if (power_cut) {
			d.host.power_off_after(0);
			d.strata.power_off_after(0);
			d.files.power_off_after(0);
		}
		stop.request(task::cancel_reason::shutdown);
		for (int round = 0; round < 100000; ++round) {
			loop_s.run();
			chore_s.run();
			if (held.held()) {
				const auto t = held.tickets().front();
				if (power_cut) {
					held.fail(t, std::make_exception_ptr(std::system_error(EIO, std::generic_category(), "power cut")));
				} else {
					held.complete(t);
				}
				continue;
			}
			if (turns_done && driver_done) { break; }
		}
		check(turns_done && driver_done, "the loop stops");
		g.reset();
		lp.reset();
		host.reset();
	}
};

template <class T>
struct started {
	bool done = false;
	std::exception_ptr error;
	std::optional<T> value;
};
template <class T>
std::shared_ptr<started<T>> start(process& p, task::task<T> work) {
	auto s = std::make_shared<started<T>>();
	task::spawn(p.loop_ref(), std::move(work), [s](std::exception_ptr e, std::optional<T> v) {
		s->done = true;
		s->error = e;
		s->value = std::move(v);
	});
	return s;
}
std::shared_ptr<started<bool>> start(process& p, task::task<void> work) {
	auto wrap = [](task::task<void> w) -> task::task<bool> {
		co_await std::move(w);
		co_return true;
	};
	return start(p, wrap(std::move(work)));
}

// Counts crash points across a run; at `crash_at` it throws crash_now.
struct clock {
	std::uint64_t events = 0, crash_at = ~std::uint64_t(0);
	void pump(process& p, const std::function<bool()>& done) {
		for (;;) {
			p.loop_s.run();
			if (done()) { return; }
			if (events == crash_at) { throw crash_now{}; }
			if (!p.event()) {
				if (done()) { return; }
				auto s = p.lp->driver().state(gid);
				std::cerr << "stuck: applied " << p.g->applied() << (p.g->stalled() ? " stalled" : "") << (p.g->fenced() ? " fenced: " + p.g->fence_reason() : "")
						  << ", role " << (s ? int(s->role) : -1) << " committed " << (s ? s->committed : 0) << " core applied " << (s ? s->applied : 0) << " in flight " << p.lp->driver().in_flight() << " waiting " << p.lp->driver().waiting_groups().size() << " timers " << p.lp->driver().armed_group_timers() << " loop timers " << p.loop_s.armed_timers() << '\n';
				throw stuck{};
			}
			++events;
		}
	}
	// Runs until nothing is left to do.
	void idle(process& p) {
		for (;;) {
			p.loop_s.run();
			if (events == crash_at) { throw crash_now{}; }
			if (!p.event()) { return; }
			++events;
		}
	}
};

struct acknowledged {
	cs::Placement placement;
	std::string command;
};

// The script, in one world: every case of A1 once.
struct run {
	disks& d;
	blob::app& app;
	clock& c;
	std::unique_ptr<process>& p;
	std::vector<acknowledged>& acks;
	std::uint64_t seed;
	bool exact = true;  // a full, uncrashed run checks each step's answer

	void expect(bool ok, const std::string& what) {
		if (exact) { check(ok, what); }
	}
	void open() {
		auto s = start(*p, p->g->open());
		c.pump(*p, [&] { return s->done; });
		expect(!s->error, "open: " + what(s->error));
	}
	std::shared_ptr<started<sg::answer>> begin(const std::string& command, sg::ack level = sg::ack::quorum) {
		if (std::getenv("GROUP_TRACE")) { std::cerr << "  [" << c.events << "] " << command << '\n'; }
		return start(*p, p->g->propose(command, level));
	}
	void finish(const std::shared_ptr<started<sg::answer>>& s, const std::string& command, std::optional<strata::verdict::outcome> want = strata::verdict::accepted) {
		if (std::getenv("GROUP_TRACE")) { std::cerr << "  [" << c.events << "] wait " << command << '\n'; }
		c.pump(*p, [&] { return s->done; });
		if (!s->error && s->value) {
			acks.push_back({s->value->placement, command});
			if (want && s->value->applied) { expect(s->value->applied->result == *want, command + " answered " + std::to_string(int(s->value->applied->result))); }
		} else {
			expect(false, command + ": " + what(s->error));
		}
	}
	void put(const std::string& command, sg::ack level = sg::ack::quorum, std::optional<strata::verdict::outcome> want = strata::verdict::accepted) {
		finish(begin(command, level), command, want);
	}
	std::shared_ptr<started<cs::CheckpointPublished>> checkpoint(task::cancel_token token = {}) {
		return start(*p, p->g->checkpoint(std::move(token)));
	}
	void checkpointed(bool ok = true) {
		auto s = checkpoint();
		c.pump(*p, [&] { return s->done; });
		expect(ok == !s->error, "checkpoint: " + what(s->error));
	}

	void script() {
		open();
		put("P|a|alpha");
		// An upload open across a checkpoint, which follows chunks (the capture's mark).
		put("U|1|0|aaaa", sg::ack::none, std::nullopt);
		put("U|1|4|bbbb", sg::ack::none, std::nullopt);
		checkpointed();
		// A staging file consumed above the bundle; an aborted upload; a refused write.
		put("U|1|8|cccc");
		put("F|1|b");
		put("U|2|0|zzzz");
		put("A|2");
		put("C|a|beta|99", sg::ack::quorum, strata::verdict::version_conflict);
		// A volume rotation, then a checkpoint racing a write and an upload's final entry.
		put("P|c|0123456789012345678901234567890123456789");
		put("P|d|dd");
		put("U|3|0|third-upload");
		{
			// The checkpoint starts while the final entry applies, after its file effects.
			std::shared_ptr<started<cs::CheckpointPublished>> k;
			app.after_effects = [&](const sg::entry& e) {
				if (!k && e.command == "F|3|k") { k = checkpoint(); }
			};
			auto w = begin("P|e|ee");
			auto f = begin("F|3|k");
			finish(w, "P|e|ee");
			finish(f, "F|3|k");
			app.after_effects = nullptr;
			expect(k != nullptr, "the racing checkpoint started");
			if (k) {
				c.pump(*p, [&] { return k->done; });
				expect(!k->error, "racing checkpoint: " + what(k->error));
			}
		}
		// A strata commit outside the group, then a cancelled checkpoint (strata commits, nothing
		// publishes).
		c.idle(*p);
		p->g->strata().commit();
		put("P|f|ff");
		{
			task::cancel_source cancel;
			cancel.request();
			auto k = checkpoint(cancel.token());
			c.pump(*p, [&] { return k->done; });
			expect(k->error != nullptr, "the cancelled checkpoint publishes nothing");
		}
		// A write whose first strata attempt answers no_space.
		{
			auto hog = d.storage.open();
			expect(hog.try_grow(d.storage.capacity() - d.storage.used() - 16), "the budget is nearly spent");
			auto w = begin("P|g|gg");
			c.pump(*p, [&] { return w->done || p->g->stalled(); });
			expect(!w->done, "the write waits for space");
			hog = {};
			p->g->resume();
			finish(w, "P|g|gg");
		}
		// A blob push that fails partway with ENOSPC, rewritten in place once space frees.
		{
			d.files.short_writes(4);
			d.files.set_capacity(d.files.used() + 6);
			auto w = begin("P|h|hhhhhhhhhhhh");
			c.pump(*p, [&] { return w->done || p->g->stalled(); });
			expect(!w->done, "the push waits for space");
			d.files.set_capacity(~std::uint64_t(0));
			d.files.short_writes(0);
			p->g->resume();
			finish(w, "P|h|hhhhhhhhhhhh");
		}
		// A checkpoint whose file sync fails: the replica fences and reopens through recovery.
		put("P|i|iiii");
		d.files.add_fault(ff::fault{ff::op::sync, 1, EIO, false, true});
		checkpointed(false);
		d.files.clear_faults();
		expect(p->g->fenced(), "a failed file sync fences the replica");
		c.idle(*p);
		p->shut(false);
		p = std::make_unique<process>(d, app, seed + 1);
		open();
		put("P|j|jj");
		checkpointed();
		put("P|a|alpha2");
		put("U|4|0|open-at-the-end", sg::ack::none, std::nullopt);
	}
};

// --- the oracle -------------------------------------------------------------------------

// An in-memory file set with file_set's apply interface.
struct mem_files {
	struct file {
		std::string bytes;
		std::optional<std::uint32_t> crc;
	};
	std::map<std::string, file> live;
	void write(std::string_view name, std::uint64_t offset, std::string_view bytes) {
		auto& f = live[std::string(name)];
		if (f.bytes.size() < offset + bytes.size()) { f.bytes.resize(offset + bytes.size(), '\0'); }
		f.bytes.replace(offset, bytes.size(), bytes);
	}
	std::string read(std::string_view name, std::uint64_t offset, std::size_t n) { return live.at(std::string(name)).bytes.substr(offset, n); }
	void seal(std::string_view name) {
		auto& f = live.at(std::string(name));
		if (!f.crc) { f.crc = kronuz::crc32c::crc32c(f.bytes); }
	}
	void retire(std::string_view name, std::uint64_t) { live.erase(std::string(name)); }
};

struct digest {
	std::map<std::string, std::pair<std::uint64_t, std::string>> documents;  // key: version, bytes
	std::map<std::string, std::pair<std::optional<std::uint32_t>, std::string>> files;
	std::string state;
	bool operator==(const digest&) const = default;
};
const std::vector<std::string> keys = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"};

template <class Read>
void documents(const blob::index_type& ix, Read&& read, digest& out) {
	for (const auto& key : keys) {
		auto found = ix.find_key(key);
		if (!found) { continue; }
		auto stored = ix.get(key);
		std::string loc(*stored);
		const auto a = loc.find(':'), b = loc.find(':', a + 1);
		const auto volume = std::stoull(loc.substr(0, a)), offset = std::stoull(loc.substr(a + 1, b - a - 1)), length = std::stoull(loc.substr(b + 1));
		out.documents[key] = {found->second, read(blob::volume_name(volume), offset, std::size_t(length))};
	}
}

digest of(group_type& g, blob::app& app) {
	digest out;
	documents(g.strata(), [&](const std::string& n, std::uint64_t o, std::size_t l) { return g.files().read(n, o, l); }, out);
	for (const auto& [name, f] : g.files().capture()) { out.files[name] = {f.crc, g.files().read(name, 0, std::size_t(f.length))}; }
	out.state = app.encode(g.state());
	return out;
}

// A replay from empty of the log's committed commands, through the application alone.
digest replay(const std::map<std::uint64_t, blob::app::seen_entry>& log, std::uint64_t through) {
	ff::filesystem fs;
	ff::io io(fs);
	strata::basic_index_options<strata::document_layout> o;
	o.keyed = true;
	auto ix = blob::index_type::create(io, o, {'r', 'e', 'f'});
	blob::app app;
	blob::state s;
	mem_files files;
	for (std::uint64_t i = 1; i <= through; ++i) {
		const auto& e = log.at(i);
		const sg::entry view{i, e.term, e.kind, e.command};
		app.effects(s, view, files);
		if (auto t = app.transaction(s, view)) { ix.apply(*t, strata::files::position{i, 1}); }
		app.advance(s, view);
	}
	digest out;
	documents(ix, [&](const std::string& n, std::uint64_t o, std::size_t l) { return files.read(n, o, l); }, out);
	for (const auto& [name, f] : files.live) { out.files[name] = {f.crc, f.bytes}; }
	out.state = app.encode(s);
	return out;
}

// After recovery settles: no applied entry lost or changed, every acknowledged command in the
// recovered log, reads open, and the digest a replay's.
void verify(group_type& g, blob::app& app, const std::map<std::uint64_t, blob::app::seen_entry>& log, const std::vector<acknowledged>& acks) {
	check(!app.diverged, "an index applied twice with different entries");
	check(!g.fenced(), "the recovered group is not fenced: " + g.fence_reason());
	const auto through = g.applied();
	check(log.empty() || log.rbegin()->first <= through, "an applied entry was lost: applied " + std::to_string(through) + ", seen " + std::to_string(log.empty() ? 0 : log.rbegin()->first));
	for (std::uint64_t i = 1; i <= through; ++i) {
		if (!check(log.count(i), "the log has a hole at " + std::to_string(i))) { return; }
	}
	for (const auto& a : acks) {
		auto it = log.find(a.placement.index);
		check(a.placement.index <= through && it != log.end() && it->second.term == a.placement.term && it->second.command == a.command, "acknowledged " + a.command + " at " + std::to_string(a.placement.index) + " is in the recovered log");
	}
	check(g.readable(), "reads open once applied reaches P_H");
	check(g.applied() >= g.durable_head().index, "applied reaches P_H");
	const auto mine = of(g, app), theirs = replay(log, through);
	if (!check(mine == theirs, "the digest equals a replay from empty through " + std::to_string(through))) {
		for (const auto& [k, v] : theirs.documents) {
			auto it = mine.documents.find(k);
			if (it == mine.documents.end() || it->second != v) { std::cerr << "  document " << k << ": replay v" << v.first << " '" << v.second << "', group " << (it == mine.documents.end() ? std::string("none") : "v" + std::to_string(it->second.first) + " '" + it->second.second + "'") << '\n'; }
		}
		for (const auto& [k, v] : mine.documents) {
			if (!theirs.documents.count(k)) { std::cerr << "  document " << k << " only in the group\n"; }
		}
		for (const auto& [n, f] : theirs.files) {
			auto it = mine.files.find(n);
			if (it == mine.files.end() || it->second != f) { std::cerr << "  file " << n << " differs (" << f.second.size() << " bytes in the replay)\n"; }
		}
		for (const auto& [n, f] : mine.files) {
			if (!theirs.files.count(n)) { std::cerr << "  file " << n << " only in the group\n"; }
		}
		if (mine.state != theirs.state) { std::cerr << "  state: group '" << mine.state << "', replay '" << theirs.state << "'\n"; }
	}
}

struct outcome {
	std::uint64_t events = 0, recovery_events = 0;
	bool crashed = false;
};

// One trial: the script crashed at `at` (none: run whole, then cut power at its end), then a
// recovery crashed at `again` (none: not crashed), then a recovery checked by the oracle.
outcome trial(std::uint64_t at, std::uint64_t again, std::uint64_t seed) {
	disks d;
	std::map<std::uint64_t, blob::app::seen_entry> log;
	blob::app app;
	app.seen = &log;
	std::vector<acknowledged> acks;
	clock c;
	c.crash_at = at;
	std::unique_ptr<process> p = std::make_unique<process>(d, app, seed);
	run r{d, app, c, p, acks, seed, at == ~std::uint64_t(0)};
	outcome out;
	try {
		r.script();
		c.idle(*p);
	} catch (const crash_now&) {
		out.crashed = true;
	} catch (const stuck&) {
		check(false, "the script is stuck at event " + std::to_string(c.events));
		return out;
	}
	app.after_effects = nullptr;
	out.events = c.events;
	auto cut = [&](std::uint64_t s) {
		p->shut(true);
		d.host.crash(s);
		d.strata.crash(s * 3 + 1);
		d.files.crash(s * 5 + 2);
		d.files.clear_faults();
		d.files.set_capacity(~std::uint64_t(0));
		d.files.short_writes(0);
	};
	cut(seed);
	if (again != ~std::uint64_t(0)) {
		clock second;
		second.crash_at = again;
		p = std::make_unique<process>(d, app, seed + 7);
		try {
			auto s = start(*p, p->g->open());
			second.pump(*p, [&] { return s->done; });
			second.idle(*p);
		} catch (const crash_now&) {
		} catch (const stuck&) {
			check(false, "the first recovery is stuck");
		}
		cut(seed + 11);
	}
	clock last;
	p = std::make_unique<process>(d, app, seed + 13);
	auto s = start(*p, p->g->open());
	try {
		last.pump(*p, [&] { return s->done; });
		if (check(!s->error, "recovery: " + what(s->error))) {
			last.idle(*p);
			verify(*p->g, app, log, acks);
		}
	} catch (const stuck&) {
		check(false, "recovery is stuck");
	}
	out.recovery_events = last.events;
	p->shut(false);
	return out;
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t first = argc > 1 ? std::stoull(argv[1]) : 0, stride = argc > 2 ? std::stoull(argv[2]) : 1;
	context = "uncrashed";
	const auto whole = trial(~std::uint64_t(0), ~std::uint64_t(0), 1);
	std::uint64_t points = 0, seconds = 0;
	for (std::uint64_t at = first; at <= whole.events; at += stride) {
		context = "crash at " + std::to_string(at);
		const auto once = trial(at, ~std::uint64_t(0), 1000 + at);
		++points;
		// A second crash at every point of that recovery.
		for (std::uint64_t again = 0; again <= once.recovery_events; ++again) {
			context = "crash at " + std::to_string(at) + ", again at " + std::to_string(again);
			trial(at, again, 5000 + at * 131 + again);
			++seconds;
		}
	}
	std::printf("lone voter: %llu script events, %llu crash points, %llu second crashes: %s\n", (unsigned long long)whole.events, (unsigned long long)points,
		(unsigned long long)seconds, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
