// Group commit (docs/design/rocksdb-parity.md D5; turn-log-parity A4): K writers on I indexes of a
// host with L loops, one served turn log each, commit over slow barriers, counted per writer. In
// a closed loop, once the writers fall into step, each commit rides the barrier after its previous
// acknowledgement, never the one after: writers sharing an index (one group) too, on one log and
// on a log per loop. Writers that think past the cohort bound arrive during a barrier, and each
// commit rides at most the barrier in flight at its join and the next.
// Usage: test_barrier_cohort [K I L think] (default: every configuration in main).
#include "kv_test.hh"
#include "strata/core/turn_seal.hh"

#include <chrono>
#include <latch>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace jr = kronuz::journal;

constexpr std::uint64_t command_bytes = 32 << 10;
constexpr int rounds = 24, settle = 3;  // the first rounds fall into step from a ragged start
constexpr auto barrier = std::chrono::milliseconds(64);

jr::Identity host_identity() {
	jr::Identity id{};
	const char name[] = "cohort-host";
	std::copy(name, name + sizeof name - 1, id.begin());
	return id;
}

// The log's directory on its own faultfs, whose sync_latency is the barrier; the host's owner lock
// is a token, as in standalone_turn_log_test's host_view.
class log_io final : public jr::IO {
public:
	explicit log_io(faultfs::filesystem& fs) : io_(fs) {}
	jr::FilesystemClass directory_filesystem() const override { return io_.directory_filesystem(); }
	jr::DirectoryTrust directory_trust() const override { return io_.directory_trust(); }
	std::unique_ptr<jr::OwnerLock> acquire_owner(bool) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::OwnerLock> lock_owner(const jr::OwnerLockOptions&) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::File> open_existing(std::string_view n) override { return io_.open_existing(n); }
	std::unique_ptr<jr::File> open_existing_read_only(std::string_view n) override { return io_.open_existing_read_only(n); }
	std::unique_ptr<jr::File> open_reclaim_candidate_read_only(std::string_view n) override { return io_.open_reclaim_candidate_read_only(n); }
	void open_existing_read_only_into(std::string_view n, jr::File& f) override { io_.open_existing_read_only_into(n, f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, jr::File& f) override { return io_.open_reclaim_candidate_read_only_into(n, f); }
	std::unique_ptr<jr::File> create_exclusive(std::string_view n) override { return io_.create_exclusive(n); }
	void replace(std::string_view from, std::string_view to) override { io_.replace(from, to); }
	void remove(std::string_view n) override { io_.remove(n); }
	std::optional<jr::EntryFootprint> entry_footprint(std::string_view n) override { return io_.entry_footprint(n); }
	void order_files_and_directory(std::span<jr::File* const> files) override { io_.order_files_and_directory(files); }
	void sync_directory() override { io_.sync_directory(); }
	std::unique_ptr<jr::DirectoryCursor> scan_directory() override { return io_.scan_directory(); }

private:
	faultfs::io io_;
};

struct config {
	int writers, indexes;
	std::uint32_t loops;
	bool think;  // each writer sleeps past the cohort bound before each write
};

void run(const config& c) {
	jr::TurnLogOptions g;
	g.segment_bytes = 4 << 20;
	g.block_bytes = 4096;
	faultfs::filesystem log_fs;
	jr::Host table(std::make_shared<log_io>(log_fs), host_identity(), jr::HostOptions{c.loops, g});
	std::vector<std::unique_ptr<turn_sealer>> sealers;
	for (std::uint32_t l = 0; l < c.loops; ++l) { sealers.push_back(std::make_unique<turn_sealer>(table, l, host_identity(), g, command_bytes)); }

	// Index i on loop i mod L, its group the next id there; writer w on index w mod I.
	std::vector<std::unique_ptr<faultfs::filesystem>> fss;
	std::vector<std::unique_ptr<faultfs::io>> ios;
	std::vector<kv_index> ixs;
	std::vector<turn_sealer*> logs;
	ixs.reserve(std::size_t(c.indexes));
	std::uint64_t group = 0;
	for (int i = 0; i < c.indexes; ++i) {
		const std::uint32_t loop = std::uint32_t(i) % c.loops;
		do { ++group; } while (table.loop(group) != loop);
		fss.push_back(std::make_unique<faultfs::filesystem>());
		ios.push_back(std::make_unique<faultfs::io>(*fss.back()));
		kv_index::options o;
		o.keyed = true;
		o.log = sealers[loop].get();
		o.log_group = group;
		o.command_bytes = command_bytes;
		ixs.push_back(kv_index::create(*ios.back(), o, uuid_of(("cohort-" + std::to_string(i)).c_str())));
		logs.push_back(sealers[loop].get());
	}
	for (auto& s : sealers) {
		while (s->turn()) {}  // the joins' records
	}
	log_fs.sync_latency(barrier);
	std::vector<std::thread> loops;
	for (auto& s : sealers) { loops.emplace_back([s = s.get()] { s->serve(); }); }

	// waited[w][r]: the turns writer w's log wrote between its acknowledgement r - 1 (its start
	// for r = 0) and acknowledgement r; with `think`, between write r's join and its
	// acknowledgement.
	std::vector<std::vector<std::uint64_t>> waited(static_cast<std::size_t>(c.writers), std::vector<std::uint64_t>(rounds));
	std::vector<std::mutex> owners(std::size_t(c.indexes));  // writers sharing an index apply in turn
	std::latch start(c.writers);
	std::vector<std::thread> ts;
	for (int w = 0; w < c.writers; ++w) {
		ts.emplace_back([&, w] {
			const std::size_t i = std::size_t(w % c.indexes);
			kv_index& ix = ixs[i];
			turn_sealer& log = *logs[i];
			const std::string key = "w" + std::to_string(w);
			std::minstd_rand rng(std::uint32_t(w) + 1);
			start.arrive_and_wait();
			std::uint64_t last = log.turns_written();
			for (int r = 0; r < rounds; ++r) {
				if (c.think) {
					// 1/8 to 3/4 of a barrier: past the cohort bound (1/16), so the next turn
					// starts without this writer and it joins during the barrier.
					std::this_thread::sleep_for(barrier / 8 + barrier * int(rng() % 64) / 102);
					last = log.turns_written();
				}
				files::position at;
				{
					const std::lock_guard own(owners[i]);
					CHECK(ix.apply(tx({replace(key, std::to_string(r))}), {}).result == verdict::accepted);
					at = ix.applied();
				}
				ix.await_durable(at);
				const std::uint64_t now = log.turns_written();
				waited[std::size_t(w)][std::size_t(r)] = now - last;
				last = now;
				// Acknowledged: durable through its position, and the writer reads its write.
				const std::lock_guard own(owners[i]);
				const auto got = ix.get(key);
				CHECK(ix.log_synced() >= at.index && got && *got == std::to_string(r));
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	ixs.clear();  // their leaves, while the loops serve
	for (auto& s : sealers) { s->stop(); }
	for (auto& t : loops) { t.join(); }
	log_fs.sync_latency({});

	std::uint64_t hist[4] = {}, turns = 0;
	for (auto& s : sealers) { turns += s->turns_written(); }
	for (int w = 0; w < c.writers; ++w) {
		std::printf("  writer %d:", w);
		for (int r = 0; r < rounds; ++r) {
			const std::uint64_t n = waited[std::size_t(w)][std::size_t(r)];
			std::printf(" %llu", (unsigned long long)n);
			if (c.think || r >= settle) { ++hist[std::min<std::uint64_t>(n, 3)]; }
		}
		std::printf("\n");
	}
	const char* name = c.think ? " think" : "";
	std::printf("K=%d I=%d L=%u%s: %llu commits took 1 turn, %llu 2, %llu 3 or more; %llu turns\n", c.writers, c.indexes, c.loops, name, (unsigned long long)hist[1],
		(unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)turns);
	const int before = failures;
	if (c.think) {
		// Each commit rides the barrier in flight at its join, if any, and the next: never a third.
		CHECK(hist[0] == 0 && hist[3] == 0);
	} else {
		// Each writer's commit rides exactly the next barrier: none waits a second one, and none
		// shares a barrier with its previous acknowledgement.
		CHECK(hist[0] == 0 && hist[2] == 0 && hist[3] == 0);
		CHECK(hist[1] == std::uint64_t(c.writers) * std::uint64_t(rounds - settle));
	}
	if (failures != before) { std::fprintf(stderr, "K=%d I=%d L=%u%s failed\n", c.writers, c.indexes, c.loops, name); }
	std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc > 1) {
		const auto arg = [&](int i, int d) { return argc > i ? std::atoi(argv[i]) : d; };
		const int k = arg(1, 4);
		run({k, arg(2, k), std::uint32_t(arg(3, 1)), arg(4, 0) != 0});
	} else {
		// One log: a group per writer, two writers per group, writers that think. A log per loop:
		// a writer per log, and two writers sharing an index on each of two logs.
		const config cs[] = {{4, 4, 1, false}, {4, 2, 1, false}, {8, 4, 1, false}, {4, 4, 1, true}, {4, 4, 4, false}, {8, 8, 8, false}, {4, 2, 2, false}};
		for (const config& c : cs) { run(c); }
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
