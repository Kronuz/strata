// T8, the ordering litmus (shared-reader SR-D4): hazard.hh's own steps against the owner's
// publish and scan, natively, counting free-while-read outcomes: the reader validated the old
// pointer while the owner's scan, which ran after the owner published the new one, missed the
// reader's record. Two shapes per iteration, each the store-load pair of the review's
// sb_litmus.cc: a claim (acquire(), its compare-exchange then its validating load) and a retry
// (settle(), its store into a record it owns then its validating load). The reader and the owner
// start each iteration from one start word, so both sides' store-load pairs race (CR3a).
//
// The protocol's orders must give zero. The mutants weaken one side or both: the reader's record
// write as release (with its validation as acquire), the owner's publish as release (alone, and
// with its scan as acquire), and every order. On x86_64 each must be caught, or the test proves
// nothing on this host and fails. Weakening only a load (the validation, or the scan) cannot show
// on x86_64, where loads keep their order and the adjacent store or locked instruction fences
// them, so those are left to the arm64 jobs, which report without failing. The claim's
// compare-exchange is a locked instruction on x86_64 at any order, so the reader's mutant shows
// only in the retry shape there. ThreadSanitizer does not reorder stores, so under a sanitizer it
// runs briefly and asserts only the protocol's zero.
#include "strata/core/hazard.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STRATA_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define STRATA_SANITIZED 1
#endif
#endif
#ifndef STRATA_SANITIZED
#define STRATA_SANITIZED 0
#endif

