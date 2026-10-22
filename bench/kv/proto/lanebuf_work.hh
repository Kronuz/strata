// The bake-off's workloads (docs/design/write-buffer.md T1 to T3, T11), engine-agnostic so the
// RocksDB adapter runs the same loops as the prototype. An engine is a class with:
//   using ticket = ...;                                  // a checkout of one index
//   ticket checkout(unsigned index);                     // never exclusive, never waits
//   bool put(ticket&, string_view key, string_view value);
//   bool get(ticket&, string_view key, std::string& out);
//   std::size_t scan(ticket&, string_view from, unsigned n, std::uint64_t& bytes);  // seek + n nexts
//   void checkin(ticket&);
//   std::string stats_json();                            // engine counters, a JSON object
//   void quiesce();                                      // after loads: wait for background work
#pragma once

#include "../kvdriver.hh"

#include <cstdarg>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace lbwork {

using kvbench::histogram;
using sv = std::string_view;
using u64 = std::uint64_t;

struct args {
	std::map<std::string, std::string> m;
	args(int argc, char** argv) {
		for (int i = 1; i + 1 < argc; i += 2) { m[argv[i]] = argv[i + 1]; }
	}
	std::string str(const char* k, const char* d) const {
		auto it = m.find(k);
		return it == m.end() ? d : it->second;
	}
	u64 num(const char* k, u64 d) const {
		auto it = m.find(k);
		return it == m.end() ? d : std::strtoull(it->second.c_str(), nullptr, 10);
	}
	double real(const char* k, double d) const {
		auto it = m.find(k);
		return it == m.end() ? d : std::strtod(it->second.c_str(), nullptr);
	}
};

inline std::vector<std::string> split(const std::string& s) {
	std::vector<std::string> out;
	std::stringstream ss(s);
	std::string t;
	while (std::getline(ss, t, ',')) {
		if (!t.empty()) { out.push_back(t); }
	}
	return out;
}

// Runs fn(t) on n threads released together; returns the wall seconds between release and join.
template <class F>
double run_threads(unsigned n, F&& fn) {
	std::atomic<unsigned> ready{0};
	std::atomic<bool> go{false};
	std::vector<std::thread> th;
	for (unsigned t = 0; t < n; ++t) {
		th.emplace_back([&, t] {
			ready.fetch_add(1);
			while (!go.load(std::memory_order_acquire)) { std::this_thread::yield(); }
			fn(t);
		});
	}
	while (ready.load() < n) { std::this_thread::yield(); }
	const auto t0 = kvbench::clock::now();
	go.store(true, std::memory_order_release);
	for (auto& t : th) { t.join(); }
	return kvbench::since(t0);
}

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
	va_list ap, ap2;
	va_start(ap, f);
	va_copy(ap2, ap);
	const int n = std::vsnprintf(nullptr, 0, f, ap);
	va_end(ap);
	std::string out(std::size_t(n > 0 ? n : 0) + 1, '\0');
	std::vsnprintf(out.data(), out.size(), f, ap2);
	va_end(ap2);
	out.resize(std::size_t(n > 0 ? n : 0));
	return out;
}

// T1: fillrandom / overwrite on one index shared by W writers.
template <class E>
std::string fill(E& e, const args& a) {
	const std::string w = a.str("--workload", "fillrandom");
	const unsigned W = unsigned(a.num("--writers", 1));
	const u64 N = a.num("--records", 200000), ops = a.num("--ops", N), vsz = a.num("--value", 100);
	if constexpr (requires { e.seed(unsigned(0), u64(0), u64(0)); }) { e.seed(unsigned(a.num("--segments", 0)), a.num("--seg-records", 100000), vsz); }
	if (w == "overwrite") {
		auto tk = e.checkout(0);
		kvbench::values gen(7);
		for (u64 i = 0; i < N; ++i) { e.put(tk, kvbench::key_of(i), gen.next(vsz)); }
		e.checkin(tk);
		e.quiesce();
	}
	const auto u0 = kvbench::sample();
	std::atomic<u64> done{0};
	const double secs = run_threads(W, [&](unsigned t) {
		auto tk = e.checkout(0);
		kvbench::values gen(100 + t);
		kvbench::rng r(1000 + t);
		const u64 per = ops / W;
		u64 n = 0;
		for (u64 i = 0; i < per; ++i) {
			const u64 id = w == "overwrite" ? r.below(N) : u64(t) * per + i;
			n += e.put(tk, kvbench::key_of(id), gen.next(vsz)) ? 1 : 0;
		}
		done.fetch_add(n);
		e.checkin(tk);
	});
	const auto u1 = kvbench::sample();
	const u64 total = done.load();
	const double growth = w == "overwrite" ? double(ops) : double(total);
	return fmt("{\"workload\":\"%s\",\"writers\":%u,\"value\":%llu,\"records\":%llu,\"ops\":%llu,\"seconds\":%.4f,\"ops_per_sec\":%.0f,\"ns_per_op\":%.1f,"
		"\"cpu_sec\":%.4f,\"cpu_ns_per_op\":%.1f,\"rss_per_new_record\":%.1f,\"engine\":%s}",
		w.c_str(), W, (unsigned long long)vsz, (unsigned long long)N, (unsigned long long)total, secs, double(total) / secs, secs * 1e9 / double(total) * W, u1.cpu - u0.cpu,
		(u1.cpu - u0.cpu) * 1e9 / double(total), double(u1.rss - u0.rss) / growth, e.stats_json().c_str());
}

