// Per-window latency percentiles, an open-loop pacer, and a key=value argument parser,
// shared by the strata and RocksDB drivers of docs/research/no-stalls.md. A sample lands
// in the one-second window of its completion; a window's percentiles are taken once it is
// closed, so a stall shows up in the windows it spans, never as an average.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nostalls {

using clock = std::chrono::steady_clock;

inline double secs(clock::time_point a, clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }
inline double micros(clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }

struct pct {
	std::size_t n = 0;
	double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

// Percentiles of `v` (sorted here) by the nearest-rank rule.
inline pct percentiles(std::vector<double>& v) {
	pct p;
	p.n = v.size();
	if (v.empty()) { return p; }
	std::sort(v.begin(), v.end());
	auto at = [&](double q) { return v[std::min(v.size() - 1, std::size_t(std::ceil(q * double(v.size()))) - (q > 0 ? 1 : 0))]; };
	p.p50 = at(0.50);
	p.p99 = at(0.99);
	p.p999 = at(0.999);
	p.max = v.back();
	return p;
}

// Samples by window: window w holds completions in [t0 + w·len, t0 + (w + 1)·len).
class windows {
public:
	explicit windows(clock::time_point t0, double len_s = 1.0) : t0_(t0), len_(len_s) {}
	void add(clock::time_point done, double latency_us) {
		const auto w = std::size_t(std::max(0.0, secs(t0_, done) / len_));
		std::lock_guard l(mu_);
		if (w >= by_.size()) { by_.resize(w + 1); }
		by_[w].push_back(latency_us);
	}
	// Window w's percentiles; its samples are released.
	pct take(std::size_t w) {
		std::vector<double> v;
		{
			std::lock_guard l(mu_);
			if (w < by_.size()) { v.swap(by_[w]); }
		}
		return percentiles(v);
	}

private:
	clock::time_point t0_;
	double len_;
	std::mutex mu_;
	std::vector<std::vector<double>> by_;
};

// key=value arguments with defaults.
class args {
public:
	args(int argc, char** argv, int first) {
		for (int i = first; i < argc; ++i) {
			std::string_view a(argv[i]);
			const auto eq = a.find('=');
			if (eq == std::string_view::npos) { continue; }
			kv_.emplace_back(std::string(a.substr(0, eq)), std::string(a.substr(eq + 1)));
		}
	}
	std::string get(std::string_view k, std::string_view d) const {
		for (const auto& [a, b] : kv_) {
			if (a == k) { return b; }
		}
		return std::string(d);
	}
	double num(std::string_view k, double d) const {
		const auto s = get(k, "");
		return s.empty() ? d : std::strtod(s.c_str(), nullptr);
	}
	std::uint64_t u64(std::string_view k, std::uint64_t d) const { return std::uint64_t(num(k, double(d))); }

private:
	std::vector<std::pair<std::string, std::string>> kv_;
};

// Waits until `t`: sleeps to 2 ms before it (a sleep overshoots by 1 to 9 ms on macOS),
// then spins, so a schedule is kept to tens of µs; a late caller returns at once.
inline void wait_until(clock::time_point t) {
	const auto coarse = t - std::chrono::milliseconds(2);
	if (coarse > clock::now()) { std::this_thread::sleep_until(coarse); }
	while (clock::now() < t) {}
}

// Prints a percentile set as four tab-separated µs columns after the count.
inline void print_pct(const pct& p) { std::printf("\t%zu\t%.0f\t%.0f\t%.0f\t%.0f", p.n, p.p50, p.p99, p.p999, p.max); }

}  // namespace nostalls
