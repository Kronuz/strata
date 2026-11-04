// Records per flush at a fixed buffer share: document_layout, 16 B keys, VALUE-byte values,
// RECORDS replace_key writes in key order, one flush thread, no budget pressure. A flush is a drop
// in buffered_count(); records per flush is the mean count just before each drop. Also prints the
// peak resident bytes; CADENCE_NO_MERGE turns merges off. Built against either tree's headers (base or branch):
//   flush_cadence VALUE SHARE_MB RECORDS DIR
#include "bench/kv/kvgen.hh"

#include "strata/core/budget.hh"
#include "strata/core/index.hh"

#include "completion/posix.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <unistd.h>

static std::uint64_t resident() {
#if defined(__APPLE__)
	mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) { return 0; }
	return info.resident_size;
#elif defined(__linux__)
	unsigned long long pages = 0, rss = 0;
	if (FILE* f = std::fopen("/proc/self/statm", "r")) {
		if (std::fscanf(f, "%llu %llu", &pages, &rss) != 2) { rss = 0; }
		std::fclose(f);
	}
	return std::uint64_t(rss) * std::uint64_t(sysconf(_SC_PAGESIZE));
#else
	return 0;
#endif
}

int main(int argc, char** argv) {
	if (argc != 5) { std::fprintf(stderr, "usage: flush_cadence VALUE SHARE_MB RECORDS DIR\n"); return 2; }
	const std::uint64_t value = std::strtoull(argv[1], nullptr, 10), share = std::strtoull(argv[2], nullptr, 10) << 20, records = std::strtoull(argv[3], nullptr, 10);
	const std::string dir = argv[4];
	using kv_index = strata::basic_index<strata::document_layout>;
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	strata::byte_budget budget(std::uint64_t(16) << 30);
	std::uint64_t flushes = 0, before = 0, peak = 0;
	{
		kronuz::journal::PosixIO io(dir, 0600, kronuz::journal::PosixIOOptions{true, true});
		kv_index::options o;
		o.keyed = true;
		o.budget = &budget;
		o.buffer_share = share;
		o.auto_merge = !std::getenv("CADENCE_NO_MERGE");  // set: flushes only, no merges
		const std::array<char, 16> uuid{'c', 'a', 'd', 'e', 'n', 'c', 'e'};
		auto ix = std::make_unique<kv_index>(kv_index::create(io, o, uuid));
		kv_index::transaction tx;
		tx.ops.resize(1);
		tx.ops[0].type = kv_index::operation::replace_key;
		kvbench::values gen(1);
		std::uint64_t prev = 0;
		for (std::uint64_t id = 0; id < records; ++id) {
			auto& op = tx.ops[0];
			op.key = kvbench::key_of(id);
			op.doc.key = op.key;
			op.doc.stored.assign(gen.next(value));
			if (ix->apply(tx, {}).result != strata::verdict::accepted) { std::abort(); }
			const std::uint64_t now = ix->buffered_count();
			if (now < prev) {
				++flushes;
				before += prev;
			}
			prev = now;
			if (id % 4096 == 0) { peak = std::max(peak, resident()); }
		}
		peak = std::max(peak, resident());
		std::printf("{\"value\":%llu,\"share_mb\":%llu,\"records\":%llu,\"flushes\":%llu,\"records_per_flush\":%.1f,\"peak_rss\":%llu,\"digest\":\"%016llx\"}\n", (unsigned long long)value,
			(unsigned long long)(share >> 20), (unsigned long long)records, (unsigned long long)flushes, flushes ? double(before) / double(flushes) : 0.0, (unsigned long long)peak,
			(unsigned long long)ix->digest());
	}
	std::filesystem::remove_all(dir);
	return 0;
}
