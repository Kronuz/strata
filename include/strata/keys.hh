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

// The key dictionary (docs/design/format-13.md, F8; candidate K1): one segment's keys,
// each naming one ordinal.
//   keys       termdict blocks over the keys in byte order; each value the key's ordinal,
//              LEB128.
//   key_index  [key count LEB][with keys: min key (LEB length, bytes), max key (likewise),
//              filter length LEB, fusefilter filter8 over xxHash64 seed 0 (I12)][the blocks'
//              termdict index: alphabet and FST].
// Keys are nonempty and distinct; a document has at most one.
#pragma once

#include "fusefilter.hh"
#include "hashes.hh"
#include "strata/format.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::keys {

struct sections {
	std::string keys, index;
};

// Encodes (key, ordinal) pairs; sorts them, and throws std::invalid_argument on an empty
// or repeated key.
inline sections encode(std::vector<std::pair<std::string_view, ordinal_t>> entries) {
	std::sort(entries.begin(), entries.end());
	std::array<bool, 256> used{};
	std::vector<std::uint64_t> hashes;
	hashes.reserve(entries.size());
	for (std::size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].first.empty()) { throw std::invalid_argument("strata: empty key"); }
		if (i && entries[i].first == entries[i - 1].first) { throw std::invalid_argument("strata: a key repeated within a segment"); }
		for (char c : entries[i].first) { used[std::uint8_t(c)] = true; }
		hashes.push_back(xxh64::hash(entries[i].first));
	}
	std::string alphabet;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { alphabet.push_back(char(c)); }
	}
	termdict::builder b(alphabet);
	std::string value;
	for (const auto& [key, ord] : entries) {
		value.clear();
		format::put_leb(value, ord);
		b.add(key, value);
	}
	sections out;
	format::put_leb(out.index, entries.size());
	if (!entries.empty()) {
		format::put_leb(out.index, entries.front().first.size());
		out.index += entries.front().first;
		format::put_leb(out.index, entries.back().first.size());
		out.index += entries.back().first;
		const std::string filter = fusefilter::filter8::build(hashes).serialize();
		format::put_leb(out.index, filter.size());
		out.index += filter;
	}
	auto t = b.finish();
	out.keys = std::move(t.blocks);
	out.index += t.index;
	return out;
}

// A segment's keys over its two sections. The index's header and filter are parsed at
// construction (the caller verified key_index whole); the termdict index is copied, so
// lookups never traverse mapped bytes, and blocks are verified through `verify` as read.
// Keys are checked in order and against the count as a cursor walks them.
class reader {
public:
	reader() = default;
	reader(std::string_view keys, std::string_view index, std::uint64_t doc_count, termdict::reader::verifier verify, void* context)
		: doc_count_(doc_count) {
		const char* p = index.data();
		const char* end = p + index.size();
		count_ = format::get_leb(&p, end);
		if (count_ > doc_count) { corrupt("key count"); }
		auto bytes = [&] {
			const std::uint64_t n = format::get_leb(&p, end);
			if (n > std::uint64_t(end - p)) { corrupt("key_index"); }
			std::string_view out(p, std::size_t(n));
			p += n;
			return out;
		};
		if (count_) {
			min_ = bytes();
			max_ = bytes();
			if (min_.empty() || min_ > max_ || (count_ == 1 && min_ != max_)) { corrupt("key range"); }
			filter_ = fusefilter::filter8_view::parse(bytes());
			if (!filter_) { corrupt("key filter"); }
		}
		dict_index_ = std::make_unique<std::string>(p, std::size_t(end - p));
		try {
			dict_.emplace(keys, *dict_index_, verify, context);
		} catch (const termdict::corrupt&) {
			corrupt("key_index");
		}
	}

	std::uint64_t size() const noexcept { return count_; }
	std::string_view min() const noexcept { return min_; }
	std::string_view max() const noexcept { return max_; }

	// Whether the segment may hold key: inside the range and in the filter.
	bool may_hold(std::string_view key) const {
		if (!count_ || key < min_ || key > max_) { return false; }
		return filter_->contains(xxh64::hash(key));
	}

	// The ordinal key names, if any.
	std::optional<ordinal_t> find(std::string_view key) const {
		if (!may_hold(key)) { return std::nullopt; }
		std::optional<std::string_view> v;
		try {
			v = dict_->find(key);
		} catch (const termdict::corrupt&) {
			corrupt("keys checksum");
		}
		if (!v) { return std::nullopt; }
		return ordinal(*v);
	}

	// Every key in order with its ordinal; the merge's and the digest's walk. Keys must
	// strictly increase and their count must equal the index's.
	class cursor {
	public:
		explicit cursor(const reader& r) : r_(&r) {
			if (r.dict_) { c_.emplace(*r.dict_); }
		}
		bool next() {
			try {
				if (!c_ || !c_->next()) {
					if (seen_ != r_->count_) { corrupt("key count"); }
					return false;
				}
			} catch (const termdict::corrupt&) {
				corrupt("keys checksum");
			}
			if (++seen_ > r_->count_ || (seen_ > 1 && c_->key() <= std::string_view(last_)) || c_->key().empty()) { corrupt("key order"); }
			last_.assign(c_->key());
			ord_ = r_->ordinal(c_->value());
			return true;
		}
		std::string_view key() const noexcept { return c_->key(); }
		ordinal_t ordinal() const noexcept { return ord_; }

	private:
		const reader* r_;
		std::optional<termdict::reader::cursor> c_;
		std::string last_;
		std::uint64_t seen_ = 0;
		ordinal_t ord_ = 0;
	};

	// Calls f(key, ordinal) from the first key >= from, in order, until f returns false.
	template <class F>
	void scan(std::string_view from, F&& f) const {
		if (!dict_) { return; }
		try {
			dict_->scan(from, [&](std::string_view key, std::string_view value) { return f(key, ordinal(value)); });
		} catch (const termdict::corrupt&) {
			corrupt("keys checksum");
		}
	}

	// Bytes held outside the mapping (the termdict index).
	std::size_t pinned_bytes() const noexcept { return dict_index_ ? dict_index_->capacity() : 0; }

private:
	ordinal_t ordinal(std::string_view value) const {
		const char* p = value.data();
		const char* end = p + value.size();
		const std::uint64_t ord = format::get_leb(&p, end);
		if (p != end || ord >= doc_count_) { corrupt("key ordinal"); }
		return ordinal_t(ord);
	}

	std::uint64_t doc_count_ = 0, count_ = 0;
	std::string_view min_, max_;
	std::optional<fusefilter::filter8_view> filter_;
	std::unique_ptr<std::string> dict_index_;  // on the heap, so a moved reader's views stay valid
	std::optional<termdict::reader> dict_;
};

}  // namespace strata::keys
