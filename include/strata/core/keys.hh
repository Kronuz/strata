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
#include "strata/core/budget.hh"
#include "strata/core/format.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::keys {

struct sections {
	std::string keys, index;
};

// A key held elsewhere, as a view, and its ordinal.
struct entry {
	const char* bytes;
	std::uint32_t size;
	ordinal_t ord;
	std::string_view key() const noexcept { return {bytes, size}; }
};

// filter8::build's working set for n hashes, bounded: 32 B a hash (two copies, the peel
// order) and up to 21 B a slot at 1.25 slots a hash or fewer past 10,000 hashes; the 1 MiB
// covers its 256 KiB table and smaller sets' extra slots.
inline std::uint64_t filter_build_bytes(std::uint64_t n) noexcept { return 64 * n + (std::uint64_t(1) << 20); }

// Builds the sections from keys arriving in strictly increasing order, holding none of them:
// each goes to the dictionary as it arrives and only its hash stays, for the filter (8 B a
// key, charged to `memory` as it grows). `alphabet` holds every byte any key uses; the
// dictionary's bytes depend on it, so the exact set gives a fresh build's bytes. Throws
// std::invalid_argument on an empty, repeated, or decreasing key.
class sorted_encoder {
public:
	explicit sorted_encoder(std::string_view alphabet, flush_charge* memory = nullptr) : b_(alphabet), memory_(memory) {}
	sorted_encoder(const sorted_encoder&) = delete;
	sorted_encoder& operator=(const sorted_encoder&) = delete;
	~sorted_encoder() { release(); }

	void add(std::string_view key, ordinal_t ord) {
		if (key.empty()) { throw std::invalid_argument("strata: empty key"); }
		if (!hashes_.empty() && key <= std::string_view(last_)) {
			throw std::invalid_argument(key == std::string_view(last_) ? "strata: a key repeated within a segment" : "strata: keys out of order");
		}
		if (hashes_.empty()) { first_.assign(key); }
		last_.assign(key);
		hashes_.push_back(xxh64::hash(key));
		if (memory_ && hashes_.capacity() * sizeof(std::uint64_t) > charged_) {
			memory_->grow(hashes_.capacity() * sizeof(std::uint64_t) - charged_);
			charged_ = hashes_.capacity() * sizeof(std::uint64_t);
		}
		value_.clear();
		format::put_leb(value_, ord);
		b_.add(key, value_);
	}
	std::size_t size() const noexcept { return hashes_.size(); }

	sections finish() {
		sections out;
		format::put_leb(out.index, hashes_.size());
		if (!hashes_.empty()) {
			format::put_leb(out.index, first_.size());
			out.index += first_;
			format::put_leb(out.index, last_.size());
			out.index += last_;
			const std::uint64_t build = filter_build_bytes(hashes_.size());
			if (memory_) { memory_->grow(build); }
			const std::string filter = fusefilter::filter8::build(hashes_).serialize();
			if (memory_) { memory_->shrink(build); }
			format::put_leb(out.index, filter.size());
			out.index += filter;
		}
		auto t = b_.finish();
		out.keys = std::move(t.blocks);
		out.index += t.index;
		release();
		return out;
	}

private:
	void release() noexcept {
		if (memory_) { memory_->shrink(std::exchange(charged_, 0)); }
		std::vector<std::uint64_t>().swap(hashes_);
	}
	termdict::builder b_;
	flush_charge* memory_;
	std::vector<std::uint64_t> hashes_;
	std::string first_, last_, value_;
	std::uint64_t charged_ = 0;
};

// The bytes keys use, in order: termdict's alphabet for them.
inline std::string alphabet_of(const std::array<bool, 256>& used) {
	std::string alphabet;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { alphabet.push_back(char(c)); }
	}
	return alphabet;
}

// Encodes (key, ordinal) entries, sorting them in place; throws std::invalid_argument on an
// empty or repeated key. Its scratch (hashes, the filter's build) is charged to `memory`.
inline sections encode(std::span<entry> entries, flush_charge* memory = nullptr) {
	std::sort(entries.begin(), entries.end(), [](const entry& a, const entry& b) { return a.key() < b.key() || (a.key() == b.key() && a.ord < b.ord); });
	std::array<bool, 256> used{};
	for (const entry& e : entries) {
		for (char c : e.key()) { used[std::uint8_t(c)] = true; }
	}
	sorted_encoder enc(alphabet_of(used), memory);
	for (const entry& e : entries) { enc.add(e.key(), e.ord); }
	return enc.finish();
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
		} catch (const std::invalid_argument&) {
			corrupt("key_index");  // the FST's own check
		}
	}

	std::uint64_t size() const noexcept { return count_; }
	std::string_view min() const noexcept { return min_; }
	std::string_view max() const noexcept { return max_; }

	// Whether the segment may hold key: inside the range and in the filter.
	bool may_hold(std::string_view key) const { return may_hold(key, xxh64::hash(key)); }
	// The same with the key's hash, which every segment's filter shares (I12).
	bool may_hold(std::string_view key, std::uint64_t hash) const {
		if (!count_ || key < min_ || key > max_) { return false; }
		return filter_->contains(hash);
	}

	// The ordinal key names, if any.
	std::optional<ordinal_t> find(std::string_view key) const { return find(key, xxh64::hash(key)); }
	std::optional<ordinal_t> find(std::string_view key, std::uint64_t hash) const {
		if (!may_hold(key, hash)) { return std::nullopt; }
		std::optional<std::string_view> v;
		try {
			v = dict_->find(key);
		} catch (const termdict::corrupt&) {
			corrupt("keys checksum");
		}
		if (!v) { return std::nullopt; }
		return ordinal(*v);
	}

	// Every key in order with its ordinal; keys_plugin's each_key walks it for check(),
	// merges, and the digest. Keys must strictly increase and their count must equal the index's.
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

	// Keys >= from, pulled one at a time without checking the whole walk (scan's lazy form).
	class range {
	public:
		range(const reader& r, std::string_view from) : r_(&r) {
			if (r.dict_) { c_ = std::make_unique<termdict::reader::cursor>(*r.dict_); more_ = seek(from); }
		}
		bool valid() const noexcept { return more_; }
		std::string_view key() const noexcept { return c_->key(); }
		ordinal_t ordinal() const { return r_->ordinal(c_->value()); }
		void next() {
			try {
				more_ = c_->next();
			} catch (const termdict::corrupt&) {
				corrupt("keys checksum");
			}
		}

	private:
		bool seek(std::string_view from) {
			try {
				return c_->seek(from);
			} catch (const termdict::corrupt&) {
				corrupt("keys checksum");
			}
		}
		const reader* r_;
		std::unique_ptr<termdict::reader::cursor> c_;
		bool more_ = false;
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
