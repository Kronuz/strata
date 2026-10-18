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

// An index's buffer at one flush thread (docs/research/indexing.md, inverter C): each
// token becomes a 20-byte tuple (hash, ordinal, offset in its document's text, position
// and length) appended in arrival order, the text copied once, and nothing is looked up
// until the flush. The flush partitions the tuples by hash into 256 runs, groups each run
// with a table that stays in cache and a stable counting sort, checks every token's bytes
// against its term's (equal hashes never merge two terms), and hands each term's postings
// to segment_writer::flush_from in ordinal order: the segment a segment_writer given the
// same documents writes.
#pragma once

#include "strata/writer.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

namespace tuple_detail {

// Appended to in place and read in order; chunks never move, so growth copies nothing.
template <class T, unsigned Shift = 12>
class chunked {
public:
	static constexpr std::size_t chunk = std::size_t(1) << Shift;
	void push_back(const T& v, flush_charge* memory) {
		if (left_ == 0) {
			chunks_.push_back(std::unique_ptr<T[]>(new T[chunk]));  // left uninitialized: every slot is written before it is read
			if (memory) { memory->grow(chunk * sizeof(T)); }
			at_ = chunks_.back().get();
			left_ = chunk;
		}
		*at_++ = v;
		--left_;
		++size_;
	}
	T& operator[](std::size_t i) noexcept { return chunks_[i >> Shift][i & (chunk - 1)]; }
	const T& operator[](std::size_t i) const noexcept { return chunks_[i >> Shift][i & (chunk - 1)]; }
	std::size_t size() const noexcept { return size_; }
	// Calls f(span) over the elements, a chunk at a time.
	template <class F>
	void each_span(F&& f) const {
		for (std::size_t c = 0; c * chunk < size_; ++c) { f(std::span<const T>(chunks_[c].get(), std::min(chunk, size_ - c * chunk))); }
	}

private:
	std::vector<std::unique_ptr<T[]>> chunks_;
	T* at_ = nullptr;  // the next slot in the last chunk
	std::size_t size_ = 0, left_ = 0;
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

}  // namespace tuple_detail

class tuple_buffer {
public:
	// Where the buffer reports its allocations (D17); none: nothing is counted.
	flush_charge* memory = nullptr;
	// Every hash is masked with it: tests narrow it to make terms collide.
	std::uint64_t hash_mask = ~std::uint64_t(0);

