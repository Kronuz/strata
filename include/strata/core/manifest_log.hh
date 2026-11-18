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

// The manifest log's records (manifest-log design D3): a snapshot of the index's commit state,
// then one edit per commit carrying only what changed. A record is its kind, its generation and
// LEB128 fields, every integer in its shortest form; journal's framing carries and checks it
// (D2). docs/mechanisms/manifest-log-codec.md.
#pragma once

#include "strata/core/fields.hh"
#include "strata/core/files.hh"

#include "journal/framing.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace strata::files {

// manifest-<g>'s framing (D2): STRLOG01, the index UUID as identity, g as segment, epoch 1,
// 512-byte blocks, no block manifests.
inline constexpr char log_magic[8] = {'S', 'T', 'R', 'L', 'O', 'G', '0', '1'};
inline constexpr std::uint32_t log_block_bytes = 512;
inline ::kronuz::journal::framing::Stream log_stream(const std::array<char, 16>& uuid, std::uint64_t generation) {
	return {std::string_view(log_magic, 8), uuid, log_block_bytes, generation, 1, false};
}

enum class log_kind : std::uint8_t { snapshot = 1, edit = 2 };

// A segment's deleted ordinals (D42): bitset words while they take no more memory than the
// ordinals (at least doc_count / 64 deleted), sorted ordinals otherwise, so a set holds at most
// 8 bytes per ordinal and at most its bitset's words (plus a 1/63 summary). It keeps the bytes its
// ascending deltas encode to, so an entry's size is known without walking the set. A delete edit
// costs the edit, not the set (SR2): words carry a summary, a bit per nonzero word level over
// level, so a neighbour is a few words at each level away; ordinals are ascending runs, each more
// than twice the next, so an insert merges each ordinal O(log n) times. The segment holds the
// document count it is taken against.
class log_deleted {
public:
	// The set of `ordinals` (ascending, distinct, each below doc_count).
	static log_deleted of(const std::vector<std::uint64_t>& ordinals, std::uint64_t doc_count) {
		log_deleted d;
		d.insert(ordinals, doc_count);
		return d;
	}

	std::uint64_t size() const noexcept { return count_; }
	bool empty() const noexcept { return count_ == 0; }
	bool dense() const noexcept { return !words_.empty(); }
	std::uint64_t deltas_bytes() const noexcept { return deltas_; }
	const std::vector<std::uint64_t>& words() const noexcept { return words_; }  // empty unless dense()
	bool contains(std::uint64_t o) const noexcept {
		if (dense()) { return (o >> 6) < words_.size() && (words_[o >> 6] >> (o & 63) & 1); }
		for (const auto& run : runs_) {
			if (std::binary_search(run.begin(), run.end(), o)) { return true; }
		}
		return false;
	}
	template <class F>
	void for_each(F&& f) const {
		if (dense()) {
			for (std::size_t w = 0; w < words_.size(); ++w) {
				for (std::uint64_t b = words_[w]; b; b &= b - 1) { f(64 * std::uint64_t(w) + std::uint64_t(std::countr_zero(b))); }
			}
			return;
		}
		if (runs_.size() <= 1) {
			for (const auto& run : runs_) {
				for (std::uint64_t o : run) { f(o); }
			}
			return;
		}
		std::vector<std::size_t> at(runs_.size());  // a merge of the runs
		for (;;) {
			std::size_t pick = runs_.size();
			for (std::size_t r = 0; r < runs_.size(); ++r) {
				if (at[r] < runs_[r].size() && (pick == runs_.size() || runs_[r][at[r]] < runs_[pick][at[pick]])) { pick = r; }
			}
			if (pick == runs_.size()) { return; }
			f(runs_[pick][at[pick]++]);
		}
	}
	std::vector<std::uint64_t> ordinals() const {
		std::vector<std::uint64_t> out;
		out.reserve(count_);
		for_each([&](std::uint64_t o) { out.push_back(o); });
		return out;
	}

	// Adds `fresh` (ascending, distinct, each below doc_count and absent: the caller checks).
	// Costs O(k) on words and O(k log n) amortized on ordinals, plus the delta bytes' lookups of
	// each new run's old neighbours, O(log n) words each on words, O(log^2 n) on ordinals.
	void insert(const std::vector<std::uint64_t>& fresh, std::uint64_t doc_count) { insert(fresh, doc_count, deltas_ + delta_change(fresh, doc_count)); }
	// The same with the delta bytes after it, as deltas_after() gave them. An allocation failing in
	// a merge of runs leaves the set whole, with one run more.
	void insert(const std::vector<std::uint64_t>& fresh, std::uint64_t doc_count, std::uint64_t deltas) {
		if (fresh.empty()) { return; }
		if (dense()) {
			for (std::uint64_t o : fresh) { mark(o); }
		} else if (dense_at(count_ + fresh.size(), doc_count)) {
			std::vector<std::uint64_t> words(words_for(doc_count));
			for_each([&](std::uint64_t o) { words[o >> 6] |= std::uint64_t(1) << (o & 63); });
			for (std::uint64_t o : fresh) { words[o >> 6] |= std::uint64_t(1) << (o & 63); }
			std::vector<std::vector<std::uint64_t>> up = summary_of(words);
			words_ = std::move(words);
			up_ = std::move(up);
			runs_.clear();
		} else {
			runs_.emplace_back(fresh);
		}
		count_ += fresh.size();
		deltas_ = deltas;
		// Each run more than twice the next: O(log n) runs, each ordinal merged O(log n) times.
		while (runs_.size() >= 2 && runs_[runs_.size() - 2].size() <= 2 * runs_.back().size()) {
			const auto& a = runs_[runs_.size() - 2];
			const auto& b = runs_.back();
			std::vector<std::uint64_t> merged;
			merged.reserve(a.size() + b.size());
			std::merge(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(merged));
			runs_.pop_back();
			runs_.back() = std::move(merged);
		}
	}

	// The delta bytes the set would encode to with `fresh` added (as insert() takes it).
	std::uint64_t deltas_after(const std::vector<std::uint64_t>& fresh, std::uint64_t doc_count) const { return deltas_ + delta_change(fresh, doc_count); }

	// The set a bitmap of `n` words holds (an index's deleted bits), in its form; n covers doc_count.
	static log_deleted of_words(const std::uint64_t* words, std::size_t n, std::uint64_t doc_count) {
		log_deleted d;
		for (std::size_t w = 0; w < n; ++w) { d.count_ += std::uint64_t(std::popcount(words[w])); }
		if (!d.count_) { return d; }
		std::uint64_t prev = 0;
		bool first = true;
		const bool dense = dense_at(d.count_, doc_count);
		std::vector<std::uint64_t> ordinals;
		if (!dense) { ordinals.reserve(d.count_); }
		for (std::size_t w = 0; w < n; ++w) {
			for (std::uint64_t b = words[w]; b; b &= b - 1) {
				const std::uint64_t o = 64 * std::uint64_t(w) + std::uint64_t(std::countr_zero(b));
				d.deltas_ += leb(first ? o : o - prev);
				prev = o;
				first = false;
				if (!dense) { ordinals.push_back(o); }
			}
		}
		if (dense) {
			d.words_.assign(words, words + words_for(doc_count));
			d.up_ = summary_of(d.words_);
		} else {
			d.runs_.push_back(std::move(ordinals));
		}
		return d;
	}
	// The decoder's: the set from words or ordinals already in their form, with its delta bytes.
	static log_deleted adopt_words(std::vector<std::uint64_t> words, std::uint64_t count, std::uint64_t deltas) {
		log_deleted d;
		d.up_ = summary_of(words);
		d.words_ = std::move(words);
		d.count_ = count;
		d.deltas_ = deltas;
		return d;
	}
	static log_deleted adopt_ordinals(std::vector<std::uint64_t> ordinals, std::uint64_t deltas) {
		log_deleted d;
		d.count_ = ordinals.size();
		if (!ordinals.empty()) { d.runs_.push_back(std::move(ordinals)); }
		d.deltas_ = deltas;
		return d;
	}
	static std::size_t words_for(std::uint64_t doc_count) noexcept { return std::size_t(doc_count / 64 + (doc_count % 64 != 0)); }
	// Words take no more memory than `count` ordinals.
	static bool dense_at(std::uint64_t count, std::uint64_t doc_count) noexcept { return count && words_for(doc_count) <= count; }

