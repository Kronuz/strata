// Read-your-writes under ack=none (docs/design/rocksdb-parity.md A1): the lease holder reads at
// applied, every view at V, so the writer sees its write when apply returns and no other reader
// sees it before its barrier; a build forced during the held barrier installs nothing early.
// held: the barrier held by the test, which writes the turns; crash: the hidden write is the one
// a power cut removes; race: a served loop with slow barriers, the owner and a view reader on
// their own threads (run under TSan).
// Usage: test_ryw [held|crash|race]...; none runs every case.
#include "kv_test.hh"
#include "strata/core/turn_seal.hh"

#include "journal/turn_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace jr = kronuz::journal;
using strata::turn_sealer;

constexpr std::uint64_t group = 7;
constexpr std::uint64_t command_bytes = 32 << 10;

jr::Identity host_identity() {
	jr::Identity id{};
	const char name[] = "ryw-host";
	std::copy(name, name + sizeof name - 1, id.begin());
	return id;
}
jr::TurnLogOptions geometry() {
	jr::TurnLogOptions g;
	g.segment_bytes = 256 << 10;
	g.block_bytes = 4096;
	return g;
}

// The host's names under "host-" in the index's own faultfs directory, so one power cut orders
// both, as standalone_turn_log_test's host_view; its owner lock is a token.
class host_view final : public jr::IO {
public:
	static constexpr std::string_view prefix = "host-";
	explicit host_view(fl::io& io) : io_(io) {}
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
	fl::io& io_;
};

// A journal host with one loop, and its sealer for the indexes.
struct host {
	jr::Host table;
	turn_sealer sealer;
	explicit host(fl::io& io) : table(std::make_shared<host_view>(io), host_identity(), jr::HostOptions{1, geometry()}), sealer(table, 0, host_identity(), geometry(), command_bytes) {}
};

kv_index::options logged_options(turn_sealer& s) {
	kv_index::options o;
	o.keyed = true;
	o.log = &s;
	o.log_group = group;
	o.command_bytes = command_bytes;
	return o;
}

using state_t = std::map<std::string, std::string>;
const state_t durable_state = {{"a", "0"}, {"b", "0"}, {"d", "0"}};
const state_t written_state = {{"a", "1"}, {"c", "1"}, {"d", "0"}};
// The ack=none write: a replace of a segment's key, a delete of another (a pending delete into
// the segment's bits) and a new key.
transaction write() { return tx({replace("a", "1"), erase("b"), replace("c", "1")}); }

// Every live record, by get and by scan, which must agree.
template <class R>
state_t state_of(const R& r) {
	state_t got, scanned;
	for (const char* k : {"a", "b", "c", "d"}) {
		if (auto v = r.get(k)) { got[k] = *v; }
	}
	r.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { scanned.emplace(std::string(k), std::string(v)); });
	CHECK(got == scanned);
	return got;
}

template <class Ready, class Each>
bool until(Ready ready, Each each) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!ready() && std::chrono::steady_clock::now() < deadline) {
		each();
		std::this_thread::yield();
	}
	return ready();
}

// --- held: the owner reads its write, views do not, until the barrier --------------------------

void held() {
	fl::filesystem fs;
	fl::io io(fs);
	host h(io);
	h.sealer.served(true);  // writers wait for turns, which this thread writes
	std::atomic<int> stage{0}, go{0};
	kv_index::reader_handle handle;
	std::thread owner([&] {
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("ryw-held"));
		CHECK(ix.apply(tx({replace("a", "0"), replace("b", "0"), replace("d", "0")}), {}).result == verdict::accepted);
		ix.refresh();  // waits for its barrier, then flushes: the three keys in a durable segment
		handle = ix.reader();
		stage = 1;
		while (go.load() < 1) { std::this_thread::yield(); }
		CHECK(ix.apply(write(), {}).result == verdict::accepted);  // returns before its barrier
		CHECK(ix.log_synced() < ix.applied().index);                // so a power cut removes it
		CHECK(state_of(ix) == written_state);
		CHECK(ix.find_key("c").has_value() && !ix.find_key("b"));
		if (const auto a = ix.find_key("a")) { CHECK(ix.find_docid(a->first).has_value()); }
		stage = 2;
		while (go.load() < 2) { std::this_thread::yield(); }
		ix.refresh();  // a build forced during the held barrier: it waits for the log, then installs
		CHECK(state_of(ix) == written_state);
		stage = 3;
		while (go.load() < 3) { std::this_thread::yield(); }
	});
	const auto nothing = [] {};
	CHECK(until([&] { return stage.load() == 1; }, [&] { h.sealer.turn(); }));
	const std::uint64_t v = handle.visible();
	const std::size_t segments = handle.acquire()->segments().size();
	CHECK(segments == 1);
	// What a reader off the owner's thread sees: a live read at V and a view at its own position,
	// each acquired after the write.
	const auto views_hold = [&](const state_t& s) {
		const auto live = handle.live();
		const auto g = handle.acquire();
		return state_of(live.at) == s && state_of(g->at()) == s;
	};
	go = 1;
	CHECK(until([&] { return stage.load() == 2; }, nothing));
	CHECK(handle.visible() == v && views_hold(durable_state));
	go = 2;
	CHECK(until([&] { return h.sealer.waiting() == 1 || stage.load() == 3; }, nothing));
	CHECK(stage.load() == 2);  // the build waits for the barrier
	CHECK(handle.visible() == v && views_hold(durable_state) && handle.acquire()->segments().size() == segments);
	CHECK(h.sealer.turn());  // the barrier
	CHECK(until([&] { return stage.load() == 3; }, nothing));
	CHECK(handle.visible() == v + 1 && views_hold(written_state));
	CHECK(handle.acquire()->segments().size() == segments + 1);  // the build installed after it
	go = 3;
	owner.join();
	handle = {};
}

