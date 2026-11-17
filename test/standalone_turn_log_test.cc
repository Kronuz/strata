// Standalone durability through the turn log (docs/design/standalone-turn-log.md): the command
// codec and its fixtures (T9), RFTW0001's checkpoint rule (T11), the position taken with the
// append (T12), V and flushes held at the barrier (T4), a power-off sweep with checkpoints and
// recycled segments (T1), a process crash at every mutation (T2), a failed barrier (T3), log
// damage below the head (T5), no_space and owed verdicts at replay (T7, T7b), the command limit
// and the turn budget (T8), runtime refusals (T10), and a served loop with group commit, a
// checkpointing writer and a reader (the TSan case).
// Usage: standalone_turn_log_test [--seeds N] [codec|t11|t12|t4|t1|t1r|t2|t3|t5|t7|t7b|t8|t10|served]...;
// none runs every case. --seeds sets T1's power-loss seeds per crash point (default 8).
#include "kv_test.hh"
#include "strata/core/command.hh"
#include "strata/core/turn_seal.hh"

#include "journal/turn_driver.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {

namespace jr = kronuz::journal;
using strata::turn_sealer;

constexpr std::uint64_t group = 7;

// Whether f throws an E (and not some other exception).
template <class E, class F>
bool throws_as(F&& f) {
	try {
		f();
	} catch (const E&) {
		return true;
	} catch (...) {
		return false;
	}
	return false;
}
constexpr std::uint64_t small_command = 32 << 10;

jr::Identity host_identity() {
	jr::Identity id{};
	const char name[] = "turn-log-host";
	std::copy(name, name + sizeof name - 1, id.begin());
	return id;
}
jr::TurnLogOptions geometry() {
	jr::TurnLogOptions g;
	g.segment_bytes = 256 << 10;
	g.block_bytes = 4096;
	return g;
}

// A host's turn log in the index's own faultfs directory, so one cut orders both; the log's
// names are none of strata's.
struct host {
	jr::TurnDriver driver;
	turn_sealer sealer;
	explicit host(faultfs::io& io, const jr::TurnLogOptions& g = geometry(), std::uint64_t command = small_command)
		: driver(io, host_identity(), g), sealer(driver, g, command) {}
};

kv_index::options logged_options(turn_sealer& s, std::uint64_t command = small_command) {
	kv_index::options o;
	o.keyed = true;
	o.log = &s;
	o.log_group = group;
	o.command_bytes = command;
	return o;
}

// An open recovers the group: replay, then release (D6).
kv_index reopen(faultfs::io& io, host&, const kv_index::options& o) { return kv_index::open(io, o); }

