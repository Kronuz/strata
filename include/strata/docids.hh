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

// The docids section's codec (docs/design/reordering.md, D1): an ordinal's docid and a
// docid's ordinal, in three kinds chosen by the order the segment was written in.
//   contiguous (0)  [kind u8][base u64]: docid = base + ordinal.
//   monotone (1)    [kind u8][count u64][per 128 ordinals: first docid u64, delta offset u64]
//                   [LEB128 deltas, each at least 1, for every ordinal but a block's first].
//   permuted (2)    [kind u8][count u64][docid width u8][ordinal width u8]
//                   [forward: count entries of docid - min_docid, docid width bits each]
//                   [8 zero bytes][sorted: the ordinals in docid order, ordinal width bits
//                   each][8 zero bytes][index: per 128 sorted entries, the first one's docid u64].
// Bit arrays are little-endian, entry i at bit i * width; the zero bytes let every entry be
// read with one 8-byte load and one more byte. Only a reordering merge writes kind 2, and
// only when its docids do not increase, so docid_ordered() is exact.
#pragma once

#include "strata/budget.hh"
#include "strata/format.hh"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::docids {

inline constexpr std::uint8_t contiguous = 0;
inline constexpr std::uint8_t monotone = 1;
inline constexpr std::uint8_t permuted = 2;

namespace detail {

// Entry i of a bit array of `width` bits per entry (0 to 64), followed by its zero padding.
inline std::uint64_t get_bits(const char* bits, std::uint64_t i, unsigned width) noexcept {
	if (width == 0) { return 0; }
	const std::uint64_t at = i * width;
	const char* p = bits + (at >> 3);
	const unsigned shift = unsigned(at & 7);
	std::uint64_t v;
	std::memcpy(&v, p, 8);
	v >>= shift;  // little-endian, as format.hh requires
	if (shift + width > 64) { v |= std::uint64_t(std::uint8_t(p[8])) << (64 - shift); }
	return width == 64 ? v : v & ((std::uint64_t(1) << width) - 1);
}

inline void put_bits(std::string& out, std::span<const std::uint64_t> values, unsigned width) {
	std::string bits((values.size() * width + 7) / 8 + 8, '\0');  // with the zero padding
	for (std::size_t i = 0; i < values.size(); ++i) {
		const std::uint64_t at = std::uint64_t(i) * width;
		for (unsigned b = 0; b < width;) {
			const std::uint64_t bit = at + b;
			const unsigned room = std::min(8 - unsigned(bit & 7), width - b);
			const std::uint64_t chunk = (values[i] >> b) & ((1u << room) - 1);
			bits[bit >> 3] = char(std::uint8_t(bits[bit >> 3]) | (chunk << (bit & 7)));
			b += room;
		}
	}
	out += bits;
}

inline std::size_t bit_bytes(std::uint64_t count, unsigned width) noexcept { return std::size_t((count * width + 7) / 8) + 8; }

}  // namespace detail

// Encodes the docids of ordinals 0..n-1 in that order. Docids must be distinct and nonzero;
// a permuted segment's docid sort is charged to `memory` while it lives.
inline std::string encode(std::span<const std::uint64_t> docids, flush_charge* memory = nullptr) {
	std::string out;
	bool increasing = true, contiguous_ids = true;
	for (std::size_t i = 1; i < docids.size() && increasing; ++i) {
		increasing = docids[i] > docids[i - 1];
		contiguous_ids = contiguous_ids && docids[i] == docids[0] + i;
	}
	if (increasing && contiguous_ids) {
		out.push_back(char(contiguous));
		format::put<std::uint64_t>(out, docids.empty() ? 1 : docids[0]);
		return out;
	}
	if (increasing) {
		out.push_back(char(monotone));
		format::put<std::uint64_t>(out, docids.size());
		std::string deltas;
		for (std::size_t b = 0; b < docids.size(); b += format::block) {
			format::put<std::uint64_t>(out, docids[b]);
			format::put<std::uint64_t>(out, deltas.size());
			for (std::size_t i = b + 1; i < std::min(docids.size(), b + format::block); ++i) { format::put_leb(deltas, docids[i] - docids[i - 1]); }
		}
		return out + deltas;
	}
	std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted(docids.size());  // (docid, ordinal)
	const std::uint64_t charged = sorted.capacity() * sizeof(sorted[0]) * 2;  // the pairs and the arrays built from them
	if (memory) { memory->grow(charged); }
	for (std::size_t i = 0; i < docids.size(); ++i) { sorted[i] = {docids[i], i}; }
	std::sort(sorted.begin(), sorted.end());
	for (std::size_t i = 1; i < sorted.size(); ++i) {
		if (sorted[i].first == sorted[i - 1].first) { throw std::invalid_argument("strata: docids must be distinct within a segment"); }
	}
	if (sorted.front().first == 0) { throw std::invalid_argument("strata: docid 0 is reserved"); }
	const std::uint64_t lo = sorted.front().first, hi = sorted.back().first;
	const unsigned docid_width = unsigned(std::bit_width(hi - lo)), ordinal_width = unsigned(std::bit_width(std::uint64_t(docids.size() - 1)));
	out.push_back(char(permuted));
	format::put<std::uint64_t>(out, docids.size());
	out.push_back(char(docid_width));
	out.push_back(char(ordinal_width));
	std::vector<std::uint64_t> column(docids.size());
	for (std::size_t i = 0; i < docids.size(); ++i) { column[i] = docids[i] - lo; }
	detail::put_bits(out, column, docid_width);
	for (std::size_t i = 0; i < sorted.size(); ++i) { column[i] = sorted[i].second; }
	detail::put_bits(out, column, ordinal_width);
	for (std::size_t i = 0; i < sorted.size(); i += format::block) { format::put<std::uint64_t>(out, sorted[i].first); }
	if (memory) { memory->shrink(charged); }
	return out;
}

