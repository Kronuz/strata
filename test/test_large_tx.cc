// Large transactions on a logged index (docs/design/rocksdb-parity.md A3,
// docs/mechanisms/large-transactions.md): a transaction whose command passes command_bytes streams
// as chunk records and is all or nothing across a crash.
// admit: a logged index admits what an unlogged one does, past 64 MiB, and refuses what it refuses;
// sweep: a power cut (8 seeds) and a process crash at every mutation of a run with a multi-chunk
// transaction and a kv-shaped batch at 4 KiB chunks, then a close, a reopen, writes and a second
// crash;
// big: the same at strided cuts over a kv batch past 1 MiB and a transaction past 64 MiB at kv's
// 1 MiB chunks; visible: a served loop and a view reader, which never sees the transaction before
// its last chunk is durable (run under TSan).
// Usage: test_large_tx [--seeds N] [admit|sweep|big|visible]...; none runs every case.
#include "kv_test.hh"
#include "strata/core/turn_seal.hh"

#include "journal/turn_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace jr = kronuz::journal;
using strata::turn_sealer;

constexpr std::uint64_t group = 7;
int seeds = 8;

jr::Identity host_identity() {
	jr::Identity id{};
	const char name[] = "large-tx-host";
	std::copy(name, name + sizeof name - 1, id.begin());
	return id;
}

