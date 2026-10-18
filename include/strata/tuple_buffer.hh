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

// An index's buffer at one flush thread (docs/research/indexing.md, inverter C, batched
// into sorted runs): each token becomes a 20-byte tuple (hash, ordinal, offset in its
// document's text, position and length) in one of 256 buckets by the hash's top byte, the
// text copied once, and nothing is looked up per token. Every batch of about a million
// tuples is grouped bucket by bucket with a small table and a stable counting sort, each
// token checked against its term's bytes (equal hashes never merge two terms), and the
// batch's terms, sorted, become a run: one compressed entry of postings per term, written
// in one sequential piece. No term table outlives a batch. The flush merges the runs by
// term, reading each one in order, and hands each term's postings to
// segment_writer::flush_from in ordinal order: the segment a segment_writer given the
// same documents writes.
#pragma once

#include "strata/writer.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace strata {

namespace tuple_detail {

// Appended to in place and read in order; chunks never move, so growth copies nothing.
// clear() keeps the chunks for the next fill.
template <class T, unsigned Shift = 12>
class chunked {
public:
	static constexpr std::size_t chunk = std::size_t(1) << Shift;
	void push_back(const T& v, flush_charge* memory) {
		if (left_ == 0) {
			if (size_ >> Shift == chunks_.size()) {
				chunks_.push_back(std::unique_ptr<T[]>(new T[chunk]));  // uninitialized: every slot is written before it is read
				if (memory) { memory->grow(chunk * sizeof(T)); }
			}
			at_ = chunks_[size_ >> Shift].get();
			left_ = chunk;
		}
		*at_++ = v;
		--left_;
		++size_;
	}
	T& operator[](std::size_t i) noexcept { return chunks_[i >> Shift][i & (chunk - 1)]; }
	const T& operator[](std::size_t i) const noexcept { return chunks_[i >> Shift][i & (chunk - 1)]; }
	std::size_t size() const noexcept { return size_; }
	void clear() noexcept { size_ = 0, left_ = 0, at_ = nullptr; }

private:
	std::vector<std::unique_ptr<T[]>> chunks_;
	T* at_ = nullptr;  // the next slot in the current chunk
	std::size_t size_ = 0, left_ = 0;
};

// Bytes appended in pieces that never move; a piece is addressed by its 64-bit offset.
// Pages start at 4 KiB and double to 1 MiB, so a small buffer allocates little.
class byte_log {
public:
	static constexpr unsigned shift = 20;  // offsets: page << 20 | offset; a piece never spans two pages
	std::uint64_t append(std::size_t n, flush_charge* memory) {
		if (pages_.empty() || fill_ + n > size_) {
			size_ = std::max(std::min(std::size_t(1) << shift, std::size_t(4096) << pages_.size()), n);  // a larger piece takes a page of its own
			pages_.push_back(std::unique_ptr<char[]>(new char[size_]));
			if (memory) { memory->grow(size_); }
			fill_ = 0;
		}
		const std::uint64_t at = (std::uint64_t(pages_.size() - 1) << shift) | fill_;
		fill_ += n;
		return at;
	}
	char* at(std::uint64_t offset) noexcept { return pages_[offset >> shift].get() + (offset & ((std::uint64_t(1) << shift) - 1)); }
	const char* at(std::uint64_t offset) const noexcept { return pages_[offset >> shift].get() + (offset & ((std::uint64_t(1) << shift) - 1)); }

private:
	std::vector<std::unique_ptr<char[]>> pages_;
	std::size_t fill_ = 0, size_ = 0;  // the last page's fill and size
};

// The bytes, zero-padded to a word, through a multiply and an xorshift per word; a word
// load may read past the term, so the bytes come from the padded text arena.
inline std::uint64_t hash(const char* p, std::size_t len) noexcept {
	constexpr std::uint64_t P = 0x9E3779B97F4A7C15ull;
	std::uint64_t w;
	if (len <= 8) {
		std::memcpy(&w, p, 8);
		w &= ~std::uint64_t(0) >> (64 - 8 * len);
		w *= P;
		w ^= w >> 29;
		return w + std::uint64_t(len) * 0xD6E8FEB86659FD93ull;
	}
	std::uint64_t h = std::uint64_t(len) * P;
	std::size_t i = 0;
	for (; i + 8 < len; i += 8) {
		std::memcpy(&w, p + i, 8);
		h = (h ^ w) * P;
		h ^= h >> 29;
	}
	std::memcpy(&w, p + len - 8, 8);  // the last word, overlapping the one before
	h = (h ^ w) * P;
	return h ^ (h >> 32);
}

inline void put_leb(char*& p, std::uint64_t v) noexcept {
	while (v >= 0x80) {
		*p++ = char(v | 0x80);
		v >>= 7;
	}
	*p++ = char(v);
}
inline std::uint64_t get_leb(const char*& p) noexcept {
	std::uint64_t v = 0;
	for (unsigned s = 0;; s += 7) {
		const std::uint8_t b = std::uint8_t(*p++);
		v |= std::uint64_t(b & 0x7F) << s;
		if (b < 0x80) { return v; }
	}
}

}  // namespace tuple_detail

class tuple_buffer {
public:
	// Where the buffer reports its allocations (D17); none: nothing is counted.
	flush_charge* memory = nullptr;
	// Every hash is masked with it: tests narrow it to make terms collide.
	std::uint64_t hash_mask = ~std::uint64_t(0);
	// Tuples grouped into a run at a time: about 20 MiB of them by default, so a bucket's
	// share (1/256) stays in L2.
	std::size_t batch_tuples = std::size_t(1) << 20;
	// Each drain writes its terms' final blocks and the flush appends them (format 13's bit
	// 14, a prototype: no reader yet); STRATA_ENCODE_ONCE=1. Fixed before the first drain.
	bool encode_once = [] {
		const char* e = std::getenv("STRATA_ENCODE_ONCE");
		return e && *e == '1';
	}();

	// A document: its terms, each once with its wdf and positions, and its text's tokens in
	// order, each adding one to its term's wdf; a term is in one of the two and positions
	// increase. `key` is one of its terms or empty. Returns its ordinal.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens,
		std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		return add_with(docid, terms, text, stored, version, key, [&](auto&& token, std::string_view) {
			for (const auto& tk : tokens) { token(tk.offset, tk.size, tk.position); }
			return length;
		});
	}
	// A document whose tokens are its text's words (split_words) at positions 1, 2, ..., its
	// length their count; the text is split where it was copied.
	ordinal_t add_split(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::string_view stored, std::uint64_t version,
		std::string_view key) {
		if (text.size() > std::numeric_limits<std::uint32_t>::max()) { throw std::invalid_argument("strata: text too large to split"); }
		return add_with(docid, terms, text, stored, version, key, [&](auto&& token, std::string_view copy) {
			std::uint32_t position = 0;
			split_words(copy, [&](std::size_t offset, std::size_t size) { token(std::uint32_t(offset), std::uint32_t(size), ++position); });
			return position;
		});
	}

