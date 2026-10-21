// The flush sizes its writer before the first document: with 100,000 documents in one run the
// docids and the stored offsets are each one allocation of exactly 800,000 bytes and the borrowed
// records' views one of 1,600,000, where growing by doubling would allocate neither size. This
// program replaces the global allocation functions to count those sizes while a commit flushes.
// A sanitizer's runtime owns the allocation functions, so under one it runs the flush uncounted.
#include "kv_test.hh"

#include <atomic>
#include <cstdlib>
#include <new>

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

static constexpr std::size_t docs = 100000;
static std::atomic<bool> counting{false};
static std::atomic<std::uint64_t> exact_ids{0}, exact_views{0};

#if STRATA_COUNT_ALLOCATIONS
static void* counted(std::size_t n, std::size_t align = 0) {
	if (counting.load(std::memory_order_relaxed)) {
		if (n == docs * sizeof(std::uint64_t)) { exact_ids.fetch_add(1, std::memory_order_relaxed); }
		if (n == docs * sizeof(std::string_view)) { exact_views.fetch_add(1, std::memory_order_relaxed); }
	}
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

int main() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "Q";
	auto ix = kv_index::create(io, o, uuid_of("flush-alloc"));
	for (std::size_t d = 0; d < docs; ++d) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%07zu", d);
		CHECK(ix.apply(tx({replace(k, "v")}), {}).result == verdict::accepted);
	}
	counting = true;
	ix.commit();
	counting = false;
	CHECK(ix.segments().size() == 1 && ix.segments()[0].seg->core().doc_count() == docs);
#if STRATA_COUNT_ALLOCATIONS
	// Two allocations of the docids' size and one of the views' come from elsewhere in the flush;
	// the writer's hint adds two and one more (without it: 2 and 1).
	CHECK(exact_ids.load() >= 4);
	CHECK(exact_views.load() >= 2);
#endif
	if (failures) { return 1; }
	std::puts("flush_alloc: ok");
	return 0;
}