	// The same members and delta bytes, however the runs fell.
	bool operator==(const log_deleted& o) const {
		if (count_ != o.count_ || deltas_ != o.deltas_ || dense() != o.dense()) { return false; }
		if (dense()) { return words_ == o.words_; }
		return runs_ == o.runs_ || ordinals() == o.ordinals();
	}

private:
	static std::uint64_t leb(std::uint64_t v) noexcept { return (std::uint64_t(std::bit_width(v | 1)) + 6) / 7; }

	// Words' summary levels: up[0] has a bit per nonzero word, each next level one per nonzero
	// word of the one below, up to a single word.
	static std::vector<std::vector<std::uint64_t>> summary_of(const std::vector<std::uint64_t>& words) {
		std::vector<std::vector<std::uint64_t>> up;
		for (const std::vector<std::uint64_t>* low = &words; low->size() > 1; low = &up.back()) {
			std::vector<std::uint64_t> next(low->size() / 64 + (low->size() % 64 != 0));
			for (std::size_t i = 0; i < low->size(); ++i) {
				if ((*low)[i]) { next[i >> 6] |= std::uint64_t(1) << (i & 63); }
			}
			up.push_back(std::move(next));
		}
		return up;
	}
	std::size_t levels() const noexcept { return 1 + up_.size(); }
	const std::vector<std::uint64_t>& level(std::size_t l) const noexcept { return l ? up_[l - 1] : words_; }
	// Sets bit `o` in the words and its summary bits up to a word already nonzero.
	void mark(std::uint64_t o) noexcept {
		for (std::size_t l = 0; l < levels(); ++l, o >>= 6) {
			std::uint64_t& w = l ? up_[l - 1][o >> 6] : words_[o >> 6];
			const bool was = w != 0;
			w |= std::uint64_t(1) << (o & 63);
			if (was) { return; }
		}
	}
	// The largest bit set below `x` at level `l`, and the smallest set at or above it.
	std::optional<std::uint64_t> prev_bit(std::size_t l, std::uint64_t x) const noexcept {
		const std::vector<std::uint64_t>& v = level(l);
		if (x == 0) { return std::nullopt; }
		x = std::min<std::uint64_t>(x - 1, 64 * std::uint64_t(v.size()) - 1);  // the largest at or below x
		std::size_t w = std::size_t(x >> 6);
		std::uint64_t b = v[w] & (~std::uint64_t(0) >> (63 - (x & 63)));
		if (!b) {
			const std::optional<std::uint64_t> pw = l + 1 < levels() ? prev_bit(l + 1, w) : std::nullopt;
			if (!pw) { return std::nullopt; }  // the top is one word
			w = std::size_t(*pw);
			b = v[w];
		}
		return 64 * std::uint64_t(w) + 63 - std::uint64_t(std::countl_zero(b));
	}
	std::optional<std::uint64_t> next_bit(std::size_t l, std::uint64_t x) const noexcept {
		const std::vector<std::uint64_t>& v = level(l);
		std::size_t w = std::size_t(x >> 6);
		if ((x >> 6) >= v.size()) { return std::nullopt; }
		std::uint64_t b = v[w] & (~std::uint64_t(0) << (x & 63));
		if (!b) {
			const std::optional<std::uint64_t> nw = l + 1 < levels() ? next_bit(l + 1, std::uint64_t(w) + 1) : std::nullopt;
			if (!nw) { return std::nullopt; }
			w = std::size_t(*nw);
			b = v[w];
		}
		return 64 * std::uint64_t(w) + std::uint64_t(std::countr_zero(b));
	}
	// The largest member below `o`, and the smallest above it, in the set before an insert.
	std::optional<std::uint64_t> below(std::uint64_t o) const noexcept {
		if (dense()) { return prev_bit(0, o); }
		std::optional<std::uint64_t> best;
		for (const auto& run : runs_) {
			const auto it = std::lower_bound(run.begin(), run.end(), o);
			if (it != run.begin() && (!best || *std::prev(it) > *best)) { best = *std::prev(it); }
		}
		return best;
	}
	// The smallest member above `o` and below `limit`, else nullopt.
	std::optional<std::uint64_t> above(std::uint64_t o, std::uint64_t limit) const noexcept {
		std::optional<std::uint64_t> best;
		if (dense()) {
			best = next_bit(0, o + 1);
		} else {
			for (const auto& run : runs_) {
				const auto it = std::upper_bound(run.begin(), run.end(), o);
				if (it != run.end() && (!best || *it < *best)) { best = *it; }
			}
		}
		return best && *best < limit ? best : std::nullopt;
	}
	// The delta bytes `fresh` adds: each run of new ordinals between two old neighbours replaces
	// the old gap with the gaps through the run.
	std::uint64_t delta_change(const std::vector<std::uint64_t>& fresh, std::uint64_t doc_count) const {
		std::uint64_t add = 0, drop = 0;
		std::size_t i = 0;
		while (i < fresh.size()) {
			const std::optional<std::uint64_t> p = below(fresh[i]);
			add += p ? leb(fresh[i] - *p) : leb(fresh[i]);
			std::optional<std::uint64_t> s;
			for (; i + 1 < fresh.size(); ++i) {
				if ((s = above(fresh[i], fresh[i + 1]))) { break; }
				add += leb(fresh[i + 1] - fresh[i]);
			}
			if (!s) { s = above(fresh[i], doc_count); }
			if (s) {
				add += leb(*s - fresh[i]);
				drop += p ? leb(*s - *p) : leb(*s);
			}
			++i;
		}
		return add - drop;
	}

	std::uint64_t count_ = 0, deltas_ = 0;
	std::vector<std::uint64_t> words_;               // dense: words_for(doc_count) of them
	std::vector<std::vector<std::uint64_t>> up_;     // dense: the words' summary levels
	std::vector<std::vector<std::uint64_t>> runs_;   // sparse: ascending, disjoint, each more than twice the next
};

// A segment as the log holds it: segment_ref's fields and its deleted set.
struct log_segment {
	std::uint64_t id = 0;
	std::array<std::uint8_t, 16> identity{};
	std::uint64_t size = 0;
	std::uint32_t footer_crc = 0;
	std::uint64_t doc_count = 0, live_count = 0, live_length = 0;
	log_deleted deleted;  // doc_count - live_count ordinals, its doc_count the segment's
	std::uint64_t values_generation = 0;
	std::vector<attribute> attributes;  // tags strictly increasing; the known ones only
	bool operator==(const log_segment&) const = default;
};

// The bytes a snapshot's sections take, entry by entry, without their counts. apply() keeps
// them as edits change the state, so the writer's S is exact (the re-check's C7).
struct log_sizes {
	std::uint64_t pins = 0, segments = 0, metadata = 0, fields = 0;
	bool operator==(const log_sizes&) const = default;
};

// The state a snapshot holds and an edit changes.
struct log_state {
	std::array<char, 16> uuid{};
	std::uint64_t generation = 0;
	std::uint64_t revision = 0, last_docid = 0, next_segment = 1, applied_index = 0, applied_count = 0, schema_version = 0;
	std::uint64_t configured = 0, mask = 0, min_version = 0, max_version = 0;  // versions fit 32 bits
	std::uint64_t doc_count = 0, total_length = 0, live_count = 0, live_length = 0;
	std::vector<std::uint64_t> pins;  // ascending
	std::map<std::uint64_t, log_segment> segments;
	std::map<std::string, std::string> metadata;
	field_table fields;
	std::vector<attribute> attributes;  // the index's, tags strictly increasing
	log_sizes sizes;                    // recount() after building a state by hand
	std::set<std::array<std::uint8_t, 16>> identities;  // the segments', so apply() checks I13 per added segment
	position applied() const noexcept { return {applied_index, applied_count}; }
	bool operator==(const log_state&) const = default;
};

// The scalars in record order: an edit's presence mask has bit i for log_scalars[i].
namespace log_scalar {
inline constexpr unsigned revision = 0, last_docid = 1, next_segment = 2, applied_index = 3, applied_count = 4, schema_version = 5, configured = 6, mask = 7,
	min_version = 8, max_version = 9, doc_count = 10, total_length = 11, live_count = 12, live_length = 13, count = 14;
}
inline constexpr std::uint64_t log_state::* log_scalars[log_scalar::count] = {&log_state::revision, &log_state::last_docid, &log_state::next_segment,
	&log_state::applied_index, &log_state::applied_count, &log_state::schema_version, &log_state::configured, &log_state::mask, &log_state::min_version,
	&log_state::max_version, &log_state::doc_count, &log_state::total_length, &log_state::live_count, &log_state::live_length};

