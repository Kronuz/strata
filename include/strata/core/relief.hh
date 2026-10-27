// Returns freed allocator pages to the OS after a flush or merge (STRATA_RELIEF=<min ms between calls>).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace strata::detail {

inline void relieve_memory() noexcept {
	static const long period_ms = [] { const char* e = std::getenv("STRATA_RELIEF"); return e ? std::atol(e) : -1L; }();
	if (period_ms < 0) { return; }
	static std::atomic<long> last{-1000000};
	const long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	long prev = last.load(std::memory_order_relaxed);
	if (now - prev < period_ms || !last.compare_exchange_strong(prev, now)) { return; }
#if defined(__APPLE__)
	malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
	malloc_trim(0);
#endif
}

struct relief_on_exit { ~relief_on_exit() { relieve_memory(); } };

}  // namespace strata::detail