std::string read_file(const std::filesystem::path& p) {
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

operation set_doc_by_docid(std::uint64_t docid, const std::string& body) {
	operation o;
	o.type = operation::replace_docid;
	o.docid = docid;
	o.doc = doc("", body);
	return o;
}

// --- T9: the codec --------------------------------------------------------------------------

void codec() {
	const transaction kv1 = tx({add("alpha", "one")});
	const transaction kv2 = tx({replace("alpha", "two", 1), erase("beta")});
	transaction all = tx({add("a", "x"), replace("a", "y"), erase("b"), set_doc_by_docid(5, "z")});
	operation del;
	del.type = operation::delete_docid;
	del.docid = 6;
	del.expected_version = 0;
	all.ops.push_back(del);
	all.ops.push_back(set_meta("m", "v"));
	operation unmeta;
	unmeta.type = operation::delete_meta;
	unmeta.meta_key = "m";
	all.ops.push_back(unmeta);
	all.expected_schema = 3;
	all.set_schema = 4;
	all.fields.push_back({"title", field_role::indexed, field_postings::positions, true});
	all.expected_fields = field_mark{2, 0xdeadbeef};

	const auto batch_of = [](const std::vector<transaction>& ts) {
		std::string body;
		for (const auto& t : ts) { command::add_transaction(body, command::encode(t)); }
		return command::batch_payload(ts.size(), body);
	};
	command::checkpoint_record cp;
	const char uuid[] = "index-uuid-0001";
	std::copy(uuid, uuid + 15, cp.uuid.begin());
	cp.generation = 9;
	cp.at = {12, 3};
	const std::vector<std::pair<std::string, std::string>> files = {
		{"batch-kv.bin", batch_of({kv1, kv2})}, {"batch-all.bin", batch_of({all})}, {"checkpoint.bin", command::encode_checkpoint(cp)}};
	const std::filesystem::path dir = std::filesystem::path(STRATA_FIXTURES) / "command";
	if (std::getenv("STRATA_WRITE_FIXTURES")) {
		std::filesystem::create_directories(dir);
		for (const auto& [name, bytes] : files) { std::ofstream(dir / name, std::ios::binary) << bytes; }
	}
	// The fixtures were written by an encoder of their own (the design's D3 table), not this one.
	for (const auto& [name, bytes] : files) {
		const std::string fixed = read_file(dir / name);
		if (fixed != bytes) { std::fprintf(stderr, "fixture %s differs from the encoder's bytes\n", name.c_str()); }
		CHECK(fixed == bytes);
	}
	// Each decodes back to what encoded it: the same bytes again, and the fields as given.
	for (const auto& [name, bytes] : files) {
		if (command::kind_of(bytes) == command::checkpoint) {
			CHECK(command::decode_checkpoint(bytes) == cp);
			continue;
		}
		std::string body;
		const auto ts = command::decode_batch(bytes);
		for (const auto t : ts) {
			command::add_transaction(body, command::encode(command::decode<document_layout>(t)));
		}
		CHECK(command::batch_payload(ts.size(), body) == bytes);
	}
	const transaction back = command::decode<document_layout>(command::decode_batch(files[1].second)[0]);
	CHECK(back.ops.size() == all.ops.size() && back.expected_schema == 3 && back.set_schema == 4 && back.fields == all.fields &&
		back.expected_fields == all.expected_fields);
	CHECK(back.ops[3].type == operation::replace_docid && back.ops[3].docid == 5 && back.ops[3].doc.stored == "z" && back.ops[3].doc.key.empty());
	CHECK(back.ops[4].type == operation::delete_docid && back.ops[4].docid == 6 && back.ops[4].expected_version == 0u);
	CHECK(back.ops[1].key == "a" && back.ops[1].doc.key == "a" && back.ops[1].doc.stored == "y" && !back.ops[1].expected_version);
	CHECK(back.ops[5].meta_key == "m" && back.ops[5].meta_value == "v" && back.ops[6].meta_key == "m");
	// Another kind or version is another build's log, never damage.
	for (const auto& [name, bytes] : files) {
		const std::pair<int, int> others[] = {{0, 1}, {3, 1}, {1, 0}, {1, 2}, {2, 2}};
		for (const auto& [k, v] : others) {
			std::string other = bytes;
			other[0] = char(k);
			other[1] = char(v);
			CHECK(throws_as<unsupported_index>([&] { (void)command::kind_of(other); }));
			CHECK(throws_as<unsupported_index>([&] { (void)command::decode_batch(other); }));
			CHECK(throws_as<unsupported_index>([&] { (void)command::decode_checkpoint(other); }));
		}
	}
	// Every strict prefix, a trailing byte, an overlong integer and an unknown op kind are damage.
	for (const auto& [name, bytes] : files) {
		const bool batch = command::kind_of(bytes) == command::batch;
		const auto decodes = [&](const std::string& b) {
			if (!batch) {
				(void)command::decode_checkpoint(b);
				return;
			}
			for (const auto t : command::decode_batch(b)) { (void)command::decode<document_layout>(t); }
		};
		for (std::size_t n = 0; n < bytes.size(); ++n) { CHECK(throws_as<corrupt_index>([&] { decodes(bytes.substr(0, n)); })); }
		CHECK(throws_as<corrupt_index>([&] { decodes(bytes + '\0'); }));
	}
	std::string overlong = files[0].second;
	overlong.replace(2, 1, "\x82\x00", 2);  // the count 2 in two bytes
	CHECK(throws_as<corrupt_index>([&] { (void)command::decode_batch(overlong); }));
	std::string one = command::encode(kv1);
	CHECK(command::encode(command::decode<document_layout>(one)) == one);
	one[1] = 7;  // an op kind past delete_meta
	CHECK(throws_as<corrupt_index>([&] { (void)command::decode<document_layout>(one); }));
	std::string masked = command::encode(kv1);
	masked.back() = char(16);  // a mask bit no member names
	CHECK(throws_as<corrupt_index>([&] { (void)command::decode<document_layout>(masked); }));
	std::string flagged = command::encode(kv1);
	flagged[2] = 2;  // an op flag past expected_version
	CHECK(throws_as<corrupt_index>([&] { (void)command::decode<document_layout>(flagged); }));
	CHECK(throws_as<corrupt_index>([&] { (void)command::decode_batch(command::batch_payload(0, "")); }));
	operation bad;
	bad.type = operation::kind(9);
	CHECK(throws_as<std::invalid_argument>([&] { (void)command::encode(tx({bad})); }));
}

// --- T11: RFTW0001's checkpoint rule, as D5 relies on it --------------------------------------

void t11() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	const auto record = [](std::uint64_t seq, jr::TurnRecordKind kind) {
		jr::TurnRecord r;
		r.group = 1;
		r.sequence = seq;
		r.kind = kind;
		r.payload = kind == jr::TurnRecordKind::Batch ? "batch" : "checkpoint";
		return r;
	};
	{
		jr::TurnRecovery recovered;
		jr::TurnLog log(io, host_identity(), recovered, geometry());
		const jr::TurnRecord x = record(1, jr::TurnRecordKind::Batch), acked = record(2, jr::TurnRecordKind::Batch);
		log.append(std::span(&x, 1));
		log.append(std::span(&acked, 1));
		for (std::uint64_t seq : {1, 2}) {
			const jr::TurnRecord early = record(seq, jr::TurnRecordKind::Checkpoint);
			CHECK(throws_as<std::invalid_argument>([&] { log.append(std::span(&early, 1)); }));
		}
		const jr::TurnRecord cp = record(3, jr::TurnRecordKind::Checkpoint);
		log.append(std::span(&cp, 1));
	}
	// The checkpoint at the last sequence plus one drops every earlier record of the group, the
	// acknowledged sequence 2 included: why D5 holds the lease until the record is staged.
	const jr::TurnRecovery r = jr::TurnLog::read(io, host_identity(), geometry());
	CHECK(r.groups.count(1) && r.groups.at(1).size() == 1 && r.groups.at(1)[0].sequence == 3 && r.groups.at(1)[0].kind == jr::TurnRecordKind::Checkpoint);
}