// An edit's sections, in record order: its section mask has bit i for section i.
namespace log_section {
inline constexpr unsigned pins = 0, segments = 1, deletes = 2, metadata = 3, fields = 4, segment_attributes = 5, attributes = 6, count = 7;
}

// Ordinals newly deleted in a segment the state holds, and its live figures after them.
struct log_deletes {
	std::uint64_t segment = 0;
	std::vector<std::uint64_t> ordinals;  // ascending
	std::uint64_t live_count = 0, live_length = 0;
	bool operator==(const log_deletes&) const = default;
};

struct log_segment_attributes {
	std::uint64_t segment = 0;
	std::vector<attribute> attributes;  // the whole set after the edit
	bool operator==(const log_segment_attributes&) const = default;
};

// The difference between two states of one index (L2). Lists ascend by id or key; an empty
// list is an absent section, and fields_from is 0 without declarations.
struct log_edit {
	std::uint64_t generation = 0;
	std::uint32_t scalars = 0;                              // bit i: values[i] is carried
	std::array<std::uint64_t, log_scalar::count> values{};  // 0 where not carried
	std::vector<std::uint64_t> pins_added, pins_removed;
	std::vector<std::uint64_t> segments_removed;
	std::vector<log_segment> segments_added;
	std::vector<log_deletes> deletes;
	std::vector<std::pair<std::string, std::string>> metadata_set;
	std::vector<std::string> metadata_removed;
	std::uint64_t fields_from = 0;  // the table size the declarations extend
	std::vector<field_declaration> fields;
	std::vector<log_segment_attributes> segment_attributes;
	std::optional<std::vector<attribute>> attributes;  // the index's whole set, when it changed
	bool operator==(const log_edit&) const = default;
};

namespace log_detail {

using detail::put;
using detail::reader;

inline constexpr std::uint64_t leb_bytes(std::uint64_t v) noexcept { return (std::uint64_t(std::bit_width(v | 1)) + 6) / 7; }
inline constexpr std::uint64_t sized_bytes(std::uint64_t n) noexcept { return leb_bytes(n) + n; }
inline std::uint64_t attributes_bytes(const std::vector<attribute>& as) noexcept {
	std::uint64_t n = leb_bytes(as.size());
	for (const auto& a : as) { n += leb_bytes(a.tag) + sized_bytes(a.bytes.size()); }
	return n;
}
inline std::uint64_t pair_bytes(const std::string& k, const std::string& v) noexcept { return sized_bytes(k.size()) + sized_bytes(v.size()); }
inline std::uint64_t decl_bytes(const field_declaration& d) noexcept { return sized_bytes(d.name.size()) + 3; }

// A deleted set's encoding, the smaller of a bitset (1) and ascending deltas (2), the bitset on
// a tie; 0 when none is deleted. `bytes` is the body's size.
struct deleted_form {
	std::uint64_t encoding = 0, bytes = 0;
};
inline deleted_form deleted_encoding(std::uint64_t doc_count, std::uint64_t dead, std::uint64_t deltas) noexcept {
	if (dead == 0) { return {}; }
	const std::uint64_t bitset = doc_count / 8 + (doc_count % 8 != 0);
	return bitset <= deltas ? deleted_form{1, bitset} : deleted_form{2, deltas};
}
inline deleted_form deleted_encoding(const log_segment& s) noexcept { return deleted_encoding(s.doc_count, s.deleted.size(), s.deleted.deltas_bytes()); }

// An entry's bytes with its live figures and deleted set as given.
inline std::uint64_t entry_bytes(const log_segment& s, std::uint64_t live_count, std::uint64_t live_length, std::uint64_t deltas) noexcept {
	const deleted_form form = deleted_encoding(s.doc_count, s.doc_count - live_count, deltas);
	return leb_bytes(s.id) + 16 + leb_bytes(s.size) + leb_bytes(s.footer_crc) + leb_bytes(s.doc_count) + leb_bytes(live_count) + leb_bytes(live_length) + 1 +
		(form.encoding ? sized_bytes(form.bytes) : 0) + leb_bytes(s.values_generation) + attributes_bytes(s.attributes);
}
inline std::uint64_t entry_bytes(const log_segment& s) noexcept { return entry_bytes(s, s.live_count, s.live_length, s.deleted.deltas_bytes()); }

// [id][identity, 16 bytes][size][footer CRC][documents][live][live length][deleted encoding]
// [sized body unless none][values generation][attributes]
inline void put_entry(std::string& out, const log_segment& s) {
	put(out, s.id);
	out.append(reinterpret_cast<const char*>(s.identity.data()), 16);
	for (std::uint64_t v : {s.size, std::uint64_t(s.footer_crc), s.doc_count, s.live_count, s.live_length}) { put(out, v); }
	const deleted_form form = deleted_encoding(s);
	put(out, form.encoding);
	if (form.encoding == 1) {
		std::string bits(form.bytes, '\0');
		s.deleted.for_each([&](std::uint64_t d) { bits[d >> 3] = char(std::uint8_t(bits[d >> 3]) | (1u << (d & 7))); });
		detail::put_bytes(out, bits);
	} else if (form.encoding == 2) {
		put(out, form.bytes);
		std::uint64_t prev = 0;
		bool first = true;
		s.deleted.for_each([&](std::uint64_t d) {
			put(out, first ? d : d - prev);
			prev = d;
			first = false;
		});
	}
	put(out, s.values_generation);
	detail::put_attributes(out, s.attributes);
}

// `n` ascending ordinals from a deltas body, each passed to `f`: the first absolute, then gaps
// of at least 1.
template <class F>
inline void get_ordinals(reader& in, std::uint64_t n, F&& f) {
	std::uint64_t prev = 0;
	for (std::uint64_t i = 0; i < n; ++i) {
		const std::uint64_t v = in.get();
		if (i && v == 0) { in.bad("ordinals not ascending"); }
		if (i && v > ~std::uint64_t(0) - prev) { in.bad("ordinal range"); }
		prev = i ? prev + v : v;
		f(prev);
	}
}
inline std::vector<std::uint64_t> get_ordinals(reader& in, std::uint64_t n) {
	std::vector<std::uint64_t> out;
	out.reserve(n);  // the caller bounds n by the bytes left
	get_ordinals(in, n, [&](std::uint64_t o) { out.push_back(o); });
	return out;
}

inline log_segment get_entry(reader& in) {
	log_segment s;
	s.id = in.get();
	if (s.id == 0) { in.bad("segment id"); }  // ids start at 1
	std::memcpy(s.identity.data(), in.bytes(16).data(), 16);
	s.size = in.get();
	s.footer_crc = in.u32();
	s.doc_count = in.get();
	s.live_count = in.get();
	s.live_length = in.get();
	if (s.live_count > s.doc_count) { in.bad("segment live count"); }
	const std::uint64_t dead = s.doc_count - s.live_count, encoding = in.get();
	if ((encoding == 0) != (dead == 0) || encoding > 2) { in.bad("deleted encoding"); }
	// D42: the set takes its form from its count before anything is allocated, so a bitset body
	// decodes to at most its own bytes (rounded to a word) and a deltas body of B bytes, holding
	// at most B ordinals, to at most 8 B.
	const bool dense = log_deleted::dense_at(dead, s.doc_count);
	if (encoding == 1) {
		const std::string_view bits = in.sized();
		if (bits.size() != s.doc_count / 8 + (s.doc_count % 8 != 0)) { in.bad("bitset size"); }
		if (s.doc_count % 8 && std::uint8_t(bits.back()) >> (s.doc_count % 8)) { in.bad("bits past the document count"); }
		std::vector<std::uint64_t> words, ordinals;
		std::uint64_t count = 0, deltas = 0, prev = 0;
		auto take = [&](std::uint64_t o) {
			deltas += leb_bytes(count ? o - prev : o);
			prev = o;
			if (++count > dead) { in.bad("live count"); }
			if (!dense) { ordinals.push_back(o); }
		};
		if (dense) {
			words.assign(log_deleted::words_for(s.doc_count), 0);
			std::memcpy(words.data(), bits.data(), bits.size());  // little-endian hosts only, as the segment format
			for (std::size_t w = 0; w < words.size(); ++w) {
				for (std::uint64_t b = words[w]; b; b &= b - 1) { take(64 * std::uint64_t(w) + std::uint64_t(std::countr_zero(b))); }
			}
		} else {
			ordinals.reserve(dead);  // below doc_count / 64, which the bitset's size bounds
			for (std::size_t b = 0; b < bits.size(); ++b) {
				for (unsigned byte = std::uint8_t(bits[b]); byte; byte &= byte - 1) { take(8 * std::uint64_t(b) + std::uint64_t(std::countr_zero(byte))); }
			}
		}
		if (count != dead) { in.bad("live count"); }
		s.deleted = dense ? log_deleted::adopt_words(std::move(words), count, deltas) : log_deleted::adopt_ordinals(std::move(ordinals), deltas);
	} else if (encoding == 2) {
		const std::uint64_t bytes = in.get();
		if (dead > bytes) { in.bad("deltas size"); }  // a delta takes a byte at least
		reader body(in.bytes(bytes), "manifest log");
		std::uint64_t last = 0;
		if (dense) {
			std::vector<std::uint64_t> words(log_deleted::words_for(s.doc_count));  // at most 8 bytes per ordinal: dead >= the words
			get_ordinals(body, dead, [&](std::uint64_t o) {
				if (o >= s.doc_count) { in.bad("ordinal past the document count"); }
				words[o >> 6] |= std::uint64_t(1) << (o & 63);
				last = o;
			});
			s.deleted = log_deleted::adopt_words(std::move(words), dead, bytes);
		} else {
			std::vector<std::uint64_t> ordinals = get_ordinals(body, dead);
			last = ordinals.back();
			s.deleted = log_deleted::adopt_ordinals(std::move(ordinals), bytes);
		}
		body.finish();
		if (last >= s.doc_count) { in.bad("ordinal past the document count"); }
	}
	if (deleted_encoding(s).encoding != encoding) { in.bad("deleted encoding"); }  // one encoding per set
	s.values_generation = in.get();
	if (s.values_generation != 0) { in.bad("values generation"); }
	s.attributes = detail::get_attributes(in, known_attribute, "segment");
	return s;
}

// [name, sized][role][postings][flags] (fields_detail::put_decl).
inline field_declaration get_decl(reader& in) {
	field_declaration d;
	d.name = std::string(in.sized());
	const std::string_view b = in.bytes(3);
	if (std::uint8_t(b[2]) > 1) { in.bad("field flags"); }
	d.role = field_role(std::uint8_t(b[0]));
	d.postings = field_postings(std::uint8_t(b[1]));
	d.norms = b[2] == 1;
	if (const char* why = field_problem(d)) { in.bad(why); }
	return d;
}

inline void add(std::uint64_t& to, std::uint64_t v, const char* why) {
	if (v > ~std::uint64_t(0) - to) { throw corrupt_index(std::string("manifest log: ") + why); }
	to += v;
}

using scalar_values = std::array<std::uint64_t, log_scalar::count>;
inline scalar_values scalars_of(const log_state& s) noexcept {
	scalar_values v;
	for (unsigned i = 0; i < log_scalar::count; ++i) { v[i] = s.*log_scalars[i]; }
	return v;
}

// What every state holds whatever its records: the structures within the mask, the segment
// versions ordered and present exactly with segments, live length within the total.
inline void check_scalars(const scalar_values& v, bool segments) {
	namespace sc = log_scalar;
	auto bad = [](const char* why) { throw corrupt_index(std::string("manifest log: ") + why); };
	if ((v[sc::configured] & ~v[sc::mask]) || v[sc::min_version] > v[sc::max_version]) { bad("structures or versions"); }
	if (segments ? v[sc::min_version] == 0 : (v[sc::min_version] || v[sc::max_version])) { bad("segment versions"); }
	if (v[sc::live_length] > v[sc::total_length]) { bad("totals"); }
}

// Readability (F13-R42): the mask's bits and the segment versions against what this binary reads.
inline void check_readable(std::uint64_t mask, std::uint64_t min_version, std::uint64_t max_version, const readable_by& r) {
	if (const std::uint64_t lacked = mask & ~r.bits) {
		throw unsupported_index("strata: unsupported index: structure bit " + std::to_string(std::countr_zero(lacked)));
	}
	if ((min_version && min_version < r.min_version) || max_version > r.max_version) {
		throw unsupported_index("strata: unsupported index: segment format versions " + std::to_string(min_version) + " to " + std::to_string(max_version));
	}
}

inline std::uint64_t get_version(reader& in) {
	const std::uint64_t v = in.get();
	if (v > 0xffffffffu) { in.bad("field range"); }
	return v;
}

}  // namespace log_detail