// The host's names under "host-" in the index's own faultfs directory, so one power cut orders
// both, as standalone_turn_log_test's host_view; its owner lock is a token.
class host_view final : public jr::IO {
public:
	static constexpr std::string_view prefix = "host-";
	explicit host_view(faultfs::io& io) : io_(io) {}
	jr::FilesystemClass directory_filesystem() const override { return io_.directory_filesystem(); }
	jr::DirectoryTrust directory_trust() const override { return io_.directory_trust(); }
	std::unique_ptr<jr::OwnerLock> acquire_owner(bool) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::OwnerLock> lock_owner(const jr::OwnerLockOptions&) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::File> open_existing(std::string_view n) override { return io_.open_existing(path(n)); }
	std::unique_ptr<jr::File> open_existing_read_only(std::string_view n) override { return io_.open_existing_read_only(path(n)); }
	std::unique_ptr<jr::File> open_reclaim_candidate_read_only(std::string_view n) override { return io_.open_reclaim_candidate_read_only(path(n)); }
	void open_existing_read_only_into(std::string_view n, jr::File& f) override { io_.open_existing_read_only_into(path(n), f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, jr::File& f) override { return io_.open_reclaim_candidate_read_only_into(path(n), f); }
	std::unique_ptr<jr::File> create_exclusive(std::string_view n) override { return io_.create_exclusive(path(n)); }
	void replace(std::string_view from, std::string_view to) override { io_.replace(path(from), path(to)); }
	void remove(std::string_view n) override { io_.remove(path(n)); }
	std::optional<jr::EntryFootprint> entry_footprint(std::string_view n) override { return io_.entry_footprint(path(n)); }
	void order_files_and_directory(std::span<jr::File* const> files) override { io_.order_files_and_directory(files); }
	void sync_directory() override { io_.sync_directory(); }
	std::unique_ptr<jr::DirectoryCursor> scan_directory() override {
		std::vector<std::string> names;
		for (auto c = io_.scan_directory(); auto n = c->next();) {
			if (n->starts_with(prefix)) { names.push_back(n->substr(prefix.size())); }
		}
		struct cursor final : jr::DirectoryCursor {
			std::vector<std::string> names;
			std::size_t at = 0;
			std::optional<std::string> next() override { return at < names.size() ? std::optional(names[at++]) : std::nullopt; }
		};
		auto c = std::make_unique<cursor>();
		c->names = std::move(names);
		return c;
	}

private:
	static std::string path(std::string_view n) { return std::string(prefix) + std::string(n); }
	faultfs::io& io_;
};

// A log geometry and the command size its segments are checked against.
struct shape {
	jr::TurnLogOptions g;
	std::uint64_t command = 0;
};
// 4 KiB chunks in 64 KiB segments, so the run recycles and forwards; kv's 1 MiB in 4 MiB.
shape small_chunks() {
	shape s;
	s.g.segment_bytes = 64 << 10;
	s.g.block_bytes = 4096;
	s.command = 4 << 10;
	return s;
}
shape kv_chunks() {
	shape s;
	s.g.segment_bytes = 4 << 20;
	s.command = 1 << 20;
	return s;
}

struct host {
	jr::Host table;
	turn_sealer sealer;
	host(faultfs::io& io, const shape& s)
		: table(std::make_shared<host_view>(io), host_identity(), jr::HostOptions{1, s.g}), sealer(table, 0, host_identity(), s.g, s.command) {}
};

kv_index::options logged_options(turn_sealer& s, std::uint64_t command) {
	kv_index::options o;
	o.keyed = true;
	o.log = &s;
	o.log_group = group;
	o.command_bytes = command;
	return o;
}

// A value of `n` bytes that names its key and its writer.
std::string value(const std::string& key, int writer, std::size_t n) {
	std::string v = key + "/" + std::to_string(writer) + "/";
	v.resize(std::max(n, v.size()), char('a' + writer % 26));
	return v;
}
// `docs` documents of `bytes` each under `stem`, with an erase of `gone`: one transaction.
transaction large(const std::string& stem, int writer, int docs, std::size_t bytes, const std::string& gone = {}) {
	std::vector<operation> ops;
	for (int i = 0; i < docs; ++i) {
		const std::string k = stem + std::to_string(i);
		ops.push_back(replace(k, value(k, writer, bytes)));
	}
	if (!gone.empty()) { ops.push_back(erase(gone)); }
	return tx(std::move(ops));
}
transaction small(const std::string& key, int writer) { return tx({replace(key, value(key, writer, 16))}); }

// What an index holds of the keys a run names, each value as its hash (the large ones are MiBs).
using state_t = std::map<std::string, std::uint64_t>;
std::uint64_t hash_of(std::string_view v) { return std::hash<std::string_view>{}(v) ^ v.size(); }
void apply_to(state_t& s, const transaction& t) {
	for (const auto& o : t.ops) {
		if (o.type == operation::delete_key) {
			s.erase(o.key);
		} else {
			s[o.key] = hash_of(o.doc.stored);
		}
	}
}
std::vector<std::string> keys_of(const std::vector<transaction>& ts) {
	std::vector<std::string> keys;
	for (const auto& t : ts) {
		for (const auto& o : t.ops) { keys.push_back(o.key); }
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return keys;
}
state_t state_of(const kv_index& ix, const std::vector<std::string>& keys) {
	state_t s;
	for (const auto& k : keys) {
		ix.get(k, [&](std::string_view v) { s[k] = hash_of(v); });
	}
	return s;
}

// A run: its transactions in apply order, and after which ones the index commits (a checkpoint).
struct run_plan {
	shape s;
	std::vector<transaction> txs;
	std::vector<bool> commit_after;
	std::vector<transaction> more;  // the second life's writes
	std::vector<std::string> keys;
	std::vector<state_t> prefixes;  // prefixes[i]: the state after the first i transactions
	void seal() {
		std::vector<transaction> all = txs;
		all.insert(all.end(), more.begin(), more.end());
		keys = keys_of(all);
		prefixes.assign(1, {});
		for (const auto& t : txs) {
			prefixes.push_back(prefixes.back());
			apply_to(prefixes.back(), t);
		}
		commit_after.resize(txs.size());
	}
};

// Applies the plan's transactions, a turn after each; `acked` counts those whose records are
// durable. A cut makes a call throw, which ends the run.
void run(kv_index& ix, turn_sealer& s, const run_plan& p, std::vector<files::position>& positions, std::size_t& acked) {
	const auto ack = [&] {
		const std::uint64_t synced = ix.log_synced();
		while (acked < positions.size() && positions[acked].index <= synced) { ++acked; }
	};
	for (std::size_t i = 0; i < p.txs.size(); ++i) {
		const verdict v = ix.apply(p.txs[i], {});
		if (v.result != verdict::accepted) { throw std::runtime_error("not accepted"); }
		positions.push_back(ix.applied());
		s.turn();
		if (p.commit_after[i]) { ix.commit(); }
		ack();
	}
	while (s.turn()) {}
	ack();
}

// A fresh index for the plan: created, its join's records durable.
void fresh(faultfs::io& io, const run_plan& p) {
	host h(io, p.s);
	kv_index ix = kv_index::create(io, logged_options(h.sealer, p.s.command), uuid_of("large-tx"));
	CHECK(ix.apply(small("warm", 0), {}).result == verdict::accepted);
	ix.commit();
	while (h.sealer.turn()) {}
}

struct tally {
	std::size_t cases = 0, partial = 0, interrupted = 0;
	std::uint64_t total = 0;
};

// The run cut at mutation k, then a process crash (no seed) or a power loss: recovery holds one
// prefix of whole transactions, at least the acknowledged ones. The index then closes before any
// write, so its leave checkpoints the recovered state over a dropped run's records, and reopens
// with the same state. It writes again, commits, and a second crash of the same kind keeps it all.
void crash_case(const run_plan& p, std::uint64_t k, std::optional<std::uint64_t> seed, tally& t) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	fresh(io, p);
	std::vector<files::position> positions;
	std::size_t acked = 0;
	try {
		host h(io, p.s);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, p.s.command));
		fs.power_off_after(k);
		run(ix, h.sealer, p, positions, acked);
	} catch (const std::exception&) {
	}
	t.interrupted += positions.size() < p.txs.size();
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
		host h(io, p.s);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, p.s.command));
		const state_t got = state_of(ix, p.keys);
		std::size_t at = acked;
		while (at < p.prefixes.size() && p.prefixes[at] != got) { ++at; }
		if (at >= p.prefixes.size()) { std::fprintf(stderr, "cut %llu seed %lld: no whole prefix from %zu matches\n", (unsigned long long)k, label, acked); }
		CHECK(at < p.prefixes.size());
		if (at >= p.prefixes.size()) { return; }
		t.partial += at != acked;
		expected = got;
		ix.close();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "cut %llu seed %lld: recovery or its close threw: %s\n", (unsigned long long)k, label, e.what());
		CHECK(!"recovery or its close threw");
		return;
	}
	try {
		host h(io, p.s);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, p.s.command));
		CHECK(state_of(ix, p.keys) == expected);
		for (const auto& m : p.more) {
			CHECK(ix.apply(m, {}).result == verdict::accepted);
			apply_to(expected, m);
		}
		ix.commit();
		while (h.sealer.turn()) {}
		CHECK(ix.log_synced() >= ix.applied().index);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "cut %llu seed %lld: the reopen after the close threw: %s\n", (unsigned long long)k, label, e.what());
		CHECK(!"reopen threw");
		return;
	}
	cut(1);
	try {
		host h(io, p.s);
		kv_index ix = kv_index::open(io, logged_options(h.sealer, p.s.command));
		CHECK(state_of(ix, p.keys) == expected);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "cut %llu seed %lld: second recovery threw: %s\n", (unsigned long long)k, label, e.what());
		CHECK(!"second recovery threw");
		return;
	}
	++t.cases;
}

