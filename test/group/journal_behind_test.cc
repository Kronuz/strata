// A group whose recovered log ends before its index's durable head refuses at open (quest
// strata-journal-behind; docs/mechanisms/group-open.md). P_H is strata's durable head, E the
// recovered log's last index, read after recover.
//
// clean-stop: a group closed twice; the journal from the first close put back under the index of
// the second. The open refuses naming P_H and E, every index and volume file byte-equal; the
// journal the index stopped with, put back, opens and reads every write.
// crash: the same with the journal copied from a running group and the process killed after more
// writes and a checkpoint.
// missing: the journal replaced by an empty one: the group is created over an index with state
// and refuses as P_H > E.
// sound: a crash after a checkpoint and more acknowledged writes, journal and directory through
// faultfs's crash: the reopen has P_H < E, replays (P_H, E] and serves every acknowledged write.
// `group_journal_behind_test [clean-stop|crash|missing|sound]`.
#include "blob_app.hh"
#include "group_catalog_borrow.hh"
#include "host_io.hh"
#include "oracle.hh"

#include "faultlab/faultfs.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace sg = strata::group;
using group_test::group_type;

int failures = 0;
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
constexpr cs::GroupId gid = 9;

group_type::config config(std::optional<std::string> closed = std::nullopt) {
	group_type::config c;
	c.id = gid;
	c.options.configuration.local = 1;
	c.options.configuration.voters = {1};
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 8192;
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{20, 40, 100, 200, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'J', 'B', 'H'};
	c.closed = std::move(closed);
	return c;
}

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

// The journal's device (replaceable: a copy put back) and the index's directory.
struct disks {
	std::unique_ptr<fl::filesystem> host = std::make_unique<fl::filesystem>();
	fl::filesystem dir;
	strata::byte_budget storage{std::uint64_t(1) << 30};
};

// Every file of the index's directory, the index's and the file set's volumes, with its bytes.
std::map<std::string, std::string> files_of(const fl::filesystem& fs) {
	std::map<std::string, std::string> out;
	for (const auto& name : fs.list()) {
		try {
			out[name] = std::string(fs.view(name));
		} catch (...) {
			out[name] = "<directory>";
		}
	}
	return out;
}

// One process: a journal host and a loop on inline schedulers, the group borrowed over the
// index's directory.
struct process {
	disks& d;
	blob::app& app;
	task::inline_scheduler loop_s, chore_s;
	j::InlineCompletion io;
	std::unique_ptr<j::Host> host;
	std::unique_ptr<sg::loop> lp;
	group_test::borrowed<group_type> g;
	task::cancel_source stop;
	bool turns_done = false, driver_done = false;

	process(disks& disk, blob::app& a) : d(disk), app(a) {
		host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(*d.host), j::Identity{'J', 'B', 'P'}, j::HostOptions{1, turn_options});
		lp = std::make_unique<sg::loop>(ref(), j::completion_ref::of(io), *host, 0, 13);
		task::spawn(ref(), host->driver(0).run(j::completion_ref::of(io), stop.token()), [this](std::exception_ptr) { turns_done = true; });
		task::spawn(ref(), lp->run(stop.token()), [this](std::exception_ptr) { driver_done = true; });
		loop_s.run();
	}
	task::scheduler_ref ref() { return task::scheduler_ref::of(loop_s); }
	void pump(const std::function<bool()>& done, std::uint64_t limit = 100000) {
		for (std::uint64_t i = 0; i < limit && !done(); ++i) {
			loop_s.run();
			if (!chore_s.run_until([] { return false; })) { loop_s.advance(5'000'000); }
		}
		loop_s.run();
	}
	template <class T>
	void settle(const std::shared_ptr<started<T>>& call) {
		pump([&] { return call->done.load(); });
	}
	std::exception_ptr open(std::optional<std::string> closed = std::nullopt) {
		g.emplace(*lp, config(std::move(closed)), std::make_unique<fl::io>(d.dir), d.storage, {'j', 'b', 'h'}, app, task::scheduler_ref::of(chore_s));
		auto o = start(ref(), g->open());
		settle(o);
		return o->error;
	}
	void write(const std::string& command) {
		auto w = start(ref(), g->propose(command));
		settle(w);
		check(!w->error && w->value, command + " is acknowledged: " + what(w->error));
	}
	cs::Index checkpoint() {
		auto k = start(ref(), g->checkpoint());
		settle(k);
		check(!k->error && k->value, "the checkpoint: " + what(k->error));
		return k->value ? k->value->through : 0;
	}
	std::string close() {
		auto c = start(ref(), g->close());
		settle(c);
		check(!c->error, "close: " + what(c->error));
		auto record = g->closed_record();
		check(record.has_value(), "a lone voter at rest closes with its closed state");
		return record.value_or("");
	}
	cs::Index last() {
		const auto s = lp->driver().state(gid);
		return s ? s->last : 0;
	}
	// The documents the group serves: key to blob bytes.
	std::map<std::string, std::string> documents() {
		auto r = start(ref(), g->read_applied([this](blob::index_type& ix) {
			group_test::digest out;
			group_test::documents(ix, [this](const std::string& n, std::uint64_t o, std::size_t l) { return g->files().read(n, o, l); }, out);
			std::map<std::string, std::string> of;
			for (const auto& [key, v] : out.documents) { of[key] = v.second; }
			return of;
		}));
		settle(r);
		check(!r->error && r->value, "a read: " + what(r->error));
		return r->value.value_or(std::map<std::string, std::string>{});
	}
	// The process stops (no close): nothing more is written, synced or not.
	void shut() {
		stop.request(task::cancel_reason::shutdown);
		pump([&] { return turns_done && driver_done; });
		if (g) { g->fenced("the process stopped"); }
		pump([] { return false; }, 1000);
		g.reset();
		lp.reset();
		host.reset();
	}
};