// T2/T3: readers beside writers on one index. kind get: readrandom's gets; scan: seek + 10 nexts.
template <class E>
std::string rww(E& e, const args& a) {
	const unsigned W = unsigned(a.num("--writers", 1)), R = unsigned(a.num("--readers", 1));
	const u64 N = a.num("--records", 200000), vsz = a.num("--value", 100);
	const double D = a.real("--seconds", 3), mbs = a.real("--write-mb-s", 0);
	const std::string kind = a.str("--kind", "get");
	if constexpr (requires { e.seed(unsigned(0), u64(0), u64(0)); }) { e.seed(unsigned(a.num("--segments", 0)), a.num("--seg-records", 100000), vsz); }
	{
		auto tk = e.checkout(0);
		kvbench::values gen(7);
		for (u64 i = 0; i < N; ++i) { e.put(tk, kvbench::key_of(i), gen.next(vsz)); }
		e.checkin(tk);
		e.quiesce();
	}
	std::atomic<bool> stop{false};
	std::vector<histogram> hist(R);
	std::vector<u64> rops(R), wops(W + 1), chain(R);
	const auto u0 = kvbench::sample();
	std::thread timer([&] {
		std::this_thread::sleep_for(std::chrono::duration<double>(D));
		stop.store(true);
	});
	const double secs = run_threads(W + R, [&](unsigned t) {
		auto tk = e.checkout(0);
		if (t < W) {
			kvbench::values gen(100 + t);
			kvbench::rng r(1000 + t);
			const double per_s = mbs > 0 ? mbs * 1e6 / double(16 + vsz) / double(W) : 0;
			const auto t0 = kvbench::clock::now();
			u64 n = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				e.put(tk, kvbench::key_of(r.below(N)), gen.next(vsz));
				++n;
				if (per_s > 0) {
					while (!stop.load(std::memory_order_relaxed) && double(n) > per_s * kvbench::since(t0)) { std::this_thread::sleep_for(std::chrono::microseconds(50)); }
				}
			}
			wops[t] = n;
		} else {
			const unsigned ri = t - W;
			kvbench::rng r(5000 + t);
			std::string out;
			u64 bytes = 0, n = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				const auto t0 = kvbench::clock::now();
				if (kind == "get") {
					e.get(tk, kvbench::key_of(r.below(N)), out);
				} else {
					e.scan(tk, kvbench::key_of(r.below(N)), 10, bytes);
				}
				hist[ri].add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(kvbench::clock::now() - t0).count()));
				++n;
			}
			rops[ri] = n;
			asm volatile("" ::"r"(bytes));
		}
		e.checkin(tk);
	});
	timer.join();
	const auto u1 = kvbench::sample();
	histogram all;
	u64 rt = 0, wt = 0;
	for (unsigned i = 0; i < R; ++i) {
		all.merge(hist[i]);
		rt += rops[i];
	}
	for (unsigned i = 0; i < W; ++i) { wt += wops[i]; }
	return fmt("{\"kind\":\"%s\",\"writers\":%u,\"readers\":%u,\"write_mb_s\":%.1f,\"seconds\":%.3f,\"read_ops_per_sec\":%.0f,\"write_ops_per_sec\":%.0f,"
		"\"p50_ns\":%.0f,\"p99_ns\":%.0f,\"p999_ns\":%.0f,\"cpu_sec\":%.3f,\"engine\":%s}",
		kind.c_str(), W, R, mbs, secs, double(rt) / secs, double(wt) / secs, all.percentile(0.5), all.percentile(0.99), all.percentile(0.999), u1.cpu - u0.cpu,
		e.stats_json().c_str());
}

