// The counting build's heap meter, shared by bench/idle_index and bench/catalog_tiers: under IDLE_COUNT,
// global operator new counts live heap requested, and keeps the allocation sites of the allocations
// made while `profile` is set (backtrace addresses, symbolized afterwards). Include in one file per binary.
#pragma once

#ifdef IDLE_COUNT
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <map>
#include <new>
static std::atomic<std::size_t> live_bytes{0}, live_count{0};
static std::atomic<bool> profile{false};
static thread_local bool in_site = false;  // this thread is inside the recorder: its own allocations go unrecorded
static std::atomic_flag site_lock = ATOMIC_FLAG_INIT;  // the pool's threads record too
struct site_rec { std::size_t bytes = 0, count = 0; void* pcs[8]; int n = 0; };
static std::map<std::uint64_t, site_rec>* sites = nullptr;
static std::uint64_t site_key(void* const* pcs, int n) {
	std::uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < n; ++i) { h = (h ^ reinterpret_cast<std::uint64_t>(pcs[i])) * 1099511628211ull; }
	return h;
}
struct site_guard {
	site_guard() {
		in_site = true;
		while (site_lock.test_and_set(std::memory_order_acquire)) {}
	}
	~site_guard() {
		site_lock.clear(std::memory_order_release);
		in_site = false;
	}
};
void* operator new(std::size_t n) {
	auto* p = static_cast<std::size_t*>(std::malloc(n + 16));
	if (!p) { throw std::bad_alloc(); }
	p[0] = n;
	p[1] = 0;
	live_bytes += n; ++live_count;
	if (profile.load(std::memory_order_relaxed) && !in_site) {
		void* pcs[12];
		const int d = backtrace(pcs, 12);
		const int skip = 1, n8 = std::min(8, d - skip);
		const std::uint64_t key = site_key(pcs + skip, n8);
		const site_guard g;
		auto& r = (*sites)[key];
		p[1] = key;
		r.bytes += n; ++r.count; r.n = n8;
		std::memcpy(r.pcs, pcs + skip, sizeof(void*) * std::size_t(n8));
	}
	return p + 2;
}
void operator delete(void* q) noexcept {
	if (!q) { return; }
	auto* p = static_cast<std::size_t*>(q) - 2;
	live_bytes -= p[0]; --live_count;
	if (p[1] && sites && !in_site) {
		const site_guard g;
		auto it = sites->find(p[1]);
		if (it != sites->end()) { it->second.bytes -= p[0]; --it->second.count; }
	}
	std::free(p);
}
void operator delete(void* q, std::size_t) noexcept { operator delete(q); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* q) noexcept { operator delete(q); }
void operator delete[](void* q, std::size_t) noexcept { operator delete(q); }
#endif