// --- crash: what views hid is what a power cut takes ------------------------------------------

void crash() {
	for (const std::uint64_t seed : {0, 1, 2, 3}) {  // 0: a process crash, which keeps every byte written
		fl::filesystem fs;
		fl::io io(fs);
		{
			host h(io);
			kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("ryw-crash"));
			CHECK(ix.apply(tx({replace("a", "0"), replace("b", "0"), replace("d", "0")}), {}).result == verdict::accepted);
			ix.commit();  // the head and its checkpoint record, durable
			while (h.sealer.turn()) {}
			kv_index::reader_handle handle = ix.reader();
			CHECK(ix.apply(write(), {}).result == verdict::accepted);
			CHECK(ix.log_synced() < ix.applied().index);
			CHECK(state_of(ix) == written_state && state_of(handle.live().at) == durable_state);
			fs.power_off_after(0);
		}  // destroyed without close or turn, power off
		if (seed) {
			fs.crash(seed);
		} else {
			fs.power_on();
		}
		host h(io);
		kv_index ix = kv_index::open(io, logged_options(h.sealer));
		CHECK(state_of(ix) == durable_state);
	}
}

// --- race: slow barriers, the owner reading its writes, a reader at V (TSan) -------------------

void race() {
	constexpr int n = 3000;
	fl::filesystem fs;
	fl::io io(fs);
	host h(io);
	std::thread loop([&] { h.sealer.serve(); });
	std::uint64_t missed = 0, reads = 0, exposed = 0, behind = 0;
	{
		kv_index ix = kv_index::create(io, logged_options(h.sealer), uuid_of("ryw-race"));
		CHECK(ix.apply(tx({replace("k", "0")}), {}).result == verdict::accepted);
		ix.commit();
		kv_index::reader_handle handle = ix.reader();
		fs.sync_latency(std::chrono::microseconds(300));
		// The log position of write i, stored once apply returned it (0: not yet).
		std::vector<std::atomic<std::uint64_t>> at(n + 1);
		std::atomic<int> applied{0};
		std::atomic<bool> stop{false};
		std::thread reader([&] {
			do {  // at least one read, however late the thread starts
				const auto live = handle.live();
				const auto got = live.at.get("k");
				const int i = got ? std::stoi(*got) : -1;
				CHECK(i >= 0);
				if (i > 0) {
					// A view shows only what the log made durable.
					const std::uint64_t s = at[std::size_t(i)].load(std::memory_order_acquire);
					if (s && h.sealer.sequence(group) < s) { ++exposed; }
				}
				behind += i >= 0 && i < applied.load(std::memory_order_acquire);
				++reads;
			} while (!stop.load());
		});
		for (int i = 1; i <= n; ++i) {
			CHECK(ix.apply(tx({replace("k", std::to_string(i))}), {}).result == verdict::accepted);
			at[std::size_t(i)].store(ix.applied().index, std::memory_order_release);
			applied.store(i, std::memory_order_release);
			missed += ix.get("k") != std::to_string(i);  // ack=none: read with no wait
			if (i % 500 == 0) { ix.refresh(); }
			if (i % 1500 == 0) { ix.commit(); }
		}
		ix.commit();
		CHECK(handle.live().at.get("k") == std::to_string(n));
		stop = true;
		reader.join();
		fs.sync_latency({});
	}
	h.sealer.stop();
	loop.join();
	std::printf("race: %d writes, %llu reads, %llu behind the owner, %llu missed, %llu exposed\n", n, (unsigned long long)reads, (unsigned long long)behind,
		(unsigned long long)missed, (unsigned long long)exposed);
	CHECK(missed == 0 && exposed == 0);
	CHECK(reads > 0 && behind > 0);  // the reader ran inside the barrier window
}

}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, void (*)()>> cases = {{"held", held}, {"crash", crash}, {"race", race}};
	std::vector<std::string> wanted(argv + 1, argv + argc);
	for (const auto& [name, fn] : cases) {
		if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), name) == wanted.end()) { continue; }
		const int before = failures;
		fn();
		if (failures == before) { std::printf("%s: ok\n", name.c_str()); }
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