// Sets a state's sizes from its sections: for a state built by hand.
inline void recount(log_state& s) {
	using namespace log_detail;
	s.sizes = {};
	s.identities.clear();
	for (std::uint64_t p : s.pins) { s.sizes.pins += leb_bytes(p); }
	for (const auto& [id, seg] : s.segments) {
		s.sizes.segments += entry_bytes(seg);
		s.identities.insert(seg.identity);
	}
	for (const auto& [k, v] : s.metadata) { s.sizes.metadata += pair_bytes(k, v); }
	for (std::size_t i = 0; i < s.fields.size(); ++i) { s.sizes.fields += decl_bytes(s.fields[i]); }
}

// What a snapshot's size depends on: its generation, scalars, section counts and sizes. A state
// has one; check_edit() gives the one an edit leads to before anything changes.
struct log_shape {
	std::array<char, 16> uuid{};
	std::uint64_t generation = 0;
	log_detail::scalar_values scalars{};
	log_sizes sizes;
	std::uint64_t pins = 0, segments = 0, metadata = 0, fields = 0, attributes = 0;  // counts; the index attributes' bytes
};
inline log_shape shape_of(const log_state& s) noexcept {
	return {s.uuid, s.generation, log_detail::scalars_of(s), s.sizes, s.pins.size(), s.segments.size(), s.metadata.size(), s.fields.size(),
		log_detail::attributes_bytes(s.attributes)};
}

// The bytes encode_snapshot() takes, from the sizes apply() keeps: no section is walked.
inline std::uint64_t snapshot_size(const log_shape& s) noexcept {
	using namespace log_detail;
	std::uint64_t n = leb_bytes(std::uint64_t(log_kind::snapshot)) + leb_bytes(s.generation) + leb_bytes(version) + 16;
	for (std::uint64_t v : s.scalars) { n += leb_bytes(v); }
	return n + leb_bytes(s.pins) + s.sizes.pins + leb_bytes(s.segments) + s.sizes.segments + leb_bytes(s.metadata) + s.sizes.metadata + leb_bytes(s.fields) +
		s.sizes.fields + s.attributes;
}
inline std::uint64_t snapshot_size(const log_state& s) noexcept { return snapshot_size(shape_of(s)); }

// D7's S: the bytes a snapshot takes as the first record of a log, framing included.
inline std::uint64_t snapshot_bytes(const log_shape& s) {
	return ::kronuz::journal::framing::record_end(log_stream(s.uuid, s.generation), 0, snapshot_size(s));
}
inline std::uint64_t snapshot_bytes(const log_state& s) { return snapshot_bytes(shape_of(s)); }

// [kind 1][generation][files version][UUID, 16 bytes][the scalars][pins: count, each]
// [segments: count, entries][metadata: count, (key, value) sized][fields: count, declarations]
// [index attributes]
inline std::string encode_snapshot(const log_state& s) {
	using log_detail::put;
	std::string out;
	put(out, std::uint64_t(log_kind::snapshot));
	put(out, s.generation);
	put(out, version);
	out.append(s.uuid.data(), 16);
	for (auto m : log_scalars) { put(out, s.*m); }
	put(out, s.pins.size());
	for (std::uint64_t p : s.pins) { put(out, p); }
	put(out, s.segments.size());
	for (const auto& [id, seg] : s.segments) { log_detail::put_entry(out, seg); }
	put(out, s.metadata.size());
	for (const auto& [k, v] : s.metadata) {
		detail::put_bytes(out, k);
		detail::put_bytes(out, v);
	}
	put(out, s.fields.size());
	for (std::size_t i = 0; i < s.fields.size(); ++i) { fields_detail::put_decl(out, s.fields[i]); }
	detail::put_attributes(out, s.attributes);
	return out;
}

