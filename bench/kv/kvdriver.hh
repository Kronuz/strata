// The key-value benchmark's driver, shared by every engine (rocksdb-survey.md, section 4): it
// parses the row's settings, opens the engine, loads, runs the workload on T threads from
// kvgen.hh's streams, times every phase, and prints one JSON line: throughput, latency
// percentiles, CPU per operation, memory, disk, and bytes written.
//
// An engine is a class with:
//   explicit E(const kvbench::config&);        // opens (creates) the store under c.dir
//   void put(unsigned t, std::string_view key, std::string_view value);
//   bool get(unsigned t, std::string_view key, std::string& value);
//   std::uint64_t scan(unsigned t, std::string_view from, std::uint32_t n, std::string& sink);  // n 0: to the end
//   void durable(unsigned t);                   // after each write batch under durability=sync
//   void settle();                              // flush, finish merges, make everything durable
//   void reopen();                              // close and open again (the many-index row)
//   std::string stats();                        // engine counters, a JSON object
//   void on_phase(const char* name, durability); // optional: a phase begins
#pragma once

#include "kvgen.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#endif

namespace kvbench {

enum class durability { none, wal, sync };

struct config {
	std::string dir;
	workload w = workload::fillrandom;
	std::uint64_t records = 100000;
	std::uint64_t ops = 100000;
	std::size_t value_size = 100;
	unsigned threads = 1;
	durability dur = durability::none;
	unsigned batch = 1;           // writes per durable batch (sync)
	std::uint64_t memory = 256ull << 20;  // the equal memory budget, bytes
	std::uint64_t seed = 42;
	double max_seconds = 0;       // caps the run phase (slow durable rows)
	unsigned indexes = 1;         // the many-index row: K stores sharing the budget
	unsigned background = 2;      // background threads (merges, compactions, flushes)
	std::string variant;          // an engine configuration name (calibration)
};

// Log-linear latency histogram: 64 sub-buckets per power of two, about 1.6% resolution.
class histogram {
public:
	void add(std::uint64_t ns) noexcept {
		++counts_[index(ns)];
		++n_;
	}
	void merge(const histogram& o) noexcept {
		for (std::size_t i = 0; i < counts_.size(); ++i) { counts_[i] += o.counts_[i]; }
		n_ += o.n_;
	}
	double percentile(double p) const noexcept {
		if (!n_) { return 0; }
		const auto want = std::uint64_t(std::ceil(p * double(n_)));
		std::uint64_t seen = 0;
		for (std::size_t i = 0; i < counts_.size(); ++i) {
			seen += counts_[i];
			if (seen >= want) { return value(i); }
		}
		return value(counts_.size() - 1);
	}
	std::uint64_t count() const noexcept { return n_; }

private:
	static constexpr unsigned sub = 6;
	static std::size_t index(std::uint64_t v) noexcept {
		if (v < (1u << sub)) { return std::size_t(v); }
		const unsigned e = 63 - unsigned(__builtin_clzll(v));
		return (std::size_t(e - sub + 1) << sub) + std::size_t((v >> (e - sub)) & ((1u << sub) - 1));
	}
	static double value(std::size_t i) noexcept {
		if (i < (1u << sub)) { return double(i); }
		const unsigned e = unsigned(i >> sub) + sub - 1;
		const std::uint64_t m = (std::uint64_t(1) << sub) + (i & ((1u << sub) - 1));
		return double(m << (e - sub)) + double(std::uint64_t(1) << (e - sub)) / 2;
	}
	std::vector<std::uint64_t> counts_ = std::vector<std::uint64_t>(64 << sub);
	std::uint64_t n_ = 0;
};

struct usage {
	double cpu = 0;              // user + system seconds, every thread
	std::uint64_t written = 0;  // bytes the process wrote to storage
	std::uint64_t rss = 0;      // resident now
	std::uint64_t peak = 0;     // peak resident
};
inline usage sample() {
	usage u;
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	u.cpu = double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
#if defined(__APPLE__)
	u.peak = std::uint64_t(ru.ru_maxrss);
	rusage_info_v4 ri{};
	if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
		u.written = ri.ri_diskio_byteswritten;
		u.peak = std::max<std::uint64_t>(u.peak, ri.ri_lifetime_max_phys_footprint);
	}
	mach_task_basic_info info{};
	mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS) { u.rss = info.resident_size; }