// --- T12: the position is taken with the append ----------------------------------------------

void t12() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	const transaction a = tx({replace("a", "1")}), b = tx({replace("b", "2")}), c = tx({replace("c", "3"), erase("a")}), d = tx({set_meta("m", "4")});
	files::position applied;
	contents_t before;
	std::map<std::string, std::string> meta;
	{
		host h(io);
		kv_index::options o = logged_options(h.sealer);
		int calls = 0;
		// The seam of c's apply seals the open record, which holds a and b: c then opens record 2.
		o.log_seam = [&] {
			if (++calls == 3) { CHECK(h.sealer.turn()); }
		};
		kv_index ix = kv_index::create(io, o, uuid_of("t12"));
		CHECK(ix.apply(a, {}).result == verdict::accepted && ix.applied() == (files::position{1, 1}));
		CHECK(ix.apply(b, {}).result == verdict::accepted && ix.applied() == (files::position{1, 2}));
		CHECK(ix.apply(c, {}).result == verdict::accepted && ix.applied() == (files::position{2, 1}));
		CHECK(ix.log_synced() == 1);
		CHECK(ix.apply(d, {}).result == verdict::accepted && ix.applied() == (files::position{2, 2}));
		CHECK(h.sealer.turn() && ix.log_synced() == 2 && !h.sealer.turn());
		applied = ix.applied();
		ix.commit();
		before = contents(ix);
		meta = ix.metadata();
		CHECK(meta.count("m") && meta.at("m") == "4");
	}
	// Each transaction sits in the record its position names, as its command.
	const jr::TurnRecovery r = jr::TurnLog::read(io, host_identity(), geometry());
	CHECK(r.groups.count(group) && r.groups.at(group).size() == 2);
	if (r.groups.count(group) && r.groups.at(group).size() == 2) {
		const auto& recs = r.groups.at(group);
		const auto one = command::decode_batch(recs[0].payload), two = command::decode_batch(recs[1].payload);
		CHECK(recs[0].sequence == 1 && one.size() == 2 && one[0] == command::encode(a) && one[1] == command::encode(b));
		CHECK(recs[1].sequence == 2 && two.size() == 2 && two[0] == command::encode(c) && two[1] == command::encode(d));
		// Replayed at those positions into an unlogged index, the log reaches the same state.
		faultfs::filesystem fs2;
		faultfs::io io2(fs2);
		kv_index::options plain;
		plain.keyed = true;
		kv_index fresh = kv_index::create(io2, plain, uuid_of("t12"));
		for (const auto& rec : recs) {
			const auto ts = command::decode_batch(rec.payload);
			for (std::size_t i = 0; i < ts.size(); ++i) { CHECK(fresh.apply(command::decode<document_layout>(ts[i]), {rec.sequence, i + 1}).result == verdict::accepted); }
		}
		fresh.commit();
		CHECK(fresh.applied() == applied && contents(fresh) == before && fresh.metadata() == meta);
	}
	// The index's own replay skips what its head covers, and continues the numbering after it.
	{
		host h(io);
		kv_index ix = reopen(io, h, logged_options(h.sealer));
		CHECK(ix.applied() == applied && contents(ix) == before);
		CHECK(ix.apply(tx({replace("d", "5")}), {}).result == verdict::accepted && ix.applied() == (files::position{3, 1}));
	}
}

// --- T4: a read and a refresh() between apply and its barrier --------------------------------

void t4() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	host h(io);
	h.sealer.served(true);  // writers wait for turns, which this thread writes
	std::atomic<int> stage{0};
	std::atomic<int> go{0};
	kv_index::reader_handle handle;
	std::thread owner([&] {
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t4"));
		CHECK(ix.apply(tx({replace("a", "0")}), {}).result == verdict::accepted);
		ix.refresh();  // waits for its barrier: "a" = "0" durable and published
		handle = ix.reader();
		stage = 1;
		while (go.load() < 1) { std::this_thread::yield(); }
		// New keys only: no pending delete, so only the flush's own wait holds the refresh (M-k).
		CHECK(ix.apply(tx({replace("b", "1"), replace("c", "2")}), {}).result == verdict::accepted);
		stage = 2;
		while (go.load() < 2) { std::this_thread::yield(); }
		ix.refresh();
		stage = 3;
		while (go.load() < 3) { std::this_thread::yield(); }
	});
	const auto until = [](auto ready, auto each) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!ready() && std::chrono::steady_clock::now() < deadline) {
			each();
			std::this_thread::yield();
		}
		return ready();
	};
	const auto nothing = [] {};
	CHECK(until([&] { return stage.load() == 1; }, [&] { h.sealer.turn(); }));
	const auto read = [&](const char* key) { return handle.live().at.get(key); };
	const std::uint64_t v = handle.visible();
	go = 1;
	CHECK(until([&] { return stage.load() == 2; }, nothing));
	CHECK(read("a") == std::optional<std::string>("0") && !read("b") && !read("c") && handle.visible() == v);
	go = 2;
	// The refresh waits for the log before it flushes, so nothing it could publish shows the write.
	CHECK(until([&] { return h.sealer.waiting() == 1 || stage.load() == 3; }, nothing));
	CHECK(stage.load() == 2);
	CHECK(read("a") == std::optional<std::string>("0") && !read("b") && !read("c") && handle.visible() == v);
	CHECK(h.sealer.turn());  // the barrier
	CHECK(until([&] { return stage.load() == 3; }, nothing));
	// Acknowledged: V covers it, and live reads and the refreshed segment both show it.
	CHECK(handle.visible() == v + 1 && read("b") == std::optional<std::string>("1") && read("c") == std::optional<std::string>("2"));
	CHECK(handle.acquire()->segments().size() == 2);
	go = 3;
	owner.join();
	handle = {};
}