// [kind 2][generation][scalar mask][the carried scalars][section mask][the sections present]:
//   pins: added (count, each), removed (count, each); segments: removed ids (count, each),
//   added entries (count, entries); deletes: count, per segment [id][count][ordinals as
//   deltas][live][live length]; metadata: set (count, (key, value)), removed (count, keys);
//   fields: [table size extended][count][declarations]; segment attributes: count, per segment
//   [id][attributes]; index attributes: the set.
inline std::string encode_edit(const log_edit& e) {
	using log_detail::put;
	namespace sec = log_section;
	std::string out;
	put(out, std::uint64_t(log_kind::edit));
	put(out, e.generation);
	put(out, e.scalars);
	for (unsigned i = 0; i < log_scalar::count; ++i) {
		if (e.scalars >> i & 1) { put(out, e.values[i]); }
	}
	const std::uint64_t sections = std::uint64_t(!e.pins_added.empty() || !e.pins_removed.empty()) << sec::pins |
		std::uint64_t(!e.segments_removed.empty() || !e.segments_added.empty()) << sec::segments | std::uint64_t(!e.deletes.empty()) << sec::deletes |
		std::uint64_t(!e.metadata_set.empty() || !e.metadata_removed.empty()) << sec::metadata | std::uint64_t(!e.fields.empty()) << sec::fields |
		std::uint64_t(!e.segment_attributes.empty()) << sec::segment_attributes | std::uint64_t(e.attributes.has_value()) << sec::attributes;
	put(out, sections);
	auto ids = [&](const std::vector<std::uint64_t>& v) {
		put(out, v.size());
		for (std::uint64_t x : v) { put(out, x); }
	};
	if (sections >> sec::pins & 1) {
		ids(e.pins_added);
		ids(e.pins_removed);
	}
	if (sections >> sec::segments & 1) {
		ids(e.segments_removed);
		put(out, e.segments_added.size());
		for (const auto& s : e.segments_added) { log_detail::put_entry(out, s); }
	}
	if (sections >> sec::deletes & 1) {
		put(out, e.deletes.size());
		for (const auto& d : e.deletes) {
			put(out, d.segment);
			put(out, d.ordinals.size());
			for (std::size_t i = 0; i < d.ordinals.size(); ++i) { put(out, i ? d.ordinals[i] - d.ordinals[i - 1] : d.ordinals[i]); }
			put(out, d.live_count);
			put(out, d.live_length);
		}
	}
	if (sections >> sec::metadata & 1) {
		put(out, e.metadata_set.size());
		for (const auto& [k, v] : e.metadata_set) {
			detail::put_bytes(out, k);
			detail::put_bytes(out, v);
		}
		put(out, e.metadata_removed.size());
		for (const auto& k : e.metadata_removed) { detail::put_bytes(out, k); }
	}
	if (sections >> sec::fields & 1) {
		put(out, e.fields_from);
		put(out, e.fields.size());
		for (const auto& d : e.fields) { fields_detail::put_decl(out, d); }
	}
	if (sections >> sec::segment_attributes & 1) {
		put(out, e.segment_attributes.size());
		for (const auto& a : e.segment_attributes) {
			put(out, a.segment);
			detail::put_attributes(out, a.attributes);
		}
	}
	if (sections >> sec::attributes & 1) { detail::put_attributes(out, *e.attributes); }
	return out;
}

// A record's kind and generation, read from its start.
struct log_header {
	log_kind kind = log_kind::snapshot;
	std::uint64_t generation = 0;
};
inline log_header record_header(std::string_view record) {
	detail::reader in(record, "manifest log");
	const std::uint64_t kind = in.get();
	if (kind != 1 && kind != 2) { in.bad("record kind"); }
	const std::uint64_t generation = in.get();
	if (generation == 0) { in.bad("generation"); }
	return {log_kind(kind), generation};
}

// Decodes a snapshot and checks it: readability first (the files version, then the mask and
// segment versions against `r`), every count bounded by the bytes left, then the values; damage
// is corrupt_index.
inline log_state decode_snapshot(std::string_view record, const readable_by& r = {}) {
	using namespace log_detail;
	reader in(record, "manifest log");
	if (in.get() != std::uint64_t(log_kind::snapshot)) { in.bad("not a snapshot"); }
	log_state s;
	s.generation = in.get();
	if (s.generation == 0) { in.bad("generation"); }
	if (const std::uint64_t v = in.get(); v != version) { throw unsupported_index("manifest log: version " + std::to_string(v)); }
	std::memcpy(s.uuid.data(), in.bytes(16).data(), 16);
	for (unsigned i = 0; i < log_scalar::count; ++i) {
		s.*log_scalars[i] = i == log_scalar::min_version || i == log_scalar::max_version ? get_version(in) : in.get();
	}
	check_readable(s.mask, s.min_version, s.max_version, r);
	std::uint64_t prev = 0;
	for (std::uint64_t n = in.count(); n; --n) {
		const std::uint64_t p = in.get();
		if (p == 0 || p > s.generation || (!s.pins.empty() && p <= prev)) { in.bad("pinned generation"); }
		s.pins.push_back(prev = p);
	}
	std::uint64_t docs = 0, live = 0, live_length = 0;
	for (std::uint64_t n = in.count(25); n; --n) {
		log_segment seg = get_entry(in);
		if (seg.id >= s.next_segment || (!s.segments.empty() && seg.id <= s.segments.rbegin()->first)) { in.bad("segment id"); }
		add(docs, seg.doc_count, "totals");
		add(live, seg.live_count, "totals");
		add(live_length, seg.live_length, "totals");
		if (!s.identities.insert(seg.identity).second) { in.bad("segment identity twice"); }  // I13
		s.segments.emplace_hint(s.segments.end(), seg.id, std::move(seg));
	}
	for (std::uint64_t n = in.count(2); n; --n) {
		std::string k(in.sized()), v(in.sized());
		if (!s.metadata.empty() && k <= s.metadata.rbegin()->first) { in.bad("keys not ascending"); }
		s.metadata.emplace_hint(s.metadata.end(), std::move(k), std::move(v));
	}
	for (std::uint64_t n = in.count(5); n; --n) {
		field_declaration d = get_decl(in);
		if (s.fields.find(d.name)) { in.bad("a name twice"); }
		if (d.role == field_role::key && s.fields.key_field()) { in.bad("two key fields"); }
		if (s.fields.size() == 0xffffffffu) { in.bad("too many fields"); }
		s.fields.append(std::move(d));
	}
	s.attributes = detail::get_attributes(in, known_index_attribute, "index");
	in.finish();
	if (docs != s.doc_count || live != s.live_count || live_length != s.live_length) { in.bad("totals"); }
	check_scalars(scalars_of(s), !s.segments.empty());
	recount(s);
	return s;
}