#else
	u.peak = std::uint64_t(ru.ru_maxrss) * 1024;
	if (FILE* f = std::fopen("/proc/self/io", "r")) {
		char line[128];
		while (std::fgets(line, sizeof line, f)) {
			unsigned long long v;
			if (std::sscanf(line, "write_bytes: %llu", &v) == 1) { u.written = v; }
		}
		std::fclose(f);
	}
	if (FILE* f = std::fopen("/proc/self/statm", "r")) {
		unsigned long long size, res;
		if (std::fscanf(f, "%llu %llu", &size, &res) == 2) { u.rss = res * std::uint64_t(sysconf(_SC_PAGESIZE)); }
		std::fclose(f);
	}
#endif
	return u;
}

inline std::uint64_t disk_bytes(const std::string& dir, std::uint64_t* files = nullptr) {
	std::uint64_t n = 0, count = 0;
	std::error_code ec;
	for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
		if (e.is_regular_file(ec)) {
			n += e.file_size(ec);
			++count;
		}
	}
	if (files) { *files = count; }
	return n;
}

inline unsigned open_fds() {
	unsigned n = 0;
	std::error_code ec;
	for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/dev/fd", ec)) { ++n; }
	return n;
}

using clock = std::chrono::steady_clock;
inline double since(clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); }

struct phase_result {
	std::string name;
	double seconds = 0, cpu = 0;
	std::uint64_t ops = 0, written = 0, rss = 0;
	histogram lat;
	std::uint64_t logical = 0;  // key + value bytes written by the phase
	std::uint64_t touched = 0;  // value bytes read by the phase
};

inline void usage_exit(const char* argv0) {
	std::fprintf(stderr,
		"usage: %s --dir D --workload W [--records N] [--ops N] [--value-size B] [--threads T]\n"
		"          [--durability none|wal|sync] [--batch N] [--memory-mb M] [--seed S] [--max-seconds S]\n"
		"          [--indexes K] [--background N] [--variant NAME]\n",
		argv0);
	std::exit(2);
}

inline config parse_args(int argc, char** argv) {
	config c;
	for (int i = 1; i < argc; ++i) {
		const std::string_view a = argv[i];
		auto val = [&]() -> const char* {
			if (i + 1 >= argc) { usage_exit(argv[0]); }
			return argv[++i];
		};
		if (a == "--dir") {
			c.dir = val();
		} else if (a == "--workload") {
			if (!parse(val(), c.w)) { usage_exit(argv[0]); }
		} else if (a == "--records") {
			c.records = std::strtoull(val(), nullptr, 10);
		} else if (a == "--ops") {
			c.ops = std::strtoull(val(), nullptr, 10);
		} else if (a == "--value-size") {
			c.value_size = std::strtoull(val(), nullptr, 10);
		} else if (a == "--threads") {
			c.threads = unsigned(std::strtoul(val(), nullptr, 10));
		} else if (a == "--durability") {
			const std::string_view d = val();
			c.dur = d == "sync" ? durability::sync : d == "wal" ? durability::wal : durability::none;
		} else if (a == "--batch") {
			c.batch = std::max(1u, unsigned(std::strtoul(val(), nullptr, 10)));
		} else if (a == "--memory-mb") {
			c.memory = std::strtoull(val(), nullptr, 10) << 20;
		} else if (a == "--seed") {
			c.seed = std::strtoull(val(), nullptr, 10);
		} else if (a == "--max-seconds") {
			c.max_seconds = std::strtod(val(), nullptr);
		} else if (a == "--indexes") {
			c.indexes = std::max(1u, unsigned(std::strtoul(val(), nullptr, 10)));
		} else if (a == "--background") {
			c.background = unsigned(std::strtoul(val(), nullptr, 10));
		} else if (a == "--variant") {
			c.variant = val();
		} else {
			usage_exit(argv[0]);
		}
	}
	if (c.dir.empty() || !c.threads) { usage_exit(argv[0]); }
	return c;
}

