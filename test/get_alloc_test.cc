// Gets allocate nothing per call (format-14-kv step 1, K4): get(key) allocates the string it
// returns and nothing else, get(key, f) nothing at all, once a thread's first gets have sized
// its decode buffer and codec contexts. This program replaces the global allocation functions
// to count every allocation while it measures; it runs over faultfs, read (and, with
// STRATA_FAULTFS_MAP=1, mapped), with LZ4 blocks and with the zstd disk preset.
#include "kv_test.hh"

#include <atomic>
#include <cstdlib>
#include <new>
#include <random>

// A sanitizer's runtime owns the allocation functions (Linux links them statically), so under
// one this program runs its gets without counting.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STRATA_COUNT_ALLOCATIONS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define STRATA_COUNT_ALLOCATIONS 0
#endif
#endif
#ifndef STRATA_COUNT_ALLOCATIONS
#define STRATA_COUNT_ALLOCATIONS 1
#endif

static std::atomic<bool> counting{false};
static std::atomic<std::uint64_t> allocations{0};

#if STRATA_COUNT_ALLOCATIONS
static void* counted(std::size_t n, std::size_t align = 0) {
	if (counting.load(std::memory_order_relaxed)) { allocations.fetch_add(1, std::memory_order_relaxed); }
	void* p = nullptr;
	if (align > alignof(std::max_align_t)) {
		if (posix_memalign(&p, align, n ? n : 1) != 0) { p = nullptr; }
	} else {
		p = std::malloc(n ? n : 1);
	}
	return p;
}
void* operator new(std::size_t n) {
	if (void* p = counted(n)) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
	if (void* p = counted(n)) { return p; }
	throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a) {
	if (void* p = counted(n, std::size_t(a))) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) {
	if (void* p = counted(n, std::size_t(a))) { return p; }
	throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

static const auto uuid = uuid_of("get-alloc");

// The allocations f makes.
template <class F>
static std::uint64_t allocations_in(F&& f) {
	allocations.store(0);
	counting.store(true);
	f();
	counting.store(false);
	return allocations.load();
}

static void run(bool preset) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.keyed = true;
	o.zstd_merged = preset;
	auto ix = kv_index::create(io, o, uuid);
	std::mt19937_64 r(preset ? 2 : 1);
	auto key_of = [](std::size_t i) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%07zu", i);
		return std::string(k);
	};
	const std::size_t n = 20000;
	std::vector<std::string> values(n);
	// Five segments, the first three merged into one, then a buffer of unflushed writes.
	for (std::size_t s = 0; s < 5; ++s) {
		for (std::size_t i = s; i < n; i += 5) {
			values[i] = std::string(100 + r() % 200, char('a' + r() % 26)) + std::to_string(i);
			CHECK(ix.apply(tx({replace(key_of(i), values[i])}), {}).result == verdict::accepted);
		}
		ix.refresh();
		if (s == 2) { ix.force_merge(1); }
	}
	ix.commit();
	for (std::size_t i = 0; i < n; i += 97) {
		values[i] = std::string(64, 'b') + std::to_string(i);
		CHECK(ix.apply(tx({replace(key_of(i), values[i])}), {}).result == verdict::accepted);
	}
	CHECK(ix.segments().size() == 3 && ix.buffered_count() > 100);
	// Warm: every key once, sizing this thread's buffer and contexts and verifying every region.
	for (std::size_t i = 0; i < n; ++i) { CHECK(ix.get(key_of(i)) == values[i]); }
	std::vector<std::string> keys;
	for (std::size_t i = 0; i < 50000; ++i) { keys.push_back(key_of(r() % (n + n / 4))); }  // a fifth absent
	std::size_t hits = 0, viewed = 0;
	std::uint64_t checksum = 0;
	const std::uint64_t by_value = allocations_in([&] {
		for (const auto& k : keys) {
			if (const auto v = ix.get(k)) {
				++hits;
				checksum += v->size();
			}
		}
	});
	const std::uint64_t by_view = allocations_in([&] {
		for (const auto& k : keys) {
			viewed += ix.get(k, [&](std::string_view v) { checksum -= v.size(); });
		}
	});
	std::printf("  %s: %zu gets, %zu hits: get(key) %llu allocations, get(key, f) %llu\n", preset ? "zstd preset" : "lz4", keys.size(), hits,
		(unsigned long long)by_value, (unsigned long long)by_view);
	CHECK(hits > 30000 && hits < keys.size() && viewed == hits && checksum == 0);
	if (STRATA_COUNT_ALLOCATIONS) {
		CHECK(by_value == hits);  // the returned strings, each past the small-string buffer
		CHECK(by_view == 0);
	}
	// A get inside another's callback takes a buffer of its own; both see their records.
	ix.get(key_of(1), [&](std::string_view outer) {
		const std::string before(outer);
		CHECK(ix.get(key_of(2)) == values[2]);
		CHECK(outer == before && outer == values[1]);
	});
	ix.close();
}

int main() {
	if (!STRATA_COUNT_ALLOCATIONS) { std::printf("  under a sanitizer: gets run, allocations not counted\n"); }
	run(false);
	run(true);
	std::printf("get_alloc: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
