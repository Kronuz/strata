// The log's segments stay bounded (docs/design/rocksdb-parity.md, D2's test): an open idle index's
// checkpoint sits in the oldest segment while another group writes 10 x retain_segments segments
// of turns. The idle checkpoint moves forward (D2 item 3) and the writer checkpoints inside the
// window, (retain_segments - 1) segments of commands (D2 item 2), so the log never holds more
// than retain_segments + spare_segments + 1 segment files.
// Usage: test_checkpoint_window [explicit]: the writer's trigger set to the window by
// checkpoint_log_bytes, as the index has no window of its own yet.
#include "kv_test.hh"
#include "strata/core/turn_seal.hh"

#include <cstring>
#include <memory>
#include <string>

namespace {

namespace jr = kronuz::journal;

constexpr std::uint64_t command_bytes = 32 << 10;

jr::TurnLogOptions geometry() {
	jr::TurnLogOptions g;
	g.segment_bytes = 1 << 20;
	g.block_bytes = 4096;
	g.retain_segments = 4;
	g.spare_segments = 1;
	return g;
}

jr::Identity host_identity() {
	jr::Identity id{};
	const char name[] = "window-host";
	std::copy(name, name + sizeof name - 1, id.begin());
	return id;
}

// The log's directory on its own faultfs; the host's owner lock is a token, as in
// standalone_turn_log_test's host_view.
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

std::size_t segment_files(jr::IO& io) {
	std::size_t n = 0;
	auto cursor = io.scan_directory();
	while (auto name = cursor->next()) { n += jr::turn_log_detail::segment_number(*name).has_value(); }
	return n;
}

}  // namespace

int main(int argc, char** argv) {
	const bool explicit_trigger = argc > 1 && !std::strcmp(argv[1], "explicit");
	const jr::TurnLogOptions g = geometry();
	const std::uint64_t window = (g.retain_segments - 1) * g.segment_bytes;
	const std::size_t bound = g.retain_segments + g.spare_segments + 1;

	faultfs::filesystem log_fs;
	auto lio = std::make_shared<log_io>(log_fs);
	jr::Host host(lio, host_identity(), jr::HostOptions{1, g});
	turn_sealer sealer(host, 0, host_identity(), g, command_bytes);
	auto turns = [&] { while (sealer.turn()) {} };

	faultfs::filesystem idle_fs, writer_fs;
	faultfs::io idle_io(idle_fs), writer_io(writer_fs);
	kv_index::options o;
	o.keyed = true;
	o.log = &sealer;
	o.command_bytes = command_bytes;
	o.log_group = 1;
	kv_index idle = kv_index::create(idle_io, o, uuid_of("idle"));
	o.log_group = 2;
	if (explicit_trigger) { o.checkpoint_log_bytes = window; }
	kv_index writer = kv_index::create(writer_io, o, uuid_of("writer"));
	turns();

	// The idle index's one write and its checkpoint, then nothing more from it.
	CHECK(idle.apply(tx({replace("idle", "kept")}), {}).result == verdict::accepted);
	idle.commit();
	turns();

	auto& log = host.driver(0).log();
	const std::uint64_t first = log.segment();
	const std::string body(8 << 10, 'w');
	std::uint64_t logged = 0, checkpoints = 0, applies = 0;
	std::size_t most = segment_files(*lio);
	while (log.segment() < first + 10 * g.retain_segments) {
		const std::size_t before = writer.buffered_count();
		const std::string key = "k" + std::to_string(applies++ % 256);
		CHECK(writer.apply(tx({replace(key, body)}), {}).result == verdict::accepted);
		if (writer.buffered_count() <= before) { ++checkpoints; }  // the apply's flush, a checkpoint
		logged += key.size() + body.size();
		turns();
		most = std::max(most, segment_files(*lio));
		if (most > 4 * bound) { break; }  // unbounded: no need to fill the rest
	}
	const auto& st = log.stats();
	std::printf("segments %llu..%llu, most segment files %zu (bound %zu), forwarded %llu records %llu bytes, writer checkpoints %llu for %llu command bytes (window %llu)\n",
		(unsigned long long)first, (unsigned long long)log.segment(), most, bound, (unsigned long long)st.forwarded_records, (unsigned long long)st.forwarded_bytes,
		(unsigned long long)checkpoints, (unsigned long long)logged, (unsigned long long)window);

	CHECK(log.segment() >= first + 10 * g.retain_segments);
	CHECK(most <= bound);
	CHECK(st.forwarded_records > 0);
	// The writer checkpoints once per window of commands: the transactions' bytes, without their framing.
	CHECK(checkpoints + 1 >= logged / window && checkpoints <= logged / window + 1);
	// The idle index still reads its write.
	const auto got = idle.get("idle");
	CHECK(got && *got == "kept");

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ok\n");
	return 0;
}