// --- the crash workloads (T1, T2, T7b) ------------------------------------------------------

const std::vector<std::string>& universe() {
	static const std::vector<std::string> keys = [] {
		std::vector<std::string> k;
		for (int i = 0; i < 11; ++i) { k.push_back("k" + std::to_string(i)); }
		return k;
	}();
	return keys;
}
// Transaction i; `pad` bytes widen its values, so the log's segments fill and recycle.
transaction step(int i, std::size_t pad = 0) {
	const std::string& key = universe()[std::size_t(i * 7) % universe().size()];
	const std::string tail(pad, char('a' + i % 26));
	if (i % 5 == 4) { return tx({erase(key)}); }
	if (i % 6 == 5) { return tx({replace(key, "v" + std::to_string(i) + tail), replace(universe()[std::size_t(i) % universe().size()], "w" + std::to_string(i))}); }
	return tx({replace(key, "v" + std::to_string(i) + tail)});
}
using state_t = std::map<std::string, std::string>;
void apply_to(state_t& s, const transaction& t) {
	for (const auto& o : t.ops) {
		if (o.type == operation::delete_key) {
			s.erase(o.key);
		} else {
			s[o.key] = o.doc.stored;
		}
	}
}
state_t state_of(const kv_index& ix) {
	state_t s;
	for (const auto& k : universe()) {
		if (auto v = ix.get(k)) { s[k] = *v; }
	}
	return s;
}
std::vector<state_t> prefixes_of(int from, int n, std::size_t pad) {
	std::vector<state_t> out(1);
	for (int i = from; i < from + n; ++i) {
		out.push_back(out.back());
		apply_to(out.back(), step(i, pad));
	}
	return out;
}
// The index of the first prefix at or after `from` equal to `got`, or prefixes.size().
std::size_t prefix_of(const std::vector<state_t>& prefixes, std::size_t from, const state_t& got) {
	while (from < prefixes.size() && prefixes[from] != got) { ++from; }
	return from;
}

struct workload {
	int n = 48, more = 12;
	std::size_t pad = 0;
	int turn_every = 3, refresh_every = 9, commit_every = 17;
	int warm = 0;  // commits before the swept run, to bring the manifest log to its rollover
	jr::TurnLogOptions g = geometry();
	std::uint64_t command = small_command;
};

// The workload from step `from`: a turn, a refresh and a commit (a checkpoint) at their
// intervals, each of which waits for the log. `acked` counts the prefix whose records are durable.
void run(kv_index& ix, turn_sealer& s, const workload& w, int from, int n, std::vector<files::position>& positions, std::size_t& acked) {
	const auto ack = [&] {
		const std::uint64_t synced = ix.log_synced();
		while (acked < positions.size() && positions[acked].index <= synced) { ++acked; }
	};
	for (int i = from; i < from + n; ++i) {
		const verdict v = ix.apply(step(i, w.pad), {});
		if (v.result != verdict::accepted) { throw std::runtime_error("not accepted"); }
		positions.push_back(ix.applied());
		if (i % w.turn_every == w.turn_every - 1) { s.turn(); }
		if (i % w.refresh_every == w.refresh_every - 1) { ix.refresh(); }
		if (i % w.commit_every == w.commit_every - 1) { ix.commit(); }
		ack();
	}
	while (s.turn()) {}
	ack();
}

// A fresh index for the sweep, warmed: the mutations it took.
std::uint64_t fresh(faultfs::filesystem& fs, faultfs::io& io, const workload& w) {
	host h(io, w.g, w.command);
	kv_index ix = kv_index::create(io, logged_options(h.sealer, w.command), uuid_of("sweep"));
	for (int i = 0; i < w.warm; ++i) {
		CHECK(ix.apply(tx({set_meta("warm", std::to_string(i))}), {}).result == verdict::accepted);
		ix.commit();
	}
	while (h.sealer.turn()) {}
	return fs.mutations();
}

struct sweep_tally {
	std::size_t cuts = 0, partial = 0;
	std::uint64_t total = 0;
};