// The plan uncut: its mutations, every transaction acknowledged, and the records it took.
std::uint64_t measure(const run_plan& p, std::uint64_t& records) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	fresh(io, p);
	host h(io, p.s);
	kv_index ix = kv_index::open(io, logged_options(h.sealer, p.s.command));
	const std::uint64_t start = fs.mutations(), first = h.sealer.sequence(group);
	std::vector<files::position> positions;
	std::size_t acked = 0;
	run(ix, h.sealer, p, positions, acked);
	CHECK(acked == p.txs.size());
	CHECK(state_of(ix, p.keys) == p.prefixes.back());
	records = h.sealer.sequence(group) - first;
	return fs.mutations() - start;
}

// Each cut in `cuts`, as a process crash and under `seeds` power-loss seeds.
tally sweep_cuts(const run_plan& p, const std::vector<std::uint64_t>& cuts) {
	tally t;
	for (const std::uint64_t k : cuts) {
		crash_case(p, k, std::nullopt, t);
		for (int seed = 0; seed < seeds; ++seed) { crash_case(p, k, std::uint64_t(seed) + k * 131, t); }
	}
	return t;
}

// --- admit: the unlogged limit, past 64 MiB --------------------------------------------------

void admit() {
	const shape s = kv_chunks();
	const transaction big = large("h", 1, 66, 1 << 20);  // 66 MiB of values
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		host h(io, s);
		kv_index logged = kv_index::create(io, logged_options(h.sealer, s.command), uuid_of("admit"));
		while (h.sealer.turn()) {}
		faultfs::filesystem fs2;
		faultfs::io io2(fs2);
		kv_index::options plain;
		plain.keyed = true;
		kv_index unlogged = kv_index::create(io2, plain, uuid_of("admit-unlogged"));
		CHECK(unlogged.apply(big, {}).result == verdict::accepted);
		const std::uint64_t before = h.sealer.sequence(group);
		CHECK(logged.apply(big, {}).result == verdict::accepted);
		logged.await_durable(logged.applied());
		const std::uint64_t records = h.sealer.sequence(group) - before;
		CHECK(records >= 66 && logged.applied() == (files::position{before + records, 1}));
		CHECK(logged.get("h65") == value("h65", 1, 1 << 20));
		std::printf("admit: %zu MiB in %llu chunk records\n", std::size_t(command::encode(big).size() >> 20), (unsigned long long)records);
	}
	// Past the unlogged limit (a transaction with more operations than the buffer holds), both
	// refuse it, and the logged one leaves no trace in the log.
	faultfs::filesystem fs;
	faultfs::io io(fs);
	host h(io, s);
	kv_index::options o = logged_options(h.sealer, s.command);
	o.buffered_docs_cap = 65;
	kv_index logged = kv_index::create(io, o, uuid_of("admit-cap"));
	while (h.sealer.turn()) {}
	faultfs::filesystem fs2;
	faultfs::io io2(fs2);
	kv_index::options plain;
	plain.keyed = true;
	plain.buffered_docs_cap = 65;
	kv_index unlogged = kv_index::create(io2, plain, uuid_of("admit-cap-unlogged"));
	CHECK(unlogged.apply(big, {}).result == verdict::invalid);
	CHECK(logged.apply(big, {}).result == verdict::invalid);
	CHECK(logged.applied() == files::position{} && !h.sealer.turn() && !logged.get("h0"));
}

