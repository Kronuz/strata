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

// An index's buffer at one flush thread (docs/research/indexing.md, inverter C, batched):
// each token becomes a 20-byte tuple (hash, ordinal, offset in its document's text,
// position and length) in one of 256 buckets by the hash's top byte, the text copied once,
// and nothing is looked up per token. Every batch of tuples (a few MiB, so it stays in
// cache) is grouped bucket by bucket with a small table and a stable counting sort, each
// token checked against its term's bytes (equal hashes never merge two terms), and each
// term's postings for the batch go to the term store as one compressed chunk: one probe of
// the big term table per term and batch instead of per token, and the buffer holds
// postings, not tuples. The flush sorts the terms and hands each one's postings to
// segment_writer::flush_from in ordinal order: the segment a segment_writer given the same
// documents writes.
#pragma once

#include "strata/writer.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
	// Tuples grouped into the term store at a time: about 20 MiB of them by default, so a
	// bucket's share (1/256) stays in L2.
	std::size_t batch_tuples = std::size_t(1) << 20;

	// A document: its terms, each once with its wdf and positions, and its text's tokens in
	// order, each adding one to its term's wdf; a term is in one of the two and positions
	// increase. `key` is one of its terms or empty. Returns its ordinal.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens,
		std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		if (docids_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (stored.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		if (batched_ >= batch_tuples) { drain(); }
		const ordinal_t ord = ordinal_t(docids_.size());
		std::size_t entry_bytes = 0;
		for (const auto& in : terms) { entry_bytes += in.term.size(); }
		char* base = room(text.size() + entry_bytes);
		std::memcpy(base, text.data(), text.size());
		for (const auto& tk : tokens) {
			const char* p = base + tk.offset;
			const std::uint64_t h = tuple_detail::hash(p, tk.size) & hash_mask;
			if (tk.size < long_term && tk.position < (std::uint32_t(1) << 24)) {
				push(tuple(h, ord, tk.offset, tk.position << 8 | tk.size));
			} else {
				side_.push_back({p, tk.size, 1, 1, positions_.size(), false}, memory);
				positions_.push_back(tk.position, memory);
				push(tuple(h, ord, std::uint32_t(side_.size() - 1), long_term));
			}
		}
		std::uint32_t key_term = no_term;
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
			key_term = extra_key | std::uint32_t(extra_keys_.size());
			extra_keys_.emplace_back(key);
		}
		docids_.push_back(docid);
		versions_.push_back(version);
		lengths_.push_back(length);
		key_terms_.push_back(key_term);  // a key among the terms is resolved when its batch drains
		batch_keys_.push_back({key_at, std::uint32_t(key_at ? key.size() : 0)});
		texts_.push_back(base);
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		charge_columns();
		return ord;
	}

	std::size_t doc_count() const noexcept { return docids_.size(); }
	// Whether `docs` more documents with `entries` terms and tokens in all still fit the
	// 32-bit ordinals, term ids, and side-entry indexes; the index flushes first when not.
	bool fits(std::size_t docs, std::size_t entries) const noexcept {
		return docids_.size() + docs < std::size_t(no_more_docs) - 1 && side_.size() + entries < max_side && terms_.size() + entries < max_terms;
	}
	std::uint64_t docid(ordinal_t ord) const { return docids_.at(ord); }
	std::uint64_t version(ordinal_t ord) const { return versions_.at(ord); }
	std::uint32_t length(ordinal_t ord) const { return lengths_.at(ord); }
	std::string_view stored(ordinal_t ord) const {
		const std::uint64_t begin = ord ? stored_ends_.at(ord - 1) : 0;
		return std::string_view(stored_).substr(begin, stored_ends_.at(ord) - begin);
	}
	std::string_view key(ordinal_t ord) const {
		const std::uint32_t k = key_terms_.at(ord);
		if (k == no_term) {
			if (ord < batch_first_) { return {}; }
			const auto& [p, n] = batch_keys_[ord - batch_first_];
			return std::string_view(p, n);
		}
		if (k & extra_key) { return extra_keys_[k & ~extra_key]; }
		return terms_[k].bytes();
	}

	// The segment of the documents at `order` (ordinals, docids increasing), or of every one
	// in ordinal order when `order` is empty (their docids must increase), written by `w`, a
	// writer holding no documents yet. The batch drains first; a second flush writes the
	// same bytes.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id, std::span<const ordinal_t> order, segment_writer& w) {
		flush_charge* const run = w.memory;  // the flush's scratch and output are charged as the writer's
		{
			struct restore {
				flush_charge*& at;
				flush_charge* was;
				~restore() { at = was; }
			} r{memory, std::exchange(memory, run ? run : memory)};  // the last batch's drain is the flush's too
			drain();
		}
		const bool all = order.empty();
		const std::size_t docs = all ? doc_count() : order.size();
		std::vector<ordinal_t> renumber;  // old ordinal -> new, or no_more_docs when left out
		bool monotone = true;             // the new ordinals keep the old order
		if (!all) {
			renumber.assign(doc_count(), no_more_docs);
			for (std::size_t i = 0; i < order.size(); ++i) {
				renumber[order[i]] = ordinal_t(i);
				monotone = monotone && (i == 0 || order[i] > order[i - 1]);
			}
			if (run) { run->grow(renumber.capacity() * sizeof(ordinal_t)); }
		}
		for (std::size_t i = 0; i < docs; ++i) {
			const ordinal_t o = all ? ordinal_t(i) : order[i];
			w.add_document(docids_[o], lengths_[o], stored(o), versions_[o], key(o));
		}
		// The terms written: every one, or those with a posting left.
		std::vector<std::uint32_t> written;
		written.reserve(terms_.size());
		for (std::uint32_t t = 0; t < terms_.size(); ++t) {
			if (all || has_live(terms_[t], renumber)) { written.push_back(t); }
		}
		if (run) { run->grow(written.capacity() * sizeof(std::uint32_t)); }
		sort_terms(written, run);
		std::array<bool, 256> used{};
		for (std::uint32_t t : written) {
			for (char c : terms_[t].bytes()) { used[std::uint8_t(c)] = true; }
		}
		std::string alphabet;
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { alphabet.push_back(char(c)); }
		}
		std::string file;
		string_output out(file, run);
		string_spill positions(run);
		std::size_t next = 0;
		std::vector<live_posting> live;
		term_postings old;
		w.flush_from(segment_id, out, positions, alphabet, [&](term_postings& tp) -> std::optional<std::string_view> {
			if (next == written.size()) { return std::nullopt; }
			const term_state& t = terms_[written[next++]];
			if (all) {
				decode(t, tp);
			} else {
				decode(t, old);
				relabel(old, renumber, monotone, live, tp);
			}
			return t.bytes();
		});
		return file;
	}

	// Calls f(term, ord, wdf, positions) for every posting, in no particular order.
	template <class F>
	void scan(F&& f) const {
		term_postings tp;
		std::vector<std::uint32_t> absolute;
		for (const term_state& t : terms_) {
			decode(t, tp);
			std::size_t at = 0;
			for (std::size_t i = 0; i < tp.ords.size(); ++i) {
				absolute.clear();
				std::uint32_t p = 0;
				for (std::uint32_t c = 0; c < tp.counts[i]; ++c) { absolute.push_back(p += tp.positions[at++]); }
				f(t.bytes(), tp.ords[i], tp.wdfs[i], std::span<const std::uint32_t>(absolute));
			}
		}
		// The batch not drained yet.
		std::map<std::pair<std::string_view, ordinal_t>, std::pair<std::uint32_t, std::vector<std::uint32_t>>> postings;
		for (const auto& bucket : buckets_) {
			for (const auto& t : bucket) {
				auto& p = postings[{bytes_of(t), t.ord}];
				if (t.len() == long_term) {
					const side_entry& e = side_[t.off];
					p.first += e.wdf;
					for (std::uint32_t i = 0; i < e.count; ++i) { p.second.push_back(positions_[e.positions + i]); }
				} else {
					++p.first;
					p.second.push_back(t.pos());
				}
			}
		}
		for (const auto& [k, v] : postings) { f(k.first, k.second, v.first, std::span<const std::uint32_t>(v.second)); }
	}