// One crash point: the run cut at mutation k, then a process crash (no seed) or a power loss
// (crash(seed)); recovery holds every acknowledged transaction and one prefix of the apply order.
// The index writes again, and a second crash (the same kind) and recovery keep all of that.
void crash_case(const workload& w, const std::vector<state_t>& prefixes, std::uint64_t k, std::optional<std::uint64_t> seed, sweep_tally& t) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	fresh(fs, io, w);
	std::vector<files::position> positions;
	std::size_t acked = 0;
	try {
		host h(io, w.g, w.command);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, w.command));
		fs.power_off_after(k);
		run(ix, h.sealer, w, 0, w.n, positions, acked);
	} catch (const std::exception&) {
	}
	const long long label = seed ? (long long)*seed : -1LL;
	const auto cut = [&](std::uint64_t salt) {
		if (seed) {
			fs.crash(*seed * 7919 + salt);
		} else {
			fs.power_on();  // the process died: every byte it wrote stays
		}
	};
	cut(0);
	state_t expected;
	try {
		host h(io, w.g, w.command);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, w.command));
		const state_t got = state_of(ix);
		const std::size_t p = prefix_of(prefixes, acked, got);
		if (p >= prefixes.size()) { std::fprintf(stderr, "cut %llu seed %lld: no prefix from %zu matches\n", (unsigned long long)k, label, acked); }
		CHECK(p < prefixes.size());
		if (p >= prefixes.size()) { return; }
		t.partial += p != acked;
		expected = got;
		std::vector<files::position> again;
		std::size_t acked_again = 0;
		run(ix, h.sealer, w, 1000, w.more, again, acked_again);
		for (int i = 1000; i < 1000 + w.more; ++i) { apply_to(expected, step(i, w.pad)); }
		CHECK(acked_again == std::size_t(w.more));
	} catch (const std::exception& e) {
		std::fprintf(stderr, "cut %llu seed %lld: recovery threw: %s\n", (unsigned long long)k, label, e.what());
		CHECK(!"recovery threw");
		return;
	}
	cut(1);
	try {
		host h(io, w.g, w.command);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, w.command));
		CHECK(state_of(ix) == expected);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "cut %llu seed %lld: second recovery threw: %s\n", (unsigned long long)k, label, e.what());
		CHECK(!"second recovery threw");
		return;
	}
	++t.cuts;
}

// Every mutation of the run (after the warm-up) as a crash point, under `seeds` power-loss seeds
// each, or as a process crash when `seeds` is 0.
sweep_tally sweep(const workload& w, int seeds) {
	const std::vector<state_t> prefixes = prefixes_of(0, w.n, w.pad);
	sweep_tally t;
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		fresh(fs, io, w);
		host h(io, w.g, w.command);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, w.command));
		const std::uint64_t start = fs.mutations();
		std::vector<files::position> positions;
		std::size_t acked = 0;
		run(ix, h.sealer, w, 0, w.n, positions, acked);
		CHECK(acked == std::size_t(w.n));
		t.total = fs.mutations() - start;
		const auto& stats = h.driver.log().stats();
		if (w.pad) { CHECK(stats.segments_recycled + stats.segments_removed > 0); }
		bool rolled = false;  // a manifest log past the create's
		for (auto c = io.scan_directory(); auto name = c->next();) { rolled = rolled || (name->starts_with("manifest-") && *name != "manifest-1"); }
		if (w.warm) { CHECK(rolled); }
	}
	for (std::uint64_t k = 0; k <= t.total; ++k) {
		if (!seeds) {
			crash_case(w, prefixes, k, std::nullopt, t);
			continue;
		}
		for (int seed = 0; seed < seeds; ++seed) { crash_case(w, prefixes, k, std::uint64_t(seed) + k * 131, t); }
	}
	return t;
}

int t1_seeds = 8;

// --- T1: a power-off sweep: turns, refreshes, checkpoints, recycled 64 KiB segments ----------

jr::TurnLogOptions small_segments() {
	jr::TurnLogOptions g;
	g.segment_bytes = 64 << 10;
	g.block_bytes = 4096;
	return g;
}

void t1() {
	workload w;
	w.n = 40;
	w.more = 6;
	w.pad = 1500;
	w.turn_every = 2;
	w.commit_every = 7;
	w.g = small_segments();
	w.command = 16 << 10;
	const sweep_tally t = sweep(w, t1_seeds);
	std::printf("t1: %zu cases over %llu mutations, %d seeds each, %zu kept unacknowledged writes\n", t.cuts, (unsigned long long)t.total, t1_seeds, t.partial);
}

// T1 across a manifest-log rollover: the warm-up leaves the log a few edits short of it, and the
// run's commits cross it.
void t1r() {
	workload w;
	w.n = 24;
	w.more = 4;
	w.turn_every = 2;
	w.commit_every = 3;
	w.warm = 124;
	const sweep_tally t = sweep(w, t1_seeds);
	std::printf("t1r: %zu cases over %llu mutations, %d seeds each, %zu kept unacknowledged writes\n", t.cuts, (unsigned long long)t.total, t1_seeds, t.partial);
}

// --- T2: a process crash at every mutation ---------------------------------------------------

void t2() {
	const sweep_tally t = sweep(workload{}, 0);
	std::printf("t2: %zu cuts over %llu mutations, %zu kept unacknowledged writes\n", t.cuts, (unsigned long long)t.total, t.partial);
}

// --- T3: a failed barrier --------------------------------------------------------------------

