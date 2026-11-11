// Process meters and fixture naming shared by bench/idle_index and bench/catalog_tiers
// (docs/perf/idle-index.md): resident, dirty footprint, descriptors, mappings and threads of this
// process, latency percentiles, and the fixture's key and directory layout.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace idle_bench {

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

inline std::string key_of(std::uint64_t i) {
	char k[16];
	std::snprintf(k, sizeof k, "Q%07llu", (unsigned long long)i);
	return k;
}

inline fs::path dir_of(const fs::path& root, std::uint64_t i) { return root / ("g" + std::to_string(i / 1000)) / ("i" + std::to_string(i)); }

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
inline meters take() {
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
inline double kb_of(const char* path, const char* key) {
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
inline long count_dir(const char* p) {
	long n = 0;
	if (DIR* d = ::opendir(p)) {
		while (::readdir(d)) { ++n; }
		::closedir(d);
	}
	return n - 2;
}
inline meters take() {
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

inline void report(const char* what, const meters& b, const meters& a, double n) {
	std::printf("%-14s rss %8.0f dirty %8.0f anon %8.0f file %8.0f vsize %10.0f B/index | fds %.3f maps %.3f threads %+ld (%ld total) wired %.0f\n", what, (a.rss - b.rss) / n, (a.dirty - b.dirty) / n,
		(a.anon - b.anon) / n, (a.file - b.file) / n, (a.vsize - b.vsize) / n, double(a.fds - b.fds) / n, double(a.maps - b.maps) / n, a.threads - b.threads, a.threads, (a.wired - b.wired) / n);
}

struct pct {
	double p50 = 0, p99 = 0, mean = 0, max = 0;
};
inline pct summarize(std::vector<double>& v) {
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
inline void print_pct(const char* what, pct p) { std::printf("%-14s p50 %9.1f us p99 %9.1f us mean %9.1f us max %10.1f us\n", what, p.p50, p.p99, p.mean, p.max); }
inline double us(clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }

}  // namespace idle_bench
