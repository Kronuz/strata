// strata::group's apply off the loop and its calls against apply (quest group-core A5, A10).
//
// held: two lone voters on one loop, each with its own chore, on threads (the loop and each
// chore a blocking_scheduler on its own thread). Group A's apply blocks inside an entry; group B
// on the same loop still proposes, commits, applies, answers `quorum`, reads and checkpoints, and
// A answers once released. Then B takes a burst of writes while read_applied, checkpoints and
// loop-side observers run against its apply, so TSan sees every crossing of loop and chore.
//
// points: one lone voter on inline schedulers, its journal IO held so each completion is an event.
// Writes start at once; at every event k of that run, one call starts: a checkpoint (capture
// against apply), a close (against Committed ranges still arriving), or read_applied (against
// apply). Oracle: the capture's bundle equals a replay through its P_b; a close ends with its
// last bundle at applied, which a restarted process recovers from alone, every acknowledged entry
// with it; a read sees exactly a replay through the applied position it ran at, at or past the
// commit index when it started. `group_lifetime_test [held|points]`.
#include "blob_app.hh"
#include "host_io.hh"
#include "oracle.hh"

#include "strata/core/faultfs.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace ff = strata::faultfs;
namespace sg = strata::group;
using group_test::group_type;
using group_test::replay;

int failures = 0;  // the main thread's
std::string context;
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 30) { std::cerr << "FAIL [" << context << "]: " << what << '\n'; }
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

group_type::config config(cs::GroupId id, strata::byte_budget& storage) {
	group_type::config c;
	c.id = id;
	c.options.configuration.local = 1;
	c.options.configuration.voters = {1};
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 16384;
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{20, 40, 100, 200, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'L', 'T', char('0' + id)};
	c.uuid = {'l', 'i', 'f', 'e', char('0' + id)};
	c.strata.keyed = true;
	c.strata.storage = &storage;
	return c;
}

