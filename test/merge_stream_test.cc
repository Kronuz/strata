// A merge streams its stored section to the output block by block instead of building it whole:
// the working memory is a block and the key builders' state, not the merged segment. The bytes
// stay what the whole-section merge wrote: pinned digests of the merged files, taken from main
// before the change (recipe: compile this file's digest part against `git archive main include`).
#include "kv_test.hh"
#include "strata/core/check.hh"
#include "strata/core/merge.hh"

#include <cstdio>
#include <deque>
#include <random>
#include <span>

using segment = basic_segment<document_layout>;
using writer = basic_segment_writer<document_layout>;

namespace {

std::uint64_t fnv(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ull) {
	for (unsigned char c : s) { h = (h ^ c) * 0x100000001b3ull; }
	return h;
}

// A value of `size` bytes: `random` ones do not compress, the others mostly do.
std::string value_of(std::mt19937_64& r, std::size_t size, bool random) {
	std::string v(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { v[i] = random ? char(r()) : (i < size / 3 ? char('a' + r() % 26) : char('a' + i % 5)); }
	return v;
}

struct shape {
	std::size_t block;
	format::stored_codec codec;
	bool random;
	std::vector<std::size_t> sizes;  // record sizes, cycled
};

struct inputs {
	std::vector<std::string> files;
	std::vector<std::vector<std::uint64_t>> deleted;
	std::size_t stored_bytes = 0;
};

// Three segments of key-ordered documents with overlapping key ranges (an older copy of a key
// is deleted in its segment), more deleted at random, and keyless documents last.
inputs build(const shape& sh, std::size_t per_segment, std::uint64_t seed) {
	std::mt19937_64 r(seed);
	inputs in;
	std::uint64_t docid = 1;
	for (int s = 0; s < 3; ++s) {
		writer w;
		w.key_prefix = "Q";
		w.stored_block = sh.block;
		w.stored_codec = sh.codec;
		std::vector<std::size_t> dead;
		for (std::size_t i = 0; i < per_segment; ++i) {
			char k[24];
			std::snprintf(k, sizeof k, "Q%07zu", i * 3 + std::size_t(s));  // interleaved across the segments
			document d;
			d.stored = value_of(r, sh.sizes[(i + std::size_t(s)) % sh.sizes.size()], sh.random);
			in.stored_bytes += d.stored.size();
			w.add({docid++, std::uint64_t(s) + 1, k}, d, false);
			if (r() % 9 == 0) { dead.push_back(i); }
		}
		for (int j = 0; j < 5; ++j) {
			document d;
			d.stored = value_of(r, 100 + std::size_t(j), sh.random);
			w.add({docid++, 0, {}}, d, false);
		}
		in.files.push_back(w.flush({}));
		std::vector<std::uint64_t> bits((per_segment + 5 + 63) / 64, 0);
		for (std::size_t o : dead) { bits[o / 64] |= std::uint64_t(1) << (o % 64); }
		in.deleted.push_back(std::move(bits));
	}
	return in;
}

std::string merge(const inputs& in, flush_charge* memory = nullptr, format::stored_codec codec = format::zstd_codec(3)) {
	std::deque<segment> segs;
	std::vector<basic_merge_source<document_layout>> sources;
	for (std::size_t i = 0; i < in.files.size(); ++i) {
		segs.emplace_back(std::string_view(in.files[i]));
		sources.push_back({&segs.back(), &in.deleted[i]});
	}
	std::string out;
	string_output sink(out);
	string_spill spill;
	(void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), std::array<std::uint8_t, 16>{}, sink, spill,
		{.memory = memory, .key_prefix = "Q", .stored_codec = codec});
	return out;
}

bool valid(std::string_view bytes) {
	try {
		const segment s(bytes);
		basic_check_report<document_layout> r;
		check_segment(s, nullptr, r);
		typename stored_plugin::reader::stored_frame frame;
		for (ordinal_t o = 0; o < s.core().doc_count(); ++o) { (void)s.get<stored_plugin>().stored(o, frame); }
		return true;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "  invalid: %s\n", e.what());
		return false;
	}
}

// What every shape merges to, hashed; taken from main's whole-section merge.
const std::vector<shape>& shapes() {
	static const std::vector<shape> v{
		{4096, format::zstd_codec(3), false, {40, 300, 1500, 0, 9000, 7}},       // records past the target, and empty ones
		{4096, format::zstd_codec(3), true, {1024}},                            // incompressible: raw frames
		{16 << 10, format::lz4_codec, false, {100, 700, 16384, 1}},          // lz4, a record at the target
		{1024, format::stored_codec{format::codec::raw, 0}, false, {10, 20, 0, 0, 0}},                 // raw, many small records
		{64, format::zstd_codec(1), false, {0, 0, 1, 2, 3, 63, 64, 65}},       // the count cap closes blocks of empty records
	};
	return v;
}

// Main's merges of the shapes above (the whole-section merge).
const std::vector<std::uint64_t> pinned{
	0x27fc3c532b9668c8ull,
	0x72ae84c44026dc36ull,
	0x7b23fb8535bed4c4ull,
	0x25128a1fbb6854d8ull,
	0xeb1b126bb8900e5cull};

void digests() {
	for (std::size_t i = 0; i < shapes().size(); ++i) {
		const inputs in = build(shapes()[i], 700, 100 + i);
		const std::string out = merge(in);
		CHECK(valid(out));
		const std::uint64_t h = fnv(out);
		if (pinned.empty()) {
			std::printf("0x%016llxull,\n", (unsigned long long)h);
		} else {
			if (h != pinned[i]) { std::fprintf(stderr, "  shape %zu: digest 0x%016llx, pinned 0x%016llx\n", i, (unsigned long long)h, (unsigned long long)pinned[i]); }
			CHECK(h == pinned[i]);
		}
		// The same merge under a budget writes the same bytes.
		byte_budget budget(1ull << 40);
		flush_charge charge(&budget);
		CHECK(merge(in, &charge) == out);
	}
	// A merge's codec preset applies to the stored frames it streams.
	{
		const inputs in = build(shapes()[0], 300, 7);
		const std::string zstd = merge(in, nullptr, format::zstd_codec(3)), lz4 = merge(in, nullptr, format::lz4_codec);
		CHECK(valid(zstd) && valid(lz4) && zstd != lz4);
	}
	std::printf("  digests: %zu merge shapes\n", shapes().size());
}

// The working memory: merging records worth 12 MiB charges a few blocks and the key builders'
// state, not the section (the whole-section merge charged about three times the records).
void bounded() {
	shape sh{16 << 10, format::lz4_codec, true, {1024}};
	const inputs in = build(sh, 4000, 5);
	byte_budget budget(1ull << 40);
	flush_charge charge(&budget);
	const std::string out = merge(in, &charge, format::lz4_codec);
	CHECK(valid(out));
	std::printf("  bounded: %zu stored bytes, budget peak %llu\n", in.stored_bytes, (unsigned long long)budget.peak());
	CHECK(budget.peak() < in.stored_bytes / 4);
}

}  // namespace

int main() {
	digests();
	bounded();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("merge_stream_test: ok\n");
	return 0;
}
