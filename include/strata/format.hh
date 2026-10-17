/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The segment file format (docs/design/phase-2.md): header, sections, table of
// contents, footer, term info, and the length quantization used by skip data. Every
// fixed-width field is little-endian; variable-length integers are LEB128.

#pragma once

#include "crc32c.hh"
#include "termdict.hh"
#include "varint.hh"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

using ordinal_t = std::uint32_t;

// Raised when segment bytes break a format rule; the segment is fenced.
struct corrupt_segment : std::runtime_error {
	explicit corrupt_segment(const char* what) : std::runtime_error(std::string("strata: corrupt segment: ") + what) {}
};

// Raised for a segment written in a format version this binary does not read (F2): not
// damage, so it never fences an index.
struct unsupported_segment : std::runtime_error {
	explicit unsupported_segment(const char* what) : std::runtime_error(std::string("strata: unsupported segment: ") + what) {}
};

// Throws corrupt_segment, out of line and cold, so the decoders that check keep no throw
// path inline.
[[noreturn, gnu::cold, gnu::noinline]] inline void corrupt(const char* what) { throw corrupt_segment(what); }

namespace format {

inline constexpr char header_magic[8] = {'S', 'T', 'R', 'S', 'E', 'G', '0', '1'};
inline constexpr char footer_magic[4] = {'S', 'E', 'N', 'D'};
// The version written, and the oldest one read (from format 13, N - 1: format-13.md F2).
inline constexpr std::uint32_t version = 13;
inline constexpr std::uint32_t min_read_version = 13;
inline constexpr std::size_t header_size = 32;
inline constexpr std::size_t footer_size = 56;
// Each section is verified per region of 2^region_shift bytes, recorded in its table of
// contents entry (4 KiB by default: docs/perf/p15-region-size.md).
inline constexpr std::uint8_t default_region_shift = 12;
inline constexpr std::uint8_t min_region_shift = 12, max_region_shift = 20;
// Stored records are packed into zstd frames of at most this many uncompressed bytes (a
// larger record takes a frame of its own).
inline constexpr std::size_t stored_block_size = 64 * 1024;
// The largest stored record a document may carry; a frame then holds at most one block
// of records or one record this size, plus its length header, which bounds what reading
// a frame allocates even when a file's checksums were recomputed (review C9).
inline constexpr std::uint64_t max_stored_record = 64ull << 20;
// The longest term: the term dictionary's key limit (fst's 32-bit depth), past Lucene's
// 32,766 and Tantivy's 65,530 bytes.
inline constexpr std::size_t max_term_bytes = termdict::max_key_bytes;
inline constexpr std::uint64_t max_stored_frame = stored_block_size + max_stored_record + 64;
inline constexpr std::size_t section_alignment = 64;
inline constexpr std::size_t block = 128;

// Section kinds (format-13.md F3). Ids are stable; 11, format 12's key filter, is retired.
enum class section : std::uint16_t {
	header = 0,
	terms = 1,
	term_index = 2,
	postings = 3,
	positions = 4,
	lengths = 5,
	stored = 6,
	docids = 7,
	stats = 8,
	norms = 9,  // one encode_length byte per document, for Lucene-compatible scoring
	versions = 10,   // one version per ordinal, bit-packed from the segment's minimum
	pk_filter = 11,  // retired: format 12's key filter, now part of key_index
	keys = 12,       // key blocks: key -> ordinal (keys.hh)
	key_index = 13,  // the key range, the key filter, and the key blocks' index (keys.hh)
	values = 14,
	points = 15,
	term_lists = 16,
	index_sort = 17,
};
inline constexpr std::uint16_t section_kinds = 18;

// The kind's name, as tools print it.
inline constexpr const char* name_of(section kind) noexcept {
	constexpr const char* names[section_kinds] = {"header", "terms", "term_index", "postings", "positions", "lengths", "stored", "docids", "stats",
		"norms", "versions", "pk_filter", "keys", "key_index", "values", "points", "term_lists", "index_sort"};
	return std::size_t(kind) < section_kinds ? names[std::size_t(kind)] : "unknown";
}

// Structure flags (F4): the footer records which structures a segment was built with.
enum structure : std::uint32_t {
	inverted = 1,
	norms_structure = 2,
	keys_structure = 4,
	stored_structure = 8,
	values_structure = 16,
	points_structure = 32,
	term_lists_structure = 64,
	index_sort_structure = 128,
};
inline constexpr std::uint32_t known_structures = 255;

// Per kind: the structure bit that requires it (0: mandatory), and its rank in file order
// (F5): streamed sections, then the pinned tail.
struct section_rule {
	std::uint32_t structure = 0;
	std::uint8_t rank = 0;
	bool known = false;
};
inline constexpr section_rule rule_of(section kind) noexcept {
	switch (kind) {
	case section::header: return {0, 0, true};
	case section::postings: return {inverted, 1, true};
	case section::positions: return {inverted, 2, true};
	case section::terms: return {inverted, 3, true};
	case section::keys: return {keys_structure, 4, true};
	case section::stored: return {stored_structure, 5, true};
	case section::values: return {values_structure, 6, true};
	case section::points: return {points_structure, 7, true};
	case section::term_lists: return {term_lists_structure, 8, true};
	case section::docids: return {0, 9, true};
	case section::stats: return {0, 10, true};
	case section::lengths: return {inverted, 11, true};
	case section::norms: return {norms_structure, 12, true};
	case section::versions: return {keys_structure, 13, true};
	case section::term_index: return {inverted, 14, true};
	case section::key_index: return {keys_structure, 15, true};
	case section::index_sort: return {index_sort_structure, 16, true};
	case section::pk_filter: break;
	}
	return {};
}

// --- little-endian fixed-width fields ---------------------------------------------------------

template <typename T>
inline void put(std::string& out, T v) {
	static_assert(std::endian::native == std::endian::little);
	char b[sizeof(T)];
	std::memcpy(b, &v, sizeof(T));
	out.append(b, sizeof(T));
}

template <typename T>
inline T get(const char* p) noexcept {
	T v;
	std::memcpy(&v, p, sizeof(T));
	return v;
}

inline void put_leb(std::string& out, std::uint64_t v) {
	char b[varint::leb128::max_bytes];
	out.append(b, varint::leb128::encode(b, v));
}

// Reads a LEB128 value from [*p, end) or throws corrupt_segment.
inline std::uint64_t get_leb(const char** p, const char* end) {
	std::uint64_t v = 0;
	if (!varint::leb128::decode(p, end, v)) { corrupt("truncated integer"); }
	return v;
}

// --- header, table of contents, footer --------------------------------------------------------

struct section_entry {
	section kind;
	std::uint64_t offset = 0;
	std::uint64_t length = 0;
	std::uint32_t crc = 0;
	std::vector<std::uint32_t> region_crcs;  // one per region of the section
	std::uint8_t region_shift = default_region_shift;
	std::uint64_t region_size() const noexcept { return std::uint64_t(1) << region_shift; }
};

inline std::string encode_header(const std::array<std::uint8_t, 16>& segment_id) {
	std::string h(header_magic, 8);
	put<std::uint32_t>(h, version);
	put<std::uint32_t>(h, 0);  // flags: none defined (F6)
	h.append(reinterpret_cast<const char*>(segment_id.data()), 16);
	return h;
}

// The section's CRC and its region CRCs over bytes.
inline section_entry describe(section kind, std::uint64_t offset, std::string_view bytes, std::uint8_t region_shift = default_region_shift) {
	section_entry e{kind, offset, bytes.size(), hashes::crc32c(bytes.data(), bytes.size()), {}, region_shift};
	const std::size_t size = e.region_size();
	for (std::size_t at = 0; at < bytes.size(); at += size) {
		e.region_crcs.push_back(hashes::crc32c(bytes.data() + at, std::min(size, bytes.size() - at)));
	}
	return e;
}

inline std::string encode_toc(const std::vector<section_entry>& entries) {
	std::string t;
	put<std::uint32_t>(t, std::uint32_t(entries.size()));
	for (const auto& e : entries) {
		put<std::uint16_t>(t, std::uint16_t(e.kind));
		put<std::uint16_t>(t, e.region_shift);
		put<std::uint64_t>(t, e.offset);
		put<std::uint64_t>(t, e.length);
		put<std::uint32_t>(t, e.crc);
		for (auto c : e.region_crcs) { put<std::uint32_t>(t, c); }
	}
	return t;
}

struct footer {
	std::uint64_t toc_offset = 0;
	std::uint64_t toc_length = 0;
	std::uint32_t toc_crc = 0;
	std::uint32_t doc_count = 0;
	std::uint64_t min_docid = 0;
	std::uint64_t max_docid = 0;
	std::uint32_t structures = 0;  // F4's bits
};

inline std::string encode_footer(const footer& f) {
	std::string out;
	put<std::uint64_t>(out, f.toc_offset);
	put<std::uint64_t>(out, f.toc_length);
	put<std::uint32_t>(out, f.toc_crc);
	put<std::uint32_t>(out, f.doc_count);
	put<std::uint64_t>(out, f.min_docid);
	put<std::uint64_t>(out, f.max_docid);
	put<std::uint32_t>(out, f.structures);
	put<std::uint32_t>(out, 0);  // reserved
	put<std::uint32_t>(out, hashes::crc32c(out.data(), out.size()));
	out.append(footer_magic, 4);
	return out;
}

inline footer decode_footer(std::string_view file) {
	if (file.size() < header_size + footer_size) { corrupt("file too small"); }
	const char* p = file.data() + file.size() - footer_size;
	if (std::memcmp(p + 52, footer_magic, 4) != 0) { corrupt("footer magic"); }
	if (hashes::crc32c(p, 48) != get<std::uint32_t>(p + 48)) { corrupt("footer checksum"); }
	footer f{get<std::uint64_t>(p), get<std::uint64_t>(p + 8), get<std::uint32_t>(p + 16), get<std::uint32_t>(p + 20),
		get<std::uint64_t>(p + 24), get<std::uint64_t>(p + 32), get<std::uint32_t>(p + 40)};
	if (f.structures & ~known_structures) { corrupt("structure flags"); }
	if (get<std::uint32_t>(p + 44) != 0) { corrupt("footer reserved"); }
	const std::uint64_t body = file.size() - footer_size;
	if (f.toc_offset > body || f.toc_length > body - f.toc_offset || f.toc_offset + f.toc_length != body) { corrupt("table of contents bounds"); }
	return f;
}

// Parses and checks the table of contents: its CRC, entry bounds, and every section's
// placement inside the file before the table of contents.
inline std::vector<section_entry> decode_toc(std::string_view file, const footer& f) {
	std::string_view t = file.substr(f.toc_offset, f.toc_length);
	if (hashes::crc32c(t.data(), t.size()) != f.toc_crc) { corrupt("table of contents checksum"); }
	const char* p = t.data();
	const char* end = p + t.size();
	auto need = [&](std::size_t n) {
		if (std::size_t(end - p) < n) { corrupt("table of contents truncated"); }
	};
	need(4);
	std::uint32_t n = get<std::uint32_t>(p);
	p += 4;
	if (n > 64) { corrupt("section count"); }
	std::vector<section_entry> entries;
	for (std::uint32_t i = 0; i < n; ++i) {
		need(24);
		section_entry e;
		e.kind = section(get<std::uint16_t>(p));
		if (!rule_of(e.kind).known) { corrupt("unknown section kind"); }
		const std::uint16_t shift = get<std::uint16_t>(p + 2);
		if (shift < min_region_shift || shift > max_region_shift) { corrupt("region size"); }
		e.region_shift = std::uint8_t(shift);
		e.offset = get<std::uint64_t>(p + 4);
		e.length = get<std::uint64_t>(p + 12);
		e.crc = get<std::uint32_t>(p + 20);
		p += 24;
		if (e.offset > f.toc_offset || e.length > f.toc_offset - e.offset) { corrupt("section bounds"); }
		const std::size_t regions = std::size_t((e.length + e.region_size() - 1) >> e.region_shift);
		need(regions * 4);
		e.region_crcs.reserve(regions);
		for (std::size_t r = 0; r < regions; ++r) { e.region_crcs.push_back(get<std::uint32_t>(p + 4 * r)); }
		p += regions * 4;
		entries.push_back(std::move(e));
	}
	if (p != end) { corrupt("table of contents trailing bytes"); }
	// Each kind at most once, entries in file order (F5), sections never overlap.
	for (std::size_t i = 1; i < entries.size(); ++i) {
		const auto& a = entries[i - 1];
		const auto& b = entries[i];
		if (rule_of(a.kind).rank >= rule_of(b.kind).rank) { corrupt("section order"); }
		if (a.offset + a.length > b.offset) { corrupt("overlapping sections"); }
	}
	return entries;
}

// Checks the table of contents against the footer's structure flags (F4): every mandatory
// section present, and each structure's sections all present when its bit is set and all
// absent when it is clear. `norms` needs `inverted`.
inline void check_structures(const std::vector<section_entry>& entries, std::uint32_t structures) {
	std::uint32_t seen[section_kinds] = {};
	for (const auto& e : entries) { seen[std::size_t(e.kind)] = 1; }
	if ((structures & norms_structure) && !(structures & inverted)) { corrupt("structure flags"); }
	for (std::uint16_t k = 0; k < section_kinds; ++k) {
		const auto r = rule_of(section(k));
		if (!r.known) { continue; }
		const bool wanted = r.structure == 0 || (structures & r.structure);
		if (wanted && !seen[k]) { corrupt("missing section"); }
		if (!wanted && seen[k]) { corrupt("section without its structure"); }
	}
}

// --- document lengths in skip data ------------------------------------------------------------

// A one-byte log scale for lengths: exact below 24, then 3 mantissa bits per power of two.
// decode(encode(n)) <= n always, so bounds computed from stored lengths never understate.
inline std::uint8_t encode_length(std::uint32_t n) noexcept {
	if (n < 24) { return std::uint8_t(n); }
	unsigned e = unsigned(std::bit_width(n)) - 4;      // n >> e is in [8, 15]
	return std::uint8_t(24 + (e - 1) * 8 + ((n >> e) - 8));
}

inline std::uint32_t decode_length(std::uint8_t q) noexcept {
	if (q < 24) { return q; }
	unsigned e = (q - 24) / 8 + 1, m = 8 + (q - 24) % 8;
	return m << e;
}

// --- term info (the termdict value) -----------------------------------------------------------

enum term_flags : std::uint8_t {
	no_wdf = 1,           // every posting has wdf 0 (boolean term)
	no_positions = 2,     // the term has no position stream
	single_posting = 4,   // df == 1: ordinal and wdf inline, no postings bytes
	counts_are_wdf = 8,   // every document's position count equals its wdf
	cf_is_df = 16,        // cf equals df: not stored
	absolute = 32,        // offsets stored; otherwise they follow the previous term's
};

// A competitive (wdf, length) pair: no posting in range has more wdf at less length.
struct impact {
	std::uint32_t wdf = 0;
	std::uint8_t length = 0;  // encode_length() of the shortest length at this wdf
};

struct term_info {
	std::uint8_t flags = 0;
	std::uint32_t df = 0;
	std::uint64_t cf = 0;  // collection frequency: sum of wdf
	// Whole-list competitive pairs; stored for terms with skip blocks (df >= 128), computed
	// from the postings for the others (segment::term).
	std::vector<impact> impacts;
	std::uint64_t postings = 0;   // offset in `postings`, or the ordinal when single_posting
	std::uint64_t positions = 0;  // offset in `positions`
	std::uint32_t single_wdf = 0; // wdf when single_posting
	// Byte lengths of the term's postings and positions, so a reader verifies their
	// regions before reading them.
	std::uint64_t postings_length = 0, positions_length = 0;
};

inline void encode_impacts(std::string& out, const std::vector<impact>& impacts) {
	put_leb(out, impacts.size());
	for (const auto& i : impacts) {
		put_leb(out, i.wdf);
		out.push_back(char(i.length));
	}
}

// Appends one encoded list of competitive pairs to out; returns how many.
inline std::size_t decode_impacts(const char** p, const char* end, std::vector<impact>& out) {
	const std::uint64_t n = get_leb(p, end);
	if (n == 0 || n > 256) { corrupt("impact count"); }  // lengths are 1 byte
	for (std::size_t k = 0; k < n; ++k) {
		impact i;
		i.wdf = std::uint32_t(get_leb(p, end));
		if (*p == end) { corrupt("impacts truncated"); }
		i.length = std::uint8_t(*(*p)++);
		out.push_back(i);
	}
	return n;
}

// Skip data prefixes each list with its byte length, so readers step over it and decode
// it only when a bound needs it.
// A block's competitive pairs in skip data: a LEB128 header (count << 1 | compact). A
// compact list (every wdf below 256) is count pairs of (wdf byte, length byte), so the
// header alone gives its size; otherwise its size (LEB128) and encode_impacts' bytes follow.
inline void encode_sized_impacts(std::string& out, const std::vector<impact>& impacts) {
	const bool compact = std::all_of(impacts.begin(), impacts.end(), [](const impact& i) { return i.wdf < 256; });
	put_leb(out, std::uint64_t(impacts.size()) << 1 | std::uint64_t(compact));
	if (compact) {
		for (const auto& i : impacts) {
			out.push_back(char(i.wdf));
			out.push_back(char(i.length));
		}
		return;
	}
	std::string list;
	encode_impacts(list, impacts);
	put_leb(out, list.size());
	out += list;
}

// Steps over a list written by encode_sized_impacts; returns all its bytes.
inline std::string_view skip_sized_impacts(const char** p, const char* end) {
	const char* start = *p;
	const std::uint64_t h = get_leb(p, end), n = h >> 1;
	if (n == 0 || n > 256) { corrupt("impact count"); }
	const std::uint64_t size = (h & 1) ? 2 * n : get_leb(p, end);
	if (size > std::uint64_t(end - *p)) { corrupt("impacts bounds"); }
	*p += size;
	return std::string_view(start, std::size_t(*p - start));
}

// Calls f(impact) for each pair of a list returned by skip_sized_impacts until f returns
// true; returns whether it did.
template <class F>
inline bool any_sized_impact(std::string_view list, F&& f) {
	const char* p = list.data();
	const char* end = p + list.size();
	const std::uint64_t h = get_leb(&p, end), n = h >> 1;
	if (!(h & 1)) {
		const std::uint64_t size = get_leb(&p, end);
		if (size != std::uint64_t(end - p)) { corrupt("impacts length"); }
		const std::uint64_t m = get_leb(&p, end);
		if (m != n) { corrupt("impact count"); }
	}
	for (std::uint64_t k = 0; k < n; ++k) {
		impact i;
		if (h & 1) {
			if (end - p < 2) { corrupt("impacts truncated"); }
			i.wdf = std::uint8_t(*p++);
		} else {
			i.wdf = std::uint32_t(get_leb(&p, end));
			if (p == end) { corrupt("impacts truncated"); }
		}
		i.length = std::uint8_t(*p++);
		if (f(i)) { return true; }
	}
	if (p != end) { corrupt("impacts length"); }
	return false;
}

inline std::vector<impact> decode_impacts(const char** p, const char* end) {
	std::vector<impact> out;
	decode_impacts(p, end, out);
	return out;
}

// Term infos in one dictionary restart run share a cursor: a term's postings and
// positions start where the previous term's end. A run's first entry, or one after a gap,
// stores both cursors (flag `absolute`); the rest only their lengths. Layout, integers
// LEB128: [df << 5 | no_wdf, no_positions, counts_are_wdf, cf_is_df, absolute as bits
// 0-4][if absolute: postings cursor, positions cursor], then for a single posting (df 1)
// [ordinal][wdf unless 1, which cf_is_df marks], else [cf unless cf_is_df][postings
// length]; with positions [positions length]; with skip blocks (df >= 128) the impacts.
struct term_run {
	std::uint64_t postings = 0, positions = 0;  // where the next term's bytes start
	bool started = false;
};

namespace detail {
inline constexpr std::uint8_t packed_flags[5] = {no_wdf, no_positions, counts_are_wdf, cf_is_df, absolute};
}

inline std::string encode_term_info(term_info t, term_run& run, bool restart) {
	const bool single = t.flags & single_posting, positionless = t.flags & no_positions;
	const bool contiguous = run.started && (single || t.postings == run.postings) && (positionless || t.positions == run.positions);
	t.flags = std::uint8_t(t.flags & ~(cf_is_df | absolute));
	if (single ? t.single_wdf == 1 : t.cf == t.df) { t.flags |= cf_is_df; }
	if (restart || !contiguous) {
		t.flags |= absolute;
		if (!single) { run.postings = t.postings; }
		if (!positionless) { run.positions = t.positions; }
	}
	std::uint64_t head = std::uint64_t(t.df) << 5;
	for (unsigned b = 0; b < 5; ++b) {
		if (t.flags & detail::packed_flags[b]) { head |= 1u << b; }
	}
	std::string out;
	put_leb(out, head);
	if (t.flags & absolute) {
		put_leb(out, run.postings);
		put_leb(out, run.positions);
	}
	if (single) {
		put_leb(out, t.postings);
		if (!(t.flags & cf_is_df)) { put_leb(out, t.single_wdf); }
	} else {
		if (!(t.flags & cf_is_df)) { put_leb(out, t.cf); }
		put_leb(out, t.postings_length);
		run.postings += t.postings_length;
	}
	if (!positionless) {
		put_leb(out, t.positions_length);
		run.positions += t.positions_length;
	}
	if (t.df >= block) { encode_impacts(out, t.impacts); }
	run.started = true;
	return out;
}

// Decodes one entry of a run, resolving its offsets from the run's cursor; without
// impacts, they are skipped unread (an entry decoded only to move the cursor).
inline term_info decode_term_info(std::string_view bytes, term_run& run, bool with_impacts = true) {
	const char* p = bytes.data();
	const char* end = p + bytes.size();
	term_info t;
	const std::uint64_t head = get_leb(&p, end);
	if (head >> 5 == 0 || head >> 5 > 0xffffffffu) { corrupt("term info"); }
	t.df = std::uint32_t(head >> 5);
	for (unsigned b = 0; b < 5; ++b) {
		if (head >> b & 1) { t.flags |= detail::packed_flags[b]; }
	}
	if (t.df == 1) { t.flags |= single_posting; }
	if (!(t.flags & absolute) && !run.started) { corrupt("term offsets without a run"); }
	if (t.flags & absolute) {
		run.postings = get_leb(&p, end);
		run.positions = get_leb(&p, end);
	}
	if (t.flags & single_posting) {
		t.postings = get_leb(&p, end);
		t.single_wdf = t.flags & cf_is_df ? 1 : std::uint32_t(get_leb(&p, end));
		t.cf = t.single_wdf;
	} else {
		t.cf = t.flags & cf_is_df ? t.df : get_leb(&p, end);
		t.postings_length = get_leb(&p, end);
		t.postings = run.postings;
		if (run.postings + t.postings_length < run.postings) { corrupt("postings range"); }
		run.postings += t.postings_length;
	}
	if (!(t.flags & no_positions)) {
		t.positions_length = get_leb(&p, end);
		t.positions = run.positions;
		if (run.positions + t.positions_length < run.positions) { corrupt("positions range"); }
		run.positions += t.positions_length;
	}
	if (t.df >= block) {
		if (with_impacts) {
			t.impacts = decode_impacts(&p, end);
		} else {
			p = end;
		}
	}
	if (p != end) { corrupt("term info"); }
	run.started = true;
	return t;
}

// The competitive pairs of a list of (wdf, length) pairs: its Pareto frontier (higher
// wdf, lower length), highest wdf first. At most 256 pairs, one per length code.
inline std::vector<impact> competitive(const std::vector<impact>& pairs) {
	// Lengths are one byte: the highest wdf at each length, then one pass from the shortest
	// keeps each length whose wdf beats every shorter one's. No sort.
	std::array<std::uint64_t, 256> best{};  // wdf + 1 at each length; 0 for none
	for (const auto& p : pairs) { best[p.length] = std::max<std::uint64_t>(best[p.length], std::uint64_t(p.wdf) + 1); }
	std::vector<impact> frontier;
	std::uint64_t beat = 0;
	for (unsigned length = 0; length < 256; ++length) {
		if (best[length] > beat) {
			beat = best[length];
			frontier.push_back({std::uint32_t(beat - 1), std::uint8_t(length)});
		}
	}
	std::reverse(frontier.begin(), frontier.end());  // highest wdf first
	if (frontier.empty()) { frontier.push_back({}); }
	return frontier;
}

}  // namespace format
}  // namespace strata