	// A document: its terms, each once with its wdf and positions, and its text's tokens in
	// order, each adding one to its term's wdf; a term is in one of the two and positions
	// increase. `key` is one of its terms or empty. Returns its ordinal.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens,
		std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		if (docids_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (stored.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		const ordinal_t ord = ordinal_t(docids_.size());
		std::size_t entry_bytes = 0;
		for (const auto& in : terms) { entry_bytes += in.term.size(); }
		char* base = room(text.size() + entry_bytes);
		std::memcpy(base, text.data(), text.size());
		for (const auto& tk : tokens) {
			const char* p = base + tk.offset;
			const std::uint64_t h = tuple_detail::hash(p, tk.size) & hash_mask;
			if (tk.size < long_term && tk.position < (std::uint32_t(1) << 24)) {
				tuples_.push_back(tuple(h, ord, tk.offset, tk.position << 8 | tk.size), memory);
			} else {
				side_.push_back({p, tk.size, 1, 1, positions_.size()}, memory);
				positions_.push_back(tk.position, memory);
				tuples_.push_back(tuple(h, ord, std::uint32_t(side_.size() - 1), long_term), memory);
			}
		}
		const char* key_at = nullptr;
		char* at = base + text.size();
		for (const auto& in : terms) {
			std::memcpy(at, in.term.data(), in.term.size());
			if (!key.empty() && !key_at && in.term == key) { key_at = at; }
			side_.push_back({at, std::uint32_t(in.term.size()), in.wdf, std::uint32_t(in.positions.size()), positions_.size()}, memory);
			for (auto p : in.positions) { positions_.push_back(p, memory); }
			tuples_.push_back(tuple(tuple_detail::hash(at, in.term.size()) & hash_mask, ord, std::uint32_t(side_.size() - 1), long_term), memory);
			at += in.term.size();
		}
		if (!key.empty() && !key_at) {  // a key that is not one of its terms
			char* copy = room(key.size());
			std::memcpy(copy, key.data(), key.size());
			key_at = copy;
		}
		docids_.push_back(docid);
		versions_.push_back(version);
		lengths_.push_back(length);
		keys_.push_back({key_at, std::uint32_t(key.size())});
		texts_.push_back(base);
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		charge_columns();
		return ord;
	}

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
	std::string_view key(ordinal_t ord) const { return keys_.at(ord).first ? std::string_view(keys_[ord].first, keys_[ord].second) : std::string_view(); }

	// The segment of the documents at `order` (ordinals, docids increasing), or of every one
	// in ordinal order when `order` is empty (their docids must increase), written by `w`, a
	// writer holding no documents yet.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id, std::span<const ordinal_t> order, segment_writer& w) const {
		flush_charge* const run = w.memory;  // the flush's scratch and output are charged as the writer's
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
		// Partition by the hash's top byte, then group each run.
		std::unique_ptr<tuple[]> parts;
		std::array<std::size_t, 257> start{};
		partition(parts, start, run);
		std::vector<group> groups;
		group_scratch sc;
		for (unsigned b = 0; b < 256; ++b) {
			const std::size_t first = groups.size();
			group_run(parts.get() + start[b], start[b + 1] - start[b], groups, sc);
			if (!all) { relabel(groups, first, renumber, monotone); }
		}
		std::sort(groups.begin(), groups.end(), [](const group& x, const group& y) { return x.term < y.term; });
		std::array<bool, 256> used{};
		for (const auto& g : groups) {
			for (char c : g.term) { used[std::uint8_t(c)] = true; }
		}
		std::string alphabet;
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { alphabet.push_back(char(c)); }
		}
		if (run) { run->grow(groups.capacity() * sizeof(group)); }
		std::string file;
		string_output out(file, run);
		string_spill positions(run);
		std::size_t next = 0;
		w.flush_from(segment_id, out, positions, alphabet, [&](term_postings& tp) -> std::optional<std::string_view> {
			if (next == groups.size()) { return std::nullopt; }
			const group& g = groups[next++];
			postings_of(g, tp);
			return g.term;
		});
		return file;
	}