// Decodes an edit: readability first (a carried mask or segment version against `r`), every
// count bounded by the bytes left, then the values in record order. What depends on the state
// it applies to is apply()'s.
inline log_edit decode_edit(std::string_view record, const readable_by& r = {}) {
	using namespace log_detail;
	namespace sec = log_section;
	reader in(record, "manifest log");
	if (in.get() != std::uint64_t(log_kind::edit)) { in.bad("not an edit"); }
	log_edit e;
	e.generation = in.get();
	if (e.generation == 0) { in.bad("generation"); }
	const std::uint64_t scalars = in.get();
	if (scalars >> log_scalar::count) { in.bad("scalar mask"); }
	e.scalars = std::uint32_t(scalars);
	for (unsigned i = 0; i < log_scalar::count; ++i) {
		if (scalars >> i & 1) { e.values[i] = i == log_scalar::min_version || i == log_scalar::max_version ? get_version(in) : in.get(); }
	}
	check_readable(scalars >> log_scalar::mask & 1 ? e.values[log_scalar::mask] : 0, scalars >> log_scalar::min_version & 1 ? e.values[log_scalar::min_version] : 0,
		scalars >> log_scalar::max_version & 1 ? e.values[log_scalar::max_version] : 0, r);
	const std::uint64_t sections = in.get();
	if (sections >> sec::count) { in.bad("section mask"); }
	auto ascending = [&](std::vector<std::uint64_t>& out, const char* why) {
		for (std::uint64_t n = in.count(); n; --n) {
			const std::uint64_t v = in.get();
			if (!out.empty() && v <= out.back()) { in.bad(why); }
			out.push_back(v);
		}
	};
	auto empty_section = [&] { in.bad("empty section"); };
	if (sections >> sec::pins & 1) {
		ascending(e.pins_added, "pins not ascending");
		ascending(e.pins_removed, "pins not ascending");
		if (e.pins_added.empty() && e.pins_removed.empty()) { empty_section(); }
	}
	if (sections >> sec::segments & 1) {
		ascending(e.segments_removed, "segment ids not ascending");
		for (std::uint64_t n = in.count(25); n; --n) {
			log_segment seg = get_entry(in);
			if (!e.segments_added.empty() && seg.id <= e.segments_added.back().id) { in.bad("segment ids not ascending"); }
			e.segments_added.push_back(std::move(seg));
		}
		if (e.segments_removed.empty() && e.segments_added.empty()) { empty_section(); }
	}
	if (sections >> sec::deletes & 1) {
		for (std::uint64_t n = in.count(5); n; --n) {
			log_deletes d;
			d.segment = in.get();
			if (!e.deletes.empty() && d.segment <= e.deletes.back().segment) { in.bad("segment ids not ascending"); }
			const std::uint64_t k = in.count();
			if (k == 0) { in.bad("no ordinals"); }
			d.ordinals = get_ordinals(in, k);
			d.live_count = in.get();
			d.live_length = in.get();
			e.deletes.push_back(std::move(d));
		}
		if (e.deletes.empty()) { empty_section(); }
	}
	if (sections >> sec::metadata & 1) {
		for (std::uint64_t n = in.count(2); n; --n) {
			std::string k(in.sized()), v(in.sized());
			if (!e.metadata_set.empty() && k <= e.metadata_set.back().first) { in.bad("keys not ascending"); }
			e.metadata_set.emplace_back(std::move(k), std::move(v));
		}
		for (std::uint64_t n = in.count(); n; --n) {
			std::string k(in.sized());
			if (!e.metadata_removed.empty() && k <= e.metadata_removed.back()) { in.bad("keys not ascending"); }
			e.metadata_removed.push_back(std::move(k));
		}
		if (e.metadata_set.empty() && e.metadata_removed.empty()) { empty_section(); }
	}
	if (sections >> sec::fields & 1) {
		e.fields_from = in.get();
		for (std::uint64_t n = in.count(5); n; --n) { e.fields.push_back(get_decl(in)); }
		if (e.fields.empty()) { empty_section(); }
	}
	if (sections >> sec::segment_attributes & 1) {
		for (std::uint64_t n = in.count(2); n; --n) {
			log_segment_attributes a;
			a.segment = in.get();
			if (!e.segment_attributes.empty() && a.segment <= e.segment_attributes.back().segment) { in.bad("segment ids not ascending"); }
			a.attributes = detail::get_attributes(in, known_attribute, "segment");
			e.segment_attributes.push_back(std::move(a));
		}
		if (e.segment_attributes.empty()) { empty_section(); }
	}
	if (sections >> sec::attributes & 1) { e.attributes = detail::get_attributes(in, known_index_attribute, "index"); }
	in.finish();
	return e;
}

// What an edit leads to, from check_edit(): the shape after it, and each delete entry's delta
// bytes after it (the walk apply() would repeat).
struct log_checked {
	log_shape after;
	std::vector<std::uint64_t> deltas;  // per e.deletes entry
};

// Checks an edit against the state it follows, with D3's checks, and computes the shape it leads
// to, changing nothing: generations contiguous; a removed id present; an added id absent, below
// the next segment id, in id order, its identity new; a newly deleted ordinal below its segment's
// document count and not yet deleted; a removed key present; the table size extended the
// table's; pins at most the generation; the document, live and live-length totals running sums
// equal to the state's. A violation is corrupt_index. Costs the edit: lookups in the state's
// maps and sets, and each delete's walk (log_deleted::insert).
inline log_checked check_edit(const log_state& s, const log_edit& e) {
	using namespace log_detail;
	auto bad = [](const char* why) { throw corrupt_index(std::string("manifest log: ") + why); };
	if (e.generation != s.generation + 1) { bad("generations not contiguous"); }
	log_checked out;
	log_shape& after = out.after;
	after = shape_of(s);
	after.generation = e.generation;
	scalar_values& next = after.scalars;  // the scalars after the edit
	for (unsigned i = 0; i < log_scalar::count; ++i) {
		if (e.scalars >> i & 1) { next[i] = e.values[i]; }
	}
	if (next[log_scalar::next_segment] < s.next_segment) { bad("next segment id lowered"); }  // ids are never reused

	for (std::uint64_t p : e.pins_added) {
		if (p == 0 || p > e.generation || std::binary_search(s.pins.begin(), s.pins.end(), p)) { bad("pinned generation"); }
		after.sizes.pins += leb_bytes(p);
	}
	for (std::uint64_t p : e.pins_removed) {
		if (!std::binary_search(s.pins.begin(), s.pins.end(), p)) { bad("removed pin absent"); }
		after.sizes.pins -= leb_bytes(p);
	}
	after.pins = after.pins + e.pins_added.size() - e.pins_removed.size();
	std::uint64_t docs = s.doc_count, live = s.live_count, live_length = s.live_length;  // the running sums
	for (std::uint64_t id : e.segments_removed) {
		const auto it = s.segments.find(id);
		if (it == s.segments.end()) { bad("removed segment absent"); }
		docs -= it->second.doc_count;
		live -= it->second.live_count;
		live_length -= it->second.live_length;
		after.sizes.segments -= entry_bytes(it->second);
	}
	auto removed = [&](std::uint64_t id) { return std::binary_search(e.segments_removed.begin(), e.segments_removed.end(), id); };
	// An added identity is new (I13): absent from the state but for a segment this edit removes,
	// and once in the edit; each lookup is in the state's identity set, so the cost is the edit's.
	std::set<std::array<std::uint8_t, 16>> added;
	for (const auto& seg : e.segments_added) {
		if (s.segments.count(seg.id)) { bad("added segment present"); }
		if (seg.id == 0 || seg.id >= next[log_scalar::next_segment]) { bad("added segment id"); }
		add(docs, seg.doc_count, "totals");
		add(live, seg.live_count, "totals");
		add(live_length, seg.live_length, "totals");
		if (!added.insert(seg.identity).second) { bad("segment identity twice"); }
		after.sizes.segments += entry_bytes(seg);
	}
	if (!added.empty()) {
		std::set<std::array<std::uint8_t, 16>> leaving;
		for (std::uint64_t id : e.segments_removed) { leaving.insert(s.segments.find(id)->second.identity); }
		for (const auto& id : added) {
			if (s.identities.count(id) && !leaving.count(id)) { bad("segment identity twice"); }
		}
	}
	after.segments = after.segments + e.segments_added.size() - e.segments_removed.size();
	out.deltas.reserve(e.deletes.size());
	for (const auto& d : e.deletes) {
		const auto it = s.segments.find(d.segment);
		if (it == s.segments.end() || removed(d.segment)) { bad("deletes in an absent segment"); }
		const log_segment& seg = it->second;
		if (d.ordinals.empty()) { bad("no ordinals"); }
		if (d.ordinals.back() >= seg.doc_count) { bad("ordinal past the document count"); }
		for (std::uint64_t o : d.ordinals) {
			if (seg.deleted.contains(o)) { bad("ordinal deleted twice"); }
		}
		if (seg.live_count < d.ordinals.size() || d.live_count != seg.live_count - d.ordinals.size()) { bad("segment live count"); }
		if (d.live_length > seg.live_length) { bad("segment live length"); }
		live -= d.ordinals.size();
		live_length -= seg.live_length - d.live_length;
		out.deltas.push_back(seg.deleted.deltas_after(d.ordinals, seg.doc_count));
		after.sizes.segments = after.sizes.segments - entry_bytes(seg) + entry_bytes(seg, d.live_count, d.live_length, out.deltas.back());
	}
	for (const auto& [k, v] : e.metadata_set) {
		if (std::binary_search(e.metadata_removed.begin(), e.metadata_removed.end(), k)) { bad("key set and removed"); }
		if (const auto it = s.metadata.find(k); it != s.metadata.end()) {
			after.sizes.metadata -= pair_bytes(k, it->second);
		} else {
			++after.metadata;
		}
		after.sizes.metadata += pair_bytes(k, v);
	}
	for (const auto& k : e.metadata_removed) {
		const auto it = s.metadata.find(k);
		if (it == s.metadata.end()) { bad("removed key absent"); }
		after.sizes.metadata -= pair_bytes(k, it->second);
		--after.metadata;
	}
	if (!e.fields.empty()) {
		if (e.fields_from != s.fields.size()) { bad("field table size"); }
		if (s.fields.size() + e.fields.size() > 0xffffffffu) { bad("too many fields"); }
		bool key = s.fields.key_field().has_value();
		std::unordered_set<std::string_view> names;  // the edit's own: one lookup per declaration
		for (const auto& d : e.fields) {
			if (s.fields.find(d.name) || !names.insert(d.name).second) { bad("a name twice"); }
			if (d.role == field_role::key && std::exchange(key, true)) { bad("two key fields"); }
			after.sizes.fields += decl_bytes(d);
		}
		after.fields += e.fields.size();
	}
	for (const auto& a : e.segment_attributes) {
		const auto it = s.segments.find(a.segment);
		if (it == s.segments.end() || removed(a.segment)) { bad("attributes of an absent segment"); }
		after.sizes.segments = after.sizes.segments - attributes_bytes(it->second.attributes) + attributes_bytes(a.attributes);
	}
	if (e.attributes) { after.attributes = attributes_bytes(*e.attributes); }
	if (docs != next[log_scalar::doc_count] || live != next[log_scalar::live_count] || live_length != next[log_scalar::live_length]) { bad("totals"); }
	check_scalars(next, after.segments != 0);
	return out;
}

