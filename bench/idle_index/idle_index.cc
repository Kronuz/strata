// The cost of one idle open index (docs/perf/idle-index.md).
//   idle_index build ROOT COUNT DOCS      COUNT indexes of DOCS documents: one real index, then
//                                         copy-on-write clones of its files (macOS clonefile,
//                                         Linux FICLONE or copy), COUNT/1000 groups of 1000
//   idle_index open  ROOT COUNT [shared] [query] [idle SECS] [obs]
//                                         opens them all in this process, one thread, and prints
//                                         resident, dirty footprint, descriptors, mappings,
//                                         threads, wakeups, and open/query/close latency
// Documents are "Q%07d" keys with 100-byte values, in the document layout. `shared` also holds
// a reader handle per index on one registry (a host's shape); `query` runs one get per index.
#include "strata/core/budget.hh"
#include "strata/core/hazard.hh"
#include "strata/core/index.hh"

#include "completion/posix.h"

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

#ifdef IDLE_COUNT
// Counts live heap requested through operator new, and keeps the allocation sites of the
// allocations made while `profile` is set (backtrace addresses, symbolized afterwards).
#include <execinfo.h>
#include <map>
static std::atomic<std::size_t> live_bytes{0}, live_count{0};
static bool profile = false;
struct site_rec { std::size_t bytes = 0, count = 0; void* pcs[8]; int n = 0; };
static std::map<std::uint64_t, site_rec>* sites = nullptr;
static std::uint64_t site_key(void* const* pcs, int n) {
	std::uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < n; ++i) { h = (h ^ reinterpret_cast<std::uint64_t>(pcs[i])) * 1099511628211ull; }
	return h;
}
void* operator new(std::size_t n) {
	auto* p = static_cast<std::size_t*>(std::malloc(n + 16));
	if (!p) { throw std::bad_alloc(); }
	p[0] = n;
	p[1] = 0;
	live_bytes += n; ++live_count;
	if (profile) {
		profile = false;
		void* pcs[12];
		const int d = backtrace(pcs, 12);
		const int skip = 1, n8 = std::min(8, d - skip);
		const std::uint64_t key = site_key(pcs + skip, n8);
		auto& r = (*sites)[key];
		p[1] = key;
		r.bytes += n; ++r.count; r.n = n8;
		std::memcpy(r.pcs, pcs + skip, sizeof(void*) * std::size_t(n8));
		profile = true;
	}
	return p + 2;
}
void operator delete(void* q) noexcept {
	if (!q) { return; }
	auto* p = static_cast<std::size_t*>(q) - 2;
	live_bytes -= p[0]; --live_count;
	if (p[1] && sites) {
		const bool was = profile;
		profile = false;
		auto it = sites->find(p[1]);
		if (it != sites->end()) { it->second.bytes -= p[0]; --it->second.count; }
		profile = was;
	}
	std::free(p);
}
void operator delete(void* q, std::size_t) noexcept { operator delete(q); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* q) noexcept { operator delete(q); }
void operator delete[](void* q, std::size_t) noexcept { operator delete(q); }
#endif

namespace fs = std::filesystem;
using namespace strata;
using kv_index = basic_index<document_layout>;
using clk = std::chrono::steady_clock;

namespace {

std::string key_of(std::uint64_t i) {
	char k[16];
	std::snprintf(k, sizeof k, "Q%07llu", (unsigned long long)i);
	return k;
}

std::string value_of(std::uint64_t i, std::size_t n) {
	std::string v(n, 'a');
	std::uint64_t x = i * 0x9E3779B97F4A7C15ull + 1;
	for (auto& c : v) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		c = char('a' + x % 26);
	}
	return v;
}

fs::path dir_of(const fs::path& root, std::uint64_t i) { return root / ("g" + std::to_string(i / 1000)) / ("i" + std::to_string(i)); }

