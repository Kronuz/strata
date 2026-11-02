// The chunked hazard registry (shared-reader revision 4) against the fixed one it replaced: the cost
// of an acquire and release on one published pointer at 1 and N threads, and of an owner's scan
// over 4, 64 and 1,024 claimed records. The fixed arm is the previous implementation, kept here
// for the comparison: H records allocated up front, a probe from a per-thread hint, the same
// protocol and orders. The chunked arm is strata::reader_registry.
//   registry [threads...] [seconds per arm]   default: 1 8, 1 s
#include "strata/core/hazard.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace strata;

namespace {

class fixed_registry {
public:
	struct alignas(hazard_line) record {
		std::atomic<const void*> p{nullptr};
	};
	explicit fixed_registry(std::size_t h) : recs_(new record[h]), h_(h), seen_(h) {}
	// Claims a record naming the pointer `current` holds and validates it; returns the record.
	record* acquire(const std::atomic<const int*>& current) {
		const int* v = current.load(std::memory_order_seq_cst);
		std::size_t& hint_ = hint();
		std::size_t i = hint_ % h_;
		record* r = nullptr;
		for (std::size_t probed = 0; probed < h_; ++probed) {
			const void* expected = nullptr;
			if (!recs_[i].p.load(std::memory_order_relaxed) && recs_[i].p.compare_exchange_strong(expected, v, std::memory_order_seq_cst, std::memory_order_relaxed)) {
				hint_ = i;
				r = &recs_[i];
				break;
			}
			if (++i == h_) { i = 0; }
		}
		if (!r) { std::abort(); }
		for (;;) {
			const int* w = current.load(std::memory_order_seq_cst);
			if (w == v) { return r; }
			r->p.store(w, std::memory_order_seq_cst);
			v = w;
		}
	}
	static void release(record* r) { r->p.store(nullptr, std::memory_order_release); }
	template <class F>
	void scan(std::uint64_t now, F&& named) {
		for (std::size_t i = 0; i < h_; ++i) {
			if (const void* p = recs_[i].p.load(std::memory_order_seq_cst)) { named(p); }
		}
		const std::unique_lock stamp(stamp_mu_, std::try_to_lock);
		if (!stamp) { return; }
		std::size_t claimed = 0;
		for (std::size_t i = 0; i < h_; ++i) {
			const void* p = recs_[i].p.load(std::memory_order_relaxed);
			if (p != seen_[i].p) { seen_[i] = {p, now}; }
			claimed += p != nullptr;
		}
		if (claimed > high_water_) { high_water_ = claimed; }
	}

private:
	static std::size_t& hint() noexcept {
		static std::atomic<std::size_t> next{0};
		thread_local std::size_t h = next.fetch_add(1, std::memory_order_relaxed);
		return h;
	}
	struct seen {
		const void* p = nullptr;
		std::uint64_t since = 0;
	};
	std::unique_ptr<record[]> recs_;
	std::size_t h_;
	std::mutex stamp_mu_;
	std::vector<seen> seen_;
	std::size_t high_water_ = 0;
};

using clock_type = std::chrono::steady_clock;
std::atomic<std::uint64_t> sink{0};

// Runs body() in a loop on `threads` threads for `secs`; returns ns per call per thread.
template <class Body>
double per_op(int threads, double secs, Body body) {
	std::atomic<bool> go{false}, stop{false};
	std::vector<std::uint64_t> ops(std::size_t(threads) * 16, 0);
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			while (!go.load(std::memory_order_acquire)) {}
			std::uint64_t n = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				for (int k = 0; k < 64; ++k) { n += body(); }
			}
			ops[std::size_t(t) * 16] = n;
		});
	}
	const auto t0 = clock_type::now();
	go.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::duration<double>(secs));
	stop.store(true);
	for (auto& t : ts) { t.join(); }
	const double ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	std::uint64_t total = 0;
	for (int t = 0; t < threads; ++t) { total += ops[std::size_t(t) * 16]; }
	return ns * threads / double(total);
}

// Single-threaded: ns per scan, over `secs`.
template <class Reg>
double scan_ns(Reg& reg, double secs) {
	std::uint64_t n = 0, now = 0, named = 0;
	const auto t0 = clock_type::now();
	while (std::chrono::duration<double>(clock_type::now() - t0).count() < secs) {
		for (int k = 0; k < 64; ++k) { reg.scan(++now, [&](const void*) { ++named; }); }
		n += 64;
	}
	const double ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	sink.fetch_add(named);
	return ns / double(n);
}

}  // namespace

int main(int argc, char** argv) {
	std::vector<int> counts;
	double secs = 1.0;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]).find('.') != std::string::npos) {
			secs = std::atof(argv[i]);
		} else {
			counts.push_back(std::atoi(argv[i]));
		}
	}
	if (counts.empty()) { counts = {1, 8}; }
	static const int view = 1;
	std::atomic<const int*> current{&view};

	std::printf("registry: line %zu, chunk %zu records, %zu bytes\n", hazard_line, reader_registry::chunk_records(), reader_registry::chunk_bytes());
	std::printf("acquire+release on one pointer, ns per op per thread\n threads  fixed(H=64)  chunked(cap 64)\n");
	for (int t : counts) {
		fixed_registry fixed(64);
		reader_registry chunked(64);
		const double f = per_op(t, secs, [&] {
			auto* r = fixed.acquire(current);
			fixed_registry::release(r);
			return std::uint64_t(1);
		});
		const double c = per_op(t, secs, [&] {
			auto g = chunked.acquire(current);
			return std::uint64_t(g ? 1 : 0);
		});
		std::printf("%8d  %11.1f  %15.1f\n", t, f, c);
	}
	std::printf("owner scan with N records claimed (the stamping pass included), ns per scan\n   claimed  fixed(H=N)  chunked  fixed(H=1024)\n");
	for (std::size_t n : {std::size_t(4), std::size_t(64), std::size_t(1024)}) {
		fixed_registry exact(n), big(1024);
		reader_registry chunked(1024);
		std::vector<fixed_registry::record*> held_exact, held_big;  // fixed arms keep their records claimed
		std::vector<reader_registry::guard<int>> gs;
		for (std::size_t i = 0; i < n; ++i) {
			held_exact.push_back(exact.acquire(current));
			held_big.push_back(big.acquire(current));
			gs.push_back(chunked.acquire(current));
		}
		const double e = scan_ns(exact, secs / 2), c = scan_ns(chunked, secs / 2), b = scan_ns(big, secs / 2);
		std::printf("%10zu  %10.1f  %7.1f  %13.1f\n", n, e, c, b);
	}
	std::printf("(sink %llu)\n", (unsigned long long)sink.load());
	return 0;
}
