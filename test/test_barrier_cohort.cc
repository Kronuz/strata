// Group commit with the cohort wait (docs/design/rocksdb-parity.md, D5): K writers, each on its own
// index and group of one served turn log, commit in a closed loop over slow barriers. Counted per
// writer, the barriers between one acknowledgement and the next: once the writers fall into step,
// each commit rides the next barrier, never the one after it.
// Usage: test_barrier_cohort [K] (default 4).
#include "kv_test.hh"
#include "strata/core/turn_seal.hh"

#include <chrono>
#include <latch>
#include <memory>
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

}  // namespace

int main(int argc, char** argv) {
	const int writers = argc > 1 ? std::atoi(argv[1]) : 4;
	jr::TurnLogOptions g;
	g.segment_bytes = 4 << 20;
	g.block_bytes = 4096;
	faultfs::filesystem log_fs;
	jr::Host table(std::make_shared<log_io>(log_fs), host_identity(), jr::HostOptions{1, g});
	turn_sealer sealer(table, 0, host_identity(), g, command_bytes);

	std::vector<std::unique_ptr<faultfs::filesystem>> fss;
	std::vector<std::unique_ptr<faultfs::io>> ios;
	std::vector<kv_index> ixs;
	ixs.reserve(std::size_t(writers));
	for (int w = 0; w < writers; ++w) {
		fss.push_back(std::make_unique<faultfs::filesystem>());
		ios.push_back(std::make_unique<faultfs::io>(*fss.back()));
		kv_index::options o;
		o.keyed = true;
		o.log = &sealer;
		o.log_group = std::uint64_t(w) + 1;
		o.command_bytes = command_bytes;
		ixs.push_back(kv_index::create(*ios.back(), o, uuid_of(("cohort-" + std::to_string(w)).c_str())));
	}
	while (sealer.turn()) {}  // the joins' records
	log_fs.sync_latency(barrier);
	std::thread loop([&] { sealer.serve(); });

	// waited[w][i]: the turns written between writer w's acknowledgement i - 1 (its start for
	// i = 0) and acknowledgement i.
	std::vector<std::vector<std::uint64_t>> waited(static_cast<std::size_t>(writers), std::vector<std::uint64_t>(rounds));
	std::latch start(writers);
	std::vector<std::thread> ts;
	for (int w = 0; w < writers; ++w) {
		ts.emplace_back([&, w] {
			kv_index& ix = ixs[std::size_t(w)];
			const std::string key = "w" + std::to_string(w);
			start.arrive_and_wait();
			std::uint64_t last = sealer.turns_written();
			for (int i = 0; i < rounds; ++i) {
				CHECK(ix.apply(tx({replace(key, std::to_string(i))}), {}).result == verdict::accepted);
				const files::position at = ix.applied();
				ix.await_durable(at);
				const std::uint64_t now = sealer.turns_written();
				waited[std::size_t(w)][std::size_t(i)] = now - last;
				last = now;
				// Acknowledged: durable through its position, and the writer reads its write.
				const auto got = ix.get(key);
				CHECK(ix.log_synced() >= at.index && got && *got == std::to_string(i));
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	ixs.clear();  // their leaves, while the loop serves
	sealer.stop();
	loop.join();
	log_fs.sync_latency({});

	std::uint64_t hist[4] = {};
	for (int w = 0; w < writers; ++w) {
		std::printf("writer %d:", w);
		for (int i = 0; i < rounds; ++i) {
			const std::uint64_t n = waited[std::size_t(w)][std::size_t(i)];
			std::printf(" %llu", (unsigned long long)n);
			if (i >= settle) { ++hist[std::min<std::uint64_t>(n, 3)]; }
		}
		std::printf("\n");
	}
	std::printf("K=%d, after %d rounds: %llu commits waited 1 barrier, %llu 2, %llu 3 or more; %llu turns\n", writers, settle,
		(unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)sealer.turns_written());
	// Every writer's commit rides exactly the next barrier: none waits a second one, and none
	// shares a barrier with its previous acknowledgement.
	CHECK(hist[0] == 0 && hist[2] == 0 && hist[3] == 0);
	CHECK(hist[1] == std::uint64_t(writers) * std::uint64_t(rounds - settle));
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