kv_index::options base_options(byte_budget* b, reader_registry* r) {
	kv_index::options o;
	o.keyed = true;
	o.budget = b;
	o.buffer_share = std::uint64_t(1) << 30;
	o.readers = r;
	return o;
}

// --- process meters ---------------------------------------------------------------------------

struct meters {
	double rss = 0;        // resident bytes
	double dirty = 0;      // private dirty / physical footprint bytes (anonymous, not reclaimable)
	double anon = 0;       // anonymous resident
	double file = 0;       // file-backed resident
	double vsize = 0;      // mapped virtual bytes
	long fds = 0;
	long maps = 0;         // mapped regions
	long threads = 0;
	double cpu = 0;        // user + system seconds
	long vcsw = 0, ivcsw = 0;
	double wired = 0;      // system-wide wired bytes (macOS: kernel memory, includes this process's mappings)
};

#ifdef __APPLE__
meters take() {
	meters m;
	task_vm_info_data_t v{};
	mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
	task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&v), &c);
	m.rss = double(v.resident_size);
	m.dirty = double(v.phys_footprint);
	m.anon = double(v.internal);
	m.file = double(v.external);
	m.vsize = double(v.virtual_size);
	{
		static std::vector<proc_fdinfo> buf;
		const int need = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
		buf.resize(std::size_t(need) / sizeof(proc_fdinfo) + 64);
		m.fds = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, buf.data(), int(buf.size() * sizeof(proc_fdinfo))) / int(sizeof(proc_fdinfo));
	}
	mach_vm_address_t a = 0;
	for (;;) {
		mach_vm_size_t sz = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t ic = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t obj;
		if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &ic, &obj) != KERN_SUCCESS) { break; }
		++m.maps;
		a += sz;
	}
	thread_act_array_t th;
	mach_msg_type_number_t tc = 0;
	if (task_threads(mach_task_self(), &th, &tc) == KERN_SUCCESS) {
		m.threads = tc;
		for (mach_msg_type_number_t i = 0; i < tc; ++i) { mach_port_deallocate(mach_task_self(), th[i]); }
		vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(th), tc * sizeof(thread_t));
	}
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	m.cpu = double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
	m.vcsw = ru.ru_nvcsw; m.ivcsw = ru.ru_nivcsw;
	{
		vm_statistics64_data_t vs{};
		mach_msg_type_number_t vc = HOST_VM_INFO64_COUNT;
		if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vs), &vc) == KERN_SUCCESS) { m.wired = double(vs.wire_count) * double(vm_page_size); }
	}
	return m;
}
#else
static double kb_of(const char* path, const char* key) {
	FILE* f = std::fopen(path, "r");
	if (!f) { return 0; }
	char line[256];
	double r = 0;
	const std::size_t kl = std::strlen(key);
	while (std::fgets(line, sizeof line, f)) {
		if (!std::strncmp(line, key, kl) && line[kl] == ':') { r = std::atof(line + kl + 1) * 1024; break; }
	}
	std::fclose(f);
	return r;
}
static long count_dir(const char* p) {
	long n = 0;
	if (DIR* d = ::opendir(p)) {
		while (::readdir(d)) { ++n; }
		::closedir(d);
	}
	return n - 2;
}
meters take() {
	meters m;
	m.rss = kb_of("/proc/self/status", "VmRSS");
	m.anon = kb_of("/proc/self/status", "RssAnon");
	m.file = kb_of("/proc/self/status", "RssFile");
	m.vsize = kb_of("/proc/self/status", "VmSize");
	m.dirty = kb_of("/proc/self/smaps_rollup", "Private_Dirty") + kb_of("/proc/self/smaps_rollup", "Shared_Dirty");
	m.fds = count_dir("/proc/self/fd") - 1;
	m.threads = count_dir("/proc/self/task");
	if (FILE* f = std::fopen("/proc/self/maps", "r")) {
		int ch;
		while ((ch = std::fgetc(f)) != EOF) { m.maps += ch == '\n'; }
		std::fclose(f);
	}
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	m.cpu = double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
	m.vcsw = ru.ru_nvcsw; m.ivcsw = ru.ru_nivcsw;
	return m;
}
#endif

