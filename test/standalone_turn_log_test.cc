// Standalone durability through the turn log (docs/design/standalone-turn-log.md), first steps:
// the command codec and its fixtures (T9), RFTW0001's checkpoint rule (T11), the position taken
// with the append (T12), V and flushes held at the barrier (T4), a process crash at every
// mutation (T2), and a served loop racing the writer and a reader (the TSan case).
// Usage: standalone_turn_log_test [codec|t11|t12|t4|t2|served]...; none runs every case.
#include "kv_test.hh"
#include "strata/core/command.hh"
#include "strata/core/turn_seal.hh"

#include "journal/turn_driver.h"

#include <atomic>
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
	explicit host(faultfs::io& io) : driver(io, host_identity(), geometry()), sealer(driver, geometry(), small_command) {}
};

kv_index::options logged_options(turn_sealer& s) {
	kv_index::options o;
	o.keyed = true;
	o.log = &s;
	o.log_group = group;
	o.command_bytes = small_command;
	return o;
}

// Opens the index and replays what the log recovered for it (D6, minus the checkpoint).
kv_index reopen(faultfs::io& io, host& h, const kv_index::options& o) {
	kv_index ix = kv_index::open(io, o);
	ix.replay(h.driver.records(group));
	h.driver.release(group);
	return ix;
}

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
	std::printf("codec: ok\n");
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
	std::printf("t11: ok\n");
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
	std::printf("t12: ok\n");
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
	std::printf("t4: ok\n");
}

// --- T2: a process crash at every mutation ---------------------------------------------------

const std::vector<std::string>& universe() {
	static const std::vector<std::string> keys = [] {
		std::vector<std::string> k;
		for (int i = 0; i < 11; ++i) { k.push_back("k" + std::to_string(i)); }
		return k;
	}();
	return keys;
}
transaction step(int i) {
	const std::string& key = universe()[std::size_t(i * 7) % universe().size()];
	if (i % 5 == 4) { return tx({erase(key)}); }
	if (i % 6 == 5) { return tx({replace(key, "v" + std::to_string(i)), replace(universe()[std::size_t(i) % universe().size()], "w" + std::to_string(i))}); }
	return tx({replace(key, "v" + std::to_string(i))});
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

// The workload from step `from`: turns every third transaction, a refresh every ninth and a
// commit every seventeenth, each of which waits for the log. `acked` counts the prefix whose
// records are durable.
void run(kv_index& ix, turn_sealer& s, int from, int n, std::vector<files::position>& positions, std::size_t& acked) {
	const auto ack = [&] {
		const std::uint64_t synced = ix.log_synced();
		while (acked < positions.size() && positions[acked].index <= synced) { ++acked; }
	};
	for (int i = from; i < from + n; ++i) {
		const verdict v = ix.apply(step(i), {});
		if (v.result != verdict::accepted) { throw std::runtime_error("not accepted"); }
		positions.push_back(ix.applied());
		if (i % 3 == 2) { s.turn(); }
		if (i % 9 == 8) { ix.refresh(); }
		if (i % 17 == 16) { ix.commit(); }
		ack();
	}
	s.turn();
	ack();
}

void t2() {
	constexpr int n = 48, more = 12;
	std::vector<state_t> prefixes(1);
	for (int i = 0; i < n; ++i) {
		prefixes.push_back(prefixes.back());
		apply_to(prefixes.back(), step(i));
	}
	const auto fresh = [&](faultfs::filesystem& fs, faultfs::io& io) {
		host h(io);
		(void)kv_index::create(io, logged_options(h.sealer), uuid_of("t2"));
		return fs.mutations();
	};
	std::uint64_t total = 0;
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		fresh(fs, io);
		host h(io);
		kv_index ix = kv_index::open(io, logged_options(h.sealer));
		const std::uint64_t start = fs.mutations();
		std::vector<files::position> positions;
		std::size_t acked = 0;
		run(ix, h.sealer, 0, n, positions, acked);
		CHECK(acked == std::size_t(n));
		total = fs.mutations() - start;
	}
	std::size_t cuts = 0, partial = 0;
	for (std::uint64_t k = 0; k <= total; ++k) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		fresh(fs, io);
		std::vector<files::position> positions;
		std::size_t acked = 0;
		try {
			host h(io);
			kv_index ix = kv_index::open(io, logged_options(h.sealer));
			fs.power_off_after(k);
			run(ix, h.sealer, 0, n, positions, acked);
		} catch (const std::exception&) {
		}
		fs.power_on();  // the process died at mutation k: every byte it wrote stays
		state_t expected;
		{
			host h(io);
			kv_index ix = reopen(io, h, logged_options(h.sealer));
			const state_t got = state_of(ix);
			// Every acknowledged transaction is in, and the state is one prefix of the apply order.
			std::size_t p = acked;
			while (p < prefixes.size() && prefixes[p] != got) { ++p; }
			if (p >= prefixes.size()) { std::fprintf(stderr, "t2: cut %llu: no prefix from %zu matches\n", (unsigned long long)k, acked); }
			CHECK(p < prefixes.size());
			if (p >= prefixes.size()) { continue; }
			partial += p != acked;
			// Writes after recovery, then a second reopen: they continue the log, never reuse it.
			expected = got;
			std::vector<files::position> again;
			std::size_t acked_again = 0;
			run(ix, h.sealer, 1000, more, again, acked_again);
			for (int i = 1000; i < 1000 + more; ++i) { apply_to(expected, step(i)); }
			CHECK(acked_again == std::size_t(more));
		}
		{
			host h(io);
			kv_index ix = reopen(io, h, logged_options(h.sealer));
			CHECK(state_of(ix) == expected);
		}
		++cuts;
	}
	std::printf("t2: ok, %zu cuts over %llu mutations, %zu kept unacknowledged writes\n", cuts, (unsigned long long)total, partial);
}

// --- served: the loop, the writer and a reader on their own threads (run under TSan) ---------

void served() {
	constexpr int n = 3000;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	host h(io);
	std::thread loop([&] { h.sealer.serve(); });
	{
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("served"));
		kv_index::reader_handle handle = ix.reader();
		std::atomic<bool> stop{false};
		std::atomic<std::uint64_t> reads{0};
		std::thread reader([&] {
			std::uint64_t last = 0;
			while (!stop.load()) {
				const std::uint64_t v = handle.visible();
				CHECK(v >= last);
				last = v;
				const auto lv = handle.live();
				const auto got = lv.at.get("n");
				// What a reader sees is never past V: transaction i sets n to i.
				if (got) { CHECK(std::stoull(*got) >= v && std::stoull(*got) <= handle.visible()); }
				if (!got) { CHECK(v == 0); }
				reads.fetch_add(1);
			}
		});
		for (int i = 1; i <= n; ++i) {
			CHECK(ix.apply(tx({replace("n", std::to_string(i))}), {}).result == verdict::accepted);
			if (i % 500 == 0) { ix.refresh(); }
		}
		ix.commit();  // waits for the log through the last transaction
		CHECK(ix.log_synced() == ix.applied().index && handle.visible() == std::uint64_t(n));
		stop = true;
		reader.join();
		CHECK(reads.load() > 0);
	}
	h.sealer.stop();
	loop.join();
	std::printf("served: ok\n");
}

}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, void (*)()>> cases = {{"codec", codec}, {"t11", t11}, {"t12", t12}, {"t4", t4}, {"t2", t2}, {"served", served}};
	for (const auto& [name, fn] : cases) {
		bool wanted = argc == 1;
		for (int i = 1; i < argc; ++i) { wanted = wanted || name == argv[i]; }
		if (wanted) { fn(); }
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