private:
	// add()'s body: `tokens(token, copy)` calls token(offset, size, position) for each token of
	// the copied text and returns the document's length.
	template <class Tokens>
	ordinal_t add_with(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::string_view stored, std::uint64_t version,
		std::string_view key, Tokens&& tokens) {
		if (docids_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (stored.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		if (batched_ >= batch_tuples) { drain(); }
		const ordinal_t ord = ordinal_t(docids_.size());
		std::size_t entry_bytes = 0;
		for (const auto& in : terms) { entry_bytes += in.term.size(); }
		char* base = room(text.size() + entry_bytes);
		if (!text.empty()) { std::memcpy(base, text.data(), text.size()); }
		auto token = [&](std::uint32_t offset, std::uint32_t size, std::uint32_t position) {
			const char* p = base + offset;
			const std::uint64_t h = tuple_detail::hash(p, size) & hash_mask;
			if (size < long_term && position < (std::uint32_t(1) << 24)) {
				push(tuple(h, ord, offset, position << 8 | size));
			} else {
				side_.push_back({p, size, 1, 1, positions_.size(), false}, memory);
				positions_.push_back(position, memory);
				push(tuple(h, ord, std::uint32_t(side_.size() - 1), long_term));
			}
		};
		const std::uint32_t length = tokens(token, std::string_view(base, text.size()));
		key_ref k{};
		const char* key_at = nullptr;
		char* at = base + text.size();
		for (const auto& in : terms) {
			std::memcpy(at, in.term.data(), in.term.size());
			const bool is_key = !key.empty() && !key_at && in.term == key;
			if (is_key) { key_at = at; }
			side_.push_back({at, std::uint32_t(in.term.size()), in.wdf, std::uint32_t(in.positions.size()), positions_.size(), is_key}, memory);
			for (auto p : in.positions) { positions_.push_back(p, memory); }
			push(tuple(tuple_detail::hash(at, in.term.size()) & hash_mask, ord, std::uint32_t(side_.size() - 1), long_term));
			at += in.term.size();
		}
		if (!key.empty() && !key_at) {  // a key that is not one of its terms
			k.extra = true;
			k.at = extra_keys_.size();
			k.size = std::uint32_t(key.size());
			extra_keys_.emplace_back(key);
		}
		docids_.push_back(docid);
		versions_.push_back(version);
		lengths_.push_back(length);
		keys_.push_back(k);  // a key among the terms is placed when its batch drains
		batch_keys_.push_back({key_at, std::uint32_t(key_at ? key.size() : 0)});
		texts_.push_back(base);
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		charge_columns();
		return ord;
	}

public:
	std::size_t doc_count() const noexcept { return docids_.size(); }
	// Whether `docs` more documents with `entries` terms and tokens in all still fit the
	// 32-bit ordinals and side-entry indexes; the index flushes first when they do not.
	bool fits(std::size_t docs, std::size_t entries) const noexcept {
		return docids_.size() + docs < std::size_t(no_more_docs) - 1 && side_.size() + entries < max_side;
	}
	std::uint64_t docid(ordinal_t ord) const { return docids_.at(ord); }
	std::uint64_t version(ordinal_t ord) const { return versions_.at(ord); }
	std::uint32_t length(ordinal_t ord) const { return lengths_.at(ord); }
	std::string_view stored(ordinal_t ord) const {
		const std::uint64_t begin = ord ? stored_ends_.at(ord - 1) : 0;
		return std::string_view(stored_).substr(begin, stored_ends_.at(ord) - begin);
	}
	std::string_view key(ordinal_t ord) const {
		const key_ref& k = keys_.at(ord);
		if (k.extra) { return extra_keys_[k.at]; }
		if (k.bytes) { return std::string_view(k.bytes, k.size); }
		if (ord < batch_first_) { return {}; }
		const auto& [p, n] = batch_keys_[ord - batch_first_];
		return std::string_view(p, n);
	}

	// Writes to `out` the segment of the documents at `order` (ordinals, docids increasing),
	// or of every one in ordinal order when `order` is empty (their docids must increase),
	// through `w`, a writer holding no documents yet; positions gather in `positions` until
	// the postings end. The batch drains first; a second flush writes the same bytes.
	void flush(const std::array<std::uint8_t, 16>& segment_id, std::span<const ordinal_t> order, segment_writer& w, output& out, spill& positions) {
		drain_for_flush(w.memory);
		STRATA_INST_T(f0);
		const flush_plan p = plan(order, w.memory);
		add_documents(w, p);
		merger m(*this, {}, {});
		term_postings old;
		std::vector<live_posting> live;
		const bool pieces = encode_once && p.all;  // the runs' blocks as they are, appended
		w.run_starts.clear();
		if (pieces) {
			for (const run_ref& r : runs_) { w.run_starts.push_back(r.first); }
		}
		w.flush_from(segment_id, out, positions, alphabet(p), [&](term_postings& tp) -> std::optional<std::string_view> {
			std::string_view term;
			while (m.next(term)) {
				if (pieces) {
					pieces_of(m.entries(), tp);
					return term;
				}
				if (postings_of(m.entries(), p, tp, old, live)) { return term; }
			}
			return std::nullopt;
		});
		STRATA_INST_ADD(flush, f0);
	}

	// --- The flush's steps, which a flush on several threads (perf/indexing-gap) takes
	// itself: drain_for_flush(), plan(), add_documents() into its writer, alphabet(),
	// splitters(), and each_term() over disjoint key ranges, from any threads at once once the
	// buffer drained.

	// Which documents the flush writes, and their ordinals in it.
	struct flush_plan {
		bool all = true;                  // every document, in ordinal order
		bool monotone = true;             // the new ordinals keep the old order
		std::vector<ordinal_t> order;     // the documents written, by new ordinal
		std::vector<ordinal_t> renumber;  // old ordinal -> new, or no_more_docs when left out
	};
	// Drains the last batch, its allocations charged to `run` (the flush's) when given.
	void drain_for_flush(flush_charge* run) {
		struct restore {
			flush_charge*& at;
			flush_charge* was;
			~restore() { at = was; }
		} r{memory, std::exchange(memory, run ? run : memory)};
		drain();
	}
	flush_plan plan(std::span<const ordinal_t> order, flush_charge* run = nullptr) const {
		flush_plan p;
		if (order.empty()) { return p; }
		p.all = false;
		p.order.assign(order.begin(), order.end());
		p.renumber.assign(doc_count(), no_more_docs);
		for (std::size_t i = 0; i < order.size(); ++i) {
			p.renumber[order[i]] = ordinal_t(i);
			p.monotone = p.monotone && (i == 0 || order[i] > order[i - 1]);
		}
		if (run) { run->grow((p.renumber.capacity() + p.order.capacity()) * sizeof(ordinal_t)); }
		return p;
	}
	// The documents' columns into `w`, in the flush's order.
	void add_documents(segment_writer& w, const flush_plan& p) const {
		const std::size_t docs = p.all ? doc_count() : p.order.size();
		for (std::size_t i = 0; i < docs; ++i) {
			const ordinal_t o = p.all ? ordinal_t(i) : p.order[i];
			w.add_document(docids_[o], lengths_[o], stored(o), versions_[o], key(o));
		}
	}
	// Every byte the written terms use, in order: the runs' terms, or, when documents are
	// left out, those with a posting left (a pass over the runs).
	std::string alphabet(const flush_plan& p) const {
		std::array<bool, 256> used = used_;
		if (!p.all) {
			used = {};
			each_term({}, {}, p, [&](std::string_view term, term_postings&) {
				for (char c : term) { used[std::uint8_t(c)] = true; }
			});
		}
		std::string a;
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { a.push_back(char(c)); }
		}
		return a;
	}
	// Up to ranges - 1 increasing terms that split the runs' terms into ranges of about equal
	// counts, sampled from the runs' marks (every 64th entry).
	std::vector<std::string_view> splitters(std::size_t ranges) const {
		std::vector<std::string_view> sample;
		for (const run_ref& r : runs_) {
			const char* base = log_.at(r.offset);
			for (std::size_t m = r.first_mark; m < r.first_mark + r.marks; ++m) { sample.push_back(view(base + marks_[m]).term); }
		}
		std::sort(sample.begin(), sample.end());
		std::vector<std::string_view> out;
		for (std::size_t k = 1; k < ranges && !sample.empty(); ++k) { out.push_back(sample[sample.size() * k / ranges]); }
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}
	// Calls f(term, tp) for each term in [lo, hi) in byte order (empty: no bound), tp its
	// postings in the plan's numbering; terms with none left are skipped.
	template <class F>
	void each_term(std::string_view lo, std::string_view hi, const flush_plan& p, F&& f) const {
		merger m(*this, lo, hi);
		term_postings tp, old;
		std::vector<live_posting> live;
		std::string_view term;
		while (m.next(term)) {
			if (postings_of(m.entries(), p, tp, old, live)) { f(term, tp); }
		}
	}

	// Calls f(term, ord, wdf, positions) for every posting, in no particular order, with few
	// allocations (the digest calls it often).
	template <class F>
	void scan(F&& f) const {
		std::vector<std::uint32_t> absolute;
		term_postings tp;
		for (const run_ref& r : runs_) {
			const char* p = log_.at(r.offset);
			for (std::uint32_t e = 0; e < r.entries; ++e) {
				const entry_view v = view(p);
				tp.clear();
				if (v.flags & once_entry) {
					decode_once(v, tp, r.first);
				} else {
					decode_entry(v, tp, ~std::uint32_t(0));
				}
				std::size_t at = 0;
				for (std::size_t i = 0; i < tp.ords.size(); ++i) {
					absolute.clear();
					std::uint32_t pos = 0;
					for (std::uint32_t c = 0; c < tp.counts[i]; ++c) { absolute.push_back(pos += tp.positions[at++]); }
					f(v.term, tp.ords[i], tp.wdfs[i], std::span<const std::uint32_t>(absolute));
				}
				p = v.end;
			}
		}
		// The batch not drained yet: each bucket grouped on a copy, the buffer left as it is.
		std::vector<tuple> copy;
		std::vector<group> groups;
		group_scratch sc;
		for (const auto& bucket : buckets_) {
			copy.assign(bucket.begin(), bucket.end());
			groups.clear();
			group_run(copy.data(), copy.size(), groups, sc);
			for (const group& g : groups) {
				for (const tuple* t = g.begin; t != g.end;) {
					const ordinal_t ord = t->ord;
					std::uint32_t wdf = 0;
					absolute.clear();
					for (; t != g.end && t->ord == ord; ++t) {
						if (t->len() == long_term) {
							const side_entry& se = side_[t->off];
							wdf += se.wdf;
							for (std::uint32_t i = 0; i < se.count; ++i) { absolute.push_back(positions_[se.positions + i]); }
						} else {
							++wdf;
							absolute.push_back(t->pos());
						}
					}
					f(g.term, ord, wdf, std::span<const std::uint32_t>(absolute));
				}
			}
		}
	}

private:
	static constexpr std::uint32_t long_term = 255;  // the length field's escape: `off` names a side entry
	static constexpr std::size_t max_side = std::size_t(1) << 32;  // `off` indexes side entries

	// hash split in halves keeps the tuple at 20 bytes on a 4-byte alignment.
	struct tuple {
		std::uint32_t hash_hi, hash_lo, ord, off, pos_len;
		tuple() = default;  // trivial: buckets are not zeroed
		tuple(std::uint64_t h, ordinal_t o, std::uint32_t offset, std::uint32_t pl) : hash_hi(std::uint32_t(h >> 32)), hash_lo(std::uint32_t(h)), ord(o), off(offset), pos_len(pl) {}
		std::uint64_t hash() const noexcept { return std::uint64_t(hash_hi) << 32 | hash_lo; }
		std::uint32_t pos() const noexcept { return pos_len >> 8; }
		std::uint32_t len() const noexcept { return pos_len & 255; }
	};
	static_assert(sizeof(tuple) == 20);
	// A term given whole (its wdf and positions) or a token too long or too far in for a tuple.
	struct side_entry {
		const char* bytes;
		std::uint32_t size, wdf, count;
		std::size_t positions;  // the first, in positions_
		bool key;               // the document's key
	};
	struct group {
		std::string_view term;
		tuple* begin;
		tuple* end;
	};
	struct group_scratch {
		std::vector<std::uint64_t> hashes;
		std::vector<std::uint32_t> ids, tids, counts;
		std::vector<tuple> sorted;
	};
	// A document's key: in its run once its batch drains (log pages never move), or one of
	// extra_keys_ (a key that was not among its terms, at: its index), or none.
	struct key_ref {
		const char* bytes = nullptr;
		std::size_t at = 0;
		std::uint32_t size = 0;
		bool extra = false;
	};
	// A batch's run: its terms' postings, sorted by term, one entry each, in one log piece.
	// An entry: [term size][term][postings][positions][cf][flags u8: 1 all wdfs 0, 2 any
	// position, 4 counts are wdfs][section byte lengths x 4], numbers LEB128, then the
	// sections: LEB128 ordinal deltas from -1, wdfs, counts (only when one differs from its
	// wdf), position deltas.
	struct run_ref {
		std::uint64_t offset;
		std::uint32_t entries;
		std::size_t first_mark, marks;  // in marks_: every 64th entry's offset in the run
		ordinal_t first;                // the batch's first ordinal
	};
	static constexpr std::uint32_t mark_every = 64;
	// An encode_once entry (flag 16) holds its piece's final blocks: after the four lengths
	// (its remainder's sections, as above, its ordinals from the run's start - 1), [last
	// ordinal - run start][most positions of a posting], and with full blocks (flag 8) the
	// byte lengths of its postings blocks, count blocks, position
	// blocks, and block records, the remainder's first position and its block's offset; those
	// sections follow the remainder's. Flag 32: counts encoded; 64: blocks hold wdfs. A
	// record: [last delta][bytes << 1 | bitset][first position][its block's offset][count
	// block's offset][pairs], from the run's start - 1 (bit 14's base rule).
	static constexpr std::uint32_t has_blocks = 8, once_entry = 16, counts_encoded = 32, wdfs_encoded = 64;
	struct entry_view {
		std::string_view term;
		std::uint32_t postings, positions, cf, flags, lengths[4];
		const char* data;  // the sections
		const char* end;   // the next entry
		std::uint32_t run = 0, last = 0, max_count = 0;
		std::uint32_t bl[4] = {0, 0, 0, 0};
		std::uint32_t rem_first_pos = 0, rem_pos_off = 0;
	};
	static entry_view view(const char* p) noexcept {
		entry_view v;
		const std::uint32_t size = std::uint32_t(tuple_detail::get_leb(p));
		v.term = std::string_view(p, size);
		p += size;
		v.postings = std::uint32_t(tuple_detail::get_leb(p));
		v.positions = std::uint32_t(tuple_detail::get_leb(p));
		v.cf = std::uint32_t(tuple_detail::get_leb(p));
		v.flags = std::uint8_t(*p++);
		for (auto& l : v.lengths) { l = std::uint32_t(tuple_detail::get_leb(p)); }
		std::size_t blocks = 0;
		if (v.flags & once_entry) {
			v.last = std::uint32_t(tuple_detail::get_leb(p));
			v.max_count = std::uint32_t(tuple_detail::get_leb(p));
			if (v.flags & has_blocks) {
				for (auto& b : v.bl) { b = std::uint32_t(tuple_detail::get_leb(p)), blocks += b; }
				v.rem_first_pos = std::uint32_t(tuple_detail::get_leb(p));
				v.rem_pos_off = std::uint32_t(tuple_detail::get_leb(p));
			}
		}
		v.data = p;
		v.end = v.data + std::size_t(v.lengths[0]) + v.lengths[1] + v.lengths[2] + v.lengths[3] + blocks;
		return v;
	}
	struct live_posting {
		ordinal_t ord;
		std::uint32_t wdf, count, first;  // first: its first position in the decoded term
		bool operator<(const live_posting& o) const noexcept { return ord < o.ord; }
	};

	// The runs' terms in byte order, each with its entries in run order (ordinal order): a
	// heap of one cursor per run, keyed by its term's first 8 bytes (big-endian), then the
	// term, then the run, so a term's entries leave it in run order.
	class merger {
	public:
		// Over the terms in [lo, hi); an empty bound is none.
		merger(const tuple_buffer& b, std::string_view lo, std::string_view hi) : hi_(hi) {
			for (std::uint32_t r = 0; r < b.runs_.size(); ++r) {
				const run_ref& run = b.runs_[r];
				const char* base = b.log_.at(run.offset);
				// The last mark before lo, then entries up to lo.
				std::size_t m = 0;
				if (!lo.empty()) {
					std::size_t first = 0, count = run.marks;
					while (count > 0) {
						const std::size_t half = count / 2;
						if (view(base + b.marks_[run.first_mark + first + half]).term < lo) {
							first += half + 1, count -= half + 1;
						} else {
							count = half;
						}
					}
					m = first ? first - 1 : 0;
				}
				cursor c{base + (run.marks ? b.marks_[run.first_mark + m] : 0), std::uint32_t(run.entries - m * mark_every), {}, r};
				if (!c.left) { continue; }
				c.v = view(c.at);
				while (!lo.empty() && c.v.term < lo && --c.left) {
					c.at = c.v.end;
					c.v = view(c.at);
				}
				if (c.left && (hi_.empty() || c.v.term < hi_)) {
					heap_.push_back({prefix(c.v.term), std::uint32_t(cursors_.size())});
					cursors_.push_back(c);
				}
			}
			for (std::size_t i = heap_.size() / 2; i-- > 0;) { sift(i); }
		}
		// The next term, its entries in entries(); false at the end.
		bool next(std::string_view& term) {
			if (heap_.empty() || (!hi_.empty() && cursors_[heap_.front().c].v.term >= hi_)) { return false; }
			entries_.clear();
			const slot top = heap_.front();
			term = cursors_[top.c].v.term;
			do {
				cursor& c = cursors_[heap_.front().c];
				entries_.push_back(c.v);
				entries_.back().run = c.run;
				if (--c.left) {
					c.at = c.v.end;
					c.v = view(c.at);
					heap_.front().prefix = prefix(c.v.term);
				} else {
					heap_.front() = heap_.back();
					heap_.pop_back();
					if (heap_.empty()) { break; }
				}
				sift(0);
			} while (heap_.front().prefix == top.prefix && cursors_[heap_.front().c].v.term == term);
			return true;
		}
		const std::vector<entry_view>& entries() const noexcept { return entries_; }

	private:
		struct cursor {
			const char* at;
			std::uint32_t left;
			entry_view v;
			std::uint32_t run;
		};
		struct slot {
			std::uint64_t prefix;  // the cursor's term's first 8 bytes, big-endian, zero-padded
			std::uint32_t c;       // in cursors_, which are in run order
		};
		// A term's first 8 bytes as a big-endian number; an entry has at least 8 bytes from its
		// term on (the term, then 3 counts, the flags, and 4 lengths), so one load reads them.
		static std::uint64_t prefix(std::string_view term) noexcept {
			std::uint64_t w;
			std::memcpy(&w, term.data(), 8);
			if constexpr (std::endian::native == std::endian::little) {
				if (term.size() < 8) { w &= (std::uint64_t(1) << (8 * term.size())) - 1; }
				return __builtin_bswap64(w);
			} else {
				return term.size() < 8 ? w & ~(~std::uint64_t(0) >> (8 * term.size())) : w;
			}
		}
		bool before(const slot& x, const slot& y) const noexcept {
			if (x.prefix != y.prefix) { return x.prefix < y.prefix; }
			const int c = cursors_[x.c].v.term.compare(cursors_[y.c].v.term);
			return c != 0 ? c < 0 : x.c < y.c;
		}
		void sift(std::size_t i) noexcept {
			const std::size_t n = heap_.size();
			const slot s = heap_[i];
			for (;;) {
				std::size_t child = 2 * i + 1;
				if (child >= n) { break; }
				if (child + 1 < n && before(heap_[child + 1], heap_[child])) { ++child; }
				if (!before(heap_[child], s)) { break; }
				heap_[i] = heap_[child];
				i = child;
			}
			heap_[i] = s;
		}
		std::string_view hi_;
		std::vector<cursor> cursors_;
		std::vector<slot> heap_;
		std::vector<entry_view> entries_;
	};

	std::string_view bytes_of(const tuple& t) const noexcept {
		if (t.len() == long_term) { return std::string_view(side_[t.off].bytes, side_[t.off].size); }
		return std::string_view(texts_[t.ord - batch_first_] + t.off, t.len());
	}

	// A tuple into its hash's bucket; buckets grow by half.
	void push(const tuple& t) {
		std::vector<tuple>& v = buckets_[t.hash_hi >> 24];
		if (v.size() == v.capacity()) {
			const std::size_t before = v.capacity();
			v.reserve(std::max<std::size_t>(16, before + before / 2));
			if (memory) { memory->grow((v.capacity() - before) * sizeof(tuple)); }
		}
		v.push_back(t);
		++batched_;
	}

	// Room for n bytes in the batch's text arena, and a zeroed word after them for hash's
	// loads (the next room starts on it). Chunks are kept from batch to batch.
	char* room(std::size_t n) {
		while (arena_at_ < arena_.size() && arena_fill_ + n + 8 > arena_sizes_[arena_at_]) { ++arena_at_, arena_fill_ = 0; }
		if (arena_at_ == arena_.size()) {
			const std::size_t size = std::max<std::size_t>(std::min(arena_chunk, std::size_t(4096) << arena_.size()), n + 8);  // 4 KiB doubling
			arena_.push_back(std::unique_ptr<char[]>(new char[size]));
			arena_sizes_.push_back(size);
			arena_fill_ = 0;
			if (memory) { memory->grow(size); }
		}
		char* p = arena_[arena_at_].get() + arena_fill_;
		std::memset(p + n, 0, 8);
		arena_fill_ += n;
		return p;
	}

	void charge_columns() {
		if (!memory) { return; }
		const std::uint64_t now = (docids_.capacity() + versions_.capacity() + stored_ends_.capacity()) * sizeof(std::uint64_t) +
			lengths_.capacity() * sizeof(std::uint32_t) + keys_.capacity() * sizeof(key_ref) + (batch_keys_.capacity() + texts_.capacity()) * 16 +
			stored_.capacity() + runs_.capacity() * sizeof(run_ref) + marks_.capacity() * sizeof(std::uint32_t);
		if (now > columns_charged_) {
			memory->grow(now - columns_charged_);
			columns_charged_ = now;
		}
	}

	// The batch as a run: each bucket grouped, the groups sorted by term, one entry each in
	// one log piece; then the batch's tuples, text, and side entries reset, memory kept.
	void drain() {
		if (batched_) {
			groups_.clear();
			encoded_.size = 0;
			entry_at_.clear();
			key_groups_.clear();
			for (unsigned b = 0; b < 256; ++b) {
				const std::size_t first = groups_.size();
				STRATA_INST_T(t0);
				group_run(buckets_[b].data(), buckets_[b].size(), groups_, scratch_);
				STRATA_INST_T(t1);
				for (std::size_t i = first; i < groups_.size(); ++i) {  // its tuples still in cache
					if (encode_once) {
						encode_entry_once(std::uint32_t(i));
					} else {
						encode_entry(std::uint32_t(i));
					}
				}
#ifdef STRATA_INST
				inst::c.group += t1 - t0;
				inst::c.encode += inst::now() - t1;
#endif
			}
			STRATA_INST_T(s0);
			sort_groups(groups_);
			// The encoded entries into the run in term order, marks and keys placed as they land.
			const std::size_t first_mark = marks_.size();
			const std::uint64_t at = log_.append(encoded_.size, memory);
			char* const run = log_.at(at);
			std::vector<std::uint32_t>& placed = placed_;  // each group's entry offset in the run
			placed.resize(groups_.size());
			std::size_t offset = 0;
			for (std::size_t k = 0; k < order_.size(); ++k) {
				const entry_ref& e = entry_at_[order_[k]];
				if (k % mark_every == 0) { marks_.push_back(std::uint32_t(offset)); }
				std::memcpy(run + offset, encoded_.data.get() + e.offset, e.size);
				placed[order_[k]] = std::uint32_t(offset);
				offset += e.size;
			}
			runs_.push_back({at, std::uint32_t(order_.size()), first_mark, marks_.size() - first_mark, batch_first_});
			STRATA_INST_ADD(sortcopy, s0);
			STRATA_INST_COUNT(batches, 1);
			STRATA_INST_COUNT(entries, order_.size());
			STRATA_INST_COUNT(run_bytes, encoded_.size);
			for (const auto& [g, ord] : key_groups_) {
				keys_[ord].bytes = run + placed[g] + entry_at_[g].term_at;
				keys_[ord].size = std::uint32_t(groups_[g].term.size());
			}
			for (auto& b : buckets_) { b.clear(); }
		}
		batched_ = 0;
		side_.clear();
		positions_.clear();
		arena_at_ = 0, arena_fill_ = 0;
		texts_.clear();
		batch_keys_.clear();
		batch_first_ = ordinal_t(docids_.size());
		charge_columns();
	}

	// Bytes that grow by half and keep their capacity, never zero-filled.
	struct growing_bytes {
		std::unique_ptr<char[]> data;
		std::size_t size = 0, capacity = 0;
		char* room(std::size_t n, flush_charge* memory) {  // n more bytes at data + size
			if (size + n > capacity) {
				const std::size_t grown = std::max(size + n, capacity + capacity / 2 + 4096);
				std::unique_ptr<char[]> more(new char[grown]);
				if (size) { std::memcpy(more.get(), data.get(), size); }
				if (memory) { memory->grow(grown - capacity); }
				data = std::move(more);
				capacity = grown;
			}
			return data.get() + size;
		}
	};
	// A group's entry in encoded_: where, how long, and where its term starts in it.
	struct entry_ref {
		std::size_t offset;
		std::uint32_t size;
		std::uint32_t term_at;
	};

	// Group i's postings (one batch's, ordinals increasing) as its run entry, appended to
	// encoded_.
	void encode_entry(std::uint32_t i) { encode_leb<false>(i); }
	// Once: an encode_once entry without full blocks (fewer than 128 postings and positions):
	// ordinals from the run's start - 1, then the last ordinal and the most positions.
	template <bool Once>
	void encode_leb(std::uint32_t i) {
		const group& g = groups_[i];
		std::size_t tuples = 0, side_positions = 0;
		for (const tuple* p = g.begin; p != g.end; ++p) {
			++tuples;
			if (p->len() == long_term) { side_positions += side_[p->off].count; }
		}
		char* ords = sec_[0].room(5 * tuples, memory);
		char* wdfs = sec_[1].room(5 * tuples, memory);
		char* counts = sec_[2].room(5 * tuples, memory);
		char* positions = sec_[3].room(5 * (tuples + side_positions), memory);
		char* const starts[4] = {ords, wdfs, counts, positions};
		std::uint32_t postings = 0, npositions = 0, cf = 0, flags = 1 | 4;
		bool counted = false;
		ordinal_t last = Once ? batch_first_ - 1 : ~ordinal_t(0);
		[[maybe_unused]] std::uint32_t max_count = 0;
		for (const tuple* p = g.begin; p != g.end;) {
			const ordinal_t ord = p->ord;
			std::uint32_t wdf = 0, count = 0, prev = 0;
			for (; p != g.end && p->ord == ord; ++p) {
				if (p->len() == long_term) {
					const side_entry& se = side_[p->off];
					wdf += se.wdf, count += se.count;
					if (se.key) { key_groups_.push_back({i, ord}); }
					for (std::uint32_t k = 0; k < se.count; ++k) {
						const std::uint32_t pos = positions_[se.positions + k];
						tuple_detail::put_leb(positions, pos - prev);
						prev = pos;
					}
				} else {
					++wdf, ++count;
					tuple_detail::put_leb(positions, p->pos() - prev);
					prev = p->pos();
				}
			}
			if constexpr (Once) { max_count = std::max(max_count, count); }
			tuple_detail::put_leb(ords, ord - last);  // from -1: the first is the ordinal + 1
			tuple_detail::put_leb(wdfs, wdf);
			tuple_detail::put_leb(counts, count);
			last = ord;
			counted = counted || count != wdf;
			++postings;
			npositions += count;
			cf += wdf;
			if (wdf) { flags &= ~1u; }
			if (count) { flags |= 2; }
			if (count != wdf) { flags &= ~4u; }
		}
		if constexpr (Once) {
			flags |= once_entry;
			if (counted) { flags |= counts_encoded; }
			if (!(flags & 1)) { flags |= wdfs_encoded; }
		}
		const std::uint32_t lengths[4] = {std::uint32_t(ords - starts[0]), std::uint32_t(wdfs - starts[1]), counted ? std::uint32_t(counts - starts[2]) : 0u,
			std::uint32_t(positions - starts[3])};
		char* const begin = encoded_.room(60 + g.term.size() + std::size_t(lengths[0]) + lengths[1] + lengths[2] + lengths[3], memory);
		char* e = begin;
		tuple_detail::put_leb(e, g.term.size());
		const std::uint32_t term_at = std::uint32_t(e - begin);
		std::memcpy(e, g.term.data(), g.term.size());
		e += g.term.size();
		for (std::uint32_t v : {postings, npositions, cf}) { tuple_detail::put_leb(e, v); }
		*e++ = char(flags);
		for (std::uint32_t l : lengths) { tuple_detail::put_leb(e, l); }
		if constexpr (Once) {
			tuple_detail::put_leb(e, last - batch_first_);
			tuple_detail::put_leb(e, max_count);
		}
		for (int k = 0; k < 4; ++k) {
			std::memcpy(e, starts[k], lengths[k]);
			e += lengths[k];
		}
		entry_at_.push_back({encoded_.size, std::uint32_t(e - begin), term_at});
		encoded_.size += std::size_t(e - begin);
		for (char c : g.term) { used_[std::uint8_t(c)] = true; }
	}

	// One entry's postings appended to tp, its ordinals renumbered from -1 (a run's entries
	// are their documents' whole postings for the batch).
	static void decode_entry(const entry_view& v, term_postings& tp, std::uint32_t ord) {
		const char* p = v.data;
		const char* wdfs = p + v.lengths[0];
		const char* counts = wdfs + v.lengths[1];
		const char* positions = counts + v.lengths[2];
		const char* wdfs_again = wdfs;  // the counts when the entry has none apart
		for (std::uint32_t i = 0; i < v.postings; ++i) {
			ord += std::uint32_t(tuple_detail::get_leb(p));
			tp.ords.push_back(ord);
			tp.wdfs.push_back(std::uint32_t(tuple_detail::get_leb(wdfs)));
			const std::uint32_t count = std::uint32_t(tuple_detail::get_leb(v.lengths[2] ? counts : wdfs_again));
			tp.counts.push_back(count);
			for (std::uint32_t k = 0; k < count; ++k) { tp.positions.push_back(std::uint32_t(tuple_detail::get_leb(positions))); }
		}
	}

	// --- encode_once (format 13's bit 14): each drain writes its terms' final blocks.

	// Group i's postings gathered into columns, then encoded as its piece's entry.
	void encode_entry_once(std::uint32_t i) {
		const group& g = groups_[i];
		if (const std::size_t tuples = std::size_t(g.end - g.begin); tuples < format::block) {
			std::size_t positions = 0;
			for (const tuple* p = g.begin; p != g.end; ++p) { positions += p->len() == long_term ? side_[p->off].count : 1; }
			if (positions < format::block) { return encode_leb<true>(i); }  // no full block: LEB128 as it is
		}
		c_ords_.clear(), c_wdfs_.clear(), c_counts_.clear(), c_pos_.clear();
		for (const tuple* p = g.begin; p != g.end;) {
			const ordinal_t ord = p->ord;
			std::uint32_t wdf = 0, count = 0, prev = 0;
			for (; p != g.end && p->ord == ord; ++p) {
				if (p->len() == long_term) {
					const side_entry& se = side_[p->off];
					wdf += se.wdf, count += se.count;
					if (se.key) { key_groups_.push_back({i, ord}); }
					for (std::uint32_t k = 0; k < se.count; ++k) {
						const std::uint32_t pos = positions_[se.positions + k];
						c_pos_.push_back(pos - prev);
						prev = pos;
					}
				} else {
					++wdf, ++count;
					c_pos_.push_back(p->pos() - prev);
					prev = p->pos();
				}
			}
			c_ords_.push_back(ord), c_wdfs_.push_back(wdf), c_counts_.push_back(count);
		}
		const std::size_t at = encoded_.size;
		const std::uint32_t term_at = encode_piece(g.term, batch_first_, false, false, encoded_);
		entry_at_.push_back({at, std::uint32_t(encoded_.size - at), term_at});
		for (char c : g.term) { used_[std::uint8_t(c)] = true; }
	}

	// The columns as an encode_once entry appended to `out`: full blocks of 128 as the
	// segment holds them (FOR deltas or a bitset, then PFOR wdfs; PFOR counts and positions)
	// with a record each, the rest in the LEB128 sections. Returns where its term starts.
	std::uint32_t encode_piece(std::string_view term, ordinal_t run_start, bool force_wdf, bool force_counts, growing_bytes& out) {
		constexpr std::size_t B = format::block;
		const std::size_t n = c_ords_.size(), m = c_pos_.size();
		std::uint32_t cf = 0, max_count = 0, flags = 1 | 4 | once_entry;
		for (std::size_t i = 0; i < n; ++i) {
			cf += c_wdfs_[i];
			if (c_wdfs_[i]) { flags &= ~1u; }
			if (c_counts_[i]) { flags |= 2; }
			if (c_counts_[i] != c_wdfs_[i]) { flags &= ~4u; }
			max_count = std::max(max_count, c_counts_[i]);
		}
		const bool with_wdf = force_wdf || !(flags & 1), with_counts = force_counts || !(flags & 4);
		if (with_wdf) { flags |= wdfs_encoded; }
		if (with_counts) { flags |= counts_encoded; }
		const std::size_t full = n / B * B, full_pos = m / B * B;
		if (full || full_pos) { flags |= has_blocks; }
		for (auto& b : blk_) { b.size = 0; }
		pos_offsets_.clear();
		for (std::size_t b = 0; b < full_pos; b += B) {
			pos_offsets_.push_back(std::uint32_t(blk_[2].size));
			blk_[2].size += bitpack::encode_pfor(c_pos_.data() + b, reinterpret_cast<std::uint8_t*>(blk_[2].room(bitpack::max_encoded_bytes, memory)));
		}
		count_offsets_.clear();
		if (with_counts) {
			for (std::size_t b = 0; b < full; b += B) {
				count_offsets_.push_back(std::uint32_t(blk_[1].size));
				blk_[1].size += bitpack::encode_pfor(c_counts_.data() + b, reinterpret_cast<std::uint8_t*>(blk_[1].room(bitpack::max_encoded_bytes, memory)));
			}
		}
		auto offset_of = [&](std::uint64_t position) {
			const std::size_t j = position / B;
			return j < pos_offsets_.size() ? pos_offsets_[j] : std::uint32_t(blk_[2].size);
		};
		std::uint32_t prev = run_start - 1;  // bit 14's base: the run's start - 1, then each block's last
		std::uint64_t position = 0;
		std::uint32_t deltas[B];
		std::uint8_t buf[bitpack::max_encoded_bytes];
		for (std::size_t b = 0; b < full; b += B) {
			const std::size_t start = blk_[0].size;
			const ordinal_t last = c_ords_[b + B - 1];
			const std::uint32_t base = prev + 1;
			const std::uint64_t span = std::uint64_t(last) - base + 1;
			bitpack::delta_encode(prev, c_ords_.data() + b, deltas);
			const std::size_t for_bytes = bitpack::encode_for(deltas, buf);
			const bool bitset = (span + 7) / 8 < for_bytes;
			if (bitset) {
				char* bits = blk_[0].room((span + 7) / 8, memory);
				std::memset(bits, 0, (span + 7) / 8);
				for (std::size_t i = b; i < b + B; ++i) {
					const std::uint64_t at = c_ords_[i] - base;
					bits[at >> 3] = char(std::uint8_t(bits[at >> 3]) | (1u << (at & 7)));
				}
				blk_[0].size += (span + 7) / 8;
			} else {
				std::memcpy(blk_[0].room(for_bytes, memory), buf, for_bytes);
				blk_[0].size += for_bytes;
			}
			if (with_wdf) { blk_[0].size += bitpack::encode_pfor(c_wdfs_.data() + b, reinterpret_cast<std::uint8_t*>(blk_[0].room(bitpack::max_encoded_bytes, memory))); }
			pairs_.clear();
			for (std::size_t i = b; i < b + B; ++i) { pairs_.push_back({c_wdfs_[i], format::encode_length(lengths_[c_ords_[i]])}); }
			const auto impacts = format::competitive(pairs_);
			char* const r0 = blk_[3].room(40 + 6 * impacts.size(), memory);
			char* r = r0;
			tuple_detail::put_leb(r, last - prev);
			tuple_detail::put_leb(r, (blk_[0].size - start) << 1 | std::size_t(bitset));
			tuple_detail::put_leb(r, position);
			tuple_detail::put_leb(r, offset_of(position));
			tuple_detail::put_leb(r, with_counts ? count_offsets_[b / B] : 0);
			tuple_detail::put_leb(r, impacts.size());
			for (const auto& im : impacts) {
				tuple_detail::put_leb(r, im.wdf);
				*r++ = char(im.length);
			}
			blk_[3].size += std::size_t(r - r0);
			for (std::size_t i = b; i < b + B; ++i) { position += c_counts_[i]; }
			prev = last;
		}
		const std::uint32_t rem_first_pos = std::uint32_t(position), rem_pos_off = offset_of(position);
		char* const s0 = sec_[0].room(5 * (n - full), memory);
		char* const s1 = sec_[1].room(5 * (n - full), memory);
		char* const s2 = sec_[2].room(5 * (n - full), memory);
		char* const s3 = sec_[3].room(5 * (m - full_pos), memory);
		char *o = s0, *w = s1, *c = s2, *q = s3;
		for (std::size_t i = full; i < n; ++i) {
			tuple_detail::put_leb(o, c_ords_[i] - prev);
			prev = c_ords_[i];
			tuple_detail::put_leb(w, c_wdfs_[i]);
			if (with_counts) { tuple_detail::put_leb(c, c_counts_[i]); }
		}
		for (std::size_t i = full_pos; i < m; ++i) { tuple_detail::put_leb(q, c_pos_[i]); }
		const std::uint32_t lengths[4] = {std::uint32_t(o - s0), std::uint32_t(w - s1), std::uint32_t(c - s2), std::uint32_t(q - s3)};
		const std::size_t blocks = (flags & has_blocks) ? blk_[0].size + blk_[1].size + blk_[2].size + blk_[3].size : 0;
		char* const begin = out.room(96 + term.size() + std::size_t(lengths[0]) + lengths[1] + lengths[2] + lengths[3] + blocks, memory);
		char* e = begin;
		tuple_detail::put_leb(e, term.size());
		const std::uint32_t term_at = std::uint32_t(e - begin);
		std::memcpy(e, term.data(), term.size());
		e += term.size();
		for (std::uint64_t v : {std::uint64_t(n), std::uint64_t(m), std::uint64_t(cf)}) { tuple_detail::put_leb(e, v); }
		*e++ = char(flags);
		for (std::uint32_t l : lengths) { tuple_detail::put_leb(e, l); }
		tuple_detail::put_leb(e, c_ords_.back() - run_start);
		tuple_detail::put_leb(e, max_count);
		if (flags & has_blocks) {
			for (const auto& b : blk_) { tuple_detail::put_leb(e, b.size); }
			tuple_detail::put_leb(e, rem_first_pos);
			tuple_detail::put_leb(e, rem_pos_off);
		}
		const char* const starts[4] = {s0, s1, s2, s3};
		for (int k = 0; k < 4; ++k) {
			if (lengths[k]) { std::memcpy(e, starts[k], lengths[k]); }
			e += lengths[k];
		}
		if (flags & has_blocks) {
			for (const auto& b : blk_) {
				if (b.size) { std::memcpy(e, b.data.get(), b.size); }
				e += b.size;
			}
		}
		out.size += std::size_t(e - begin);
		return term_at;
	}

	// An encode_once entry's postings appended to tp.
	static void decode_once(const entry_view& v, term_postings& tp, ordinal_t run_start) {
		constexpr std::size_t B = format::block;
		const bool with_wdf = v.flags & wdfs_encoded, with_counts = v.flags & counts_encoded;
		const std::size_t n = v.postings, m = v.positions, full = n / B * B, full_pos = m / B * B;
		const std::size_t o = tp.ords.size(), q = tp.positions.size();
		tp.ords.resize(o + n), tp.wdfs.resize(o + n), tp.counts.resize(o + n), tp.positions.resize(q + m);
		const char* deltas = v.data;
		const char* wdfs = deltas + v.lengths[0];
		const char* counts = wdfs + v.lengths[1];
		const char* tail = counts + v.lengths[2];
		std::uint32_t prev = run_start - 1;
		if (v.flags & has_blocks) {
			const auto* b = reinterpret_cast<const std::uint8_t*>(tail + v.lengths[3]);
			const auto* cb = b + v.bl[0];
			const auto* pb = cb + v.bl[1];
			const char* r = reinterpret_cast<const char*>(pb + v.bl[2]);
			for (std::size_t j = 0; j < full; j += B) {
				const std::uint32_t last = prev + std::uint32_t(tuple_detail::get_leb(r));
				const std::uint64_t bb = tuple_detail::get_leb(r);
				for (int k = 0; k < 3; ++k) { (void)tuple_detail::get_leb(r); }
				const std::uint64_t pairs = tuple_detail::get_leb(r);
				for (std::uint64_t k = 0; k < pairs; ++k) {
					(void)tuple_detail::get_leb(r);
					++r;
				}
				const std::size_t bytes = std::size_t(bb >> 1);
				std::uint32_t* out = tp.ords.data() + o + j;
				std::size_t used;
				if (bb & 1) {
					const std::uint32_t base = prev + 1;
					const std::uint64_t span = std::uint64_t(last) - base + 1;
					std::size_t k = 0;
					for (std::uint64_t at = 0; at < span; ++at) {
						if ((b[at >> 3] >> (at & 7)) & 1) { out[k++] = base + std::uint32_t(at); }
					}
					used = std::size_t((span + 7) / 8);
				} else {
					used = bitpack::decode_delta(b, bytes, prev, out);
				}
				if (with_wdf) {
					(void)bitpack::decode(b + used, bytes - used, tp.wdfs.data() + o + j);
				} else {
					std::fill_n(tp.wdfs.data() + o + j, B, 0u);
				}
				if (with_counts) {
					cb += bitpack::decode(cb, bitpack::max_encoded_bytes, tp.counts.data() + o + j);
				} else {
					std::copy_n(tp.wdfs.data() + o + j, B, tp.counts.data() + o + j);
				}
				b += bytes;
				prev = last;
			}
			for (std::size_t j = 0; j < full_pos; j += B) { pb += bitpack::decode(pb, bitpack::max_encoded_bytes, tp.positions.data() + q + j); }
		}
		for (std::size_t i = full; i < n; ++i) {
			prev += std::uint32_t(tuple_detail::get_leb(deltas));
			tp.ords[o + i] = prev;
			tp.wdfs[o + i] = std::uint32_t(tuple_detail::get_leb(wdfs));
			tp.counts[o + i] = with_counts ? std::uint32_t(tuple_detail::get_leb(counts)) : tp.wdfs[o + i];
		}
		for (std::size_t i = full_pos; i < m; ++i) { tp.positions[q + i] = std::uint32_t(tuple_detail::get_leb(tail)); }
	}

	// A term as pieces for flush_from: each run's entry as it is, except one without the wdf
	// or count blocks the term needs (its own were all zero, or all its wdfs), re-encoded.
	void pieces_of(const std::vector<entry_view>& entries, term_postings& tp) {
		tp.clear();
		tp.from_pieces = true;
		bool all_zero = true, any_pos = false, counts_are_wdf = true;
		for (const auto& v : entries) {
			tp.piece_df += v.postings, tp.cf += v.cf;
			all_zero = all_zero && (v.flags & 1);
			any_pos = any_pos || (v.flags & 2);
			counts_are_wdf = counts_are_wdf && (v.flags & 4);
		}
		tp.all_zero_wdf = all_zero, tp.any_positions = any_pos, tp.counts_are_wdf = counts_are_wdf;
		const bool need_wdf = !all_zero, need_counts = !counts_are_wdf && any_pos;
		redone_.clear();
		fallback_.size = 0;
		for (std::size_t k = 0; k < entries.size(); ++k) {
			const entry_view& v = entries[k];
			if (!((need_wdf && !(v.flags & wdfs_encoded) && v.postings >= format::block) || (need_counts && !(v.flags & counts_encoded)))) { continue; }
			redo_.clear();
			decode_once(v, redo_, runs_[v.run].first);
			c_ords_.assign(redo_.ords.begin(), redo_.ords.end());
			c_wdfs_.assign(redo_.wdfs.begin(), redo_.wdfs.end());
			c_counts_.assign(redo_.counts.begin(), redo_.counts.end());
			c_pos_.assign(redo_.positions.begin(), redo_.positions.end());
			const std::size_t at = fallback_.size;
			(void)encode_piece(v.term, runs_[v.run].first, need_wdf, need_counts, fallback_);
			redone_.push_back({k, at});
			++redone_pieces_;
		}
		std::size_t next = 0;
		for (std::size_t k = 0; k < entries.size(); ++k) {
			entry_view v = entries[k];
			if (next < redone_.size() && redone_[next].first == k) {
				v = view(fallback_.data.get() + redone_[next++].second);
				v.run = entries[k].run;
			}
			posting_piece pc;
			pc.run_start = runs_[v.run].first;
			pc.postings = v.postings, pc.positions = v.positions, pc.last = v.last, pc.max_count = v.max_count;
			const char* d = v.data;
			auto take = [&](std::uint32_t n) {
				const std::string_view s(d, n);
				d += n;
				return s;
			};
			pc.deltas = take(v.lengths[0]), pc.wdfs = take(v.lengths[1]), pc.counts = take(v.lengths[2]), pc.pos_tail = take(v.lengths[3]);
			if (v.flags & has_blocks) {
				pc.blocks = take(v.bl[0]), pc.count_blocks = take(v.bl[1]), pc.pos_blocks = take(v.bl[2]), pc.records = take(v.bl[3]);
				pc.rem_first_pos = v.rem_first_pos, pc.rem_pos_off = v.rem_pos_off;
			}
			tp.pieces.push_back(pc);
		}
	}

public:
	std::uint64_t redone_pieces() const noexcept { return redone_pieces_; }

private:
	// A term's postings from its entries in run order, with its flags.
	void decode(const std::vector<entry_view>& entries, term_postings& tp) const {
		tp.clear();
		std::size_t postings = 0, positions = 0;
		for (const auto& v : entries) { postings += v.postings, positions += v.positions; }
		tp.ords.reserve(postings), tp.wdfs.reserve(postings), tp.counts.reserve(postings), tp.positions.reserve(positions);
		tp.all_zero_wdf = true, tp.any_positions = false, tp.counts_are_wdf = true;
		for (const auto& v : entries) {
			if (v.flags & once_entry) {
				decode_once(v, tp, runs_[v.run].first);
			} else {
				decode_entry(v, tp, ~std::uint32_t(0));
			}
			tp.cf += v.cf;
			if (!(v.flags & 1)) { tp.all_zero_wdf = false; }
			if (v.flags & 2) { tp.any_positions = true; }
			if (!(v.flags & 4)) { tp.counts_are_wdf = false; }
		}
	}

	// A term's postings in the plan's numbering, from its entries in run order; false when
	// none is left.
	bool postings_of(const std::vector<entry_view>& entries, const flush_plan& p, term_postings& tp, term_postings& old, std::vector<live_posting>& live) const {
		if (p.all) {
			decode(entries, tp);
			return true;
		}
		decode(entries, old);
		relabel(old, p.renumber, p.monotone, live, tp);
		return !tp.ords.empty();
	}

	// A decoded term's postings in the flushed numbering: documents left out dropped,
	// ordinals renumbered, sorted again when the numbering reorders, flags recomputed.
	static void relabel(const term_postings& old, const std::vector<ordinal_t>& renumber, bool monotone, std::vector<live_posting>& live, term_postings& tp) {
		live.clear();
		std::uint32_t first = 0;
		for (std::size_t i = 0; i < old.ords.size(); ++i) {
			if (renumber[old.ords[i]] != no_more_docs) { live.push_back({renumber[old.ords[i]], old.wdfs[i], old.counts[i], first}); }
			first += old.counts[i];
		}
		if (!monotone) { std::sort(live.begin(), live.end()); }
		tp.clear();
		for (const auto& l : live) {
			tp.ords.push_back(l.ord);
			tp.wdfs.push_back(l.wdf);
			tp.counts.push_back(l.count);
			tp.positions.insert(tp.positions.end(), old.positions.begin() + l.first, old.positions.begin() + l.first + l.count);
			tp.cf += l.wdf;
			if (l.wdf) { tp.all_zero_wdf = false; }
			if (l.count) { tp.any_positions = true; }
			if (l.count != l.wdf) { tp.counts_are_wdf = false; }
		}
	}

	// One bucket: an id per distinct hash from a small table, a stable counting sort by id,
	// then every tuple checked against its group's first; a group holding two terms (a hash
	// collision) is split by bytes, each term's tuples keeping their order.
	void group_run(tuple* p, std::size_t n, std::vector<group>& groups, group_scratch& sc) const {
		if (!n) { return; }
		std::size_t cap = std::bit_ceil(std::max<std::size_t>(64, n / 2));
		auto& hashes = sc.hashes;
		auto& ids = sc.ids;
		hashes.assign(cap, 0);
		ids.resize(cap);
		sc.tids.resize(n);
		std::uint32_t distinct = 0;
		auto find = [&](std::uint64_t h) -> std::uint32_t {
			for (std::size_t k = (h >> 8) & (cap - 1);; k = (k + 1) & (cap - 1)) {
				if (hashes[k] == h) { return ids[k]; }
				if (hashes[k] == 0) {
					hashes[k] = h;
					ids[k] = distinct;
					return distinct++;
				}
			}
		};
		for (std::size_t i = 0; i < n; ++i) {
			std::uint64_t h = p[i].hash();
			h += h == 0;  // 0 marks an empty slot
			sc.tids[i] = find(h);
			if (2 * std::size_t(distinct) > cap) {  // keep the load at most a half
				std::vector<std::uint64_t> old_hashes(std::move(hashes));
				std::vector<std::uint32_t> old_ids(std::move(ids));
				cap *= 2;
				hashes.assign(cap, 0);
				ids.resize(cap);
				for (std::size_t k = 0; k < old_hashes.size(); ++k) {
					if (!old_hashes[k]) { continue; }
					std::size_t j = (old_hashes[k] >> 8) & (cap - 1);
					while (hashes[j]) { j = (j + 1) & (cap - 1); }
					hashes[j] = old_hashes[k];
					ids[j] = old_ids[k];
				}
			}
		}
		auto& counts = sc.counts;  // then each id's end in sorted
		counts.assign(distinct + 1, 0);
		for (std::size_t i = 0; i < n; ++i) { ++counts[sc.tids[i] + 1]; }
		for (std::uint32_t t = 0; t < distinct; ++t) { counts[t + 1] += counts[t]; }
		sc.sorted.resize(n);
		for (std::size_t i = 0; i < n; ++i) { sc.sorted[counts[sc.tids[i]]++] = p[i]; }
		std::memcpy(p, sc.sorted.data(), n * sizeof(tuple));
		// Tokens of at most 8 bytes with equal hashes and lengths are equal (hash's word
		// form is a bijection at each length); the rest compare bytes.
		const bool exact = hash_mask == ~std::uint64_t(0);
		std::size_t begin = 0;
		for (std::uint32_t t = 0; t < distinct; ++t) {
			const std::size_t end = counts[t];
			const tuple& f = p[begin];
			const std::string_view first = bytes_of(f);
			bool mixed = false;
			if (exact && f.len() <= 8) {
				for (std::size_t i = begin + 1; i < end && !mixed; ++i) { mixed = p[i].len() != f.len() || p[i].hash() != f.hash(); }
			} else {
				for (std::size_t i = begin + 1; i < end && !mixed; ++i) { mixed = bytes_of(p[i]) != first; }
			}
			if (mixed) {
				split(p + begin, p + end, groups);
			} else {
				groups.push_back({first, p + begin, p + end});
			}
			begin = end;
		}
	}

	// A hash group holding more than one term: each term's tuples, in their order.
	void split(tuple* begin, tuple* end, std::vector<group>& groups) const {
		std::stable_sort(begin, end, [&](const tuple& x, const tuple& y) { return bytes_of(x) < bytes_of(y); });
		for (tuple* g = begin; g != end;) {
			tuple* e = g + 1;
			while (e != end && bytes_of(*e) == bytes_of(*g)) { ++e; }
			groups.push_back({bytes_of(*g), g, e});
			g = e;
		}
	}

	// The groups' order by term into order_: 8-byte big-endian chunks, zero-padded, with the
	// bytes left (up to 9: more than a chunk) as a tiebreak; a run that ties on a chunk and
	// continues sorts again on the next. A batch's terms are distinct, so every run ends.
	struct chunk_key {
		std::uint64_t chunk;
		std::uint32_t left, index;
		bool operator<(const chunk_key& o) const noexcept { return chunk != o.chunk ? chunk < o.chunk : left < o.left; }
	};
	static chunk_key key_at(std::string_view term, std::size_t depth, std::uint32_t index) noexcept {
		std::uint64_t c = 0;
		const std::size_t n = std::min<std::size_t>(8, term.size() - depth);
		for (std::size_t i = 0; i < n; ++i) { c |= std::uint64_t(std::uint8_t(term[depth + i])) << (56 - 8 * i); }
		return {c, std::uint32_t(std::min<std::size_t>(term.size() - depth, 9)), index};
	}
	// A batch of more than a few hundred terms is sorted by LSD radix on the first chunk and
	// its length class, 8 bits a pass (a pass whose digit every key shares is skipped), and
	// the terms that tie there are sorted on past 8 bytes; a smaller one by sort_run alone.
	void sort_groups(const std::vector<group>& groups) {
		const std::size_t n = groups.size();
		keys_scratch_.resize(n);
		for (std::size_t i = 0; i < n; ++i) { keys_scratch_[i] = key_at(groups[i].term, 0, std::uint32_t(i)); }
		if (n < radix_from) {
			sort_run(groups, keys_scratch_.data(), keys_scratch_.data() + n, 0);
		} else {
			radix_sort(keys_scratch_, radix_scratch_);
			chunk_key* const e = keys_scratch_.data() + n;
			for (chunk_key* r = keys_scratch_.data(); r != e;) {
				chunk_key* t = r + 1;
				while (t != e && t->chunk == r->chunk && t->left == r->left) { ++t; }
				if (t - r > 1 && r->left == 9) {  // tied on the first chunk, and every one continues past it
					for (chunk_key* k = r; k != t; ++k) { *k = key_at(groups[k->index].term, 8, k->index); }
					sort_run(groups, r, t, 8);
				}
				r = t;
			}
		}
		order_.resize(n);
		for (std::size_t i = 0; i < n; ++i) { order_[i] = keys_scratch_[i].index; }
	}
	// By (chunk, left): left first, then the chunk's bytes from the lowest; all nine digits'
	// counts gathered in one read.
	static void radix_sort(std::vector<chunk_key>& keys, std::vector<chunk_key>& scratch) {
		const std::size_t n = keys.size();
		scratch.resize(n);
		std::array<std::array<std::uint32_t, 256>, 9> counts{};
		for (const chunk_key& k : keys) {
			++counts[0][k.left];
			for (unsigned b = 0; b < 8; ++b) { ++counts[b + 1][(k.chunk >> (8 * b)) & 255]; }
		}
		auto digit = [](const chunk_key& k, unsigned d) noexcept { return d == 0 ? unsigned(k.left) : unsigned(k.chunk >> (8 * (d - 1))) & 255; };
		chunk_key* from = keys.data();
		chunk_key* to = scratch.data();
		for (unsigned d = 0; d < 9; ++d) {
			std::array<std::uint32_t, 256>& c = counts[d];
			if (c[digit(from[0], d)] == n) { continue; }
			std::uint32_t sum = 0;
			for (std::uint32_t& x : c) { sum += std::exchange(x, sum); }
			for (std::size_t i = 0; i < n; ++i) { to[c[digit(from[i], d)]++] = from[i]; }
			std::swap(from, to);
		}
		if (from != keys.data()) { keys.swap(scratch); }
	}
	static void sort_run(const std::vector<group>& groups, chunk_key* b, chunk_key* e, std::size_t depth) {
		std::sort(b, e);
		for (chunk_key* r = b; r != e;) {
			chunk_key* t = r + 1;
			while (t != e && t->chunk == r->chunk && t->left == r->left) { ++t; }
			if (t - r > 1 && r->left == 9) {  // tied on this chunk, and every one continues past it
				for (chunk_key* k = r; k != t; ++k) { *k = key_at(groups[k->index].term, depth + 8, k->index); }
				sort_run(groups, r, t, depth + 8);
			}
			r = t;
		}
	}

	static constexpr std::size_t arena_chunk = std::size_t(4) << 20;

	// The batch: tuples by bucket, the documents' text, and side entries.
	std::array<std::vector<tuple>, 256> buckets_;  // by the hash's top byte, each in arrival order
	std::size_t batched_ = 0;                      // tuples in the buckets
	tuple_detail::chunked<side_entry, 7> side_;
	tuple_detail::chunked<std::uint32_t, 8> positions_;  // side entries' positions
	std::vector<std::unique_ptr<char[]>> arena_;         // the batch's text and terms, each padded by a word
	std::vector<std::size_t> arena_sizes_;
	std::size_t arena_at_ = 0, arena_fill_ = 0;
	std::vector<const char*> texts_;                                 // each batch document's text, from batch_first_
	std::vector<std::pair<const char*, std::uint32_t>> batch_keys_;  // and its key, until the batch drains
	ordinal_t batch_first_ = 0;
	group_scratch scratch_;
	std::vector<group> groups_;  // drain()'s
	std::vector<chunk_key> keys_scratch_, radix_scratch_;
	static constexpr std::size_t radix_from = 256;  // terms a batch: fewer sort by comparisons
	std::vector<std::uint32_t> order_;
	growing_bytes sec_[4];   // an entry's sections under construction: ordinals, wdfs, counts, positions
	growing_bytes encoded_;  // the batch's entries, in the buckets' order
	// encode_once's scratch: a group's columns, its blocks (postings, counts, positions,
	// records), and the flush's re-encoded pieces.
	std::vector<std::uint32_t> c_ords_, c_wdfs_, c_counts_, c_pos_, pos_offsets_, count_offsets_;
	growing_bytes blk_[4];
	std::vector<format::impact> pairs_;
	growing_bytes fallback_;
	term_postings redo_;
	std::vector<std::pair<std::size_t, std::size_t>> redone_;
	std::uint64_t redone_pieces_ = 0;
	std::vector<entry_ref> entry_at_;
	std::vector<std::pair<std::uint32_t, ordinal_t>> key_groups_;  // a group holding a document's key
	std::vector<std::uint32_t> placed_;
	// The runs, one per drained batch, and the bytes their terms use.
	tuple_detail::byte_log log_;
	std::vector<run_ref> runs_;
	std::vector<std::uint32_t> marks_;
	std::array<bool, 256> used_{};
	// Per document.
	std::vector<std::uint64_t> docids_, versions_;
	std::vector<std::uint32_t> lengths_;
	std::vector<key_ref> keys_;
	std::deque<std::string> extra_keys_;  // keys that were not among their documents' terms
	std::string stored_;
	std::vector<std::uint64_t> stored_ends_;
	std::uint64_t columns_charged_ = 0;
};

}  // namespace strata
