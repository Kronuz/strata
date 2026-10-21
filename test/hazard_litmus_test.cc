// T8, the ordering litmus (shared-reader SR-D4): hazard.hh's own steps against the owner's
// publish and scan, natively, counting free-while-read outcomes: the reader validated the old
// pointer while the owner's scan, which ran after the owner published the new one, missed the
// reader's record. Two shapes per iteration, each the store-load pair of the review's
// sb_litmus.cc: a claim (acquire(), its compare-exchange then its validating load) and a retry
// (settle(), its store into a record it owns then its validating load).
//
// The protocol's orders must give zero. The test then runs the same steps with release stores and
// acquire loads, and with only the reader's side weakened (the weakened-order mutants), which allow
// the store-load reorder; on x86_64 the retry shape must catch them, or the test proves nothing on
// this host and fails. (The claim's
// compare-exchange is a locked instruction on x86_64 at any order, so only the retry store can
// show it there.) ThreadSanitizer does not reorder stores, so under a sanitizer it runs briefly
// and asserts only the protocol's zero.
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

// The mutants: every store of the handshake as release and every load as acquire, the order the
// review measured (sb_litmus.cc); and the reader's side alone weakened.
struct weak_orders {
	static constexpr std::memory_order claim = std::memory_order_release;
	static constexpr std::memory_order validate = std::memory_order_acquire;
	static constexpr std::memory_order publish = std::memory_order_release;
	static constexpr std::memory_order scan = std::memory_order_acquire;
};
struct weak_reader_orders : strata::hazard_orders {
	static constexpr std::memory_order claim = std::memory_order_release;
	static constexpr std::memory_order validate = std::memory_order_acquire;
};

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
	flag go, done, ack, released;
	const int* r1 = nullptr;  // what the reader validated
	std::atomic<bool> quit{false};
	std::thread reader([&] {
		for (std::uint64_t i = 1;; ++i) {
			await(go, i);
			if (quit.load(std::memory_order_relaxed)) { return; }
			if (sh == shape::claim) {
				auto g = reg.acquire(current);
				r1 = g.get();
				done.v.store(i, std::memory_order_release);
				await(ack, i);
				g.release();
				released.v.store(i, std::memory_order_release);
			} else {
				r1 = registry::settle(reg.slot(0), &x, current);  // its record names x; the pointer moved on
				done.v.store(i, std::memory_order_release);
			}
		}
	});
	std::uint64_t bad = 0, i = 1;
	for (; i <= iters && bad < stop_at; ++i) {
		current.store(&a, std::memory_order_relaxed);
		reg.slot(0).p.store(sh == shape::claim ? nullptr : &x, std::memory_order_relaxed);
		go.v.store(i, std::memory_order_release);
		registry::publish(current, &b);
		const void* seen = nullptr;
		reg.scan(i, [&](const void* p) { seen = p; });
		await(done, i);
		if (r1 == &a && seen != &a) { ++bad; }
		if (sh == shape::claim) {
			ack.v.store(i, std::memory_order_release);
			await(released, i);
		}
	}
	quit.store(true, std::memory_order_relaxed);
	go.v.store(i, std::memory_order_release);
	reader.join();
	return bad;
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t iters = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : (STRATA_SANITIZED ? 20000 : 10000000);
	int failures = 0;
	auto timed = [](auto f) {
		const auto t0 = std::chrono::steady_clock::now();
		const std::uint64_t r = f();
		return std::pair{r, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()};
	};
	const auto [claim_bad, claim_s] = timed([&] { return run<strata::hazard_orders>(shape::claim, iters / 4, ~std::uint64_t(0)); });
	const auto [retry_bad, retry_s] = timed([&] { return run<strata::hazard_orders>(shape::retry, iters, ~std::uint64_t(0)); });
	std::printf("seq_cst: claim %llu iterations, free-while-read %llu (%.1f s); retry %llu iterations, free-while-read %llu (%.1f s)\n",
		(unsigned long long)(iters / 4), (unsigned long long)claim_bad, claim_s, (unsigned long long)iters, (unsigned long long)retry_bad, retry_s);
	if (claim_bad || retry_bad) {
		std::fprintf(stderr, "FAIL: the protocol's orders let a reader validate a view the owner's scan missed\n");
		++failures;
	}
	const auto [weak_bad, weak_s] = timed([&] { return run<weak_orders>(shape::retry, iters, 20); });
	const auto [weak_claim_bad, weak_claim_s] = timed([&] { return run<weak_orders>(shape::claim, iters / 40, 20); });
	const auto [reader_bad, reader_s] = timed([&] { return run<weak_reader_orders>(shape::retry, iters, 20); });
	std::printf("mutant (release stores, acquire loads): retry free-while-read %llu (%.1f s), claim %llu (%.1f s); reader side only: retry %llu (%.1f s)\n",
		(unsigned long long)weak_bad, weak_s, (unsigned long long)weak_claim_bad, weak_claim_s, (unsigned long long)reader_bad, reader_s);
#if defined(__x86_64__) && !STRATA_SANITIZED
	// x86_64 allows a store to pass a later load from another address unless a locked op or
	// mfence orders them, so the weakened retry store must be caught here.
	if (!weak_bad && !reader_bad) {
		std::fprintf(stderr, "FAIL: the litmus saw no store-load reorder of the weakened orders on this x86_64 host; it proves nothing here\n");
		++failures;
	}
#endif
	if (failures) { return 1; }
	std::printf("hazard litmus: ok\n");
	return 0;
}