// --- sweep: every mutation, 4 KiB chunks -------------------------------------------------------

void sweep() {
	run_plan p;
	p.s = small_chunks();
	p.txs = {small("a", 1), large("b", 2, 9, 4500, "a"), small("c", 3), large("kv", 4, 24, 2000), small("a", 5)};
	p.seal();
	p.commit_after[2] = true;
	p.more = {small("c", 6), small("b0", 7)};
	std::uint64_t records = 0;
	const std::uint64_t total = measure(p, records);
	CHECK(records >= 20);  // about 10 and 12 chunks, three batches, a checkpoint
	std::vector<std::uint64_t> cuts;
	for (std::uint64_t k = 0; k <= total; ++k) { cuts.push_back(k); }
	const tally t = sweep_cuts(p, cuts);
	CHECK(t.interrupted > 0 && t.cases == cuts.size() * std::size_t(seeds + 1));
	std::printf("sweep: %zu cases over %llu mutations (%llu records), %d seeds and a process crash each, %zu runs cut, %zu kept unacknowledged writes\n", t.cases,
		(unsigned long long)total, (unsigned long long)records, seeds, t.interrupted, t.partial);
}

// --- big: past 1 MiB and past 64 MiB at kv's chunks, strided cuts -------------------------------

void big() {
	run_plan p;
	p.s = kv_chunks();
	p.txs = {small("a", 1), large("kv", 2, 16, 100 << 10), small("b", 3), large("h", 4, 66, 1 << 20, "a"), small("c", 5)};
	p.seal();
	p.commit_after[2] = true;
	p.more = {small("c", 6), small("h0", 7)};
	std::uint64_t records = 0;
	const std::uint64_t total = measure(p, records);
	CHECK(records >= 2 + 66 + 3);
	// 40 cuts spread over the run, and the last 8 mutations, where the final chunk becomes durable.
	std::vector<std::uint64_t> cuts;
	for (std::uint64_t i = 0; i < 40; ++i) { cuts.push_back(total * i / 40); }
	for (std::uint64_t k = total >= 8 ? total - 8 : 0; k <= total; ++k) { cuts.push_back(k); }
	std::sort(cuts.begin(), cuts.end());
	cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
	const int keep = seeds;
	seeds = std::min(seeds, 2);
	const tally t = sweep_cuts(p, cuts);
	seeds = keep;
	CHECK(t.interrupted > 0);
	std::printf("big: %zu cases at %zu of %llu mutations (%llu records), %zu runs cut, %zu kept unacknowledged writes\n", t.cases, cuts.size(),
		(unsigned long long)total, (unsigned long long)records, t.interrupted, t.partial);
}

