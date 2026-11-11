// The cost of one idle open index (docs/perf/idle-index.md).
//   idle_index build ROOT COUNT DOCS [SEGMENTS]
//                                         COUNT indexes of DOCS documents in SEGMENTS commits (1):
//                                         one real index, then copy-on-write clones of its files
//                                         (macOS clonefile, Linux FICLONE or copy), COUNT/1000
//                                         groups of 1000
//   idle_index open  ROOT COUNT [shared] [query] [idle SECS] [obs] [tier hot|lazy|warm]
//                                         opens them all in this process, one thread, and prints
//                                         resident, dirty footprint, descriptors, mappings,
//                                         threads, wakeups, and open/query/close latency
// Documents are "Q%07d" keys with 100-byte values, in the document layout. `shared` also holds
// a reader handle per index on one registry (a host's shape); `query` runs one get per index.
// `tier` (cold-tier T1): hot maps every segment at open (eager_segments), lazy maps none (the
// default), warm opens hot and then demote()s. Without it the options' default applies.
#include "strata/core/budget.hh"
#include "strata/core/hazard.hh"
#include "strata/core/index.hh"

#include "completion/posix.h"
#include "meters.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <sys/attr.h>
#include <sys/clonefile.h>
#endif
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "heap_count.hh"

namespace fs = std::filesystem;
using namespace strata;
using namespace idle_bench;
using kv_index = basic_index<document_layout>;

namespace {

std::string value_of(std::uint64_t i, std::size_t n) {
	std::string v(n, 'a');
	std::uint64_t x = i * 0x9E3779B97F4A7C15ull + 1;
	for (auto& c : v) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		c = char('a' + x % 26);
	}
	return v;
}


enum class tier { none, hot, lazy, warm };
tier open_tier = tier::none;

kv_index::options base_options(byte_budget* b, reader_registry* r) {
	kv_index::options o;
	o.keyed = true;
	if (open_tier != tier::none) { o.eager_segments = open_tier != tier::lazy; }
	o.budget = b;
	o.buffer_share = std::uint64_t(1) << 30;
	o.readers = r;
	return o;
}

// --- build ------------------------------------------------------------------------------------

void clone_file(const fs::path& from, const fs::path& to) {
#ifdef __APPLE__
	if (::clonefile(from.c_str(), to.c_str(), 0) == 0) { return; }
#elif defined(__linux__)
	{
		const int s = ::open(from.c_str(), O_RDONLY);
		const int d = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (s >= 0 && d >= 0 && ::ioctl(d, FICLONE, s) == 0) { ::close(s); ::close(d); return; }
		if (s >= 0) { ::close(s); }
		if (d >= 0) { ::close(d); }
		std::error_code ec;
		fs::remove(to, ec);
	}
#endif
	fs::copy_file(from, to);
}

