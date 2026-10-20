// libFuzzer target for format 14's document segments (format-14-kv T2): the stored frame
// decoder (the codec byte, the LZ4 and zstd bounds, the record offsets) and index_sort's reader
// must decode or throw corrupt_segment or unsupported_segment, never crash, read out of bounds,
// or throw anything else. data[0] even takes the rest as a whole file; odd builds a valid
// key-ordered segment seeded by the input (its record sizes, codec, and block target too), then
// applies (position, xor) pairs from the rest to its stored section or, for data[1] odd, to its
// index_sort section, and reseals every checksum so the changes reach the decoders. A segment
// that opens is checked whole and every record and key read; one that passes check() keeps its
// keys at their ranks.
#include "../test/segment_edit.hh"
#include "strata/core/check.hh"
#include "strata/core/writer.hh"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace {

using namespace strata;
using segment = basic_segment<document_layout>;

std::string valid(std::uint64_t seed) {
	std::mt19937_64 r(seed);
	basic_segment_writer<document_layout> w;
	w.key_prefix = "Q";
	w.stored_block = r() % 2 ? std::size_t(4) << 10 : std::size_t(1) << (8 + r() % 9);
	const unsigned codec = unsigned(r() % 4);
	w.stored_codec = codec == 0 ? format::stored_codec{format::codec::raw, 0} : codec == 1 ? format::lz4_codec : format::zstd_codec(codec == 2 ? 1 : 3);
	const std::uint32_t docs = 1 + std::uint32_t(r() % 120), keyless = std::uint32_t(r() % 4);
	std::uint64_t docid = 1 + r() % 1000;
	for (std::uint32_t i = 0; i < docs + keyless; ++i) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%05u", unsigned(i * 3 + r() % 3));
		basic_document<document_layout> d;
		const std::size_t size = r() % 16 == 0 ? 300 + r() % 6000 : r() % 120;
		for (std::size_t j = 0; j < size; ++j) { d.stored.push_back(j % 3 ? char('a' + (j + i) % 5) : char(r())); }
		w.add({docid += 1 + r() % 50, i < docs ? 1 + r() % 3 : 0, i < docs ? std::string_view(k) : std::string_view()}, d, false);
	}
	std::array<std::uint8_t, 16> id{};
	return w.flush(id);
}

void read_all(std::string_view bytes) {
	try {
		const segment s(bytes);
		s.verify_all();
		const auto& stored = s.get<stored_plugin>();
		typename stored_plugin::reader::stored_frame frame;
		for (ordinal_t o = 0; o < s.core().doc_count(); ++o) { (void)stored.stored(o, frame); }
		for (std::uint32_t f = 0; f < stored.stored_frames(); ++f) { stored.decode_stored_frame(f, frame); }
		std::uint64_t keys = 0;
		s.get<keys_plugin>().each_key([&](std::string_view, ordinal_t) { ++keys; });
		basic_check_report<document_layout> r;
		check_segment(s, nullptr, r, "Q");
		if (s.core().key_ordered()) {
			ordinal_t rank = 0;
			s.get<keys_plugin>().each_key([&](std::string_view, ordinal_t o) {
				if (o != rank++) { __builtin_trap(); }  // check() passed: every key at its rank
			});
		}
	} catch (const corrupt_segment&) {
	} catch (const unsupported_segment&) {
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 2) { return 0; }
	if (data[0] % 2 == 0) {
		read_all(std::string_view(reinterpret_cast<const char*>(data + 1), size - 1));
		return 0;
	}
	std::uint64_t seed = 0;
	for (std::size_t i = 2; i < std::min<std::size_t>(size, 10); ++i) { seed = seed * 131 + data[i]; }
	const std::string good = valid(seed);
	const format::section kind = data[1] % 2 ? format::section::index_sort : format::section::stored;
	std::string bytes = test::section_bytes(good, kind);
	if (bytes.empty()) { return 0; }
	for (std::size_t i = 10; i + 2 < size; i += 3) {
		const std::size_t at = (std::size_t(data[i]) << 8 | data[i + 1]) % bytes.size();
		bytes[at] = char(bytes[at] ^ (data[i + 2] | 1));
	}
	read_all(test::reseal(good, {{kind, std::move(bytes)}}));
	return 0;
}