	// Calls f(term, ord, wdf, positions) for every posting, in no particular order.
	template <class F>
	void scan(F&& f) const {
		std::map<std::pair<std::string_view, ordinal_t>, std::pair<std::uint32_t, std::vector<std::uint32_t>>> postings;
		tuples_.each_span([&](std::span<const tuple> s) {
			for (const auto& t : s) {
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
		});
		for (const auto& [k, v] : postings) { f(k.first, k.second, v.first, std::span<const std::uint32_t>(v.second)); }
	}

private:
	static constexpr std::uint32_t long_term = 255;  // the length field's escape: `off` names a side entry
	static constexpr std::size_t max_side = std::size_t(1) << 32;  // `off` indexes side entries

	// hash split in halves keeps the tuple at 20 bytes on a 4-byte alignment.
	struct tuple {
		std::uint32_t hash_hi, hash_lo, ord, off, pos_len;
		tuple() = default;  // trivial: chunks and partitions of them are not zeroed
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

	std::string_view bytes_of(const tuple& t) const noexcept {
		if (t.len() == long_term) { return std::string_view(side_[t.off].bytes, side_[t.off].size); }
		return std::string_view(texts_[t.ord] + t.off, t.len());
	}

	// Room for n bytes in the text arena, and a zeroed word after them for hash's loads (the
	// next room starts on it).
	char* room(std::size_t n) {
		if (arena_.empty() || arena_fill_ + n + 8 > arena_size_) {
			arena_size_ = std::max<std::size_t>(std::min(arena_chunk, std::size_t(4096) << arena_.size()), n + 8);  // 4 KiB doubling
			arena_.push_back(std::unique_ptr<char[]>(new char[arena_size_]));
			arena_fill_ = 0;
			if (memory) { memory->grow(arena_size_); }
		}
		char* p = arena_.back().get() + arena_fill_;
		std::memset(p + n, 0, 8);
		arena_fill_ += n;
		return p;
	}

	void charge_columns() {
		if (!memory) { return; }
		const std::uint64_t now = (docids_.capacity() + versions_.capacity() + stored_ends_.capacity()) * sizeof(std::uint64_t) +
			lengths_.capacity() * sizeof(std::uint32_t) + (keys_.capacity() + texts_.capacity()) * 16 + stored_.capacity();
		if (now > columns_charged_) {
			memory->grow(now - columns_charged_);
			columns_charged_ = now;
		}
	}

	// Every tuple into `parts`, run b at [start[b], start[b + 1]), each run in arrival order.
	void partition(std::unique_ptr<tuple[]>& parts, std::array<std::size_t, 257>& start, flush_charge* run) const {
		std::array<std::size_t, 256> count{};
		tuples_.each_span([&](std::span<const tuple> s) {
			for (const auto& t : s) { ++count[t.hash_hi >> 24]; }
		});
		for (unsigned b = 0; b < 256; ++b) { start[b + 1] = start[b] + count[b]; }
		parts.reset(new tuple[tuples_.size()]);
		if (run) { run->grow(tuples_.size() * sizeof(tuple)); }
		// Write-combined: 16 tuples per run gather in a cache-resident buffer before a copy.
		constexpr unsigned wc = 16;
		std::vector<tuple> combine(256 * wc);
		std::array<std::uint8_t, 256> fill{};
		std::array<std::size_t, 256> cursor;
		for (unsigned b = 0; b < 256; ++b) { cursor[b] = start[b]; }
		tuples_.each_span([&](std::span<const tuple> s) {
			for (const auto& t : s) {
				const unsigned b = t.hash_hi >> 24;
				combine[b * wc + fill[b]] = t;
				if (++fill[b] == wc) {
					std::memcpy(parts.get() + cursor[b], &combine[b * wc], sizeof(tuple) * wc);
					cursor[b] += wc;
					fill[b] = 0;
				}
			}
		});
		for (unsigned b = 0; b < 256; ++b) {
			std::memcpy(parts.get() + cursor[b], &combine[b * wc], sizeof(tuple) * fill[b]);
		}
	}

	// One run: an id per distinct hash from a small table, a stable counting sort by id, then
	// every tuple's bytes compared with its group's first; a group holding two terms (a hash
	// collision) is split by bytes, each term's tuples keeping their order.
	void group_run(tuple* p, std::size_t n, std::vector<group>& groups, group_scratch& sc) const {
		if (!n) { return; }
		std::size_t cap = std::bit_ceil(std::max<std::size_t>(64, n / 2));
		auto& hashes = sc.hashes;
		auto& ids = sc.ids;
		hashes.assign(cap, 0);
		ids.assign(cap, 0);
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
				ids.assign(cap, 0);
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
		for (std::size_t i = 0; i < 16 && i < n; ++i) { prefetch(p[i]); }
		std::size_t begin = 0;
		for (std::uint32_t t = 0; t < distinct; ++t) {
			const std::size_t end = counts[t];
			const std::string_view first = bytes_of(p[begin]);
			bool mixed = false;
			for (std::size_t i = begin + 1; i < end; ++i) {
				if (i + 16 < n) { prefetch(p[i + 16]); }
				mixed = mixed || bytes_of(p[i]) != first;
			}
			if (mixed) {
				split(p + begin, p + end, groups);
			} else {
				groups.push_back({first, p + begin, p + end});
			}
			if (end + 16 < n) { prefetch(p[end + 16]); }
			begin = end;
		}
	}
	void prefetch(const tuple& t) const noexcept {
		if (t.len() != long_term) { __builtin_prefetch(texts_[t.ord] + t.off); }
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

	// The run's new groups in the flushed numbering: tuples of documents left out dropped,
	// ordinals renumbered, and resorted when the numbering does not keep the old order (a
	// stable sort, so a document's tuples stay in position order).
	void relabel(std::vector<group>& groups, std::size_t first, const std::vector<ordinal_t>& renumber, bool monotone) const {
		std::size_t kept = first;
		for (std::size_t i = first; i < groups.size(); ++i) {
			tuple* b = groups[i].begin;
			tuple* e = groups[i].end;
			tuple* w = b;
			for (tuple* t = b; t != e; ++t) {
				if (renumber[t->ord] == no_more_docs) { continue; }
				*w = *t;
				w->ord = renumber[t->ord];
				++w;
			}
			if (w == b) { continue; }
			if (!monotone) { std::stable_sort(b, w, [](const tuple& x, const tuple& y) { return x.ord < y.ord; }); }
			groups[kept++] = {groups[i].term, b, w};
		}
		groups.resize(kept);
	}

	// A group's postings: per document its wdf, position count, and positions as deltas. A
	// group of tokens alone has a position per tuple and a posting per document, so its
	// arrays are sized once from the tuple count and filled by index.
	void postings_of(const group& g, term_postings& tp) const {
		tp.clear();
		const std::size_t n = std::size_t(g.end - g.begin);
		bool tokens_only = true;
		for (const tuple* t = g.begin; t != g.end && tokens_only; ++t) { tokens_only = t->len() != long_term; }
		if (tokens_only) {
			tp.ords.resize(n), tp.wdfs.resize(n), tp.positions.resize(n);
			std::uint32_t* ords = tp.ords.data();
			std::uint32_t* wdfs = tp.wdfs.data();
			std::uint32_t* pos = tp.positions.data();
			std::size_t docs = 0;
			std::uint32_t prev = 0;
			for (const tuple* t = g.begin; t != g.end; ++t) {
				if (t == g.begin || t->ord != (t - 1)->ord) {
					ords[docs] = t->ord;
					wdfs[docs++] = 0;
					prev = 0;
				}
				++wdfs[docs - 1];
				*pos++ = t->pos() - prev;
				prev = t->pos();
			}
			tp.ords.resize(docs), tp.wdfs.resize(docs);
			tp.counts = tp.wdfs;
			tp.cf = n;
			tp.all_zero_wdf = false;
			tp.any_positions = true;
			return;
		}
		std::uint32_t prev = 0;
		for (const tuple* t = g.begin; t != g.end; ++t) {
			if (t == g.begin || t->ord != (t - 1)->ord) {
				tp.ords.push_back(t->ord);
				tp.wdfs.push_back(0);
				tp.counts.push_back(0);
				prev = 0;
			}
			if (t->len() == long_term) {
				const side_entry& e = side_[t->off];
				tp.wdfs.back() += e.wdf;
				tp.counts.back() += e.count;
				for (std::uint32_t i = 0; i < e.count; ++i) {
					const std::uint32_t p = positions_[e.positions + i];
					tp.positions.push_back(p - prev);
					prev = p;
				}
			} else {
				++tp.wdfs.back();
				++tp.counts.back();
				tp.positions.push_back(t->pos() - prev);
				prev = t->pos();
			}
		}
		for (std::size_t i = 0; i < tp.ords.size(); ++i) {
			tp.cf += tp.wdfs[i];
			if (tp.wdfs[i]) { tp.all_zero_wdf = false; }
			if (tp.counts[i]) { tp.any_positions = true; }
			if (tp.counts[i] != tp.wdfs[i]) { tp.counts_are_wdf = false; }
		}
	}

	static constexpr std::size_t arena_chunk = std::size_t(4) << 20;

	tuple_detail::chunked<tuple> tuples_;
	tuple_detail::chunked<side_entry, 10> side_;
	tuple_detail::chunked<std::uint32_t> positions_;  // side entries' positions
	std::vector<std::unique_ptr<char[]>> arena_;           // documents' text and terms, each padded by a word
	std::size_t arena_fill_ = 0, arena_size_ = 0;
	std::vector<std::uint64_t> docids_, versions_;
	std::vector<std::uint32_t> lengths_;
	std::vector<std::pair<const char*, std::uint32_t>> keys_;  // each document's key, in the arena
	std::vector<const char*> texts_;                            // each document's text in the arena
	std::string stored_;
	std::vector<std::uint64_t> stored_ends_;
	std::uint64_t columns_charged_ = 0;
};

}  // namespace strata