// Runs one phase: each thread drains its stream; writes go in batches of c.batch under
// durability sync, each batch made durable before the next. A get's or a scan's value bytes
// are summed so no engine can skip copying them.
template <class Engine>
phase_result run_phase(Engine& e, const config& c, const char* name, const std::function<stream(unsigned)>& make, bool cap) {
	phase_result p;
	p.name = name;
	std::vector<histogram> lat(c.threads);
	std::vector<std::uint64_t> ops(c.threads), logical(c.threads), touched(c.threads);
	std::vector<std::uint64_t> missed(c.threads);
	const usage u0 = sample();
	const auto t0 = clock::now();
	const auto deadline = cap && c.max_seconds > 0 ? t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(c.max_seconds)) : clock::time_point::max();
	auto body = [&](unsigned t) {
		stream s = make(t);
		values vals(c.seed * 31 + t);
		std::string value, sink;
		unsigned pending = 0;
		char key[key_size];
		op o;
		while (s.next(o)) {
			make_key(o.id, key);
			const std::string_view k(key, key_size);
			const auto start = clock::now();
			switch (o.kind) {
			case op_kind::put:
			case op_kind::update:
			case op_kind::insert: {
				const auto v = vals.next(c.value_size);
				e.put(t, k, v);
				logical[t] += k.size() + v.size();
				if (c.dur == durability::sync && ++pending == c.batch) {
					e.durable(t);
					pending = 0;
				}
				break;
			}
			case op_kind::get:
				if (e.get(t, k, value)) {
					touched[t] += value.size();
				} else {
					++missed[t];
				}
				break;
			case op_kind::scan: {
				sink.clear();
				const std::string_view from = o.len == 0 ? std::string_view("Q") : k;
				touched[t] += e.scan(t, from, o.len, sink);
				break;
			}
			case op_kind::rmw: {
				if (e.get(t, k, value)) { touched[t] += value.size(); }
				const auto v = vals.next(c.value_size);
				e.put(t, k, v);
				logical[t] += k.size() + v.size();
				if (c.dur == durability::sync && ++pending == c.batch) {
					e.durable(t);
					pending = 0;
				}
				break;
			}
			}
			lat[t].add(std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count()));
			++ops[t];
			if ((ops[t] & 63) == 0 && clock::now() >= deadline) { break; }
		}
		if (c.dur == durability::sync && pending) { e.durable(t); }
	};
	if (c.threads == 1) {
		body(0);
	} else {
		std::vector<std::thread> th;
		for (unsigned t = 0; t < c.threads; ++t) { th.emplace_back(body, t); }
		for (auto& x : th) { x.join(); }
	}
	p.seconds = since(t0);
	const usage u1 = sample();
	p.cpu = u1.cpu - u0.cpu;
	p.written = u1.written - u0.written;
	p.rss = u1.rss;
	for (unsigned t = 0; t < c.threads; ++t) {
		p.lat.merge(lat[t]);
		p.ops += ops[t];
		p.logical += logical[t];
		p.touched += touched[t];
	}
	std::uint64_t miss = 0;
	for (auto m : missed) { miss += m; }
	if (c.w != workload::readmissing && miss && std::string_view(name) == "run") { std::fprintf(stderr, "warning: %" PRIu64 " gets missed\n", miss); }
	if (c.w == workload::readmissing && std::string_view(name) == "run" && miss != p.ops) { std::fprintf(stderr, "error: %" PRIu64 " absent keys found\n", p.ops - miss); std::exit(1); }
	return p;
}

inline std::string json_phase(const phase_result& p) {
	char b[1024];
	std::snprintf(b, sizeof b,
		"{\"name\":\"%s\",\"seconds\":%.6f,\"ops\":%" PRIu64 ",\"ops_per_sec\":%.1f,\"p50_us\":%.3f,\"p99_us\":%.3f,\"p999_us\":%.3f,"
		"\"cpu_s\":%.6f,\"cpu_us_per_op\":%.4f,\"written\":%" PRIu64 ",\"logical\":%" PRIu64 ",\"touched\":%" PRIu64 ",\"rss\":%" PRIu64 "}",
		p.name.c_str(), p.seconds, p.ops, p.seconds > 0 ? double(p.ops) / p.seconds : 0.0, p.lat.percentile(0.5) / 1e3, p.lat.percentile(0.99) / 1e3,
		p.lat.percentile(0.999) / 1e3, p.cpu, p.ops ? p.cpu * 1e6 / double(p.ops) : 0.0, p.written, p.logical, p.touched, p.rss);
	return b;
}

inline std::string json_timed(const char* name, double seconds, const usage& u0, const usage& u1) {
	char b[256];
	std::snprintf(b, sizeof b, "{\"name\":\"%s\",\"seconds\":%.6f,\"cpu_s\":%.6f,\"written\":%" PRIu64 ",\"rss\":%" PRIu64 "}", name, seconds, u1.cpu - u0.cpu,
		u1.written - u0.written, u1.rss);
	return b;
}