// Replays an edit onto the state it follows: check_edit(), then the change, so a corrupt_index
// leaves the state as it was. The sizes are the checked shape's.
inline void apply(log_state& s, const log_edit& e) {
	const log_checked checked = check_edit(s, e);
	s.generation = e.generation;
	for (unsigned i = 0; i < log_scalar::count; ++i) { s.*log_scalars[i] = checked.after.scalars[i]; }
	if (!e.pins_added.empty() || !e.pins_removed.empty()) {
		std::vector<std::uint64_t> pins;
		std::set_difference(s.pins.begin(), s.pins.end(), e.pins_removed.begin(), e.pins_removed.end(), std::back_inserter(pins));
		s.pins.clear();
		std::merge(pins.begin(), pins.end(), e.pins_added.begin(), e.pins_added.end(), std::back_inserter(s.pins));
	}
	for (std::uint64_t id : e.segments_removed) {
		const auto it = s.segments.find(id);
		s.identities.erase(it->second.identity);
		s.segments.erase(it);
	}
	for (const auto& seg : e.segments_added) {
		s.identities.insert(seg.identity);
		s.segments.emplace(seg.id, seg);
	}
	for (std::size_t i = 0; i < e.deletes.size(); ++i) {
		const log_deletes& d = e.deletes[i];
		log_segment& seg = s.segments.find(d.segment)->second;
		seg.deleted.insert(d.ordinals, seg.doc_count, checked.deltas[i]);
		seg.live_count = d.live_count;
		seg.live_length = d.live_length;
	}
	for (const auto& [k, v] : e.metadata_set) { s.metadata[k] = v; }
	for (const auto& k : e.metadata_removed) { s.metadata.erase(k); }
	for (const auto& d : e.fields) { s.fields.append(d); }
	for (const auto& a : e.segment_attributes) { s.segments.find(a.segment)->second.attributes = a.attributes; }
	if (e.attributes) { s.attributes = *e.attributes; }
	s.sizes = checked.after.sizes;
}

// The edit d → c, every part from the two states (tests and fuzz seeds; the writer takes
// deletes and metadata from its change list, D3). Throws std::invalid_argument for a c no
// edit reaches from d: another index, a generation not d's next, a segment changed in place
// other than by deletes and attributes, a delete cleared, or a field table not extended.
inline log_edit diff(const log_state& d, const log_state& c) {
	auto refuse = [](const char* why) { throw std::invalid_argument(std::string("manifest log diff: ") + why); };
	if (d.uuid != c.uuid) { refuse("another index"); }
	if (c.generation != d.generation + 1) { refuse("generation"); }
	log_edit e;
	e.generation = c.generation;
	for (unsigned i = 0; i < log_scalar::count; ++i) {
		if (d.*log_scalars[i] != c.*log_scalars[i]) {
			e.scalars |= 1u << i;
			e.values[i] = c.*log_scalars[i];
		}
	}
	std::set_difference(c.pins.begin(), c.pins.end(), d.pins.begin(), d.pins.end(), std::back_inserter(e.pins_added));
	std::set_difference(d.pins.begin(), d.pins.end(), c.pins.begin(), c.pins.end(), std::back_inserter(e.pins_removed));
	for (const auto& [id, seg] : d.segments) {
		const auto it = c.segments.find(id);
		if (it == c.segments.end()) {
			e.segments_removed.push_back(id);
			continue;
		}
		const log_segment& to = it->second;
		log_segment same = to;
		same.deleted = seg.deleted;
		same.live_count = seg.live_count;
		same.live_length = seg.live_length;
		same.attributes = seg.attributes;
		if (!(same == seg)) { refuse("a segment changed in place"); }
		const std::vector<std::uint64_t> was = seg.deleted.ordinals(), is = to.deleted.ordinals();
		if (!std::includes(is.begin(), is.end(), was.begin(), was.end())) { refuse("a delete cleared"); }
		if (is.size() != was.size()) {
			log_deletes del{id, {}, to.live_count, to.live_length};
			std::set_difference(is.begin(), is.end(), was.begin(), was.end(), std::back_inserter(del.ordinals));
			e.deletes.push_back(std::move(del));
		} else if (to.live_count != seg.live_count || to.live_length != seg.live_length) {
			refuse("live figures changed without deletes");
		}
		if (to.attributes != seg.attributes) { e.segment_attributes.push_back({id, to.attributes}); }
	}
	for (const auto& [id, seg] : c.segments) {
		if (!d.segments.count(id)) { e.segments_added.push_back(seg); }
	}
	for (const auto& [k, v] : c.metadata) {
		const auto it = d.metadata.find(k);
		if (it == d.metadata.end() || it->second != v) { e.metadata_set.emplace_back(k, v); }
	}
	for (const auto& [k, v] : d.metadata) {
		if (!c.metadata.count(k)) { e.metadata_removed.push_back(k); }
	}
	if (c.fields.size() < d.fields.size()) { refuse("field table shrank"); }
	for (std::size_t i = 0; i < d.fields.size(); ++i) {
		if (!(c.fields[i] == d.fields[i])) { refuse("field table not extended"); }
	}
	if (c.fields.size() > d.fields.size()) {
		e.fields_from = d.fields.size();
		for (std::size_t i = d.fields.size(); i < c.fields.size(); ++i) { e.fields.push_back(c.fields[i]); }
	}
	if (c.attributes != d.attributes) { e.attributes = c.attributes; }
	return e;
}

// D7's bounds: a log rolls over before it passes max(F, 2 S) bytes or max(R, n, S / c) edits since
// its snapshot, S the bytes a snapshot of the current state takes framed (snapshot_bytes) and n
// its entries. F keeps a small index from rolling over every few commits, R keeps about 125 edits
// per rollover however small, and c is the snapshot bytes that decode in about one edit's replay
// time, so a large deleted set does not force a snapshot per R edits (the re-check's ML-R16).
inline constexpr std::uint64_t log_space_floor = 16 << 10;  // F
inline constexpr std::uint64_t log_replay_floor = 128;      // R
inline constexpr std::uint64_t log_decode_ratio = 512;      // c

inline std::uint64_t log_entries(const log_shape& s) noexcept { return s.pins + s.segments + s.metadata + s.fields; }
inline std::uint64_t log_entries(const log_state& s) noexcept { return log_entries(shape_of(s)); }