// An open over a journal older than the index refuses naming both positions, leaves every file
// of the index's directory byte-equal and the group out of the loop's table with its row closed.
void refused(process& p, std::exception_ptr error, cs::Index ph, cs::Index e, const std::map<std::string, std::string>& before) {
	const auto text = what(error);
	check(error != nullptr, "the open refuses");
	check(text.find("P_H = " + std::to_string(ph) + " ") != std::string::npos && text.find("E = " + std::to_string(e) + ":") != std::string::npos,
		"the refusal names P_H = " + std::to_string(ph) + " and E = " + std::to_string(e) + ": " + text);
	check(text.find("the journal is older than the index") != std::string::npos, "the refusal says the journal is older: " + text);
	check(p.g->fenced(), "the refused group is fenced");
	check(!p.lp->find(gid), "the refused open left the loop's table");
	check(p.lp->driver().groups() == 0, "the refused open closed its row");
	std::size_t volumes = 0;
	for (const auto& [name, bytes] : before) { volumes += name.starts_with("app.vol-"); }
	check(volumes > 0 && volumes < before.size(), "the directory holds the index's files and the file set's volumes: " + std::to_string(volumes) + " volumes");
	const auto after = files_of(p.d.dir);
	check(after == before, "every index and volume file is byte-equal: " + std::to_string(before.size()) + " files before, " + std::to_string(after.size()) + " after");
	if (error && after == before) { std::printf("%s: refused, %zu files byte-equal: %s\n", context.c_str(), after.size(), text.c_str()); }
}

const std::map<std::string, std::string> all_writes = {{"a", "aa"}, {"b", "bb"}, {"c", "cc"}, {"d", "dd"}, {"e", "ee"}};
void write_all(process& p, std::initializer_list<const char*> keys) {
	for (const auto* k : keys) { p.write(std::string("P|") + k + "|" + all_writes.at(k)); }
}

// The journal the index stopped with, put back, opens and serves `writes`.
void opens_with(disks& d, blob::app& app, std::unique_ptr<fl::filesystem> journal, const std::optional<std::string>& record, std::size_t writes) {
	d.host = std::move(journal);
	auto p = std::make_unique<process>(d, app);
	const auto error = p->open(record);
	check(!error, "the journal the index stopped with opens: " + what(error));
	if (!error) { check(p->documents().size() == writes, "it serves every write"); }
	p->shut();
}

void clean_stop() {
	context = "clean-stop";
	disks d;
	blob::app app;
	auto p = std::make_unique<process>(d, app);
	check(!p->open(), "the first open");
	write_all(*p, {"a", "b"});
	const auto e = p->last();
	const auto first = p->close();
	p->shut();
	auto older = d.host->snapshot();  // the journal at the first clean stop, with its record

	p = std::make_unique<process>(d, app);
	check(!p->open(first), "the reopen");
	write_all(*p, {"c", "d", "e"});
	const auto ph = p->last();  // the close checkpoints, and strata commits, through it
	const auto record = p->close();
	p->shut();
	check(ph > e, "the second stop's head is past the first's log end");
	auto newest = std::make_unique<fl::filesystem>(d.host->snapshot());

	d.host = std::make_unique<fl::filesystem>(std::move(older));
	const auto before = files_of(d.dir);
	p = std::make_unique<process>(d, app);
	refused(*p, p->open(first), ph, e, before);
	p->shut();
	opens_with(d, app, std::move(newest), record, 5);
}

