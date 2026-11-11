// The stored check decodes each frame once (stored-check-cache): over a segment of many
// documents per frame, stored_plugin::check allocates per frame, not per document, and its
// report is unchanged. This program replaces the global allocation functions to count them.
#include "kv_test.hh"
#include "strata/core/check.hh"

#include <atomic>
#include <cstdlib>
#include <new>

// A sanitizer's runtime owns the allocation functions, so under one only the report is checked.
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

using segment = basic_segment<document_layout>;
using writer = basic_segment_writer<document_layout>;

// Checks a segment of `docs` documents whose stored records are `size(i)` bytes each.
template <class Size>
static void check_frames(const char* what, std::uint64_t docs, Size&& size) {
	writer w;
	w.structures = format::bit::keys | format::bit::stored;
	std::uint64_t bytes = 0;
	for (std::uint64_t i = 0; i < docs; ++i) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%06llu", static_cast<unsigned long long>(i));
		document d;
		d.stored = std::string(size(i), 'v');
		bytes += d.stored.size();
		w.add({i + 1, 1, k}, d, false);
	}
	const std::string file = w.flush({});
	const segment s(file);
	const auto& stored = s.get<stored_plugin>();
	const std::uint32_t frames = stored.stored_frames();
	CHECK(frames > 0 && docs / frames >= 8);  // many documents per frame, or the test proves nothing
	stored_plugin::report r;
	allocations.store(0);
	counting.store(true);
	stored_plugin::check(stored, s.core(), r);
	counting.store(false);
	const std::uint64_t n = allocations.load();
	CHECK(r.stored_bytes == bytes);
	// One frame buffer, grown at most twice per frame (payload, then its offset table).
	if (STRATA_COUNT_ALLOCATIONS) { CHECK(n <= 2 * std::uint64_t(frames) + 2); }
	std::printf("  %s: %llu documents in %u frames, %llu allocations\n", what, static_cast<unsigned long long>(docs), frames, static_cast<unsigned long long>(n));
}

int main() {
	check_frames("empty records", 6000, [](std::uint64_t) { return 0; });
	check_frames("1-byte records", 6000, [](std::uint64_t) { return 1; });
	check_frames("mixed records", 6000, [](std::uint64_t i) { return i % 97; });
	std::printf("reader_check: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