void t3() {
	std::size_t lost = 0, kept = 0, dropped = 0;
	for (const bool keep : {false, true}) {
		for (std::uint64_t seed = 0; seed < 8; ++seed) {
			faultfs::filesystem fs, fs2;
			faultfs::io io(fs), io2(fs2);
			state_t before, after;
			{
				host h(io);
				kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t3"));
				kv_index::options o2 = logged_options(h.sealer);
				o2.log_group = group + 1;
				kv_index mate = kv_index::create(io2, o2, uuid_of("t3-mate"));  // a loop-mate
				CHECK(ix.apply(tx({replace("k0", "a")}), {}).result == verdict::accepted);
				ix.commit();  // the log through it, the edit, then the checkpoint record
				while (h.sealer.turn()) {}
				before = state_of(ix);
				const transaction b = tx({replace("k1", "b")}), c = tx({replace("k2", "c"), erase("k0")});
				CHECK(ix.apply(b, {}).result == verdict::accepted);
				CHECK(ix.apply(c, {}).result == verdict::accepted);
				CHECK(mate.apply(tx({replace("m", "1")}), {}).result == verdict::accepted);
				after = before;
				apply_to(after, b);
				apply_to(after, c);
				const files::position pc = ix.applied();
				fs.add_fault({faultfs::op::sync, 1, EIO, false, keep});
				CHECK(throws([&] { h.sealer.turn(); }));  // the barrier fails: fail-stop
				fs.clear_faults();
				CHECK(h.sealer.failed());
				CHECK(throws_as<outcome_unknown>([&] { ix.await_durable(pc); }));
				CHECK(ix.logged_visible() == 1);  // no acknowledgement
				// Every index on the loop fences: the writer and its loop-mate.
				CHECK(throws_as<index_fenced>([&] { (void)ix.apply(tx({replace("k3", "d")}), {}); }));
				CHECK(throws_as<index_fenced>([&] { (void)mate.apply(tx({replace("m", "2")}), {}); }));
				CHECK(throws_as<index_fenced>([&] { ix.commit(); }));
			}
			// The reopen, then a power loss: the turn's writes all present or all absent, or log_lost.
			for (int round = 0; round < 2; ++round) {
				if (round == 1) { fs.crash(seed); }
				try {
					host h(io);
					kv_index ix = kv_index::open(io, logged_options(h.sealer));
					const state_t got = state_of(ix);
					CHECK(got == before || got == after);
					(got == after ? kept : dropped) += 1;
					if (got != before && got != after) { std::fprintf(stderr, "t3 keep %d seed %llu round %d: a partial turn\n", int(keep), (unsigned long long)seed, round); }
				} catch (const log_lost&) {
					++lost;
				}
			}
		}
	}
	std::printf("t3: %zu reopens kept the failed turn, %zu dropped it, %zu reported log_lost\n", kept, dropped, lost);
}

// --- T5: log damage below the head ------------------------------------------------------------

void t5() {
	// Each case on its own copy: two applies, each committed (its checkpoint record follows it).
	const auto build = [](faultfs::io& io) {
		host h(io);
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t5"));  // generation 1
		CHECK(ix.apply(tx({replace("k0", "a")}), {}).result == verdict::accepted);
		ix.commit();
		while (h.sealer.turn()) {}
		CHECK(ix.apply(tx({replace("k1", "b")}), {}).result == verdict::accepted);
		ix.commit();  // the head names record 3, after the checkpoint record at 2
		while (h.sealer.turn()) {}
	};
	// Another index on the same group: its checkpoint record names this one's UUID.
	{
		faultfs::filesystem fs, other;
		faultfs::io io(fs), oio(other);
		build(io);
		host h(io);
		CHECK(throws_as<log_mismatch>([&] { (void)kv_index::create(oio, logged_options(h.sealer), uuid_of("t5-other")); }));
	}
	// A fallback open at the first generation: its head is older than the log's checkpoint.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		build(io);
		host h(io);
		CHECK(throws_as<log_lost>([&] { (void)kv_index::open(io, logged_options(h.sealer), 1); }));
	}
	// The log's segments gone: the head names records the log lacks.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		build(io);
		std::vector<std::string> turns;
		for (auto c = io.scan_directory(); auto name = c->next();) {
			if (name->starts_with("turns-")) { turns.push_back(*name); }
		}
		CHECK(!turns.empty());
		for (const auto& name : turns) { io.remove(name); }
		io.sync_directory();
		host h(io);
		CHECK(throws_as<log_lost>([&] { (void)kv_index::open(io, logged_options(h.sealer)); }));
	}
}

// --- T7: no_space at replay ---------------------------------------------------------------------

void t7() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	constexpr int n = 12;
	const std::vector<state_t> prefixes = prefixes_of(0, n, 300);
	{
		host h(io);
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t7"));
		for (int i = 0; i < n; ++i) { CHECK(ix.apply(step(i, 300), {}).result == verdict::accepted); }
		while (h.sealer.turn()) {}
		CHECK(ix.logged_visible() == std::uint64_t(n));
	}
	// What the index's files hold, then a capacity just above it: replay's first flush has no room.
	std::uint64_t held = 0;
	{
		byte_budget probe(~std::uint64_t(0));
		kv_index::options o;
		o.keyed = true;
		o.storage = &probe;
		kv_index plain = kv_index::open(io, o);  // unlogged: the files alone
		held = probe.used();
	}
	{
		byte_budget tight(held + 64);
		host h(io);
		kv_index::options o = logged_options(h.sealer);
		o.storage = &tight;
		bool full = false;
		try {
			(void)kv_index::open(io, o);
		} catch (const std::system_error& e) {
			full = e.code() == std::errc::no_space_on_device;
		}
		CHECK(full);
	}
	{
		host h(io);
		kv_index ix = kv_index::open(io, logged_options(h.sealer));
		CHECK(state_of(ix) == prefixes[n]);
	}
}