// A segment's docids section, validated at construction: throws corrupt_segment("docids")
// unless it is exactly one of the kinds above for `count` documents whose smallest and
// largest docids are the footer's `min_docid` and `max_docid`.
class reader {
public:
	reader() = default;
	reader(std::string_view section, std::uint64_t count, std::uint64_t min_docid, std::uint64_t max_docid)
		: s_(section), count_(count), min_(min_docid), max_(max_docid) {
		if (s_.empty()) { bad(); }
		kind_ = std::uint8_t(s_[0]);
		switch (kind_) {
		case contiguous: validate_contiguous(); break;
		case monotone: validate_monotone(); break;
		case permuted: validate_permuted(); break;
		default: bad();
		}
	}

	// Whether ordinal order is docid order (kinds 0 and 1).
	bool docid_ordered() const noexcept { return kind_ != permuted; }
	std::uint8_t kind() const noexcept { return kind_; }

	// The docid of ordinal ord < count.
	std::uint64_t docid(ordinal_t ord) const {
		if (ord >= count_) { throw std::out_of_range("strata: ordinal out of range"); }
		if (kind_ == contiguous) { return format::get<std::uint64_t>(s_.data() + 1) + ord; }
		if (kind_ == permuted) { return min_ + detail::get_bits(forward_, ord, docid_width_); }
		const std::uint64_t b = ord / format::block;
		std::uint64_t d = format::get<std::uint64_t>(s_.data() + 9 + 16 * b);
		const char* p = deltas_ + format::get<std::uint64_t>(s_.data() + 17 + 16 * b);
		for (std::uint64_t i = b * format::block; i < ord; ++i) { d += format::get_leb(&p, s_.data() + s_.size()); }
		return d;
	}

	// The ordinal holding docid, if any.
	std::optional<ordinal_t> ordinal_of(std::uint64_t docid) const {
		if (count_ == 0 || docid < min_ || docid > max_) { return std::nullopt; }
		if (kind_ == contiguous) { return ordinal_t(docid - format::get<std::uint64_t>(s_.data() + 1)); }
		const std::uint64_t blocks = (count_ + format::block - 1) / format::block;
		if (kind_ == permuted) {
			std::uint64_t lo = 0, hi = blocks;  // the last block whose first docid <= docid
			while (hi - lo > 1) {
				const std::uint64_t mid = (lo + hi) / 2;
				(format::get<std::uint64_t>(index_ + 8 * mid) <= docid ? lo : hi) = mid;
			}
			std::uint64_t first = lo * format::block, last = std::min(count_, first + format::block);  // [first, last)
			while (last - first > 1) {
				const std::uint64_t mid = (first + last) / 2;
				(sorted_docid(mid) <= docid ? first : last) = mid;
			}
			const auto ord = ordinal_t(detail::get_bits(sorted_, first, ordinal_width_));
			return sorted_docid(first) == docid ? std::optional<ordinal_t>(ord) : std::nullopt;
		}
		std::uint64_t lo = 0, hi = blocks;
		while (hi - lo > 1) {
			const std::uint64_t mid = (lo + hi) / 2;
			(format::get<std::uint64_t>(s_.data() + 9 + 16 * mid) <= docid ? lo : hi) = mid;
		}
		std::uint64_t d = format::get<std::uint64_t>(s_.data() + 9 + 16 * lo);
		const char* p = deltas_ + format::get<std::uint64_t>(s_.data() + 17 + 16 * lo);
		const std::uint64_t end = std::min<std::uint64_t>(count_, (lo + 1) * format::block);
		for (std::uint64_t ord = lo * format::block;; ++ord) {
			if (d == docid) { return ordinal_t(ord); }
			if (d > docid || ord + 1 == end) { return std::nullopt; }
			d += format::get_leb(&p, s_.data() + s_.size());
		}
	}

private:
	[[noreturn]] static void bad() { throw corrupt_segment("docids"); }

	std::uint64_t sorted_docid(std::uint64_t i) const noexcept {
		return min_ + detail::get_bits(forward_, detail::get_bits(sorted_, i, ordinal_width_), docid_width_);
	}

