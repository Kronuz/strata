// format-14-kv D3's baseline (docs/design/format-14-kv.md): resident bytes per buffered record
// of today's document-layout buffer, as core-vs-rocksdb.md measured the search layout's (683 B):
// RSS at 100,000 and at 300,000 buffered records, nothing flushed, the difference over 200,000.
#include "../kvgen.hh"

#include "strata/core/budget.hh"
#include "strata/core/index.hh"

#include "completion/posix.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

// Resident bytes: the task's on macOS, /proc/self/statm's on Linux, 0 elsewhere.
std::uint64_t resident() {
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

// Bytes in use by malloc (macOS), the heap's own count without page-touch noise.
std::uint64_t in_use() {
#if defined(__APPLE__)
	malloc_statistics_t st;
	malloc_zone_statistics(nullptr, &st);
	return st.size_in_use;
#else
	return 0;
#endif
}

// A layout whose documents carry a part beyond the stored bytes: with `extra` empty a buffered
// record holds them inline; with some, the record holds the whole document.
struct carry_plugin : strata::stored_plugin {
	struct part : strata::stored_plugin::part {
		std::string extra;
	};
	static bool inline_part(const part& p) { return p.extra.empty(); }
};
using carry_layout = strata::layout<strata::keys_plugin, carry_plugin>;

template <class Layout>
void set_extra(typename strata::basic_index<Layout>::operation&, bool) {}
template <>
void set_extra<carry_layout>(strata::basic_index<carry_layout>::operation& op, bool extra) {
	if (extra) { op.doc.extra = "x"; }
}

template <class Layout>
int measure(std::string dir, std::uint64_t value_size, bool extra) {
	using kv_index = strata::basic_index<Layout>;
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	strata::byte_budget budget(std::uint64_t(16) << 30);  // never under pressure: nothing flushes
	{
		kronuz::journal::PosixIO io(dir, 0600, kronuz::journal::PosixIOOptions{true, true});
		typename kv_index::options o;
		o.keyed = true;
		o.budget = &budget;
		o.buffer_share = std::uint64_t(8) << 30;
		const std::array<char, 16> uuid{'b', 'u', 'f', 'm', 'e', 'm'};
		auto ix = std::make_unique<kv_index>(kv_index::create(io, o, uuid));
		typename kv_index::transaction tx;
		tx.ops.resize(1);
		tx.ops[0].type = kv_index::operation::replace_key;
		kvbench::values gen(1);
		auto put = [&](std::uint64_t id) {
			auto& op = tx.ops[0];
			op.key = kvbench::key_of(id);
			op.doc.key = op.key;
			op.doc.stored.assign(gen.next(value_size));
			set_extra<Layout>(op, extra);
			if (ix->apply(tx, {}).result != strata::verdict::accepted) { std::abort(); }
		};
		std::uint64_t id = 0;
		for (; id < 100'000; ++id) { put(id); }
		const std::uint64_t use1 = in_use(), rss1 = resident(), buffered1 = ix->buffered_count();
		for (; id < 300'000; ++id) { put(id); }
		const std::uint64_t use2 = in_use(), rss2 = resident(), buffered2 = ix->buffered_count();
		if (buffered2 - buffered1 != 200'000) { std::fprintf(stderr, "the buffer flushed: %llu\n", (unsigned long long)buffered2); return 1; }
		std::uint64_t fnv = 0xcbf29ce484222325ull, scanned = 0;
		const auto mix = [&](std::string_view v) {
			for (const unsigned char c : v) { fnv = (fnv ^ c) * 0x100000001b3ull; }
			fnv = (fnv ^ 0xff) * 0x100000001b3ull;
		};
		if constexpr (requires { ix->scan(std::string_view{}, [](std::string_view, std::string_view, std::uint64_t) { return true; }); }) {
			ix->scan(std::string_view{}, [&](std::string_view k, std::string_view v, std::uint64_t) {
				mix(k);
				mix(v);
				++scanned;
				return true;
			});
		}
		std::fprintf(stderr, "scan digest %016llx over %llu; index digest %016llx\n", (unsigned long long)fnv, (unsigned long long)scanned, (unsigned long long)ix->digest());
		std::printf("{\"value\":%llu,\"heap_per_record\":%.1f,\"rss_per_record\":%.1f,\"rss1\":%llu,\"rss2\":%llu}\n", (unsigned long long)value_size, double(use2 - use1) / 200'000.0, double(rss2 - rss1) / 200'000.0,
			(unsigned long long)rss1, (unsigned long long)rss2);
	}
	std::filesystem::remove_all(dir);
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	std::string dir = ".scratch/bufmem", layout = "document";
	std::uint64_t value_size = 100;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view a = argv[i];
		if (a == "--dir") { dir = argv[i + 1]; }
		else if (a == "--value") { value_size = std::strtoull(argv[i + 1], nullptr, 10); }
		else if (a == "--layout") { layout = argv[i + 1]; }  // document, carry (extra part empty), carry-extra (whole records)
	}
	if (layout == "document") { return measure<strata::document_layout>(dir, value_size, false); }
	if (layout == "carry") { return measure<carry_layout>(dir, value_size, false); }
	if (layout == "carry-extra") { return measure<carry_layout>(dir, value_size, true); }
	std::fprintf(stderr, "unknown layout %s\n", layout.c_str());
	return 1;
}