// Whether a log of `bytes` holding `edits` edits after its snapshot, its state now `s`, is within
// `slack` times D7's bounds: the writer rolls over past 1, and the reader refuses a log past 2
// (SR2-strata), so replay costs at most a small multiple of what a fresh log of the state would.
inline bool log_within(const log_shape& s, std::uint64_t bytes, std::uint64_t edits, std::uint64_t slack = 1) {
	const std::uint64_t S = snapshot_bytes(s);
	return bytes <= slack * std::max(log_space_floor, 2 * S) && edits <= slack * std::max({log_replay_floor, log_entries(s), S / log_decode_ratio});
}
inline bool log_within(const log_state& s, std::uint64_t bytes, std::uint64_t edits, std::uint64_t slack = 1) { return log_within(shape_of(s), bytes, edits, slack); }

// open(..., to) of a generation no log replays whole (D6.8).
struct generation_not_retained : std::runtime_error {
	std::uint64_t generation;
	explicit generation_not_retained(std::uint64_t g) : std::runtime_error("strata: generation " + std::to_string(g) + " not retained"), generation(g) {}
};

// A log whose record of `generation` is damaged (D6.4): the generation before it is the last
// one the log holds whole, which `fallback()` reports (the owner's Q2, D41).
struct log_damage : corrupt_index {
	std::uint64_t generation;
	log_damage(std::uint64_t g, const std::string& why) : corrupt_index(why), generation(g) {}
};

// A log as the open reads it.
struct log_read {
	log_state state;            // the head, or the state at `to`
	std::uint64_t end = 0;      // where the last whole record replayed ends: the next append's offset
	std::uint64_t edits = 0;    // edits replayed after the snapshot
	bool dirty = false;         // a torn tail was dropped (D6.4): the next commit rolls over
	std::uint64_t damaged = 0;  // with `to`: the generation whose record is damaged past it, else 0
};

// Reads manifest-<g> (D6.2 to D6.4, D6.8): journal's scan and tail rule, then the caller's rules.
// The snapshot must read whole and name g, so a scan that stops inside the first record is
// damage whatever the tail verdict (a log is renamed only after its snapshot was synced); the
// snapshot's UUID is the blocks' identity; edits replay in order within 2x D7's bounds. Without
// `to` it replays to the end, and a damaged tail is log_damage, a corrupt_index. With `to` it
// stops at `to`, which must replay whole (generation_not_retained otherwise), and damage after it
// is reported in `damaged`. Any exception drops the partial state with the stack frame (SR4): nothing
// half-applied leaves this function.
inline log_read read_log(std::string_view bytes, std::uint64_t g, const readable_by& r = {}, std::optional<std::uint64_t> to = std::nullopt) {
	namespace fr = ::kronuz::journal::framing;
	auto bad = [](const std::string& why) { throw corrupt_index("manifest log: " + why); };
	if (bytes.substr(0, 8) == std::string_view(manifest_magic, 8)) { throw unsupported_index("strata: unsupported index: a manifest file of another build"); }
	if (to && *to < g) { throw generation_not_retained(*to); }
	std::array<char, 16> uuid{};
	if (bytes.size() >= 24) { std::memcpy(uuid.data(), bytes.data() + 8, 16); }  // a damaged header fails the scan below
	const fr::Scan scan = fr::scan(bytes, log_stream(uuid, g));
	if (scan.records.empty()) { bad("no whole snapshot"); }  // D6.3
	log_read out;
	out.state = decode_snapshot(scan.records[0].bytes, r);
	if (out.state.generation != g) { bad("snapshot generation differs from its log"); }
	if (out.state.uuid != uuid) { bad("snapshot UUID differs from its blocks"); }
	out.end = scan.records[0].end;
	for (std::size_t i = 1; i < scan.records.size() && !(to && out.state.generation == *to); ++i) {
		apply(out.state, decode_edit(scan.records[i].bytes, r));
		out.end = scan.records[i].end;
		++out.edits;
		if (!log_within(out.state, out.end, out.edits, 2)) { bad("past its bounds at generation " + std::to_string(out.state.generation)); }
	}
	if (to) {
		if (out.state.generation != *to) { throw generation_not_retained(*to); }
		if (scan.tail == fr::Tail::damage) { out.damaged = g + scan.records.size(); }
		return out;
	}
	if (scan.tail == fr::Tail::damage) {
		const std::uint64_t damaged = g + scan.records.size();
		throw log_damage(damaged, "manifest log: damaged at offset " + std::to_string(scan.at) + ", generation " + std::to_string(damaged));
	}
	out.dirty = scan.tail == fr::Tail::torn;
	return out;
}

// A fresh log's bytes: the snapshot of `s` as manifest-<s.generation>'s first record (D5).
inline std::string frame_snapshot(const log_state& s) {
	::kronuz::journal::framing::Emitter emitter(log_stream(s.uuid, s.generation), 0);
	emitter.record(encode_snapshot(s));
	return emitter.bytes();
}

// Appends records to an open manifest-<g> (D4). One write per append: the record's framed bytes
// go to the end E in one write_at, so a crash leaves the file at E, at E+n, or at a sector
// boundary between, and never cuts a fragment (pieces of at most `piece` bytes, each ending on a
// 512-byte boundary, when one write cannot carry them). A failed or short write is undone before
// it reports: the file is truncated back to E and synced, so the log stays clean and the retry
// appends at E. A failed sync is truncated back too. One record is in flight: write() refuses
// while the previous one is unsynced. A failed truncate-back or sync breaks the appender, and
// every later call throws.
class log_appender {
public:
	log_appender(kronuz::journal::File& file, const std::array<char, 16>& uuid, std::uint64_t g, std::uint64_t end, std::size_t piece = std::size_t(1) << 30)
		: file_(file), stream_(log_stream(uuid, g)), end_(end), piece_(piece < 512 ? 512 : piece / 512 * 512) {}

	std::uint64_t end() const noexcept { return end_; }
	std::uint64_t generation() const noexcept { return stream_.segment; }
	bool in_flight() const noexcept { return in_flight_; }
	bool broken() const noexcept { return broken_; }
	// Where a record of `size` bytes appended now would end.
	std::uint64_t end_after(std::uint64_t size) const { return ::kronuz::journal::framing::record_end(stream_, end_, size); }

	// Writes `record` at the end, framed, unsynced. Throws what the write threw (a short write as
	// EIO) after the truncate-back; ENOSPC is the caller's to report as space.
	void write(std::string_view record) {
		usable();
		if (in_flight_) { throw std::logic_error("manifest log: an append is already in flight"); }
		::kronuz::journal::framing::Emitter emitter(stream_, end_);
		emitter.record(record);
		const std::string& bytes = emitter.bytes();
		try {
			for (std::size_t at = 0; at < bytes.size();) {
				std::size_t n = std::min(bytes.size() - at, piece_);
				if (at + n < bytes.size()) { n -= (end_ + at + n) % 512; }  // a piece ends on a sector boundary
				if (file_.write_at(end_ + at, std::string_view(bytes).substr(at, n)) != n) { throw std::system_error(EIO, std::generic_category(), "manifest log: short write"); }
				at += n;
			}
		} catch (...) {
			undo();
			throw;
		}
		pending_ = emitter.position();
		in_flight_ = true;
	}
	// The barrier for the record in flight: sync_data, then the end moves past it. A failed sync
	// is undone as a failed write is, since its pages may stay readable without being durable
	// (Linux keeps them clean in the cache), and then breaks the appender.
	void sync() {
		usable();
		if (!in_flight_) { return; }
		try {
			file_.sync_data();
		} catch (...) {
			undo();
			broken_ = true;
			throw;
		}
		end_ = pending_;
		in_flight_ = false;
	}
	void append(std::string_view record) {
		write(record);
		sync();
	}

private:
	void usable() const {
		if (broken_) { throw std::logic_error("manifest log: the appender failed and holds no clean end"); }
	}
	void undo() {
		try {
			file_.truncate(end_);
			file_.sync_data();
		} catch (...) {
			broken_ = true;  // the caller's error stands; the index fences on a broken appender
		}
	}
	kronuz::journal::File& file_;
	::kronuz::journal::framing::Stream stream_;
	std::uint64_t end_, pending_ = 0;
	std::size_t piece_;
	bool in_flight_ = false, broken_ = false;
};

}  // namespace strata::files