	void validate_contiguous() const {
		if (s_.size() != 9) { bad(); }
		const std::uint64_t base = format::get<std::uint64_t>(s_.data() + 1);
		if (count_ && (base == 0 || base != min_ || base + (count_ - 1) != max_ || base + (count_ - 1) < base)) { bad(); }
	}

	// Each block's first docid above the previous block's last, every delta at least 1,
	// offsets in order, no trailing bytes, and the footer's minimum and maximum equal to the
	// first and last.
	void validate_monotone() {
		if (s_.size() < 9 || format::get<std::uint64_t>(s_.data() + 1) != count_) { bad(); }
		const std::uint64_t blocks = (count_ + format::block - 1) / format::block;
		if (blocks > (s_.size() - 9) / 16) { bad(); }
		deltas_ = s_.data() + 9 + 16 * blocks;
		const char* p = deltas_;
		const char* end = s_.data() + s_.size();
		std::uint64_t last = 0;
		for (std::uint64_t b = 0; b < blocks; ++b) {
			std::uint64_t d = format::get<std::uint64_t>(s_.data() + 9 + 16 * b);
			if (d <= last || format::get<std::uint64_t>(s_.data() + 17 + 16 * b) != std::uint64_t(p - deltas_)) { bad(); }
			if (b == 0 && d != min_) { bad(); }
			for (std::uint64_t i = b * format::block + 1; i < std::min<std::uint64_t>(count_, (b + 1) * format::block); ++i) {
				const std::uint64_t delta = format::get_leb(&p, end);
				if (delta == 0 || d + delta < d) { bad(); }
				d += delta;
			}
			last = d;
		}
		if (p != end || (count_ && last != max_)) { bad(); }
	}

	// The widths match the count and the docid range, the length is exact, the padding is
	// zero, every forward docid lies in [min, max], the sorted ordinals are a permutation
	// whose docids strictly increase from min to max, every index entry is its block's first
	// docid, and the forward docids do not increase (otherwise kind 0 or 1 is written).
	void validate_permuted() {
		if (s_.size() < 11 || format::get<std::uint64_t>(s_.data() + 1) != count_ || count_ < 2 || min_ == 0 || max_ <= min_) { bad(); }
		docid_width_ = std::uint8_t(s_[9]);
		ordinal_width_ = std::uint8_t(s_[10]);
		if (docid_width_ != std::bit_width(max_ - min_) || ordinal_width_ != std::bit_width(count_ - 1)) { bad(); }
		const std::uint64_t blocks = (count_ + format::block - 1) / format::block;
		const std::size_t fwd = detail::bit_bytes(count_, docid_width_), srt = detail::bit_bytes(count_, ordinal_width_);
		if (s_.size() != 11 + fwd + srt + 8 * blocks) { bad(); }
		forward_ = s_.data() + 11;
		sorted_ = forward_ + fwd;
		index_ = sorted_ + srt;
		auto padded = [&](const char* bits, std::size_t bytes, std::uint64_t used_bits) {
			for (std::uint64_t bit = used_bits; bit < 8 * std::uint64_t(bytes); ++bit) {
				if (std::uint8_t(bits[bit >> 3]) >> (bit & 7) & 1) { return false; }
			}
			return true;
		};
		if (!padded(forward_, fwd, count_ * docid_width_) || !padded(sorted_, srt, count_ * ordinal_width_)) { bad(); }
		bool increasing = true;
		std::uint64_t prev = 0;
		for (std::uint64_t i = 0; i < count_; ++i) {
			const std::uint64_t delta = detail::get_bits(forward_, i, docid_width_);
			if (delta > max_ - min_) { bad(); }
			if (i && delta <= prev) { increasing = false; }
			prev = delta;
		}
		if (increasing) { bad(); }
		std::vector<std::uint64_t> seen((count_ + 63) / 64, 0);
		std::uint64_t last = 0;
		for (std::uint64_t i = 0; i < count_; ++i) {
			const std::uint64_t ord = detail::get_bits(sorted_, i, ordinal_width_);
			if (ord >= count_ || (seen[ord >> 6] >> (ord & 63) & 1)) { bad(); }
			seen[ord >> 6] |= std::uint64_t(1) << (ord & 63);
			const std::uint64_t d = min_ + detail::get_bits(forward_, ord, docid_width_);
			if ((i == 0 && d != min_) || (i && d <= last)) { bad(); }
			if (i % format::block == 0 && format::get<std::uint64_t>(index_ + 8 * (i / format::block)) != d) { bad(); }
			last = d;
		}
		if (last != max_) { bad(); }
	}

	std::string_view s_;
	std::uint64_t count_ = 0, min_ = 0, max_ = 0;
	std::uint8_t kind_ = contiguous, docid_width_ = 0, ordinal_width_ = 0;
	const char* deltas_ = nullptr;
	const char *forward_ = nullptr, *sorted_ = nullptr, *index_ = nullptr;
};

}  // namespace strata::docids
