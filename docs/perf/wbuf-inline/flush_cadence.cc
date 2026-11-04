// Records per flush at a fixed buffer share: document_layout, 16 B keys, VALUE-byte values,
// RECORDS replace_key writes in key order, one flush thread, no budget pressure. A flush is a drop
// in buffered_count(); records per flush is the mean count just before each drop. Also prints the
// peak resident bytes (sampled, and exact: ru_maxrss), the physical footprint peak, the live heap
// peak (operator new counter) and the budget's peak; CADENCE_NO_MERGE turns merges off. Built against either tree's headers (base or branch):
//   flush_cadence VALUE SHARE_MB RECORDS DIR
#include "bench/kv/kvgen.hh"

#include "strata/core/budget.hh"
#include "strata/core/index.hh"

#include "completion/posix.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <algorithm>
#include <atomic>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mutex>
#include <vector>
#include <cstdio>
#include <new>
#include <sys/resource.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

// Live heap bytes through the global operator new, and their peak.
static std::atomic<std::int64_t> live{0}, live_peak{0};
// CADENCE_SITES: the live allocations of 4 KiB or more at the heap's peak, by call site.
struct big {
	void* p;
	std::size_t n;
	void* site[6];
};
static constexpr std::size_t big_slots = 1 << 16;
static big bigs[big_slots], at_peak[big_slots];
static std::size_t at_peak_n = 0;
static std::int64_t snapped = 0;
static std::mutex big_mu;
static bool sites = false, snapping = true;
static thread_local bool inside = false;
static void* counted(std::size_t n) {
	void* p = std::malloc(n + 16);
	if (!p) { throw std::bad_alloc(); }
	*static_cast<std::size_t*>(p) = n;
	const std::int64_t now = live.fetch_add(std::int64_t(n), std::memory_order_relaxed) + std::int64_t(n);
	std::int64_t was = live_peak.load(std::memory_order_relaxed);
	while (was < now && !live_peak.compare_exchange_weak(was, now, std::memory_order_relaxed)) {}
	if (sites && n >= 4096 && !inside) {
		inside = true;
		big e{static_cast<char*>(p) + 16, n, {}};
		void* frames[8];
		const int got = backtrace(frames, 8);
		for (int i = 2; i < got && i < 8; ++i) { e.site[i - 2] = frames[i]; }
		std::lock_guard<std::mutex> g(big_mu);
		for (std::size_t i = (std::uintptr_t(e.p) >> 4) & (big_slots - 1);; i = (i + 1) & (big_slots - 1)) {
			if (!bigs[i].p) { bigs[i] = e; break; }
		}
		if (snapping && now > snapped + (256 << 10) && now >= live_peak.load(std::memory_order_relaxed)) {
			snapped = now;
			at_peak_n = 0;
			for (std::size_t i = 0; i < big_slots; ++i) { if (bigs[i].p) { at_peak[at_peak_n++] = bigs[i]; } }
		}
		inside = false;
	}
	return static_cast<char*>(p) + 16;
}
static void forget(void* p) {
	if (!sites) { return; }
	std::lock_guard<std::mutex> g(big_mu);
	for (std::size_t i = (std::uintptr_t(p) >> 4) & (big_slots - 1), k = 0; k < big_slots; i = (i + 1) & (big_slots - 1), ++k) {
		if (!bigs[i].p) { return; }
		if (bigs[i].p == p) {
			// backward-shift delete keeps the probe chains whole
			std::size_t j = i;
			for (;;) {
				bigs[j] = {};
				std::size_t m = j;
				for (;;) {
					m = (m + 1) & (big_slots - 1);
					if (!bigs[m].p) { return; }
					const std::size_t home = (std::uintptr_t(bigs[m].p) >> 4) & (big_slots - 1);
					if ((j <= m) ? (home <= j || home > m) : (home <= j && home > m)) { break; }
				}
				bigs[j] = bigs[m];
				j = m;
			}
		}
	}
}
static void uncounted(void* p) noexcept {
	if (!p) { return; }
	char* b = static_cast<char*>(p) - 16;
	const std::size_t n = *reinterpret_cast<std::size_t*>(b);
	live.fetch_sub(std::int64_t(n), std::memory_order_relaxed);
	if (n >= 4096) { forget(p); }
	std::free(b);
}
static void print_sites() {
	struct agg { void* site[6]; std::uint64_t bytes, count; };
	std::vector<agg> a;
	std::uint64_t total = 0;
	for (std::size_t i = 0; i < at_peak_n; ++i) {
		total += at_peak[i].n;
		auto it = std::find_if(a.begin(), a.end(), [&](const agg& x) { return std::equal(x.site, x.site + 6, at_peak[i].site); });
		if (it == a.end()) { a.push_back({}); it = a.end() - 1; std::copy(at_peak[i].site, at_peak[i].site + 6, it->site); }
		it->bytes += at_peak[i].n;
		++it->count;
	}
	std::sort(a.begin(), a.end(), [](const agg& x, const agg& y) { return x.bytes > y.bytes; });
	std::fprintf(stderr, "sites at the heap peak (%lld live; %llu in allocations >= 4 KiB):\n", (long long)snapped, (unsigned long long)total);
	for (std::size_t k = 0; k < a.size() && k < 12; ++k) {
		std::fprintf(stderr, "%10llu B %6llu allocs\n", (unsigned long long)a[k].bytes, (unsigned long long)a[k].count);
		for (int f = 0; f < 6 && a[k].site[f]; ++f) {
			Dl_info d{};
			const char* name = dladdr(a[k].site[f], &d) && d.dli_sname ? d.dli_sname : "?";
			int st = 0;
			char* dm = abi::__cxa_demangle(name, nullptr, nullptr, &st);
			std::fprintf(stderr, "      %.160s\n", st == 0 && dm ? dm : name);
			std::free(dm);
		}
	}
}
void* operator new(std::size_t n) { return counted(n); }
void* operator new[](std::size_t n) { return counted(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return counted(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return counted(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { uncounted(p); }
void operator delete[](void* p) noexcept { uncounted(p); }
void operator delete(void* p, std::size_t) noexcept { uncounted(p); }
void operator delete[](void* p, std::size_t) noexcept { uncounted(p); }

static std::uint64_t max_rss() {
	rusage u{};
	getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
	return std::uint64_t(u.ru_maxrss);
#else
	return std::uint64_t(u.ru_maxrss) << 10;
#endif
}
static std::uint64_t footprint_peak() {
#if defined(__APPLE__)
	task_vm_info_data_t v{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&v), &count) != KERN_SUCCESS) { return 0; }
	return v.ledger_phys_footprint_peak;
#else
	return 0;
#endif
}

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
	sites = std::getenv("CADENCE_SITES") != nullptr;
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
		if (const char* to = std::getenv("CADENCE_VMMAP")) {  // the regions as the run ends, index still open
			const std::string cmd = "vmmap -summary " + std::to_string(getpid()) + " > " + to + " 2>&1";
			if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "vmmap failed\n"); }
		}
		snapping = false;  // the digest below is not the write path
		const std::uint64_t maxrss = max_rss(), fp = footprint_peak(), heap = std::uint64_t(live_peak.load()), charged = budget.peak();
		std::printf("{\"value\":%llu,\"share_mb\":%llu,\"records\":%llu,\"flushes\":%llu,\"records_per_flush\":%.1f,\"peak_rss\":%llu,\"max_rss\":%llu,\"footprint_peak\":%llu,\"heap_peak\":%llu,\"budget_peak\":%llu,\"digest\":\"%016llx\"}\n", (unsigned long long)value,
			(unsigned long long)(share >> 20), (unsigned long long)records, (unsigned long long)flushes, flushes ? double(before) / double(flushes) : 0.0, (unsigned long long)peak,
			(unsigned long long)maxrss, (unsigned long long)fp, (unsigned long long)heap, (unsigned long long)charged, (unsigned long long)ix->digest());
	}
	std::filesystem::remove_all(dir);
	if (sites) { inside = true; print_sites(); }
	return 0;
}