// Marks a phase boundary: the engine's optional `on_phase`, and with KV_PHASE_MARKS set a
// `phase NAME NS` line on stderr (CLOCK_MONOTONIC_RAW) that barrier traces align to.
template <class Engine>
void begin_phase(Engine& e, const char* name, durability d) {
	if (std::getenv("KV_PHASE_MARKS")) {
		timespec ts{};
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		std::fprintf(stderr, "phase %s %llu\n", name, (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec);
	}
	if constexpr (requires { e.on_phase(name, d); }) { e.on_phase(name, d); }
}

// The row: open, load (every workload but fill*), settle, run, settle; for the many-index
// row also close and reopen, then run. Prints one JSON object on stdout.
template <class Engine>
int main_of(int argc, char** argv) {
	const config c = parse_args(argc, argv);
	const auto start = clock::now();
	std::vector<std::string> phases;
	std::filesystem::remove_all(c.dir);
	std::filesystem::create_directories(c.dir);
	std::filesystem::permissions(c.dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	usage u0 = sample();
	auto t0 = clock::now();
	Engine e(c);
	phases.push_back(json_timed("open", since(t0), u0, sample()));
	std::uint64_t logical = 0, written = 0;
	const bool fill = is_fill(c.w);
	if (!fill) {
		config lc = c;
		lc.dur = durability::none;
		begin_phase(e, "load", lc.dur);
		auto p = run_phase(e, lc, "load", [&](unsigned t) { return stream::load(c.seed, t, c.threads, c.records); }, false);
		logical += p.logical;
		written += p.written;
		phases.push_back(json_phase(p));
		u0 = sample();
		t0 = clock::now();
		begin_phase(e, "load_settle", lc.dur);
		e.settle();
		const usage u1 = sample();
		written += u1.written - u0.written;
		phases.push_back(json_timed("load_settle", since(t0), u0, u1));
	}
	if (c.indexes > 1) {
		u0 = sample();
		t0 = clock::now();
		begin_phase(e, "reopen", c.dur);
		e.reopen();
		phases.push_back(json_timed("reopen", since(t0), u0, sample()));
	}
	begin_phase(e, "run", c.dur);
	auto run = run_phase(e, c, "run", [&](unsigned t) { return stream(c.w, c.seed, t, c.threads, c.records, c.ops / c.threads); }, true);
	logical += run.logical;
	written += run.written;
	phases.push_back(json_phase(run));
	// KV_PAUSE_AFTER_RUN=S: sleep S seconds here, so a heap tool can inspect the buffer.
	if (const char* pause = std::getenv("KV_PAUSE_AFTER_RUN")) { std::this_thread::sleep_for(std::chrono::seconds(std::atoi(pause))); }
	// A run that wrote nothing has nothing to settle.
	u0 = sample();
	t0 = clock::now();
	begin_phase(e, "settle", c.dur);
	if (run.logical) { e.settle(); }
	const usage u1 = sample();
	begin_phase(e, "end", c.dur);
	written += u1.written - u0.written;
	phases.push_back(json_timed("settle", since(t0), u0, u1));
	std::uint64_t files = 0;
	const std::uint64_t disk = disk_bytes(c.dir, &files);
	const unsigned fds = open_fds();
	const std::string stats = e.stats();
	const double total = since(start);
	std::printf("{\"engine\":\"%s\",\"variant\":\"%s\",\"workload\":\"%s\",\"records\":%" PRIu64 ",\"ops\":%" PRIu64
				",\"value_size\":%zu,\"threads\":%u,\"durability\":\"%s\",\"batch\":%u,\"memory\":%" PRIu64 ",\"indexes\":%u,\"background\":%u,\"seed\":%" PRIu64
				",\"phases\":[",
		Engine::name, c.variant.c_str(), name_of(c.w), c.records, c.ops, c.value_size, c.threads,
		c.dur == durability::sync ? "sync" : c.dur == durability::wal ? "wal" : "none", c.batch, c.memory, c.indexes, c.background, c.seed);
	for (std::size_t i = 0; i < phases.size(); ++i) { std::printf("%s%s", i ? "," : "", phases[i].c_str()); }
	std::printf("],\"peak_rss\":%" PRIu64 ",\"end_rss\":%" PRIu64 ",\"disk\":%" PRIu64 ",\"files\":%" PRIu64 ",\"fds\":%u,\"logical\":%" PRIu64 ",\"written\":%" PRIu64
				",\"total_seconds\":%.3f,\"stats\":%s}\n",
		sample().peak, u1.rss, disk, files, fds, logical, written, total, stats.c_str());
	std::fflush(stdout);
	return 0;
}

}  // namespace kvbench
