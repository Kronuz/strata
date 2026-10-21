// libFuzzer target for format 14's document segments (format-14-kv T2): the stored frame
// decoder (the codec byte, the LZ4 and zstd bounds, the record offsets), index_sort's reader, and
// the keys' lazy cursor (the scan's, from any key) must decode or throw corrupt_segment or
// unsupported_segment, never crash, read out of bounds, or throw anything else. data[0] even takes
// the rest as a whole file; odd builds a valid key-ordered segment seeded by the input (its
// record sizes, codec, and block target too), then applies (position, xor) pairs from the rest to
// the section data[1] % 4 picks (stored, index_sort, keys, key_index) and reseals every checksum
// so the changes reach the decoders. A segment that opens is checked whole and every record and
// key read; one that passes check() keeps its keys at their ranks.
#include "../test/segment_edit.hh"
#include "strata/core/borrow.hh"
#include "strata/core/check.hh"
#include "strata/core/writer.hh"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace strata;
using segment = basic_segment<document_layout>;

std::string valid(std::uint64_t seed) {
	std::mt19937_64 r(seed);
	const std::size_t block = r() % 2 ? std::size_t(4) << 10 : std::size_t(1) << (8 + r() % 9);
	const unsigned codec = unsigned(r() % 4);
	const format::stored_codec stored_codec = codec == 0 ? format::stored_codec{format::codec::raw, 0} : codec == 1 ? format::lz4_codec : format::zstd_codec(codec == 2 ? 1 : 3);
	const std::uint32_t docs = 1 + std::uint32_t(r() % 120), keyless = std::uint32_t(r() % 4);
	std::uint64_t docid = 1 + r() % 1000;
	struct fields {
		std::uint64_t docid, version;
		bool keyed;
	};
	borrowable_vector<basic_document<document_layout>> held;
	std::vector<fields> meta;
	for (std::uint32_t i = 0; i < docs + keyless; ++i) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%05u", unsigned(i * 3 + r() % 3));
		basic_document<document_layout> d;
		d.key = i < docs ? k : "";
		const std::size_t size = r() % 16 == 0 ? 300 + r() % 6000 : r() % 120;
		for (std::size_t j = 0; j < size; ++j) { d.stored.push_back(j % 3 ? char('a' + (j + i) % 5) : char(r())); }
		docid += 1 + r() % 50;
		meta.push_back({docid, i < docs ? 1 + r() % 3 : 0, i < docs});
		held.push_back(std::move(d));
	}
	std::array<std::uint8_t, 16> id{};
	// Copied, and borrowed from the container: the same segment either way.
	auto build = [&](bool borrow) {
		basic_segment_writer<document_layout> w;
		w.key_prefix = "Q";
		w.stored_block = block;
		w.stored_codec = stored_codec;
		auto lease = held.pin();
		std::size_t bytes = 0;
		for (const auto& d : held) { bytes += d.stored.size(); }
		if (borrow) { w.borrow_stored(lease); }
		w.reserve(held.size(), bytes);
		for (std::size_t i = 0; i < held.size(); ++i) { w.add({meta[i].docid, meta[i].version, held[i].key}, held[i], true); }
		return w.flush(id);
	};
	std::string file = build(false);
	if (build(true) != file) { __builtin_trap(); }
	return file;
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
		std::vector<std::pair<std::string, ordinal_t>> walk;
		s.get<keys_plugin>().each_key([&](std::string_view k, ordinal_t o) {
			++keys;
			walk.emplace_back(k, o);
		});
		// The lazy cursor, from the first key and from each of a few others, may throw
		// corrupt_segment but else yields the checked walk's suffix.
		for (std::size_t at = 0; at < walk.size() || at == 0; at += 1 + walk.size() / 4) {
			const std::string from = at < walk.size() ? walk[at].first : std::string();
			std::size_t i = at;
			for (auto c = s.get<keys_plugin>().range(from); c.valid(); c.next(), ++i) {
				if (i >= walk.size() || c.key() != walk[i].first || c.ordinal() != walk[i].second) { __builtin_trap(); }
			}
			if (i != walk.size()) { __builtin_trap(); }
		}
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
	const format::section kinds[] = {format::section::stored, format::section::index_sort, format::section::keys, format::section::key_index};
	const format::section kind = kinds[data[1] % 4];
	std::string bytes = test::section_bytes(good, kind);
	if (bytes.empty()) { return 0; }
	for (std::size_t i = 10; i + 2 < size; i += 3) {
		const std::size_t at = (std::size_t(data[i]) << 8 | data[i + 1]) % bytes.size();
		bytes[at] = char(bytes[at] ^ (data[i + 2] | 1));
	}
	read_all(test::reseal(good, {{kind, std::move(bytes)}}));
	return 0;
}