// T11: request threads over many small indexes. A request checks an index out, runs 4
// operations, and checks it in.
template <class E>
struct requests {
	E& e;
	unsigned K;
	std::vector<u64> n;  // records per index
	u64 vsz;
	explicit requests(E& eng, const args& a) : e(eng), K(unsigned(a.num("--indexes", 1000))), vsz(a.num("--value", 1024)) {
		const u64 lo = a.num("--min-records", 1000), hi = a.num("--max-records", 10000);
		kvbench::rng r(99);
		n.resize(K);
		for (auto& x : n) { x = lo + r.below(hi - lo + 1); }
	}
	void preload(unsigned threads) {
		run_threads(threads, [&](unsigned t) {
			kvbench::values gen(300 + t);
			for (unsigned i = t; i < K; i += threads) {
				if constexpr (requires { e.load_index(i, n[i], vsz, gen); }) {
					e.load_index(i, n[i], vsz, gen);
				} else {
					auto tk = e.checkout(i);
					for (u64 k = 0; k < n[i]; ++k) { e.put(tk, kvbench::key_of(k), gen.next(vsz)); }
					e.checkin(tk);
				}
			}
		});
		e.quiesce();
	}
	std::string run(unsigned R, const std::string& dist, const std::string& mix, double D) {
		kvbench::zipfian zipf(K, 0.99);
		std::atomic<bool> stop{false};
		std::vector<histogram> hist(R);
		std::vector<u64> cnt(R);
		const auto u0 = kvbench::sample();
		const std::string st0 = e.stats_json();
		std::thread timer([&] {
			std::this_thread::sleep_for(std::chrono::duration<double>(D));
			stop.store(true);
		});
		const bool read_heavy = mix == "1w3r";
		const double secs = run_threads(R, [&](unsigned t) {
			kvbench::rng r(7000 + t);
			kvbench::values gen(900 + t);
			std::string out;
			u64 bytes = 0, c = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				const unsigned i = dist == "zipf" ? unsigned(zipf.next(r)) : unsigned(r.below(K));
				const auto t0 = kvbench::clock::now();
				auto tk = e.checkout(i);
				const u64 span = n[i] + n[i] / 10;
				if (read_heavy) {
					const u64 id = r.below(span);
					e.put(tk, kvbench::key_of(id), gen.next(vsz));
					e.get(tk, kvbench::key_of(id), out);
					e.get(tk, kvbench::key_of(r.below(n[i])), out);
					e.get(tk, kvbench::key_of(r.below(n[i])), out);
				} else {
					e.put(tk, kvbench::key_of(r.below(span)), gen.next(vsz));
					e.put(tk, kvbench::key_of(r.below(span)), gen.next(vsz));
					e.put(tk, kvbench::key_of(r.below(span)), gen.next(vsz));
					e.get(tk, kvbench::key_of(r.below(n[i])), out);
				}
				e.checkin(tk);
				hist[t].add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(kvbench::clock::now() - t0).count()));
				++c;
			}
			cnt[t] = c;
			asm volatile("" ::"r"(bytes));
		});
		timer.join();
		const auto u1 = kvbench::sample();
		histogram all;
		u64 total = 0;
		for (unsigned t = 0; t < R; ++t) {
			all.merge(hist[t]);
			total += cnt[t];
		}
		return fmt("{\"requests\":%u,\"dist\":\"%s\",\"mix\":\"%s\",\"seconds\":%.3f,\"req_per_sec\":%.0f,\"p50_us\":%.2f,\"p99_us\":%.2f,\"p999_us\":%.2f,"
			"\"cpu_us_per_req\":%.3f,\"indexes\":%u,\"records\":%llu,\"engine_before\":%s,\"engine_after\":%s}",
			R, dist.c_str(), mix.c_str(), secs, double(total) / secs, all.percentile(0.5) / 1e3, all.percentile(0.99) / 1e3, all.percentile(0.999) / 1e3,
			(u1.cpu - u0.cpu) * 1e6 / double(total), K, (unsigned long long)total_records(), st0.c_str(), e.stats_json().c_str());
	}
	u64 total_records() const {
		u64 s = 0;
		for (u64 x : n) { s += x; }
		return s;
	}
};

}  // namespace lbwork
