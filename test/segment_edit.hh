// Rewrites a segment file with sections replaced, added, or removed and its structure bits set,
// every checksum resealed (sections, regions, table of contents, footer), so a test or a fuzz
// target reaches a decoder past the checksums. Sections stay in rank order at aligned offsets.
#pragma once

#include "strata/core/format.hh"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace strata::test {

// The bytes of a section of a well-formed file, empty when it has none.
inline std::string section_bytes(std::string_view file, format::section kind) {
	const format::footer f = format::decode_footer(file);
	for (const auto& e : format::decode_toc(file, f)) {
		if (e.kind == kind) { return std::string(file.substr(e.offset, e.length)); }
	}
	return {};
}

// `edits` maps a kind to its new bytes, or to nothing to drop it; `structures`, when given,
// replaces the footer's bits.
inline std::string reseal(std::string_view file, const std::map<format::section, std::optional<std::string>>& edits,
	std::optional<format::structure_bits> structures = std::nullopt) {
	format::footer f = format::decode_footer(file);
	const auto toc = format::decode_toc(file, f);
	std::map<format::section, std::pair<std::string, std::uint8_t>> sections;  // bytes, region shift
	for (const auto& e : toc) { sections[e.kind] = {std::string(file.substr(e.offset, e.length)), e.region_shift}; }
	for (const auto& [kind, bytes] : edits) {
		if (bytes) {
			sections[kind].first = *bytes;
			if (!sections[kind].second) { sections[kind].second = format::default_region_shift; }
		} else {
			sections.erase(kind);
		}
	}
	std::vector<format::section> order;
	for (const auto& [kind, _] : sections) { order.push_back(kind); }
	std::stable_sort(order.begin(), order.end(), [](format::section a, format::section b) { return format::rule_of(a)->rank < format::rule_of(b)->rank; });
	std::string out;
	std::vector<format::section_entry> entries;
	for (const format::section kind : order) {
		out.resize(format::align_up(out.size()), '\0');
		const auto& [bytes, shift] = sections[kind];
		entries.push_back(format::describe(kind, out.size(), bytes, shift));
		out += bytes;
	}
	out.resize(format::align_up(out.size()), '\0');
	const std::string t = format::encode_toc(entries);
	f.toc_offset = out.size();
	f.toc_length = t.size();
	f.toc_crc = ::kronuz::crc32c::crc32c(t.data(), t.size());
	if (structures) { f.structures = *structures; }
	return out + t + format::encode_footer(f);
}

}  // namespace strata::test