namespace {

template <std::memory_order C, std::memory_order V, std::memory_order P, std::memory_order S>
struct orders {
	static constexpr std::memory_order claim = C, validate = V, publish = P, scan = S;
};
constexpr auto sc = std::memory_order_seq_cst, rel = std::memory_order_release, acq = std::memory_order_acquire;

struct alignas(128) flag {
	std::atomic<std::uint64_t> v{0};
};

// Spins, yielding now and then, so an oversubscribed host still makes progress.
inline void await(const flag& f, std::uint64_t want) {
	for (unsigned spins = 0; f.v.load(std::memory_order_acquire) != want;) {
		if (++spins % 4096 == 0) { std::this_thread::yield(); }
	}
}

enum class shape { claim, retry };

// Runs `iters` iterations (stopping early at `stop_at` free-while-read outcomes); returns them.
template <class Orders>
std::uint64_t run(shape sh, std::uint64_t iters, std::uint64_t stop_at) {
	using registry = strata::basic_reader_registry<Orders>;
	static const int x = 0, a = 1, b = 2;  // a stale pointer, the old view, the new view
	registry reg(1);  // one record: every claim and scan meets on it
	std::atomic<const int*> current{&a};
	flag start, rdone, odone, ack, released;
	const int* r1 = nullptr;    // what the reader validated
	const void* seen = nullptr;  // what the owner's scan saw in the record
	std::atomic<bool> quit{false};
	std::thread reader([&] {
		for (std::uint64_t i = 1;; ++i) {
			await(start, i);
			if (quit.load(std::memory_order_relaxed)) { return; }
			if (sh == shape::claim) {
				auto g = reg.acquire(current);
				r1 = g.get();
				rdone.v.store(i, std::memory_order_release);
				await(ack, i);
				g.release();
				released.v.store(i, std::memory_order_release);
			} else {
				r1 = registry::settle(reg.slot(0), &x, current);  // its record names x; the pointer moved on
				rdone.v.store(i, std::memory_order_release);
			}
		}
	});
	std::thread owner([&] {
		for (std::uint64_t i = 1;; ++i) {
			await(start, i);
			if (quit.load(std::memory_order_relaxed)) { return; }
			registry::publish(current, &b);
			const void* s = nullptr;
			reg.scan(i, [&](const void* p) { s = p; });
			seen = s;
			odone.v.store(i, std::memory_order_release);
		}
	});
	std::uint64_t bad = 0, i = 1;
	for (; i <= iters && bad < stop_at; ++i) {
		current.store(&a, std::memory_order_relaxed);
		reg.slot(0).p.store(sh == shape::claim ? nullptr : &x, std::memory_order_relaxed);
		start.v.store(i, std::memory_order_release);
		await(rdone, i);
		await(odone, i);
		if (r1 == &a && seen != &a) { ++bad; }
		if (sh == shape::claim) {
			ack.v.store(i, std::memory_order_release);
			await(released, i);
		}
	}
	quit.store(true, std::memory_order_relaxed);
	start.v.store(i, std::memory_order_release);
	reader.join();
	owner.join();
	return bad;
}

struct outcome {
	std::uint64_t retry, claim;
};
template <class Orders>
outcome both(std::uint64_t iters, std::uint64_t stop_at) {
	return {run<Orders>(shape::retry, iters, stop_at), run<Orders>(shape::claim, iters / 4, stop_at)};
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t iters = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : (STRATA_SANITIZED ? 20000 : 10000000);
	int failures = 0;
	const auto t0 = std::chrono::steady_clock::now();
	const outcome ok = both<strata::hazard_orders>(iters, ~std::uint64_t(0));
	std::printf("seq_cst: retry %llu iterations, free-while-read %llu; claim %llu iterations, free-while-read %llu (%.1f s)\n", (unsigned long long)iters,
		(unsigned long long)ok.retry, (unsigned long long)(iters / 4), (unsigned long long)ok.claim,
		std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	if (ok.retry || ok.claim) {
		std::fprintf(stderr, "FAIL: the protocol's orders let a reader validate a view the owner's scan missed\n");
		++failures;
	}
	struct mutant {
		const char* name;
		outcome (*run)(std::uint64_t, std::uint64_t);
		std::uint64_t iters;
		bool must_fail_x86;  // a store weakened: x86_64 shows it
	};
	const std::uint64_t cap = 20;
	const mutant mutants[] = {
		{"reader: record write release, validation acquire", both<orders<rel, acq, sc, sc>>, iters, true},
		{"reader: record write release only", both<orders<rel, sc, sc, sc>>, iters, true},
		{"owner: publish release only", both<orders<sc, sc, rel, sc>>, iters, true},
		{"owner: publish release, scan acquire", both<orders<sc, sc, rel, acq>>, iters, true},
		{"every store release, every load acquire", both<orders<rel, acq, rel, acq>>, iters, true},
		{"reader: validation acquire only", both<orders<sc, acq, sc, sc>>, iters / 10, false},
		{"owner: scan acquire only", both<orders<sc, sc, sc, acq>>, iters / 10, false},
	};
	int must = 0, caught = 0;
	for (const auto& m : mutants) {
		outcome got = m.run(m.iters, cap);
#if defined(__x86_64__) && !STRATA_SANITIZED
		// A reordering window is probabilistic; a busy shared runner can miss it once.
		for (int attempt = 1; m.must_fail_x86 && !got.retry && !got.claim && attempt < 4; ++attempt) { got = m.run(m.iters, cap); }
#endif
		std::printf("mutant %-48s retry %2llu  claim %2llu  (cap %llu)\n", m.name, (unsigned long long)got.retry, (unsigned long long)got.claim,
			(unsigned long long)cap);
#if defined(__x86_64__) && !STRATA_SANITIZED
		if (m.must_fail_x86) {
			++must;
			if (got.retry || got.claim) {
				++caught;
			} else {
				std::fprintf(stderr, "warning: \"%s\" not caught in 4 runs; the window is probabilistic\n", m.name);
			}
		}
#endif
	}
	if (failures) { return 1; }
	// A host that shows none of the weakened orders can't judge the protocol: skip, don't pass.
	if (must && !caught) {
		std::printf("hazard litmus: SKIP, this host showed no weakened order\n");
		return 77;
	}
	std::printf("hazard litmus: ok\n");
	return 0;
}