void crash() {
	context = "crash";
	disks d;
	blob::app app;
	auto p = std::make_unique<process>(d, app);
	check(!p->open(), "the first open");
	write_all(*p, {"a"});
	p->checkpoint();
	write_all(*p, {"b"});
	const auto e = p->last();
	auto older = d.host->snapshot();  // a copy of the running group's journal
	write_all(*p, {"c", "d"});
	const auto ph = p->checkpoint();
	write_all(*p, {"e"});
	p->shut();
	check(ph > e, "the checkpoint's head is past the copy's log end");
	auto newest = std::make_unique<fl::filesystem>(d.host->snapshot());

	d.host = std::make_unique<fl::filesystem>(std::move(older));
	const auto before = files_of(d.dir);
	p = std::make_unique<process>(d, app);
	check(p->host->status(gid) == j::GroupStatus::stored, "the copy holds the group stored, its log not closed");
	refused(*p, p->open(), ph, e, before);
	p->shut();
	opens_with(d, app, std::move(newest), std::nullopt, 5);
}

void missing() {
	context = "missing";
	disks d;
	blob::app app;
	auto p = std::make_unique<process>(d, app);
	check(!p->open(), "the first open");
	write_all(*p, {"a", "b", "c"});
	const auto ph = p->checkpoint();
	p->shut();
	auto newest = std::make_unique<fl::filesystem>(d.host->snapshot());

	d.host = std::make_unique<fl::filesystem>();  // the journal lost: an empty device
	const auto before = files_of(d.dir);
	p = std::make_unique<process>(d, app);
	check(p->host->status(gid) == j::GroupStatus::none, "the empty journal holds no group");
	const auto error = p->open();
	// The create's log end: the refusal names it, below P_H.
	const auto text = what(error);
	const auto at = text.find("E = ");
	const cs::Index e = at == std::string::npos ? 0 : std::stoull(text.substr(at + 4));
	check(e < ph, "the created log ends below P_H: E = " + std::to_string(e));
	refused(*p, error, ph, e, before);
	p->shut();
	opens_with(d, app, std::move(newest), std::nullopt, 3);
}

void sound(std::uint64_t seed) {
	context = "sound seed " + std::to_string(seed);
	disks d;
	blob::app app;
	auto p = std::make_unique<process>(d, app);
	check(!p->open(), "the first open");
	write_all(*p, {"a", "b"});
	const auto pb = p->checkpoint();
	write_all(*p, {"c", "d", "e"});  // acknowledged at quorum: in the durable log
	const auto acknowledged = p->last();
	p->shut();
	d.host->crash(seed);
	d.dir.crash(seed);

	p = std::make_unique<process>(d, app);
	check(!p->open(), "the reopen after the crash");
	const auto ph = p->g->durable_head().index;
	const auto e = p->last();
	check(pb <= ph && ph < e && e >= acknowledged, "P_b <= P_H < E: P_b " + std::to_string(pb) + ", P_H " + std::to_string(ph) + ", E " + std::to_string(e));
	p->pump([&] { return p->g->applied() >= e; });
	check(p->g->applied() >= e, "the replay applies (P_H, E]: applied " + std::to_string(p->g->applied()));
	const auto served = p->documents();
	check(served == all_writes, "every acknowledged write is served: " + std::to_string(served.size()) + " documents");
	p->shut();
	std::printf("%s: P_b %llu, P_H %llu, E %llu, %zu documents served\n", context.c_str(), (unsigned long long)pb, (unsigned long long)ph, (unsigned long long)e, served.size());
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	if (only.empty() || only == "clean-stop") { clean_stop(); }
	if (only.empty() || only == "crash") { crash(); }
	if (only.empty() || only == "missing") { missing(); }
	if (only.empty() || only == "sound") {
		for (std::uint64_t seed = 1; seed <= 8; ++seed) { sound(seed); }
	}
	std::printf("journal-behind: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
