// Format 13's open rules (docs/design/format-13.md, T1): the version, the structure flags
// against the table of contents, unknown and misplaced kinds, and the reserved words.
#include "strata/reader.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace {

using strata::format::section;
using entries = std::vector<strata::format::section_entry>;

enum class outcome { opens, corrupt, unsupported, other };

outcome open(const std::string& file) {
	try {
		strata::segment s(file);
		return outcome::opens;
	} catch (const strata::corrupt_segment&) {
		return outcome::corrupt;
	} catch (const strata::unsupported_segment&) {
		return outcome::unsupported;
	} catch (...) {
		return outcome::other;
	}
}

std::string keyed_segment() {
	strata::segment_writer w;
	for (std::uint32_t d = 0; d < 300; ++d) {
		const std::string key = "Q" + std::to_string(d);
		const std::vector<std::uint32_t> pos{1, 4}, beta{7};
		std::vector<strata::posting_input> in{{"alpha", 2, pos}, {key, 0, {}}};
		if (d % 3 == 0) { in.push_back({"beta", 1, beta}); }
		w.add(2 * d + 1, in, 3, std::string(d % 7, 'x'), 1 + d % 5, key);
	}
	return w.flush({});
}

// The file laid out again from `edit`'s entries, in the order given, each section's bytes
// copied from the original, every checksum recomputed; `footer` then edits the footer.
std::string relayout(const std::string& file, const std::function<void(entries&)>& edit,
	const std::function<void(strata::format::footer&)>& footer = {}) {
	auto f = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, f);
	std::vector<std::string> bytes;
	for (const auto& e : toc) { bytes.push_back(file.substr(e.offset, e.length)); }
	for (std::size_t i = 0; i < toc.size(); ++i) { toc[i].offset = i; }  // remember each entry's source
	edit(toc);
	std::string out;
	for (auto& e : toc) {
		while (out.size() % strata::format::section_alignment) { out.push_back('\0'); }
		const std::string& b = bytes[e.offset];
		e = strata::format::describe(e.kind, out.size(), b, e.region_shift);
		out += b;
	}
	const std::string t = strata::format::encode_toc(toc);
	f.toc_offset = out.size();
	f.toc_length = t.size();
	f.toc_crc = hashes::crc32c(t.data(), t.size());
	out += t;
	if (footer) { footer(f); }
	return out + strata::format::encode_footer(f);
}

}  // namespace

int main() {
	const std::string good = keyed_segment();
	CHECK(open(good) == outcome::opens);
	{
		strata::segment s(good);
		CHECK(s.format_version() == 13);
		CHECK(s.structures() == (strata::format::inverted | strata::format::norms_structure | strata::format::keys_structure | strata::format::stored_structure));
		// Entries are in F5's file order: streamed sections, then the pinned tail.
		const std::vector<section> order{section::header, section::postings, section::positions, section::terms, section::stored, section::docids,
			section::stats, section::lengths, section::norms, section::versions, section::term_index, section::pk_filter};
		std::vector<section> got;
		for (const auto& e : s.sections()) { got.push_back(e.kind); }
		CHECK(got == order);
	}
	CHECK(open(relayout(good, [](entries&) {})) == outcome::opens);

	// Every section removed: corruption, except the transitional key filter, whose absence
	// means a segment with no key terms.
	{
		const auto toc = strata::format::decode_toc(good, strata::format::decode_footer(good));
		for (std::size_t i = 0; i < toc.size(); ++i) {
			const auto kind = toc[i].kind;
			const auto got = open(relayout(good, [&](entries& t) { t.erase(t.begin() + std::ptrdiff_t(i)); }));
			if (kind == section::pk_filter) {
				CHECK(got == outcome::opens);
			} else {
				if (got != outcome::corrupt) { std::fprintf(stderr, "removing %s\n", strata::format::name_of(kind)); }
				CHECK(got == outcome::corrupt);
			}
		}
	}

	// Each structure bit flipped, and unknown bits: corruption.
	for (unsigned bit = 0; bit < 32; ++bit) {
		const auto got = open(relayout(good, [](entries&) {}, [&](strata::format::footer& f) { f.structures ^= std::uint32_t(1) << bit; }));
		CHECK(got == outcome::corrupt);
	}

	// Unknown kinds, and kinds no structure writes yet (keys, key_index), in place of the
	// key filter: corruption.
	for (std::uint16_t kind : {std::uint16_t(12), std::uint16_t(13), std::uint16_t(18), std::uint16_t(200), std::uint16_t(0xffff)}) {
		const auto got = open(relayout(good, [&](entries& t) { t.back().kind = section(kind); }));
		CHECK(got == outcome::corrupt);
	}

	// Entries out of file order, even with offsets that do not overlap: corruption.
	CHECK(open(relayout(good, [](entries& t) { std::swap(t[1], t[2]); })) == outcome::corrupt);
	CHECK(open(relayout(good, [](entries& t) { std::swap(t[t.size() - 1], t[t.size() - 2]); })) == outcome::corrupt);
	CHECK(open(relayout(good, [](entries& t) { std::swap(t[0], t[1]); })) == outcome::corrupt);
	// A duplicated entry: corruption.
	CHECK(open(relayout(good, [](entries& t) { t.push_back(t.back()); })) == outcome::corrupt);

	// The footer's reserved word and the header's flags must be zero.
	{
		std::string file = good;
		const std::size_t at = file.size() - strata::format::footer_size;
		file[at + 44] = 1;
		const std::uint32_t crc = hashes::crc32c(file.data() + at, 48);
		std::memcpy(file.data() + at + 48, &crc, 4);
		CHECK(open(file) == outcome::corrupt);
	}
	{
		std::string file = good;
		file[12] = 1;
		CHECK(open(file) == outcome::corrupt);  // the header's CRC fails first, unresealed
		const std::string resealed = relayout(file, [](entries&) {});
		CHECK(open(resealed) == outcome::corrupt);
	}

	// Another version: refused as unsupported, before its footer is parsed.
	for (std::uint32_t v : {12u, 14u}) {
		std::string file = good;
		std::memcpy(file.data() + 8, &v, 4);
		CHECK(open(file) == outcome::unsupported);
		file.resize(file.size() - 8);  // a footer this version cannot parse
		CHECK(open(file) == outcome::unsupported);
	}
	CHECK(open(good.substr(0, 16)) == outcome::corrupt);

	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("format13_test: ok");
	return 0;
}
