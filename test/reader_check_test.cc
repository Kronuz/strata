// The stored check decodes each frame once (stored-check-cache): over a segment of many
// documents per frame, stored_plugin::check allocates per frame, not per document, and its
// report is unchanged. This program replaces the global allocation functions to count them.
#include "kv_test.hh"
#include "segment_edit.hh"
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

// A stored frame whose record count runs into the next frame's ordinals is corrupt: the check
// refuses it, so it agrees with point reads (stored-frame-bound).
struct raw_frame {
	std::uint32_t first, raw_size;
	std::string body;  // codec byte and payload
};

static std::string stored_section(const std::vector<raw_frame>& fs) {
	std::string out, index;
	for (const auto& f : fs) {
		format::put<std::uint32_t>(index, f.first);
		format::put<std::uint32_t>(index, f.raw_size);
		format::put<std::uint64_t>(index, out.size());
		out += f.body;
	}
	out += index;
	format::put<std::uint32_t>(out, std::uint32_t(fs.size()));
	return out;
}

static std::vector<raw_frame> parse_stored(const std::string& s) {
	const std::uint32_t n = format::get<std::uint32_t>(s.data() + s.size() - 4);
	const std::size_t index_at = s.size() - 4 - 16 * std::size_t(n);
	std::vector<raw_frame> fs;
	for (std::uint32_t f = 0; f < n; ++f) {
		const char* e = s.data() + index_at + 16 * f;
		const std::uint64_t begin = format::get<std::uint64_t>(e + 8);
		const std::uint64_t end = f + 1 < n ? format::get<std::uint64_t>(e + 24) : index_at;
		fs.push_back({format::get<std::uint32_t>(e), format::get<std::uint32_t>(e + 4), s.substr(begin, end - begin)});
	}
	return fs;
}

// Frame 0 rewritten (raw codec) to hold `extra` more records, empty ones.
static raw_frame extend_frame(const raw_frame& f0, std::uint32_t extra) {
	const char* p = f0.body.data() + 1;
	const char* end = f0.body.data() + f0.body.size();
	const std::uint64_t n = format::get_leb(&p, end);
	std::vector<std::uint64_t> lens;
	for (std::uint64_t i = 0; i < n; ++i) { lens.push_back(format::get_leb(&p, end)); }
	const std::string data(p, end);
	std::string raw;
	format::put_leb(raw, n + extra);
	for (auto l : lens) { format::put_leb(raw, l); }
	for (std::uint32_t i = 0; i < extra; ++i) { format::put_leb(raw, 0); }
	raw += data;
	return {f0.first, std::uint32_t(raw.size()), std::string(1, char(format::codec::raw)) + raw};
}

static std::string overlap_segment_source() {
	writer w;
	w.structures = format::bit::keys | format::bit::stored;
	w.stored_block = 256;
	w.stored_codec = format::stored_codec{format::codec::raw, 0};
	for (int i = 0; i < 40; ++i) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%06d", i);
		basic_document<document_layout> d;
		d.key = k;
		d.stored = "record-" + std::to_string(i) + std::string(40, char('a' + i % 26));
		w.add({std::uint64_t(i + 1), 1, k}, d, false);
	}
	return w.flush({});
}

// Frame 0 claims frame 1's ordinals as empty records; frame 1 stays valid, or gets a bad codec byte.
static void check_overlap(const char* what, bool break_frame1) {
	const std::string good = overlap_segment_source();
	auto fs = parse_stored(test::section_bytes(good, format::section::stored));
	CHECK(fs.size() >= 3 && fs[0].body[0] == char(format::codec::raw));
	if (fs.size() < 3) { return; }
	fs[0] = extend_frame(fs[0], fs[2].first - fs[1].first);
	if (break_frame1) { fs[1].body[0] = char(0x7f); }
	const segment s(test::reseal(good, {{format::section::stored, stored_section(fs)}}));
	bool threw = false;
	try {
		basic_check_report<document_layout> r;
		check_segment(s, nullptr, r);
	} catch (const corrupt_segment&) { threw = true; }
	CHECK(threw);
	std::printf("  %s: check_segment %s\n", what, threw ? "refuses the overlap" : "ACCEPTS the overlap");
}

int main() {
	check_frames("empty records", 6000, [](std::uint64_t) { return 0; });
	check_frames("1-byte records", 6000, [](std::uint64_t) { return 1; });
	check_frames("mixed records", 6000, [](std::uint64_t i) { return i % 97; });
	check_overlap("frame 1 valid", false);
	check_overlap("frame 1 bad codec", true);
	std::printf("reader_check: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