// --- visible: no view reads the transaction before its last chunk is durable ------------------

void visible() {
	const shape s = small_chunks();
	faultfs::filesystem fs;
	faultfs::io io(fs);
	host h(io, s);
	std::thread loop([&] { h.sealer.serve(); });
	std::uint64_t reads = 0, seen = 0, exposed = 0;
	{
		kv_index ix = kv_index::create(io, logged_options(h.sealer, s.command), uuid_of("visible"));
		CHECK(ix.apply(small("a", 1), {}).result == verdict::accepted);
		ix.commit();
		kv_index::reader_handle handle = ix.reader();
		fs.sync_latency(std::chrono::microseconds(200));
		const transaction t = large("v", 2, 60, 4000);  // about 60 chunks
		// The lowest durable sequence at which a view showed the transaction's last document.
		std::atomic<std::uint64_t> earliest{~std::uint64_t(0)};
		std::atomic<bool> stop{false};
		std::thread reader([&] {
			do {
				const auto live = handle.live();
				if (live.at.get("v59")) {
					earliest.store(std::min(earliest.load(), h.sealer.sequence(group)));
					++seen;
				}
				CHECK(live.at.get("v0").has_value() == live.at.get("v59").has_value());  // whole or nothing
				++reads;
			} while (!stop.load());
		});
		CHECK(ix.apply(t, {}).result == verdict::accepted);
		const std::uint64_t last = ix.applied().index;
		ix.await_durable(ix.applied());
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!seen && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
		stop = true;
		reader.join();
		fs.sync_latency({});
		exposed = earliest.load() < last;
		CHECK(seen > 0);
		ix.commit();
	}
	h.sealer.stop();
	loop.join();
	std::printf("visible: %llu reads, %llu saw the transaction, exposed early: %llu\n", (unsigned long long)reads, (unsigned long long)seen, (unsigned long long)exposed);
	CHECK(exposed == 0);
}

}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, void (*)()>> cases = {{"admit", admit}, {"sweep", sweep}, {"big", big}, {"visible", visible}};
	std::vector<std::string> wanted;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--seeds" && i + 1 < argc) {
			seeds = std::atoi(argv[++i]);
			continue;
		}
		wanted.push_back(a);
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