// --- T7b: replay under defer_io into a smaller buffer, owed flushes, a power-off sweep -----------

void t7b() {
	constexpr int n = 30, more = 4;
	constexpr std::size_t pad = 200;
	const std::vector<state_t> prefixes = prefixes_of(0, n + more, pad);
	// The run: a large buffer, every transaction acknowledged, nothing checkpointed.
	const auto build = [&](faultfs::filesystem& fs, faultfs::io& io) {
		host h(io);
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t7b"));
		for (int i = 0; i < n; ++i) {
			CHECK(ix.apply(step(i, pad), {}).result == verdict::accepted);
			if (i % 4 == 3) { h.sealer.turn(); }
		}
		while (h.sealer.turn()) {}
		return fs.mutations();
	};
	// The replay meets flush_owed and refreshes; then more writes, answered as a host would.
	std::size_t owed = 0;
	const auto replay = [&](faultfs::io& io, std::size_t& acked) {
		host h(io);
		kv_index::options o = logged_options(h.sealer);
		o.defer_io = true;
		o.buffer_share = 2048;
		kv_index ix = kv_index::open(io, o);
		acked = n;
		std::vector<files::position> positions;
		for (int i = n; i < n + more; ++i) {
			verdict v = ix.apply(step(i, pad), {});
			for (int round = 0; v.result == verdict::flush_owed && round < 4; ++round) {
				++owed;
				ix.refresh();
				v = ix.apply(step(i, pad), {});
			}
			CHECK(v.result == verdict::accepted);
			positions.push_back(ix.applied());
		}
		while (h.sealer.turn()) {}
		for (const auto& p : positions) { acked += p.index <= ix.log_synced(); }
		ix.check_in();
	};
	std::uint64_t total = 0;
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		const std::uint64_t start = build(fs, io);
		std::size_t acked = 0;
		replay(io, acked);
		CHECK(acked == std::size_t(n + more));
		total = fs.mutations() - start;
	}
	std::size_t cases = 0;
	for (std::uint64_t k = 0; k <= total; ++k) {
		for (std::uint64_t seed = 0; seed < 2; ++seed) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			build(fs, io);
			std::size_t acked = n;
			fs.power_off_after(k);
			try {
				replay(io, acked);
			} catch (const std::exception&) {
			}
			fs.crash(seed + 17 * k);
			try {
				host h(io);
				kv_index ix = kv_index::open(io, logged_options(h.sealer));
				const std::size_t p = prefix_of(prefixes, acked, state_of(ix));
				if (p >= prefixes.size()) { std::fprintf(stderr, "t7b cut %llu seed %llu: no prefix from %zu\n", (unsigned long long)k, (unsigned long long)seed, acked); }
				CHECK(p < prefixes.size());
			} catch (const std::exception& e) {
				std::fprintf(stderr, "t7b cut %llu seed %llu: recovery threw: %s\n", (unsigned long long)k, (unsigned long long)seed, e.what());
				CHECK(!"t7b recovery threw");
			}
			++cases;
		}
	}
	CHECK(owed > 0);
	std::printf("t7b: %zu cases over %llu mutations, %zu owed flushes\n", cases, (unsigned long long)total, owed);
}

// --- T8: the command limit at admission, and the turn budget ---------------------------------

void t8() {
	// The smallest segment that holds two 32 KiB commands with their framing: 19 blocks.
	jr::TurnLogOptions g = geometry();
	g.segment_bytes = 19 * 4096;
	faultfs::filesystem fs, fs2;
	faultfs::io io(fs), io2(fs2);
	host h(io, g);
	kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t8"));
	kv_index::options o2 = logged_options(h.sealer);
	o2.log_group = group + 1;
	kv_index mate = kv_index::create(io2, o2, uuid_of("t8-mate"));
	// Past command_bytes: invalid, no trace.
	CHECK(ix.apply(tx({replace("big", std::string(small_command, 'x'))}), {}).result == verdict::invalid);
	CHECK(ix.applied() == files::position{} && !ix.get("big") && !h.sealer.turn());
	// Two loop-mates each near command_bytes: the turn budget splits them across two turns.
	const std::string near(small_command - 64, 'y');
	CHECK(ix.apply(tx({replace("a", near)}), {}).result == verdict::accepted);
	CHECK(mate.apply(tx({replace("b", near)}), {}).result == verdict::accepted);
	const auto barriers = [&] { return h.driver.log().stats().barriers; };
	const std::uint64_t b0 = barriers();
	CHECK(h.sealer.turn());
	CHECK(barriers() == b0 + 1 && (ix.log_synced() == 1) != (mate.log_synced() == 1));
	CHECK(h.sealer.turn());
	CHECK(barriers() == b0 + 2 && ix.log_synced() == 1 && mate.log_synced() == 1 && !h.sealer.turn());
}

// --- T10: runtime refusals leave no trace in the log ---------------------------------------------