void report(const char* what, const meters& b, const meters& a, double n) {
	std::printf("%-14s rss %8.0f dirty %8.0f anon %8.0f file %8.0f vsize %10.0f B/index | fds %.3f maps %.3f threads %+ld (%ld total) wired %.0f\n", what, (a.rss - b.rss) / n, (a.dirty - b.dirty) / n,
		(a.anon - b.anon) / n, (a.file - b.file) / n, (a.vsize - b.vsize) / n, double(a.fds - b.fds) / n, double(a.maps - b.maps) / n, a.threads - b.threads, a.threads, (a.wired - b.wired) / n);
}

struct pct {
	double p50 = 0, p99 = 0, mean = 0, max = 0;
};
pct summarize(std::vector<double>& v) {
	pct r;
	if (v.empty()) { return r; }
	std::sort(v.begin(), v.end());
	r.p50 = v[v.size() / 2];
	r.p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
	r.max = v.back();
	double s = 0;
	for (double x : v) { s += x; }
	r.mean = s / double(v.size());
	return r;
}
void print_pct(const char* what, pct p) { std::printf("%-14s p50 %9.1f us p99 %9.1f us mean %9.1f us max %10.1f us\n", what, p.p50, p.p99, p.mean, p.max); }
double us(clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }

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

int build(const fs::path& root, std::uint64_t count, std::uint64_t docs) {
	fs::create_directories(root);
	const fs::path tmpl = root / "template";
	fs::remove_all(tmpl);
	fs::create_directories(tmpl);
	{
		kronuz::journal::PosixIO io(tmpl, 0600, kronuz::journal::PosixIOOptions{true, true});
		kv_index ix = kv_index::create(io, base_options(nullptr, nullptr), std::array<char, 16>{'i', 'd', 'l', 'e'});
		kv_index::transaction tx;
		tx.ops.resize(1);
		tx.ops[0].type = kv_index::operation::replace_key;
		for (std::uint64_t d = 0; d < docs; ++d) {
			auto& op = tx.ops[0];
			op.key = key_of(d);
			op.doc.key = op.key;
			op.doc.stored.assign(value_of(d, 100));
			if (ix.apply(tx, {}).result != verdict::accepted) { std::abort(); }
		}
		ix.commit();
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
	std::uint64_t opened = 0;
	const auto topen = clk::now();
	for (std::uint64_t i = 0; i < count; ++i) {
		const auto a = clk::now();
		try {
			slots[i].io = std::make_unique<kronuz::journal::PosixIO>(dir_of(root, i), 0600, kronuz::journal::PosixIOOptions{true, true});
			slots[i].ix = std::make_unique<kv_index>(kv_index::open(*slots[i].io, base_options(&budget, shared ? &registry : nullptr)));
			if (shared) { slots[i].rd = slots[i].ix->reader(); }
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
		std::fprintf(stderr, "usage: idle_index build ROOT COUNT DOCS | open ROOT COUNT [shared] [query] [idle SECS] [obs]\n");
		return 2;
	}
	const std::string cmd = argv[1];
#ifdef IDLE_COUNT
	if (cmd == "sites") { return open_sites(argv[2], std::strtoull(argv[3], nullptr, 10), argc > 4 && std::string(argv[4]) == "shared"); }
#endif
	const fs::path root = argv[2];
	const std::uint64_t count = std::strtoull(argv[3], nullptr, 10);
	if (cmd == "build") { return build(root, count, argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0); }
	bool shared = false, query = false, obs = false;
	double idle = 0;
	for (int i = 4; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "shared") { shared = true; }
		else if (a == "query") { query = true; }
		else if (a == "obs") { obs = true; }
		else if (a == "idle" && i + 1 < argc) { idle = std::atof(argv[++i]); }
	}
	return open_many(root, count, shared, query, idle, obs);
}