int build(const fs::path& root, std::uint64_t count, std::uint64_t docs, std::uint64_t segments) {
	fs::create_directories(root);
	const fs::path tmpl = root / "template";
	fs::remove_all(tmpl);
	fs::create_directories(tmpl);
	{
		kronuz::journal::PosixIO io(tmpl, 0600, kronuz::journal::PosixIOOptions{true, true});
		auto o = base_options(nullptr, nullptr);
		o.auto_merge = false;  // the fixture keeps its segments
		kv_index ix = kv_index::create(io, o, std::array<char, 16>{'i', 'd', 'l', 'e'});
		const std::uint64_t per = segments > 1 ? (docs + segments - 1) / segments : ~std::uint64_t(0);
		kv_index::transaction tx;
		tx.ops.resize(1);
		tx.ops[0].type = kv_index::operation::replace_key;
		for (std::uint64_t d = 0; d < docs; ++d) {
			auto& op = tx.ops[0];
			op.key = key_of(d);
			op.doc.key = op.key;
			op.doc.stored.assign(value_of(d, 100));
			if (ix.apply(tx, {}).result != verdict::accepted) { std::abort(); }
			if ((d + 1) % per == 0 && d + 1 < docs) { ix.commit(); }
		}
		ix.commit();
		std::printf("template: %zu segments\n", ix.segments().size());
	}
	std::vector<fs::path> files;
	std::uintmax_t bytes = 0;
	for (const auto& e : fs::directory_iterator(tmpl)) {
		if (e.is_regular_file()) { files.push_back(e.path().filename()); bytes += e.file_size(); }
	}
	std::printf("template: %zu files, %ju bytes\n", files.size(), bytes);
	const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
	std::atomic<std::uint64_t> next{0};
	std::vector<std::thread> ts;
	const auto t0 = clk::now();
	for (unsigned t = 0; t < nt; ++t) {
		ts.emplace_back([&] {
			for (;;) {
				const std::uint64_t i = next.fetch_add(1);
				if (i >= count) { return; }
				const fs::path d = dir_of(root, i);
				if (i % 1000 == 0) { fs::create_directories(d.parent_path()); }
				else { while (!fs::exists(d.parent_path())) { std::this_thread::yield(); } }
				fs::create_directory(d);
				for (const auto& f : files) { clone_file(tmpl / f, d / f); }
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	std::printf("built %llu indexes in %.1f s\n", (unsigned long long)count, us(t0, clk::now()) / 1e6);
	return 0;
}

// --- open -------------------------------------------------------------------------------------

struct slot {
	std::unique_ptr<kronuz::journal::PosixIO> io;
	std::unique_ptr<kv_index> ix;
	kv_index::reader_handle rd;
};

int open_many(const fs::path& root, std::uint64_t count, bool shared, bool query, double idle_secs, bool obs) {
	struct rlimit rl{};
	getrlimit(RLIMIT_NOFILE, &rl);
	rlim_t want = rl.rlim_max == RLIM_INFINITY ? rlim_t(10000000) : rl.rlim_max;
#ifdef __APPLE__
	want = std::min<rlim_t>(want, 1048575);  // kern.maxfilesperproc caps it
#endif
	rl.rlim_cur = want;
	if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) { std::perror("setrlimit"); }
	getrlimit(RLIMIT_NOFILE, &rl);
	std::printf("RLIMIT_NOFILE %llu\n", (unsigned long long)rl.rlim_cur);

	byte_budget budget(std::uint64_t(16) << 30);
	reader_registry registry(64);
	std::vector<slot> slots(count);
	std::vector<double> t_open, t_q1, t_q2, t_close;
	t_open.reserve(count); t_q1.reserve(count); t_q2.reserve(count); t_close.reserve(count);
	const meters m0 = take();
	std::uint64_t opened = 0, refused = 0;
	const auto topen = clk::now();
	for (std::uint64_t i = 0; i < count; ++i) {
		const auto a = clk::now();
		try {
			slots[i].io = std::make_unique<kronuz::journal::PosixIO>(dir_of(root, i), 0600, kronuz::journal::PosixIOOptions{true, true});
			slots[i].ix = std::make_unique<kv_index>(kv_index::open(*slots[i].io, base_options(&budget, shared ? &registry : nullptr)));
			if (shared) { slots[i].rd = slots[i].ix->reader(); }
			if (open_tier == tier::warm && !slots[i].ix->demote()) { ++refused; }
		} catch (const std::exception& e) {
			std::printf("LIMIT at index %llu: %s (errno %d: %s)\n", (unsigned long long)i, e.what(), errno, std::strerror(errno));
			slots[i].ix.reset(); slots[i].io.reset();
			break;
		}
		t_open.push_back(us(a, clk::now()));
		++opened;
	}
	const double open_total = us(topen, clk::now()) / 1e6;
	const meters m1 = take();
	const double n = double(opened);
	std::printf("opened %llu of %llu in %.2f s (shared=%d)\n", (unsigned long long)opened, (unsigned long long)count, open_total, int(shared));
	if (open_tier == tier::warm) { std::printf("demote() refused %llu\n", (unsigned long long)refused); }
	report("after open", m0, m1, n);
	print_pct("open", summarize(t_open));

	if (idle_secs > 0) {
		const meters i0 = take();
		rusage r0{}, r1{};
		getrusage(RUSAGE_SELF, &r0);
		std::this_thread::sleep_for(std::chrono::duration<double>(idle_secs));
		getrusage(RUSAGE_SELF, &r1);
		const meters i1 = take();
		const double cpu = double(r1.ru_utime.tv_sec - r0.ru_utime.tv_sec + r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + double(r1.ru_utime.tv_usec - r0.ru_utime.tv_usec + r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) * 1e-6;
		std::printf("idle %.1f s: cpu %.4f s, voluntary switches %ld, involuntary %ld, threads %ld -> %ld\n", idle_secs, cpu, r1.ru_nvcsw - r0.ru_nvcsw, r1.ru_nivcsw - r0.ru_nivcsw, i0.threads, i1.threads);
	}

	if (query) {
		const std::string k = key_of(0);
		auto run = [&](std::vector<double>& out) {
			std::uint64_t found = 0;
			for (std::uint64_t i = 0; i < opened; ++i) {
				const auto a = clk::now();
				if (shared) {
					auto lv = slots[i].rd.live();
					found += lv.at.get(k).has_value();
				} else {
					found += slots[i].ix->get(k).has_value();
				}
				out.push_back(us(a, clk::now()));
			}
			return found;
		};
		const auto f1 = run(t_q1);
		const meters m2 = take();
		report("after query 1", m0, m2, n);
		const auto f2 = run(t_q2);
		const meters m3 = take();
		std::printf("found %llu / %llu\n", (unsigned long long)f1, (unsigned long long)opened);
		print_pct("first query", summarize(t_q1));
		print_pct("second query", summarize(t_q2));
		report("after query 2", m0, m3, n);
		(void)f2;
	}

#ifdef IDLE_COUNT
	std::printf("heap live: %.1f B/index in %.2f allocations\n", double(live_bytes) / n, double(live_count) / n);
#endif
	(void)obs;

	const auto tclose = clk::now();
	for (std::uint64_t i = 0; i < opened; ++i) {
		const auto a = clk::now();
		slots[i].rd = {};
		slots[i].ix.reset();
		slots[i].io.reset();
		t_close.push_back(us(a, clk::now()));
	}
	print_pct("close", summarize(t_close));
	std::printf("closed in %.2f s\n", us(tclose, clk::now()) / 1e6);
	return 0;
}

#ifdef IDLE_COUNT
// Opens COUNT indexes with the allocation sites recorded; prints the live ones per index.
int open_sites(const fs::path& root, std::uint64_t count, bool shared) {
	byte_budget budget(std::uint64_t(16) << 30);
	reader_registry registry(64);
	{  // one open first, so one-time statics are not charged
		kronuz::journal::PosixIO io(dir_of(root, count), 0600, kronuz::journal::PosixIOOptions{true, true});
		kv_index ix = kv_index::open(io, base_options(&budget, shared ? &registry : nullptr));
		if (shared) { (void)ix.reader(); }
	}
	std::vector<slot> slots(count);
	sites = new std::map<std::uint64_t, site_rec>;
	const std::size_t b0 = live_bytes, c0 = live_count;
	profile = true;
	for (std::uint64_t i = 0; i < count; ++i) {
		slots[i].io = std::make_unique<kronuz::journal::PosixIO>(dir_of(root, i), 0600, kronuz::journal::PosixIOOptions{true, true});
		slots[i].ix = std::make_unique<kv_index>(kv_index::open(*slots[i].io, base_options(&budget, shared ? &registry : nullptr)));
		if (shared) { slots[i].rd = slots[i].ix->reader(); }
	}
	profile = false;
	const double n = double(count);
	std::printf("live heap per index: %.1f B in %.2f allocations\n", double(live_bytes - b0) / n, double(live_count - c0) / n);
#ifdef __APPLE__
	std::printf("slide 0x%llx\n", (unsigned long long)_dyld_get_image_vmaddr_slide(0));
#endif
	std::vector<std::pair<std::size_t, const site_rec*>> v;
	for (auto& [k, r] : *sites) { if (r.bytes) { v.emplace_back(r.bytes, &r); } }
	std::sort(v.rbegin(), v.rend());
	for (auto& [b, r] : v) {
		std::printf("SITE %.1f B %.2f allocs:", double(b) / n, double(r->count) / n);
		for (int i = 0; i < r->n; ++i) { std::printf(" %p", r->pcs[i]); }
		std::printf("\n");
	}
	return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: idle_index build ROOT COUNT DOCS [SEGMENTS] | open ROOT COUNT [shared] [query] [idle SECS] [obs] [tier hot|lazy|warm]\n");
		return 2;
	}
	const std::string cmd = argv[1];
#ifdef IDLE_COUNT
	if (cmd == "sites") { return open_sites(argv[2], std::strtoull(argv[3], nullptr, 10), argc > 4 && std::string(argv[4]) == "shared"); }
#endif
	const fs::path root = argv[2];
	const std::uint64_t count = std::strtoull(argv[3], nullptr, 10);
	if (cmd == "build") { return build(root, count, argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0, argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 1); }
	bool shared = false, query = false, obs = false;
	double idle = 0;
	for (int i = 4; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "shared") { shared = true; }
		else if (a == "query") { query = true; }
		else if (a == "obs") { obs = true; }
		else if (a == "idle" && i + 1 < argc) { idle = std::atof(argv[++i]); }
		else if (a == "tier" && i + 1 < argc) {
			const std::string t = argv[++i];
			open_tier = t == "hot" ? tier::hot : t == "lazy" ? tier::lazy : t == "warm" ? tier::warm : tier::none;
			if (open_tier == tier::none) { std::fprintf(stderr, "tier: hot, lazy or warm\n"); return 2; }
		}
	}
	return open_many(root, count, shared, query, idle, obs);
}