private:
	static constexpr std::uint32_t long_term = 255;  // the length field's escape: `off` names a side entry
	static constexpr std::size_t max_side = std::size_t(1) << 32;  // `off` indexes side entries
	static constexpr std::size_t max_terms = std::size_t(1) << 31;  // term ids, below extra_key
	static constexpr std::uint32_t no_term = ~std::uint32_t(0), extra_key = std::uint32_t(1) << 31;

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
	// A term of the store: its bytes, its postings as a chain of chunks in the log, newest
	// first (append() describes a chunk), and what flush_from needs about them.
	struct term_state {
		const char* data;
		std::uint32_t size, df = 0, chunks = 0, max_count = 0;
		std::uint64_t cf = 0, positions = 0, head = 0;  // head: the newest chunk's offset + 1; 0: none
		ordinal_t last = 0;
		bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
		std::string_view bytes() const noexcept { return std::string_view(data, size); }
	};
	struct live_posting {
		ordinal_t ord;
		std::uint32_t wdf, count, first;  // first: its first position in the decoded term
		bool operator<(const live_posting& o) const noexcept { return ord < o.ord; }
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
			(lengths_.capacity() + key_terms_.capacity()) * sizeof(std::uint32_t) + (batch_keys_.capacity() + texts_.capacity()) * 16 + stored_.capacity() +
			terms_.capacity() * sizeof(term_state) + table_hashes_.capacity() * 8 + table_ids_.capacity() * 4;
		if (now > columns_charged_) {
			memory->grow(now - columns_charged_);
			columns_charged_ = now;
		}
	}

	// The batch into the store: each bucket grouped, each group's postings one chunk of its
	// term; then the batch's tuples, text, and side entries are reset, their memory kept.
	void drain() {
		if (batched_) {
			std::vector<group> groups;
			for (unsigned b = 0; b < 256; ++b) {
				groups.clear();
				group_run(buckets_[b].data(), buckets_[b].size(), groups, scratch_);
				prefetch_slots(groups, 0, std::min<std::size_t>(ahead, groups.size()));
				for (std::size_t i = 0; i < groups.size(); ++i) {
					if (i + ahead < groups.size()) { prefetch_slots(groups, i + ahead, i + ahead + 1); }
					append(intern(groups[i]), groups[i]);
				}
				buckets_[b].clear();
			}
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

	static constexpr std::size_t ahead = 8;  // groups whose table slots are fetched ahead of their probe
	void prefetch_slots(const std::vector<group>& groups, std::size_t from, std::size_t to) const noexcept {
		if (table_ids_.empty()) { return; }
		const std::size_t mask = table_ids_.size() - 1;
		for (std::size_t i = from; i < to; ++i) {
			const std::size_t k = (groups[i].begin->hash() >> 8) & mask;
			__builtin_prefetch(&table_hashes_[k]);
			__builtin_prefetch(&table_ids_[k]);
		}
	}

	// The store's id of a group's term, added when new. Terms of up to 8 bytes with equal
	// hashes and lengths are equal (hash is a bijection at each length); the rest compare bytes.
	std::uint32_t intern(const group& g) {
		const std::uint64_t h = g.begin->hash();
		const bool exact = hash_mask == ~std::uint64_t(0) && g.term.size() <= 8;
		if (table_ids_.empty()) { table_hashes_.assign(1 << 10, 0), table_ids_.assign(1 << 10, no_term); }
		if (2 * (terms_.size() + 1) > table_ids_.size()) { grow_table(); }  // keep the load at most a half
		const std::size_t mask = table_ids_.size() - 1;
		for (std::size_t k = (h >> 8) & mask;; k = (k + 1) & mask) {
			if (table_ids_[k] == no_term) {
				table_hashes_[k] = h;
				table_ids_[k] = std::uint32_t(terms_.size());
				char* copy = term_bytes(g.term.size());
				std::memcpy(copy, g.term.data(), g.term.size());
				terms_.push_back({copy, std::uint32_t(g.term.size())});
				return table_ids_[k];
			}
			if (table_hashes_[k] == h) {
				const term_state& t = terms_[table_ids_[k]];
				if (t.size == g.term.size() && (exact || t.bytes() == g.term)) { return table_ids_[k]; }
			}
		}
	}
	void grow_table() {
		std::vector<std::uint64_t> hashes(table_hashes_.size() * 2, 0);
		std::vector<std::uint32_t> ids(table_ids_.size() * 2, no_term);
		const std::size_t mask = ids.size() - 1;
		for (std::size_t k = 0; k < table_ids_.size(); ++k) {
			if (table_ids_[k] == no_term) { continue; }
			std::size_t j = (table_hashes_[k] >> 8) & mask;
			while (ids[j] != no_term) { j = (j + 1) & mask; }
			hashes[j] = table_hashes_[k];
			ids[j] = table_ids_[k];
		}
		table_hashes_.swap(hashes);
		table_ids_.swap(ids);
		charge_columns();
	}
	// Stable bytes for a term, in pages of the term arena.
	char* term_bytes(std::size_t n) {
		if (term_pages_.empty() || term_fill_ + n > term_page_size_) {
			term_page_size_ = std::max<std::size_t>(std::min(arena_chunk, std::size_t(4096) << term_pages_.size()), n);
			term_pages_.push_back(std::unique_ptr<char[]>(new char[term_page_size_]));
			term_fill_ = 0;
			if (memory) { memory->grow(term_page_size_); }
		}
		char* p = term_pages_.back().get() + term_fill_;
		term_fill_ += n;
		return p;
	}

	// A group's postings (one batch's, ordinals increasing) as a chunk of its term: a 32-byte
	// header ([older chunk + 1 u64][postings u32][byte lengths u32 of the four sections][0
	// u32]), then LEB128 ordinal deltas (the first of a term is its ordinal + 1, as in a
	// postings tail), wdfs, counts (only when one differs from its wdf), and position deltas.
	void append(std::uint32_t id, const group& g) {
		term_state& t = terms_[id];
		std::size_t tuples = 0, side_positions = 0;
		for (const tuple* p = g.begin; p != g.end; ++p) {
			++tuples;
			if (p->len() == long_term) { side_positions += side_[p->off].count; }
		}
		for (auto* s : {&sec_ords_, &sec_wdfs_, &sec_counts_}) { s->resize(5 * tuples); }
		sec_positions_.resize(5 * (tuples + side_positions));
		char* ords = sec_ords_.data();
		char* wdfs = sec_wdfs_.data();
		char* counts = sec_counts_.data();
		char* positions = sec_positions_.data();
		std::uint32_t postings = 0;
		bool counted = false;
		for (const tuple* p = g.begin; p != g.end;) {
			const ordinal_t ord = p->ord;
			std::uint32_t wdf = 0, count = 0, prev = 0;
			for (; p != g.end && p->ord == ord; ++p) {
				if (p->len() == long_term) {
					const side_entry& e = side_[p->off];
					wdf += e.wdf, count += e.count;
					if (e.key) { key_terms_[ord] = id; }
					for (std::uint32_t i = 0; i < e.count; ++i) {
						const std::uint32_t pos = positions_[e.positions + i];
						tuple_detail::put_leb(positions, pos - prev);
						prev = pos;
					}
				} else {
					++wdf, ++count;
					tuple_detail::put_leb(positions, p->pos() - prev);
					prev = p->pos();
				}
			}
			tuple_detail::put_leb(ords, t.df ? ord - t.last : std::uint64_t(ord) + 1);
			tuple_detail::put_leb(wdfs, wdf);
			tuple_detail::put_leb(counts, count);
			counted = counted || count != wdf;
			t.last = ord;
			++t.df;
			t.cf += wdf;
			t.positions += count;
			t.max_count = std::max(t.max_count, count);
			if (wdf) { t.all_zero_wdf = false; }
			if (count) { t.any_positions = true; }
			if (count != wdf) { t.counts_are_wdf = false; }
			++postings;
		}
		const std::uint32_t lengths[4] = {std::uint32_t(ords - sec_ords_.data()), std::uint32_t(wdfs - sec_wdfs_.data()),
			counted ? std::uint32_t(counts - sec_counts_.data()) : 0u, std::uint32_t(positions - sec_positions_.data())};
		const std::size_t bytes = 32 + std::size_t(lengths[0]) + lengths[1] + lengths[2] + lengths[3];
		const std::uint64_t at = log_.append(bytes, memory);
		char* c = log_.at(at);
		std::memcpy(c, &t.head, 8);
		std::memcpy(c + 8, &postings, 4);
		std::memcpy(c + 12, lengths, 16);
		std::memset(c + 28, 0, 4);
		c += 32;
		const char* sections[4] = {sec_ords_.data(), sec_wdfs_.data(), sec_counts_.data(), sec_positions_.data()};
		for (int k = 0; k < 4; ++k) {
			std::memcpy(c, sections[k], lengths[k]);
			c += lengths[k];
		}
		t.head = at + 1;
		++t.chunks;
	}

	// A term's chunks, oldest first.
	const std::vector<const char*>& chain_of(const term_state& t) const {
		chain_.clear();
		for (std::uint64_t c = t.head; c;) {
			const char* p = log_.at(c - 1);
			chain_.push_back(p);
			std::memcpy(&c, p, 8);
		}
		std::reverse(chain_.begin(), chain_.end());
		return chain_;
	}

	// A term's postings, decoded from its chunks: ordinals, wdfs, counts, position deltas.
	void decode(const term_state& t, term_postings& tp) const {
		tp.clear();
		tp.ords.reserve(t.df), tp.wdfs.reserve(t.df), tp.counts.reserve(t.df), tp.positions.reserve(t.positions);
		ordinal_t ord = 0;
		bool first = true;
		for (const char* p : chain_of(t)) {
			std::uint32_t postings, lengths[4];
			std::memcpy(&postings, p + 8, 4);
			std::memcpy(lengths, p + 12, 16);
			const char* ords = p + 32;
			const char* wdfs = ords + lengths[0];
			const char* counts = wdfs + lengths[1];
			const char* positions = counts + lengths[2];
			const char* wdfs_again = wdfs;  // the counts when the chunk has none apart
			for (std::uint32_t i = 0; i < postings; ++i) {
				const std::uint64_t delta = tuple_detail::get_leb(ords);
				ord = first ? ordinal_t(delta - 1) : ordinal_t(ord + delta);
				first = false;
				tp.ords.push_back(ord);
				tp.wdfs.push_back(std::uint32_t(tuple_detail::get_leb(wdfs)));
				const std::uint32_t count = std::uint32_t(tuple_detail::get_leb(lengths[2] ? counts : wdfs_again));
				tp.counts.push_back(count);
				for (std::uint32_t k = 0; k < count; ++k) { tp.positions.push_back(std::uint32_t(tuple_detail::get_leb(positions))); }
			}
		}
		tp.cf = t.cf;
		tp.all_zero_wdf = t.all_zero_wdf;
		tp.any_positions = t.any_positions;
		tp.counts_are_wdf = t.counts_are_wdf;
	}
	bool has_live(const term_state& t, const std::vector<ordinal_t>& renumber) const {
		term_postings tp;
		decode(t, tp);
		return std::any_of(tp.ords.begin(), tp.ords.end(), [&](ordinal_t o) { return renumber[o] != no_more_docs; });
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

	// Term ids in byte order of their terms: 8-byte big-endian chunks, zero-padded, with
	// the bytes left (up to 9: more than a chunk) as a tiebreak; a run that ties on a chunk
	// and continues sorts again on the next. Terms are distinct, so every run ends.
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
	void sort_terms(std::vector<std::uint32_t>& ids, flush_charge* run) const {
		std::vector<chunk_key> keys(ids.size());
		if (run) { run->grow(keys.capacity() * sizeof(chunk_key)); }
		for (std::size_t i = 0; i < ids.size(); ++i) { keys[i] = key_at(terms_[ids[i]].bytes(), 0, ids[i]); }
		sort_run(keys.data(), keys.data() + keys.size(), 0);
		for (std::size_t i = 0; i < keys.size(); ++i) { ids[i] = keys[i].index; }
	}
	void sort_run(chunk_key* b, chunk_key* e, std::size_t depth) const {
		std::sort(b, e);
		for (chunk_key* r = b; r != e;) {
			chunk_key* t = r + 1;
			while (t != e && t->chunk == r->chunk && t->left == r->left) { ++t; }
			if (t - r > 1 && r->left == 9) {  // tied on this chunk, and every one continues past it
				for (chunk_key* k = r; k != t; ++k) { *k = key_at(terms_[k->index].bytes(), depth + 8, k->index); }
				sort_run(r, t, depth + 8);
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
	std::vector<std::unique_ptr<char[]>> arena_;      // the batch's text and terms, each padded by a word
	std::vector<std::size_t> arena_sizes_;
	std::size_t arena_at_ = 0, arena_fill_ = 0;
	std::vector<const char*> texts_;                            // each batch document's text, from batch_first_
	std::vector<std::pair<const char*, std::uint32_t>> batch_keys_;  // and its key, until the batch drains
	ordinal_t batch_first_ = 0;
	group_scratch scratch_;
	std::string sec_ords_, sec_wdfs_, sec_counts_, sec_positions_;  // append()'s sections
	mutable std::vector<const char*> chain_;                        // chain_of()'s answer
	// The store: the term table, the terms and their bytes, and their postings' chunks.
	std::vector<std::uint64_t> table_hashes_;
	std::vector<std::uint32_t> table_ids_;
	std::vector<term_state> terms_;
	std::vector<std::unique_ptr<char[]>> term_pages_;
	std::size_t term_fill_ = 0, term_page_size_ = 0;
	tuple_detail::byte_log log_;
	// Per document.
	std::vector<std::uint64_t> docids_, versions_;
	std::vector<std::uint32_t> lengths_, key_terms_;  // key_terms_: the key's term id, no_term, or extra_key | index
	std::deque<std::string> extra_keys_;              // keys that were not among their documents' terms
	std::string stored_;
	std::vector<std::uint64_t> stored_ends_;
	std::uint64_t columns_charged_ = 0;
};

}  // namespace strata