void t10() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	constexpr int n = 6;
	state_t expected{{"k0", "a"}};
	{
		host h(io);
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("t10"));
		CHECK(ix.apply(tx({replace("k0", "a")}), {}).result == verdict::accepted);
		ix.commit();  // k0 in a segment
		while (h.sealer.turn()) {}
	}
	std::vector<std::string> names;
	for (auto c = io.scan_directory(); auto name = c->next();) { names.push_back(*name); }
	std::uint64_t held = 0;
	{
		byte_budget probe(~std::uint64_t(0));
		kv_index::options o;
		o.keyed = true;
		o.storage = &probe;
		kv_index plain = kv_index::open(io, o);  // unlogged: the files alone
		held = probe.used();
	}
	{
		byte_budget storage(held + (8 << 10));
		host h(io);
		kv_index::options o = logged_options(h.sealer);
		o.storage = &storage;
		o.eager_segments = false;  // k0's lookup reads its segment inside stage 1
		kv_index ix = kv_index::open(io, o);
		// A stage 1 whose segment read fails (k0's first lookup) throws, rolled back and never logged.
		for (const auto& name : names) {
			if (name.starts_with("seg-")) { fs.add_media_fault(name, faultfs::media::read_error, 0, ~std::uint64_t(0) >> 1); }
		}
		CHECK(throws([&] { (void)ix.apply(tx({replace("k0", "thrown", 1)}), {}); }));
		CHECK(fs.media_hits() > 0);
		fs.clear_faults();
		CHECK(ix.heal().empty());  // the file reads whole again
		// no_space and a version conflict: refused before the log.
		CHECK(ix.apply(tx({replace("huge", std::string(12000, 'h')), replace("huge2", std::string(12000, 'i'))}), {}).result == verdict::no_space);
		CHECK(ix.apply(tx({replace("k0", "x", 5)}), {}).result == verdict::version_conflict);
		const files::position before = ix.applied();
		for (int i = 0; i < n; ++i) {
			CHECK(ix.apply(step(i), {}).result == verdict::accepted);
			apply_to(expected, step(i));
		}
		CHECK(ix.applied().index > before.index && ix.applied().applied == std::uint64_t(n));  // one record, the refused ones absent
		while (h.sealer.turn()) {}
	}
	fs.crash(3);
	{
		host h(io);
		kv_index ix = kv_index::open(io, logged_options(h.sealer));
		CHECK(state_of(ix) == expected && !ix.get("huge") && !ix.get("huge2"));
	}
}

// --- served: the loop, group commit, a checkpointing writer and a reader (run under TSan) ----

void served() {
	constexpr int writers = 4, n = 600;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	host h(io);
	std::thread loop([&] { h.sealer.serve(); });
	{
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("served"));
		kv_index::reader_handle handle = ix.reader();
		std::mutex owner;  // the writers take turns as the index's owner; acknowledgements wait outside it
		std::atomic<bool> stop{false};
		std::atomic<std::uint64_t> reads{0};
		std::thread reader([&] {
			std::uint64_t last = 0;
			while (!stop.load()) {
				const std::uint64_t v = handle.visible();
				CHECK(v >= last);
				last = v;
				reads.fetch_add(1);
			}
		});
		std::vector<std::thread> ts;
		for (int w = 0; w < writers; ++w) {
			ts.emplace_back([&, w] {
				const std::string key = "w" + std::to_string(w);
				for (int i = 1; i <= n; ++i) {
					files::position at;
					{
						const std::lock_guard lock(owner);
						CHECK(ix.apply(tx({replace(key, std::to_string(i))}), {}).result == verdict::accepted);
						at = ix.applied();
						if (w == 0 && i % 150 == 0) { ix.commit(); }  // a checkpoint among the others' acknowledgements
						if (w == 1 && i % 200 == 0) { ix.refresh(); }
					}
					ix.await_durable(at);
					// Acknowledged: the writer reads its own write.
					const auto got = handle.live().at.get(key);
					CHECK(got && std::stoi(*got) >= i);
				}
			});
		}
		for (auto& t : ts) { t.join(); }
		{
			const std::lock_guard lock(owner);
			ix.commit();
			CHECK(ix.log_synced() >= ix.applied().index && handle.visible() == std::uint64_t(writers * n));
		}
		stop = true;
		reader.join();
		CHECK(reads.load() > 0);
		std::printf("served: %llu turns for %d transactions\n", (unsigned long long)h.driver.log().stats().turns, writers * n);
	}
	h.sealer.stop();
	loop.join();
}

}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, void (*)()>> cases = {{"codec", codec}, {"t11", t11}, {"t12", t12}, {"t4", t4}, {"t1", t1}, {"t1r", t1r},
		{"t2", t2}, {"t3", t3}, {"t5", t5}, {"t7", t7}, {"t7b", t7b}, {"t8", t8}, {"t10", t10}, {"served", served}};
	std::vector<std::string> wanted;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--seeds" && i + 1 < argc) {
			t1_seeds = std::atoi(argv[++i]);
		} else {
			wanted.emplace_back(argv[i]);
		}
	}
	for (const auto& [name, fn] : cases) {
		if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), name) == wanted.end()) { continue; }
		const int before = failures;
		fn();
		if (failures == before) { std::printf("%s: ok\n", name.c_str()); }
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