// A call's result, written where it finishes, read once `done` is.
template <class T>
struct started {
	std::atomic<bool> done{false};
	std::exception_ptr error;
	std::optional<T> value;
};
template <class T>
std::shared_ptr<started<T>> start(task::scheduler_ref on, task::task<T> work) {
	auto s = std::make_shared<started<T>>();
	task::spawn(on, std::move(work), [s](std::exception_ptr e, std::optional<T> v) {
		s->error = e;
		s->value = std::move(v);
		s->done.store(true, std::memory_order_release);
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

// --- held: threads ------------------------------------------------------------------------

// A blocking_scheduler driven on its own thread until ended.
struct driven {
	task::blocking_scheduler s;
	std::atomic<bool> quit{false};
	std::thread t;
	void begin() {
		t = std::thread([this] { s.drive([this] { return quit.load(); }, [] { return true; }); });
	}
	void wake() { s.post_call([](void*) {}, nullptr); }
	void end() {
		quit.store(true);
		wake();
		if (t.joinable()) { t.join(); }
	}
	task::scheduler_ref ref() { return task::scheduler_ref::of(s); }
};

bool until(const std::function<bool()>& done, double seconds = 10) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (!done()) {
		if (std::chrono::steady_clock::now() > end) { return false; }
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	return true;
}
template <class T>
bool all_done(const std::vector<std::shared_ptr<started<T>>>& xs) {
	for (const auto& x : xs) {
		if (!x->done.load(std::memory_order_acquire)) { return false; }
	}
	return true;
}

void held() {
	context = "held";
	ff::filesystem host_disk, dir_a, dir_b;
	strata::byte_budget budget_a{std::uint64_t(1) << 30}, budget_b{std::uint64_t(1) << 30};
	blob::app app_a, app_b;
	// A's apply blocks inside its "P|held|x" entry, between file effects and strata, until released.
	std::atomic<bool> a_inside{false}, a_release{false};
	app_a.after_effects = [&](const sg::entry& e) {
		if (e.command != "P|held|x") { return; }
		a_inside.store(true);
		while (!a_release.load()) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
	};

	driven loop_t, chore_a, chore_b;
	j::InlineCompletion inline_io;
	auto host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(host_disk), j::Identity{'H', 'L', 'D'}, j::HostOptions{1, turn_options});
	auto lp = std::make_unique<sg::loop>(loop_t.ref(), j::completion_ref::of(inline_io), *host, 0, 3);
	task::cancel_source stop;
	std::atomic<bool> turns_done{false}, driver_done{false};
	task::spawn(loop_t.ref(), host->driver(0).run(j::completion_ref::of(inline_io), stop.token()), [&](std::exception_ptr) { turns_done.store(true); });
	task::spawn(loop_t.ref(), lp->run(stop.token()), [&](std::exception_ptr) { driver_done.store(true); });
	auto a = std::make_unique<group_type>(*lp, config(1, budget_a), std::make_unique<ff::io>(dir_a), app_a, chore_a.ref());
	auto b = std::make_unique<group_type>(*lp, config(2, budget_b), std::make_unique<ff::io>(dir_b), app_b, chore_b.ref());
	loop_t.begin();
	chore_a.begin();
	chore_b.begin();
	const auto L = loop_t.ref();

	auto oa = start(L, a->open());
	auto ob = start(L, b->open());
	check(until([&] { return oa->done && ob->done; }) && !oa->error && !ob->error, "both groups open: " + what(oa->error) + ", " + what(ob->error));

	// A's chore held inside an entry.
	auto wa = start(L, a->propose("P|held|x"));
	check(until([&] { return a_inside.load(); }), "A's apply reaches the held entry");

	// B, on the same loop: writes answered with apply's verdict, a read and a checkpoint.
	std::size_t answered = 0;
	for (int i = 0; i < 5; ++i) {
		auto w = start(L, b->propose("P|k|v" + std::to_string(i)));
		if (!check(until([&] { return w->done.load(); }), "B's write " + std::to_string(i) + " answers while A's apply is held")) { break; }
		answered += !w->error && w->value && w->value->applied && w->value->applied->result == strata::verdict::accepted;
	}
	check(answered == 5, "B's five writes accepted while A's apply is held: " + std::to_string(answered));
	auto r = start(L, b->read_applied([](blob::index_type& ix) {
		auto f = ix.find_key("k");
		return f ? f->second : std::uint64_t(0);
	}));
	check(until([&] { return r->done.load(); }) && !r->error && r->value == std::uint64_t(5), "B's read answers version 5 while A's apply is held");
	auto k = start(L, b->checkpoint());
	check(until([&] { return k->done.load(); }) && !k->error, "B checkpoints while A's apply is held: " + what(k->error));
	check(!wa->done.load() && a->applied() < 2, "A's write waits on its held apply");
	std::printf("held: B answered %zu writes, a read and a checkpoint while A's apply held at applied %llu\n", answered, (unsigned long long)a->applied());

	a_release.store(true);
	check(until([&] { return wa->done.load(); }) && !wa->error && wa->value && wa->value->applied && wa->value->applied->result == strata::verdict::accepted, "A answers once released: " + what(wa->error));

	// B's apply against reads, checkpoints and loop-side observers, across threads.
	std::vector<std::shared_ptr<started<sg::answer>>> burst;
	std::vector<std::shared_ptr<started<std::uint64_t>>> reads;
	std::vector<std::shared_ptr<started<cs::CheckpointPublished>>> checkpoints;
	for (int i = 0; i < 60; ++i) {
		burst.push_back(start(L, b->propose("P|" + std::string(1, char('a' + i % 5)) + "|w" + std::to_string(i), i % 3 ? sg::ack::none : sg::ack::quorum)));
		if (i % 7 == 0) {
			reads.push_back(start(L, b->read_applied([](blob::index_type& ix) {
				std::uint64_t sum = 0;
				for (const char* key : {"a", "b", "c", "d", "e"}) {
					if (auto f = ix.find_key(key)) { sum += f->second; }
				}
				return sum;
			})));
		}
		if (i % 20 == 10) { checkpoints.push_back(start(L, b->checkpoint())); }
		(void)b->applied();
		(void)b->readable();
		(void)b->stalled();
	}
	check(until([&] { return all_done(burst) && all_done(reads) && all_done(checkpoints); }, 30), "B's burst, reads and checkpoints finish");
	std::size_t errors = 0;
	std::uint64_t last = 0;
	bool ordered = true;
	const auto note = [&](const std::exception_ptr& e) {
		if (e) {
			++errors;
			std::cerr << "  burst call failed: " << what(e) << '\n';
		}
	};
	for (auto& x : burst) { note(x->error); }
	for (auto& x : reads) {
		note(x->error);
		if (x->value) {
			ordered = ordered && *x->value >= last;
			last = *x->value;
		}
	}
	for (auto& x : checkpoints) { note(x->error); }
	check(errors == 0, "no call in the burst fails: " + std::to_string(errors));
	check(ordered, "reads started in order see versions in order");
	auto ca = start(L, a->close());
	auto cb = start(L, b->close());
	check(until([&] { return ca->done && cb->done; }) && !ca->error && !cb->error, "both close: " + what(ca->error) + ", " + what(cb->error));
	std::printf("held: then a burst of %zu writes, %zu reads, %zu checkpoints against B's apply; B applied %llu\n", burst.size(), reads.size(), checkpoints.size(), (unsigned long long)b->applied());

	stop.request(task::cancel_reason::shutdown);
	loop_t.wake();
	check(until([&] { return turns_done && driver_done; }), "the loop stops");
	loop_t.end();
	chore_a.end();
	chore_b.end();
	a.reset();
	b.reset();
	lp.reset();
	host.reset();
}

// --- points: inline, enumerated -------------------------------------------------------------

constexpr cs::GroupId gid = 7;

struct disks {
	ff::filesystem host, dir;
	strata::byte_budget storage{std::uint64_t(1) << 30};
};

struct process {
	disks& d;
	task::inline_scheduler loop_s, chore_s;
	j::HeldCompletion held;
	j::InlineCompletion inline_io;
	std::unique_ptr<j::Host> host;
	std::unique_ptr<sg::loop> lp;
	std::unique_ptr<group_type> g;
	task::cancel_source stop;
	bool turns_done = false, driver_done = false;

	process(disks& disk, blob::app& app) : d(disk) {
		host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(d.host), j::Identity{'P', 'T', 'S'}, j::HostOptions{1, turn_options});
		lp = std::make_unique<sg::loop>(ref(), j::completion_ref::of(inline_io), *host, 0, 11);
		task::spawn(ref(), host->driver(0).run(j::completion_ref::of(held), stop.token()), [this](std::exception_ptr) { turns_done = true; });
		task::spawn(ref(), lp->run(stop.token()), [this](std::exception_ptr) { driver_done = true; });
		g = std::make_unique<group_type>(*lp, config(gid, d.storage), std::make_unique<ff::io>(d.dir), app, task::scheduler_ref::of(chore_s));
		loop_s.run();
	}
	task::scheduler_ref ref() { return task::scheduler_ref::of(loop_s); }
	// One event: a chore step, else one held IO completion (`io_first`: the other way round, so
	// commits run ahead of apply and ranges queue). False when idle.
	bool io_first = false;
	bool event() {
		loop_s.run();
		if (io_first && held.held()) {
			held.complete(held.tickets().front());
			return true;
		}
		std::size_t n = 0;
		if (chore_s.run_until([&] { return n++ == 1; })) { return true; }
		if (held.held()) {
			held.complete(held.tickets().front());
			return true;
		}
		return false;
	}
	// Events (and, when idle, clock ticks) until `done` or `limit`.
	void pump(const std::function<bool()>& done, std::uint64_t limit = 100000) {
		for (std::uint64_t i = 0; i < limit && !done(); ++i) {
			if (!event()) { loop_s.advance(5'000'000); }
		}
		loop_s.run();
	}
	void shut() {
		stop.request(task::cancel_reason::shutdown);
		pump([&] { return turns_done && driver_done; });
		g->fenced("the process stopped");
		pump([] { return false; }, 1000);
		g.reset();
		lp.reset();
		host.reset();
	}
};

// `both`: a checkpoint and a close started together.
enum class call { checkpoint, close, read, both };
const char* name(call c) { return c == call::checkpoint ? "checkpoint" : c == call::close ? "close" : c == call::read ? "read" : "checkpoint and close"; }

// Puts, an upload consumed into a volume, a delete and a volume rotation.
const std::vector<std::string> writes = {"P|b|bb", "P|c|cccc", "U|1|0|up", "P|b|b2", "F|1|d", "D|c", "P|e|eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "P|f|ff"};

struct versions {
	std::uint64_t applied = 0;
	std::map<std::string, std::uint64_t> of;
};

constexpr std::uint64_t never = ~std::uint64_t(0);

// Runs where the call met apply mid-flight: entries applied after it started.
std::map<std::string, std::uint64_t> crossings;

// One run: the writes start at event 0 and `c` at event `at` (never: the writes alone, the run's
// event count returned); `io_first` lets commits run ahead of apply.
std::uint64_t run(call c, std::uint64_t at, bool io_first) {
	disks d;
	std::map<std::uint64_t, blob::app::seen_entry> log;
	blob::app app;
	app.seen = &log;
	auto p = std::make_unique<process>(d, app);
	p->io_first = io_first;
	auto o = start(p->ref(), p->g->open());
	p->pump([&] { return o->done.load(); });
	if (!check(!o->error, "open: " + what(o->error))) { return 0; }
	auto first = start(p->ref(), p->g->propose("P|a|aa"));
	p->pump([&] { return first->done.load(); });

	std::vector<std::shared_ptr<started<sg::answer>>> ws;
	for (std::size_t i = 0; i < writes.size(); ++i) { ws.push_back(start(p->ref(), p->g->propose(writes[i], i % 2 ? sg::ack::none : sg::ack::quorum))); }
	std::shared_ptr<started<cs::CheckpointPublished>> k;
	std::shared_ptr<started<bool>> cl;
	std::shared_ptr<started<versions>> r;
	cs::Index committed_at_read = 0, applied_at_start = 0;
	const auto settled = [&] { return all_done(ws) && (!k || k->done) && (!cl || cl->done) && (!r || r->done); };
	std::uint64_t events = 0;
	for (; events < 100000; ++events) {
		if (events == at) {
			applied_at_start = p->g->applied();
			if (c == call::checkpoint || c == call::both) { k = start(p->ref(), p->g->checkpoint()); }
			if (c == call::close || c == call::both) { cl = start(p->ref(), p->g->close()); }
			if (c == call::read) {
				committed_at_read = p->lp->driver().state(gid)->committed;
				r = start(p->ref(), p->g->read_applied([g = p->g.get()](blob::index_type& ix) {
					versions out;
					out.applied = g->applied();
					for (const auto& key : group_test::keys) {
						if (auto f = ix.find_key(key)) { out.of[key] = f->second; }
					}
					return out;
				}));
			}
		}
		if (settled() && (at == never || events >= at)) { break; }
		if (!p->event()) { p->loop_s.advance(5'000'000); }
	}
	p->pump([] { return false; }, 2000);
	if (at == never) {
		p->shut();
		return events;
	}

	// Acknowledged: `quorum` answered with a verdict, `none` placed.
	std::vector<std::pair<cs::Placement, std::string>> acked;
	for (std::size_t i = 0; i < ws.size(); ++i) {
		if (!ws[i]->error && ws[i]->value) { acked.emplace_back(ws[i]->value->placement, writes[i]); }
	}
	cs::Index published = 0, closed_at = 0;
	if (k) {
		check(!k->error, "the checkpoint: " + what(k->error));
		if (k->value) { published = k->value->through; }
		crossings["checkpoint captured past its start"] += published > applied_at_start;
		crossings["checkpoint with entries applied after it"] += p->g->applied() > published;
	}
	if (cl) {
		check(!cl->error, "the close: " + what(cl->error));
		closed_at = p->g->applied();
		crossings["close with entries applied after it started"] += closed_at > applied_at_start;
	}
	if (r && check(!r->error && r->value, "the read: " + what(r->error))) {
		const auto& v = *r->value;
		crossings["read waiting on apply"] += v.applied > applied_at_start;
		check(v.applied >= committed_at_read, "the read ran at or past the commit index when it started: " + std::to_string(v.applied) + " < " + std::to_string(committed_at_read));
		std::map<std::string, std::uint64_t> want;
		for (const auto& [key, doc] : replay(log, v.applied).documents) { want[key] = doc.first; }
		check(v.of == want, "the read sees a replay through " + std::to_string(v.applied));
	}

	// A restarted process: what the last bundle holds, then what recovery reaches.
	p->shut();
	p = std::make_unique<process>(d, app);
	auto reopen = start(p->ref(), p->g->open());
	p->pump([&] { return reopen->done.load(); });
	if (!check(!reopen->error, "reopen: " + what(reopen->error))) {
		p->shut();
		return events;
	}
	const auto& b = p->g->recovered();
	if (k && published && !cl) {
		check(b.position == published, "the restart recovers the checkpoint's bundle: P_b " + std::to_string(b.position) + ", published " + std::to_string(published));
		const auto want = replay(log, b.position);
		check(app.decode(b.state) == app.decode(want.state), "the bundle's application state is a replay's through P_b " + std::to_string(b.position));
		bool files = b.files.size() == want.files.size();
		for (const auto& [n, f] : b.files) { files = files && want.files.count(n) && want.files.at(n).second.size() == f.length && want.files.at(n).first == f.crc; }
		check(files, "the bundle's file set is a replay's through P_b " + std::to_string(b.position));
	}
	if (cl) {
		check(b.position == closed_at, "the close's last bundle covers applied: P_b " + std::to_string(b.position) + ", applied at close " + std::to_string(closed_at));
		check(p->g->applied() == closed_at, "a reopen recovers from the bundle alone: applied " + std::to_string(p->g->applied()) + ", closed at " + std::to_string(closed_at));
	}
	p->pump([] { return false; }, 5000);
	for (const auto& [placement, command] : acked) {
		auto it = log.find(placement.index);
		check(placement.index <= p->g->applied() && it != log.end() && it->second.command == command, "acknowledged " + command + " at " + std::to_string(placement.index) + " is recovered");
	}
	p->shut();
	return events;
}

void points() {
	for (const bool io_first : {false, true}) {
		const std::string order = io_first ? ", commits first" : ", apply first";
		for (const auto c : {call::checkpoint, call::close, call::read, call::both}) {
			context = std::string(name(c)) + order + ", writes alone";
			const auto total = run(c, never, io_first);
			for (std::uint64_t at = 0; at <= total; ++at) {
				context = std::string(name(c)) + order + " at event " + std::to_string(at);
				run(c, at, io_first);
			}
			std::printf("points: %s%s started at each of %llu events\n", name(c), order.c_str(), (unsigned long long)total + 1);
		}
	}
	// Each crossing happens at some point, or the enumeration proves nothing.
	for (const auto& [what, n] : crossings) {
		std::printf("points: %s: %llu runs\n", what.c_str(), (unsigned long long)n);
		context = what;
		check(n > 0, "the crossing happens");
	}
	check(crossings.size() == 4, "every crossing counted");
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	if (only.empty() || only == "held") { held(); }
	if (only.empty() || only == "points") { points(); }
	std::printf("lifetime: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
