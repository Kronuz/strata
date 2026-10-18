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

// The in-memory segment: inverts documents as they arrive, then flushes one segment
// file in a single sequential pass.
//
// Postings of a term with df >= 128 (all integers LEB128 unless noted):
//   [group count][tail first position index][level 1: per 32 blocks: last ordinal delta,
//    first position index delta, level-0 byte length,
//    blocks byte length, impacts][level 0: per block: last ordinal delta, block byte
//    length, first position index delta, impacts][blocks][tail]
// A block is a bitpack FOR block of ordinal deltas, then (unless no-wdf) a PFOR block
// of wdf. The tail holds ordinal deltas, then wdf. Terms with 1 < df < 128 store only
// the tail; df == 1 lives in the term info.
//
// Positions of a term: [per-document counts when they differ from wdf: count block byte
// lengths, PFOR blocks aligned with the postings blocks, tail][position block count]
// [position block byte lengths][PFOR blocks of 128][tail].

#pragma once

#include "bitpack.hh"
#include "compressor_zstd.h"
#include "docset.hh"
#include "fst.hh"
#include "fusefilter.hh"
#include "hashes.hh"
#include "strata/budget.hh"
#include "strata/docids.hh"
#include "strata/format.hh"
#include "task.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef STRATA_INST
#include <chrono>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#endif

namespace strata {

using docset::no_more_docs;

#ifdef STRATA_INST
// Phase timers of the inverter's drain and flush (an instrumented build only).
namespace inst {
inline std::uint64_t now() noexcept {
#if defined(__APPLE__)
	return mach_absolute_time();
#else
	return std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
struct clock {
	std::uint64_t group = 0, encode = 0, sortcopy = 0, next = 0, terms = 0, tail = 0, flush = 0, run_bytes = 0, entries = 0, batches = 0;
	~clock() {
		double unit = 1e-9;
#if defined(__APPLE__)
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		unit = 1e-9 * double(tb.numer) / double(tb.denom);
#endif
		std::fprintf(stderr, "inst: group %.3f encode %.3f sortcopy %.3f | flush %.3f = next %.3f + terms %.3f + tail %.3f s | %llu batches, %llu entries, %llu run bytes\n",
			double(group) * unit, double(encode) * unit, double(sortcopy) * unit, double(flush) * unit, double(next) * unit, double(terms) * unit, double(tail) * unit,
			(unsigned long long)batches, (unsigned long long)entries, (unsigned long long)run_bytes);
	}
};
inline clock c;
}  // namespace inst
#define STRATA_INST_T(x) const std::uint64_t x = ::strata::inst::now()
#define STRATA_INST_ADD(field, t0) (::strata::inst::c.field += ::strata::inst::now() - (t0))
#define STRATA_INST_COUNT(field, n) (::strata::inst::c.field += (n))
#else
#define STRATA_INST_T(x)
#define STRATA_INST_ADD(field, t0)
#define STRATA_INST_COUNT(field, n)
#endif

// One term of a document being added.
struct posting_input {
	std::string_view term;
	std::uint32_t wdf = 1;
	std::span<const std::uint32_t> positions;  // sorted, unique; empty for positionless terms
};

// A token of a document's text: the term at [offset, offset + size) of the text, at a position.
struct text_token {
	std::uint32_t offset = 0, size = 0, position = 0;
};

namespace text_detail {
// The high bit of each byte of w that is a space, exactly.
inline std::uint64_t spaces(std::uint64_t w) noexcept {
	constexpr std::uint64_t ones = 0x0101010101010101ull, low7 = 0x7F7F7F7F7F7F7F7Full;
	const std::uint64_t x = w ^ (ones * ' ');
	return ~(((x & low7) + low7) | x | low7);
}
}  // namespace text_detail

// Calls f(offset, size) for each word of text, a maximal run of bytes other than ' ', in
// order, 8 bytes at a time.
template <class F>
inline void split_words(std::string_view text, F&& f) {
	std::size_t start = 0;  // where the current word starts
	auto end = [&](std::size_t j) {
		if (j > start) { f(start, j - start); }
		start = j + 1;
	};
	std::size_t at = 0;
	if constexpr (std::endian::native == std::endian::little) {
		for (; at + 8 <= text.size(); at += 8) {
			std::uint64_t w;
			std::memcpy(&w, text.data() + at, 8);
			for (std::uint64_t m = text_detail::spaces(w); m; m &= m - 1) { end(at + std::size_t(std::countr_zero(m)) / 8); }
		}
	}
	for (; at < text.size(); ++at) {
		if (text[at] == ' ') { end(at); }
	}
	end(text.size());
}

// The number of words in text and their bytes, without splitting it.
struct word_counts {
	std::uint64_t words = 0, bytes = 0;
};
inline word_counts count_words(std::string_view text) noexcept {
	word_counts c;
	bool in_word = false;  // the byte before
	std::size_t at = 0;
	if constexpr (std::endian::native == std::endian::little) {
		for (; at + 8 <= text.size(); at += 8) {
			std::uint64_t w;
			std::memcpy(&w, text.data() + at, 8);
			const std::uint64_t letters = ~text_detail::spaces(w) & 0x8080808080808080ull;
			const std::uint64_t before = letters << 8 | (in_word ? 0x80u : 0u);  // each byte's previous byte, in words
			c.words += std::uint64_t(std::popcount(letters & ~before));
			c.bytes += std::uint64_t(std::popcount(letters));
			in_word = letters >> 63;
		}
	}
	for (; at < text.size(); ++at) {
		const bool letter = text[at] != ' ';
		c.words += letter && !in_word;
		c.bytes += letter;
		in_word = letter;
	}
	return c;
}

// Receives a segment file's bytes in order.
struct output {
	virtual ~output() = default;
	virtual void write(const char* data, std::size_t size) = 0;
	void write(std::string_view bytes) { write(bytes.data(), bytes.size()); }
};

// Holds a section produced alongside another until it is appended.
struct spill : output {
	virtual void copy_to(output& out) = 0;
};

// Appends to a string, charging its reallocations to `memory` when given.
class string_output final : public output {
public:
	explicit string_output(std::string& s, flush_charge* memory = nullptr) : s_(s), memory_(memory) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (!memory_) {
			s_.append(data, size);
			return;
		}
		const std::size_t before = s_.capacity();
		s_.append(data, size);
		if (s_.capacity() > before) { memory_->grow(s_.capacity() - before); }
	}

private:
	std::string& s_;
	flush_charge* memory_;
};

// A temporary file written once through out(), then read back whole (a mapping): a
// reordering merge's forward index and its stored records.
struct scratch {
	virtual ~scratch() = default;
	virtual output& out() = 0;
	virtual std::string_view bytes() = 0;
};

class string_scratch final : public scratch {
public:
	explicit string_scratch(flush_charge* memory = nullptr) : out_(s_, memory) {}
	output& out() override { return out_; }
	std::string_view bytes() override { return s_; }

private:
	std::string s_;
	string_output out_;
};

class string_spill final : public spill {
public:
	string_spill() = default;
	explicit string_spill(flush_charge* memory) : memory_(memory) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (!memory_) {
			s_.append(data, size);
			return;
		}
		const std::size_t before = s_.capacity();
		s_.append(data, size);
		if (s_.capacity() > before) {
			memory_->grow(s_.capacity() - before);
			charged_ += s_.capacity() - before;
		}
	}
	void copy_to(output& out) override {
		out.write(s_);
		std::string().swap(s_);
		if (memory_) { memory_->shrink(std::exchange(charged_, 0)); }
	}

private:
	std::string s_;
	flush_charge* memory_ = nullptr;
	std::uint64_t charged_ = 0;
};

// Holds a spilled section in chunks that never move (64 KiB doubling to 1 MiB): growth
// copies nothing, and copy_to() frees each chunk once written.
class chunked_spill final : public spill {
public:
	explicit chunked_spill(flush_charge* memory = nullptr) : memory_(memory) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		while (size) {
			if (fill_ == size_) {
				size_ = chunk_size(chunks_.size());
				chunks_.push_back(std::unique_ptr<char[]>(new char[size_]));
				fill_ = 0;
				if (memory_) { memory_->grow(size_); }
			}
			const std::size_t n = std::min(size, size_ - fill_);
			std::memcpy(chunks_.back().get() + fill_, data, n);
			fill_ += n, data += n, size -= n;
		}
	}
	void copy_to(output& out) override {
		for (std::size_t c = 0; c < chunks_.size(); ++c) {
			const std::size_t n = chunk_size(c);
			out.write(chunks_[c].get(), c + 1 == chunks_.size() ? fill_ : n);
			chunks_[c].reset();
			if (memory_) { memory_->shrink(n); }
		}
		chunks_.clear();
		fill_ = size_ = 0;
	}

private:
	static std::size_t chunk_size(std::size_t c) noexcept { return std::size_t(64) << 10 << std::min<std::size_t>(c, 4); }
	std::vector<std::unique_ptr<char[]>> chunks_;
	std::size_t fill_ = 0, size_ = 0;  // the last chunk's
	flush_charge* memory_ = nullptr;
};

// Writes to a file; throws std::runtime_error on failure.
class file_output final : public output {
public:
	explicit file_output(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
		if (!f_) { throw std::runtime_error("strata: cannot create " + path); }
	}
	~file_output() override {
		if (f_) { std::fclose(f_); }
	}
	file_output(const file_output&) = delete;
	file_output& operator=(const file_output&) = delete;
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (std::fwrite(data, 1, size, f_) != size) { throw std::runtime_error("strata: write failed"); }
	}
	// Flushes and closes; throws when the data did not reach the file.
	void close() {
		const bool ok = std::fflush(f_) == 0;
		std::fclose(std::exchange(f_, nullptr));
		if (!ok) { throw std::runtime_error("strata: write failed"); }
	}

private:
	std::FILE* f_;
};

// A temporary file holding a spilled section; removed when copied or destroyed.
class file_spill final : public spill {
public:
	explicit file_spill(std::string path) : path_(std::move(path)), f_(std::fopen(path_.c_str(), "w+b")) {
		if (!f_) { throw std::runtime_error("strata: cannot create " + path_); }
	}
	~file_spill() override { release(); }
	file_spill(const file_spill&) = delete;
	file_spill& operator=(const file_spill&) = delete;
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (std::fwrite(data, 1, size, f_) != size) { throw std::runtime_error("strata: spill write failed"); }
	}
	void copy_to(output& out) override {
		if (std::fflush(f_) != 0 || std::fseek(f_, 0, SEEK_SET) != 0) { throw std::runtime_error("strata: spill read failed"); }
		std::vector<char> buf(1 << 20);
		for (std::size_t n; (n = std::fread(buf.data(), 1, buf.size(), f_)) > 0;) { out.write(buf.data(), n); }
		if (std::ferror(f_)) { throw std::runtime_error("strata: spill read failed"); }
		release();
	}

private:
	void release() {
		if (f_) {
			std::fclose(f_);
			std::remove(path_.c_str());
			f_ = nullptr;
		}
	}
	std::string path_;
	std::FILE* f_;
};

// Tracks bytes written and pads to an alignment.
class counting_output final : public output {
public:
	explicit counting_output(output& out) : out_(out) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		out_.write(data, size);
		size_ += size;
	}
	std::uint64_t size() const noexcept { return size_; }
	void align(std::size_t alignment) {
		static constexpr char zeros[64] = {};
		while (size_ % alignment) { write(zeros, std::min<std::size_t>(sizeof zeros, alignment - size_ % alignment)); }
	}

private:
	output& out_;
	std::uint64_t size_ = 0;
};

// One section's bytes, checksummed as they pass (the whole section and each region).
// Small writes gather in 64 KiB first; finish() passes the rest, so nothing else may write
// to the same output before it.
class section_stream {
public:
	explicit section_stream(counting_output& out, std::uint8_t region_shift = format::default_region_shift)
		: out_(out), offset_(out.size()), shift_(region_shift), size_(std::size_t(1) << region_shift) {}
	section_stream(const section_stream&) = delete;
	section_stream& operator=(const section_stream&) = delete;
	void write(std::string_view bytes) {
		if (pending_.size() + bytes.size() > gather) { drain(); }
		if (bytes.size() >= gather) {
			pass(bytes);
		} else {
			pending_.append(bytes);
		}
		length_ += bytes.size();
	}
	std::uint64_t size() const noexcept { return length_; }
	format::section_entry finish(format::section kind) {
		drain();
		if (region_fill_) { regions_.push_back(region_.value()); }
		return {kind, offset_, length_, crc_.value(), std::move(regions_), shift_};
	}

private:
	static constexpr std::size_t gather = 64 << 10;
	void drain() {
		if (pending_.empty()) { return; }
		pass(pending_);
		pending_.clear();
	}
	// Bytes to the output and both checksums: the section's and each region's.
	void pass(std::string_view bytes) {
		out_.write(bytes);
		crc_.update(bytes.data(), bytes.size());
		for (std::size_t at = 0; at < bytes.size();) {
			const std::size_t n = std::min(bytes.size() - at, size_ - region_fill_);
			region_.update(bytes.data() + at, n);
			region_fill_ += n;
			at += n;
			if (region_fill_ == size_) {
				regions_.push_back(region_.value());
				region_ = {};
				region_fill_ = 0;
			}
		}
	}

	counting_output& out_;
	std::uint64_t offset_, length_ = 0;
	std::uint8_t shift_;
	std::size_t size_;
	hashes::Crc32c crc_, region_;
	std::size_t region_fill_ = 0;
	std::vector<std::uint32_t> regions_;
	std::string pending_;  // writes gathered, not yet passed
};

// A term's postings from one run (format 13's bit 14, `postings_runs`): its full blocks of
// 128 as the segment holds them, each block's base max(previous last, run_start - 1), and
// the remainder in the tail codec (LEB128), as a batch's drain encoded them once.
struct posting_piece {
	ordinal_t run_start = 0;  // the run's first ordinal
	std::uint32_t postings = 0, positions = 0;
	std::uint32_t last = 0;       // the last ordinal, from run_start
	std::uint32_t max_count = 0;  // the most positions of one posting
	std::string_view deltas, wdfs, counts, pos_tail;             // the remainder: ordinal deltas, wdfs, counts, the positions past full blocks
	std::string_view blocks, count_blocks, pos_blocks, records;  // the full blocks and one record per postings block
	std::uint32_t rem_first_pos = 0, rem_pos_off = 0;            // the remainder's first position and its block's offset
};

// One term's postings as a flush or a merge produces them: ordinals ascending, a wdf and a
// position count per ordinal, and every position as deltas within its document. A term
// given as pieces (from_pieces) leaves the columns empty.
struct term_postings {
	std::vector<std::uint32_t> ords, wdfs, counts, positions;
	std::uint64_t cf = 0;
	bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	bool from_pieces = false;
	std::uint32_t piece_df = 0;
	std::vector<posting_piece> pieces;
	void clear() {
		ords.clear(), wdfs.clear(), counts.clear(), positions.clear();
		cf = 0;
		all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
		from_pieces = false, piece_df = 0;
		pieces.clear();
	}
};

class segment_writer {
public:
	// Writes 64-bit skip offsets even where 32 bits fit (tests the wide layout).
	bool wide_skip_offsets = false;
	// Documents arrive in output order with distinct docids in any order (a reordering
	// merge, reordering.md D2); flushes leave it off, so docids increase.
	bool permuted = false;
	// The verification region size of every section, as a power of two.
	std::uint8_t region_shift = format::default_region_shift;
	// The run's draw on the indexing budget, if any: the term table, posting buffers,
	// per-document columns, and the flush's scratch and output report their allocations.
	flush_charge* memory = nullptr;
	// When given, the stored section checks it per frame and throws task::cancelled.
	const task::cancel_token* cancel = nullptr;
	// The runs' first ordinals when terms come as pieces (bit 14's run table, section 12).
	std::vector<ordinal_t> run_starts;
	static constexpr std::uint16_t runs_section = 12;

	// Adds a document and returns its ordinal. Docids strictly increase, so ordinal order
	// is docid order. `length` is the document length used by weighting (Xapian: the sum
	// of wdf over non-boolean terms). `stored` is the document's stored record, returned
	// by segment::stored. `key` is the document's key term (one of its terms) or empty.
	// Each term appears once, with its wdf and its positions in increasing order.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::uint32_t length, std::string_view stored = {},
		std::uint64_t version = 0, std::string_view key = {}) {
		const ordinal_t ord = begin_document(docid, length, stored, version, key);
		for (const auto& in : terms) {
			term_buffer& t = buffer_for(in.term);
			if (t.df && t.last == ord) { throw std::invalid_argument("strata: term repeated within a document"); }
			open_posting(t, ord);
			for (auto p : in.positions) { add_position(t, p); }
			close_posting(t, in.wdf, std::uint32_t(in.positions.size()));
		}
		return ord;
	}

	// The same from a document's tokens in order, as a tokenizer produces them: each
	// occurrence of a term adds one to its wdf and its position to its positions, so
	// nothing has to group them first. Positions increase.
	struct token {
		std::string_view term;
		std::uint32_t position;
	};
	ordinal_t add_tokens(std::uint64_t docid, std::span<const token> tokens, std::uint32_t length, std::string_view stored = {},
		std::uint64_t version = 0, std::string_view key = {}) {
		const ordinal_t ord = begin_document(docid, length, stored, version, key);
		touched_.clear();
		for (const auto& tk : tokens) {
			term_buffer& t = buffer_for(tk.term);
			if (!t.df || t.last != ord) {
				open_posting(t, ord);
				touched_.push_back(&t);
			}
			add_position(t, tk.position);
		}
		for (term_buffer* t : touched_) { close_posting(*t, t->open_count, t->open_count); }
		return ord;
	}

	// A document as terms, each once with its wdf and positions, and its text's tokens in
	// order, each adding one to its term's wdf; a term is in one of the two. Positions increase.
	ordinal_t add(std::uint64_t docid, std::span<const posting_input> terms, std::string_view text, std::span<const text_token> tokens,
		std::uint32_t length, std::string_view stored = {}, std::uint64_t version = 0, std::string_view key = {}) {
		const ordinal_t ord = begin_document(docid, length, stored, version, key);
		for (const auto& in : terms) {
			term_buffer& t = buffer_for(in.term);
			if (t.df && t.last == ord) { throw std::invalid_argument("strata: term repeated within a document"); }
			open_posting(t, ord);
			for (auto p : in.positions) { add_position(t, p); }
			close_posting(t, in.wdf, std::uint32_t(in.positions.size()));
			t.open_prev = closed;
		}
		touched_.clear();
		for (const auto& tk : tokens) {
			term_buffer& t = buffer_for(text.substr(tk.offset, tk.size));
			if (!t.df || t.last != ord) {
				open_posting(t, ord);
				touched_.push_back(&t);
			} else if (t.open_prev == closed || tk.position <= t.open_prev) {
				throw std::invalid_argument("strata: term repeated within a document");
			}
			add_position(t, tk.position);
		}
		for (term_buffer* t : touched_) { close_posting(*t, t->open_count, t->open_count); }
		return ord;
	}

	// Adds a document's per-document data only, for a writer whose terms come from
	// flush_from (a merge); returns its ordinal.
	ordinal_t add_document(std::uint64_t docid, std::uint32_t length, std::string_view stored = {}, std::uint64_t version = 0, std::string_view key = {}) {
		return begin_document(docid, length, stored, version, key);
	}

	std::size_t doc_count() const noexcept { return lengths_.size(); }

	// Writes the segment file and returns its bytes.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id) {
		std::string file;
		string_output out(file, memory);
		string_spill positions(memory);
		flush(segment_id, out, positions);
		return file;
	}

	// Streams the segment file to out in one pass. The positions section is produced
	// alongside the postings, so it goes to `positions` until the postings end.
	void flush(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill) {
		std::vector<std::pair<std::string_view, std::size_t>> keys;
		keys.reserve(buffers_.size());
		if (memory) { memory->grow(keys.capacity() * sizeof(keys[0])); }
		for (const auto& slot : table_) {
			if (slot.key) { keys.push_back({std::string_view(slot.key, slot.length), slot.buffer}); }
		}
		std::sort(keys.begin(), keys.end());
		std::array<bool, 256> used{};
		for (const auto& [key, _] : keys) {
			for (char c : key) { used[std::uint8_t(c)] = true; }
		}
		std::string alphabet;
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c]) { alphabet.push_back(char(c)); }
		}
		std::size_t next = 0;
		flush_from(segment_id, sink, positions_spill, alphabet, [&](term_postings& tp) -> std::optional<std::string_view> {
			if (next == keys.size()) { return std::nullopt; }
			const auto [key, buffer] = keys[next++];
			const term_buffer& t = buffers_[buffer];
			tp.clear();
			decode_all(t.ords, tp.ords, t.df);
			for (std::size_t i = 1; i < tp.ords.size(); ++i) { tp.ords[i] += tp.ords[i - 1]; }
			for (auto& o : tp.ords) { --o; }  // stored as ordinal + 1 deltas
			decode_all(t.wdfs, tp.wdfs, t.df);
			if (t.counts_are_wdf) {
				tp.counts = tp.wdfs;
			} else {
				decode_all(t.counts, tp.counts, t.df);
			}
			if (t.any_positions) {
				std::size_t total = 0;
				for (auto c : tp.counts) { total += c; }
				decode_all(t.positions, tp.positions, total);
			}
			tp.cf = t.cf;
			tp.all_zero_wdf = t.all_zero_wdf;
			tp.any_positions = t.any_positions;
			tp.counts_are_wdf = t.counts_are_wdf;
			return key;
		});
	}

	// Writes the segment with terms from next(tp), which fills tp and returns the term, in
	// strictly increasing order, or nothing at the end. Documents come from add() or
	// add_document(); a term's ordinals name them. `alphabet` holds every byte the terms
	// use (the dictionary codes its keys over it; empty: all 256).
	template <class Next>
	void flush_from(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill, std::string_view alphabet, Next&& next) {

		counting_output out(sink);
		std::vector<format::section_entry> toc;
		{
			const std::string header = format::encode_header(segment_id);
			section_stream h(out, region_shift);
			h.write(header);
			toc.push_back(h.finish(format::section::header));
		}
		out.align(format::section_alignment);
		section_stream postings(out, region_shift);
		counting_output spill_out(positions_spill);
		section_stream positions(spill_out, region_shift);
		termdict::builder dict(alphabet);
		format::term_run run;
		term_postings tp;
		std::string encoded_postings, term_positions;
		std::vector<format::impact> all;  // a long list's (wdf, length) pairs
		std::string last_key;
		bool first_term = true;
		std::uint64_t scratch = 0;  // the per-term buffers' charged capacity
		STRATA_INST_T(loop0);
		[[maybe_unused]] std::uint64_t next_ticks = 0;
		for (;;) {
			STRATA_INST_T(n0);
			const auto got = next(tp);
#ifdef STRATA_INST
			next_ticks += inst::now() - n0;
#endif
			if (!got) { break; }
			const std::string_view key = *got;
			if (!first_term && key <= std::string_view(last_key)) { throw std::invalid_argument("strata: terms must increase"); }
			first_term = false;
			last_key.assign(key);
			const auto& t = tp;
			const auto& ords = tp.ords;
			const auto& wdfs = tp.wdfs;
			const auto& counts = tp.counts;
			const auto& pos = tp.positions;
			const std::uint32_t df = t.from_pieces ? t.piece_df : std::uint32_t(ords.size());
			if (df == 0 || (!t.from_pieces && (wdfs.size() != df || counts.size() != df))) { throw std::invalid_argument("strata: malformed term postings"); }
			if (t.from_pieces) {
				for (const auto& pc : t.pieces) { max_positions_ = std::max(max_positions_, pc.max_count); }
			} else {
				for (auto c : counts) { max_positions_ = std::max(max_positions_, c); }
			}

			format::term_info info;
			info.df = df;
			info.cf = t.cf;
			if (t.all_zero_wdf) { info.flags |= format::no_wdf; }
			if (!t.any_positions) { info.flags |= format::no_positions; }
			if (t.counts_are_wdf) { info.flags |= format::counts_are_wdf; }

			if (t.from_pieces) {
				piece_bases(t);
				if (t.any_positions) {
					info.positions = positions.size();
					term_positions.clear();
					write_piece_positions(term_positions, t);
					info.positions_length = term_positions.size();
					positions.write(term_positions);
				}
				if (df == 1) {
					const posting_piece& pc = t.pieces.front();
					const char* d = pc.deltas.data();
					const char* w = pc.wdfs.data();
					info.flags |= format::single_posting;
					info.postings = std::uint32_t(format::get_leb(&d, d + pc.deltas.size()) + pc.run_start - 1);
					info.single_wdf = std::uint32_t(format::get_leb(&w, w + pc.wdfs.size()));
				} else {
					info.postings = postings.size();
					encoded_postings.clear();
					write_piece_postings(encoded_postings, t, info.impacts);
					info.postings_length = encoded_postings.size();
					postings.write(encoded_postings);
				}
				dict.add(key, format::encode_term_info(info, run, dict.next_is_restart()));
				continue;
			}

			if (df >= format::block) {  // the term info stores whole-list pairs only with skip blocks
				all.clear();
				for (std::size_t i = 0; i < ords.size(); ++i) { all.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
				info.impacts = format::competitive(all);
			}

			positions_layout layout;
			if (t.any_positions) {
				info.positions = positions.size();
				term_positions.clear();
				layout = write_positions(term_positions, counts, pos, t.counts_are_wdf);
				info.positions_length = term_positions.size();
				positions.write(term_positions);
			}
			if (df == 1) {
				info.flags |= format::single_posting;
				info.postings = ords[0];
				info.single_wdf = wdfs[0];
			} else {
				info.postings = postings.size();
				encoded_postings.clear();
				write_postings(encoded_postings, ords, wdfs, !t.all_zero_wdf, counts, t.any_positions ? &layout : nullptr, !t.counts_are_wdf);
				info.postings_length = encoded_postings.size();
				postings.write(encoded_postings);
			}
			dict.add(key, format::encode_term_info(info, run, dict.next_is_restart()));
			if (memory) {
				const std::uint64_t now = (tp.ords.capacity() + tp.wdfs.capacity() + tp.counts.capacity() + tp.positions.capacity()) * sizeof(std::uint32_t) +
					all.capacity() * sizeof(format::impact) + encoded_postings.capacity() + term_positions.capacity();
				if (now > scratch) {
					memory->grow(now - scratch);
					scratch = now;
				}
			}
		}
#ifdef STRATA_INST
		inst::c.next += next_ticks;
		inst::c.terms += inst::now() - loop0 - next_ticks;
#endif
		STRATA_INST_T(tail0);
		toc.push_back(postings.finish(format::section::postings));
		out.align(format::section_alignment);
		{
			auto entry = positions.finish(format::section::positions);
			entry.offset = out.size();
			positions_spill.copy_to(out);
			toc.push_back(std::move(entry));
		}
		auto tables = dict.finish();
		if (memory) { memory->grow(tables.blocks.capacity() + tables.index.capacity()); }
		std::string stats;
		format::put<std::uint64_t>(stats, lengths_.size());
		format::put<std::uint64_t>(stats, total_length_);
		format::put<std::uint32_t>(stats, lengths_.empty() ? 0 : *std::min_element(lengths_.begin(), lengths_.end()));
		format::put<std::uint32_t>(stats, lengths_.empty() ? 0 : *std::max_element(lengths_.begin(), lengths_.end()));
		format::put<std::uint32_t>(stats, max_positions_);
		auto append = [&](format::section kind, const std::string& bytes) {
			out.align(format::section_alignment);
			section_stream sec(out, region_shift);
			sec.write(bytes);
			toc.push_back(sec.finish(kind));
		};
		// A section built only to be appended is charged while it lives.
		auto append_built = [&](format::section kind, const std::string& bytes) {
			if (memory) { memory->grow(bytes.capacity()); }
			append(kind, bytes);
			if (memory) { memory->shrink(bytes.capacity()); }
		};
		append(format::section::terms, tables.blocks);
		append(format::section::term_index, tables.index);
		append_built(format::section::lengths, pack_u32(lengths_));
		{
			std::string norms(lengths_.size(), '\0');
			for (std::size_t i = 0; i < lengths_.size(); ++i) { norms[i] = char(format::encode_length(lengths_[i])); }
			append_built(format::section::norms, norms);
		}
		append_built(format::section::docids, encode_docids());
		append_built(format::section::versions, encode_versions());
		if (!key_hashes_.empty()) { append_built(format::section::pk_filter, encode_pk_filter()); }
		append_built(format::section::stored, encode_stored());
		if (!run_starts.empty()) {  // bit 14's run table: [count u32][first ordinal u32 each]
			std::string runs;
			format::put<std::uint32_t>(runs, std::uint32_t(run_starts.size()));
			for (ordinal_t s : run_starts) { format::put<std::uint32_t>(runs, s); }
			append(format::section(runs_section), runs);
		}
		append(format::section::stats, stats);
		out.align(format::section_alignment);

		format::footer f;
		f.toc_offset = out.size();
		std::string toc_bytes = format::encode_toc(toc);
		f.toc_length = toc_bytes.size();
		f.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
		f.doc_count = std::uint32_t(lengths_.size());
		f.min_docid = docids_.empty() ? 0 : *std::min_element(docids_.begin(), docids_.end());
		f.max_docid = docids_.empty() ? 0 : *std::max_element(docids_.begin(), docids_.end());
		out.write(toc_bytes);
		out.write(format::encode_footer(f));
		STRATA_INST_ADD(tail, tail0);
	}

private:
	struct term_buffer {
		std::string ords, wdfs, counts, positions;  // LEB128 streams while buffering
		ordinal_t last = 0;
		std::uint32_t df = 0, max_wdf = 0;
		std::uint32_t open_count = 0, open_prev = 0;  // the open posting: positions so far, the last one
		std::uint64_t cf = 0;
		bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	};
	static constexpr std::uint32_t closed = ~std::uint32_t(0);  // open_prev once a terms entry closed it

	// The per-document data; returns the document's ordinal.
	ordinal_t begin_document(std::uint64_t docid, std::uint32_t length, std::string_view stored, std::uint64_t version, std::string_view key) {
		// Ordinals are 32-bit and the largest value means no more documents.
		if (lengths_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (docid == 0 || (!permuted && !docids_.empty() && docid <= docids_.back())) { throw std::invalid_argument("strata: docids must increase within a segment"); }
		if (stored.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		const ordinal_t ord = ordinal_t(lengths_.size());
		versions_.push_back(version);
		if (!key.empty()) {
			key_hashes_.push_back(xxh64::hash(key));
			if (keys_min_.empty() && keys_max_.empty()) {
				keys_min_ = keys_max_ = key;
			} else {
				if (key < keys_min_) { keys_min_ = key; }
				if (key > keys_max_) { keys_max_ = key; }
			}
		}
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		lengths_.push_back(length);
		docids_.push_back(docid);
		total_length_ += length;
		if (memory) {
			const std::uint64_t now = lengths_.capacity() * sizeof(std::uint32_t) +
				(stored_ends_.capacity() + docids_.capacity() + versions_.capacity() + key_hashes_.capacity()) * sizeof(std::uint64_t) + stored_.capacity();
			if (now > columns_charged_) {
				memory->grow(now - columns_charged_);
				columns_charged_ = now;
			}
		}
		return ord;
	}

	// A term's posting in document ord: opened, given its positions in order, then closed
	// with its wdf and position count.
	void open_posting(term_buffer& t, ordinal_t ord) {
		put_stream(t.ords, t.df ? ord - t.last : ord + 1);
		t.last = ord;
		++t.df;
		t.open_count = 0;
		t.open_prev = 0;
	}
	void add_position(term_buffer& t, std::uint32_t p) {
		put_stream(t.positions, p - t.open_prev);
		t.open_prev = p;
		++t.open_count;
	}
	void close_posting(term_buffer& t, std::uint32_t wdf, std::uint32_t count) {
		put_stream(t.wdfs, wdf);
		t.cf += wdf;
		t.max_wdf = std::max(t.max_wdf, wdf);
		if (wdf) { t.all_zero_wdf = false; }
		if (count) { t.any_positions = true; }
		// Counts are kept only once one differs from its wdf; until then they are the wdfs,
		// whose LEB128 stream is the same bytes.
		if (t.counts_are_wdf && count != wdf) {
			t.counts_are_wdf = false;
			const std::size_t before = t.counts.capacity();
			t.counts.assign(t.wdfs, 0, t.wdfs.size() - leb_size(wdf));
			if (memory && t.counts.capacity() > before) { memory->grow(t.counts.capacity() - before); }
		}
		if (!t.counts_are_wdf) { put_stream(t.counts, count); }
		max_positions_ = std::max(max_positions_, count);
	}
	// Appends to a posting stream, charging its reallocations.
	void put_stream(std::string& stream, std::uint64_t v) {
		if (!memory) {
			format::put_leb(stream, v);
			return;
		}
		const std::size_t before = stream.capacity();
		format::put_leb(stream, v);
		if (stream.capacity() > before) { memory->grow(stream.capacity() - before); }
	}

	// Open addressing with linear probing over stored hashes; grows at half load.
	struct slot {
		std::uint64_t hash = 0;
		const char* key = nullptr;  // in arena_
		std::uint32_t length = 0;
		std::uint32_t buffer = 0;
	};

	term_buffer& buffer_for(std::string_view term) {
		if (table_.empty()) {
			table_.resize(1 << 16);
			if (memory) { memory->grow(table_.size() * sizeof(slot)); }
		}
		const std::uint64_t h = ::wordwise::hash(term.data(), term.size());
		std::size_t mask = table_.size() - 1;
		for (std::size_t i = h & mask;; i = (i + 1) & mask) {
			slot& sl = table_[i];
			if (!sl.key) { break; }
			if (sl.hash == h && sl.length == term.size() && std::memcmp(sl.key, term.data(), term.size()) == 0) { return buffers_[sl.buffer]; }
		}
		// Term bytes live in stable arena chunks so slots can point at them.
		if (arena_.empty() || arena_.back().size() + term.size() > arena_.back().capacity()) {
			arena_.emplace_back();
			arena_.back().reserve(std::max<std::size_t>(1 << 20, term.size()));
			if (memory) { memory->grow(arena_.back().capacity()); }
		}
		std::string& chunk = arena_.back();
		const char* at = chunk.data() + chunk.size();
		chunk.append(term);
		if ((buffers_.size() + 1) * 2 > table_.size()) { grow(); }
		insert({h, at, std::uint32_t(term.size()), std::uint32_t(buffers_.size())});
		if (memory) { memory->grow(sizeof(term_buffer)); }
		return buffers_.emplace_back();
	}

	void insert(const slot& s) {
		const std::size_t mask = table_.size() - 1;
		std::size_t i = s.hash & mask;
		while (table_[i].key) { i = (i + 1) & mask; }
		table_[i] = s;
	}

	void grow() {
		if (memory) { memory->grow(table_.size() * sizeof(slot)); }  // the table doubles
		std::vector<slot> old(table_.size() * 2);
		old.swap(table_);
		for (const auto& s : old) {
			if (s.key) { insert(s); }
		}
	}

	static std::size_t leb_size(std::uint64_t v) noexcept {
		std::size_t n = 1;
		while (v >= 0x80) { v >>= 7, ++n; }
		return n;
	}

	static void decode_all(const std::string& stream, std::vector<std::uint32_t>& out, std::size_t n) {
		out.resize(n);
		const char* p = stream.data();
		const char* end = p + stream.size();
		for (auto& v : out) { v = std::uint32_t(format::get_leb(&p, end)); }
	}

	struct block_meta {
		ordinal_t last;
		std::uint64_t bytes;
		std::uint64_t first_position;  // index into the term's position stream
		std::vector<format::impact> impacts;
		std::uint64_t pos_offset = 0;    // byte offset of the position block holding first_position
		std::uint64_t count_offset = 0;  // byte offset of the block's count block
		std::uint8_t code = 0;           // 0 FOR deltas, 1 a bitset, 2 a short block in the tail codec (bit 14)
	};

	// Where write_positions put each block, for the skip data.
	struct positions_layout {
		std::vector<std::uint64_t> block_offsets;  // each full position block, from the blocks' start
		std::uint64_t blocks_bytes = 0;            // where the tail starts
		std::vector<std::uint64_t> count_offsets;  // each postings block's count block
		std::uint64_t offset_of(std::uint64_t first_position) const {
			const std::uint64_t j = first_position / format::block;
			return j < block_offsets.size() ? block_offsets[j] : blocks_bytes;
		}
	};

	void write_postings(std::string& out, const std::vector<std::uint32_t>& ords, const std::vector<std::uint32_t>& wdfs, bool with_wdf,
		const std::vector<std::uint32_t>& counts, const positions_layout* layout, bool with_counts) {
		const std::size_t full = ords.size() / format::block * format::block;
		if (full == 0) {  // a tail alone: deltas from ordinal + 1, then wdfs
			std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();
			for (auto o : ords) { format::put_leb(out, o - prev), prev = o; }
			if (with_wdf) {
				for (auto w : wdfs) { format::put_leb(out, w); }
			}
			return;
		}
		std::string blocks;
		std::vector<block_meta> meta;
		std::uint32_t deltas[format::block];
		std::uint8_t buf[bitpack::max_encoded_bytes];
		std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();  // the first delta is ordinal + 1
		std::uint64_t position = 0;
		std::string bits;
		for (std::size_t b = 0; b < full; b += format::block) {
			const std::size_t start = blocks.size();
			// A dense block is a bitset over (previous last, last] when that is smaller.
			const ordinal_t base = b ? ords[b - 1] + 1 : 0;
			const std::uint64_t span = std::uint64_t(ords[b + format::block - 1]) - base + 1;
			bitpack::delta_encode(prev, ords.data() + b, deltas);
			const std::size_t for_bytes = bitpack::encode_for(deltas, buf);
			const bool bitset = (span + 7) / 8 < for_bytes;
			if (bitset) {
				bits.assign((span + 7) / 8, '\0');
				for (std::size_t i = b; i < b + format::block; ++i) {
					const std::uint64_t at = ords[i] - base;
					bits[at >> 3] = char(std::uint8_t(bits[at >> 3]) | (1u << (at & 7)));
				}
				blocks += bits;
			} else {
				blocks.append(reinterpret_cast<char*>(buf), for_bytes);
			}
			if (with_wdf) { blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(wdfs.data() + b, buf)); }
			pairs_.clear();
			for (std::size_t i = b; i < b + format::block; ++i) { pairs_.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			meta.push_back({ords[b + format::block - 1], blocks.size() - start, position, format::competitive(pairs_)});
			meta.back().code = bitset ? 1 : 0;
			if (layout) {
				meta.back().pos_offset = layout->offset_of(position);
				if (with_counts) { meta.back().count_offset = layout->count_offsets[b / format::block]; }
			}
			for (std::size_t i = b; i < b + format::block; ++i) { position += counts[i]; }
			prev = ords[b + format::block - 1];
		}
		std::string tail;
		for (std::size_t i = full; i < ords.size(); ++i) { format::put_leb(tail, ords[i] - prev), prev = ords[i]; }
		if (with_wdf) {
			for (std::size_t i = full; i < ords.size(); ++i) { format::put_leb(tail, wdfs[i]); }
		}
		if (meta.empty()) {
			out += tail;
			return;
		}
		write_skip(out, meta, blocks, tail, layout != nullptr, with_counts, position, layout ? layout->offset_of(position) : 0, 1);
	}

	// A list of 128 or more postings from its blocks' records: [groups][wide][tail's first
	// position and its block's offset][sizes][level 1][pairs][level 0][blocks][tail]. A
	// level-0 entry's byte length is shifted by code_shift over its code (1: format 12's
	// bitset bit; 2: bit 14's three codes).
	void write_skip(std::string& out, const std::vector<block_meta>& meta, const std::string& blocks, const std::string& tail, bool with_positions,
		bool with_counts, std::uint64_t position, std::uint64_t tail_offset, unsigned code_shift) {
		// Level 0, grouped by 32 blocks. Level 1 is a fixed-width array, one entry per group,
		// so a reader locates groups without parsing: last ordinal (u32), the offset of its
		// competitive pairs in the pairs area (u32), its level-0 and blocks offsets, and for
		// terms with positions its first position index and position and count block
		// offsets, each u32, or u64 when the term flags itself wide. Level-0 positional fields (present only with positions, counts only
		// when they differ from wdf) are deltas restarting per group.
		with_counts = with_counts && with_positions;
		std::string level1, pairs, level0;
		const std::size_t groups = (meta.size() + 31) / 32;
		struct group_fields {
			ordinal_t last;
			std::uint32_t pairs;
			std::uint64_t level0, blocks, first_position, pos_offset, count_offset;
		};
		std::vector<group_fields> fields;
		ordinal_t prev_last = 0;
		std::uint64_t blocks_before = 0;
		for (std::size_t g = 0; g < groups; ++g) {
			std::string group;
			const block_meta& first = meta[g * 32];
			block_meta prev = first;
			std::uint64_t group_bytes = 0;
			std::vector<format::impact> group_pairs;
			const std::uint64_t level0_offset = level0.size(), blocks_offset = blocks_before;
			for (std::size_t i = g * 32; i < std::min(meta.size(), g * 32 + 32); ++i) {
				format::put_leb(group, meta[i].last - (i ? prev_last : 0));
				format::put_leb(group, meta[i].bytes << code_shift | std::uint64_t(meta[i].code));
				if (with_positions) {
					format::put_leb(group, meta[i].first_position - prev.first_position);
					format::put_leb(group, meta[i].pos_offset - prev.pos_offset);
					if (with_counts) { format::put_leb(group, meta[i].count_offset - prev.count_offset); }
				}
				format::encode_sized_impacts(group, meta[i].impacts);
				prev_last = meta[i].last;
				prev = meta[i];
				group_bytes += meta[i].bytes;
				group_pairs.insert(group_pairs.end(), meta[i].impacts.begin(), meta[i].impacts.end());
			}
			blocks_before += group_bytes;
			if (pairs.size() > std::numeric_limits<std::uint32_t>::max()) { throw std::length_error("strata: skip data too large"); }
			fields.push_back({meta[std::min(meta.size(), g * 32 + 32) - 1].last, std::uint32_t(pairs.size()), level0_offset, blocks_offset,
				first.first_position, first.pos_offset, first.count_offset});
			format::encode_impacts(pairs, format::competitive(std::move(group_pairs)));
			level0 += group;
		}
		// Offsets take 32 bits unless one of this term's needs 64.
		bool wide = wide_skip_offsets;
		for (const auto& f : fields) {
			wide = wide || std::max({f.level0, f.blocks, f.first_position, f.pos_offset, f.count_offset}) > std::numeric_limits<std::uint32_t>::max();
		}
		auto put_offset = [&](std::uint64_t v) {
			if (wide) {
				format::put<std::uint64_t>(level1, v);
			} else {
				format::put<std::uint32_t>(level1, std::uint32_t(v));
			}
		};
		for (const auto& f : fields) {
			format::put<std::uint32_t>(level1, f.last);
			format::put<std::uint32_t>(level1, f.pairs);
			put_offset(f.level0);
			put_offset(f.blocks);
			if (with_positions) {
				put_offset(f.first_position);
				put_offset(f.pos_offset);
				put_offset(f.count_offset);
			}
		}
		format::put_leb(out, groups);
		out.push_back(char(wide));
		if (with_positions) {
			format::put_leb(out, position);  // the tail's first position index and its block offset
			format::put_leb(out, tail_offset);
		}
		format::put_leb(out, pairs.size());
		format::put_leb(out, level0.size());
		format::put_leb(out, blocks.size());
		out += level1;
		out += pairs;
		out += level0;
		out += blocks;
		out += tail;
	}

	// Where each piece's positions, position bytes, and count bytes start in its term's.
	struct piece_base {
		std::uint64_t first_pos, pos, count;
	};
	void piece_bases(const term_postings& t) {
		bases_.clear();
		std::uint64_t first = 0, pos = 0, count = 0;
		for (const auto& pc : t.pieces) {
			bases_.push_back({first, pos, count});
			first += pc.positions;
			pos += pc.pos_blocks.size() + pc.pos_tail.size();
			count += pc.count_blocks.size() + pc.counts.size();
		}
	}
	// Bit 14's positions: the pieces' count blocks and count tails, then their position blocks
	// and position tails, each back to back; the byte lengths count up to the last tail.
	void write_piece_positions(std::string& out, const term_postings& t) const {
		const posting_piece& last = t.pieces.back();
		std::uint64_t counts = 0, pos = 0, full = 0;
		for (const auto& pc : t.pieces) {
			counts += pc.count_blocks.size() + pc.counts.size();
			pos += pc.pos_blocks.size() + pc.pos_tail.size();
			full += pc.positions / format::block;
		}
		if (!t.counts_are_wdf) {
			format::put_leb(out, counts - last.counts.size());
			for (const auto& pc : t.pieces) {
				out += pc.count_blocks;
				out += pc.counts;
			}
		}
		format::put_leb(out, full);
		format::put_leb(out, pos - last.pos_tail.size());
		for (const auto& pc : t.pieces) {
			out += pc.pos_blocks;
			out += pc.pos_tail;
		}
	}
	// Bit 14's postings from pieces: below 128 postings the pieces' tails as one tail (each
	// first delta rebased), else every piece's full blocks and, but for the last piece's (the
	// list's tail), its remainder as a short block, the skip data built from their records.
	void write_piece_postings(std::string& out, const term_postings& t, std::vector<format::impact>& impacts) {
		const bool with_wdf = !t.all_zero_wdf, with_positions = t.any_positions, with_counts = !t.counts_are_wdf;
		const auto& ps = t.pieces;
		auto rebase = [](std::string& to, const posting_piece& pc, std::uint32_t prev) {
			const char* d = pc.deltas.data();
			const char* e = d + pc.deltas.size();
			const std::uint32_t first = std::uint32_t(format::get_leb(&d, e)) + pc.run_start - 1;
			format::put_leb(to, std::uint32_t(first - prev));
			to.append(d, std::size_t(e - d));
		};
		if (t.piece_df < format::block) {
			std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();
			for (const auto& pc : ps) {
				rebase(out, pc, prev);
				prev = pc.run_start + pc.last;
			}
			if (with_wdf) {
				for (const auto& pc : ps) { out += pc.wdfs; }
			}
			return;
		}
		piece_meta_.clear();
		piece_blocks_.clear();
		piece_tail_.clear();
		all_pairs_.clear();
		std::uint64_t tail_position = 0, tail_offset = 0;
		for (std::size_t k = 0; k < ps.size(); ++k) {
			const posting_piece& pc = ps[k];
			const piece_base& b = bases_[k];
			if (!pc.records.empty()) {
				piece_blocks_ += pc.blocks;
				const char* r = pc.records.data();
				const char* re = r + pc.records.size();
				std::uint32_t prev = pc.run_start - 1;
				for (std::uint32_t j = 0; j < pc.postings / format::block; ++j) {
					block_meta bm;
					bm.last = prev + std::uint32_t(format::get_leb(&r, re));
					const std::uint64_t bytes = format::get_leb(&r, re);
					bm.bytes = bytes >> 1;
					bm.code = std::uint8_t(bytes & 1);
					bm.first_position = b.first_pos + format::get_leb(&r, re);
					bm.pos_offset = b.pos + format::get_leb(&r, re);
					bm.count_offset = b.count + format::get_leb(&r, re);
					const std::uint64_t n = format::get_leb(&r, re);
					for (std::uint64_t i = 0; i < n; ++i) {
						format::impact im;
						im.wdf = std::uint32_t(format::get_leb(&r, re));
						im.length = std::uint8_t(*r++);
						bm.impacts.push_back(im);
					}
					all_pairs_.insert(all_pairs_.end(), bm.impacts.begin(), bm.impacts.end());
					prev = bm.last;
					piece_meta_.push_back(std::move(bm));
				}
			}
			const bool rem = pc.postings % format::block != 0;
			if (rem) {
				remainder_pairs(pc, piece_meta_.empty() || pc.records.empty() ? pc.run_start - 1 : piece_meta_.back().last);
				all_pairs_.insert(all_pairs_.end(), rem_pairs_.begin(), rem_pairs_.end());
			}
			if (k + 1 < ps.size()) {
				if (!rem) { continue; }
				const std::size_t start = piece_blocks_.size();
				format::put_leb(piece_blocks_, pc.postings % format::block);
				piece_blocks_ += pc.deltas;
				if (with_wdf) { piece_blocks_ += pc.wdfs; }
				block_meta bm;
				bm.last = pc.run_start + pc.last;
				bm.bytes = piece_blocks_.size() - start;
				bm.code = 2;
				bm.first_position = b.first_pos + pc.rem_first_pos;
				bm.pos_offset = b.pos + pc.rem_pos_off;
				bm.count_offset = b.count + pc.count_blocks.size();
				bm.impacts = rem_pairs_;
				piece_meta_.push_back(std::move(bm));
				continue;
			}
			tail_position = b.first_pos + pc.rem_first_pos;
			tail_offset = b.pos + pc.rem_pos_off;
			if (!rem) { continue; }
			if (pc.records.empty()) {
				rebase(piece_tail_, pc, piece_meta_.back().last);  // a run's first postings: their base was run_start - 1
			} else {
				piece_tail_ += pc.deltas;
			}
			if (with_wdf) { piece_tail_ += pc.wdfs; }
		}
		impacts = format::competitive(all_pairs_);
		write_skip(out, piece_meta_, piece_blocks_, piece_tail_, with_positions, with_counts, tail_position, tail_offset, 2);
	}

	// A piece's remainder's competitive pairs (as format::competitive orders them) from its
	// LEB128 ordinal deltas, from `base`, and wdfs, into rem_pairs_.
	void remainder_pairs(const posting_piece& pc, std::uint32_t base) {
		rem_keys_.clear();
		const char* d = pc.deltas.data();
		const char* const de = d + pc.deltas.size();
		const char* w = pc.wdfs.data();
		const char* const we = w + pc.wdfs.size();
		while (d < de) {
			base += std::uint32_t(format::get_leb(&d, de));
			const std::uint32_t wdf = std::uint32_t(format::get_leb(&w, we));
			rem_keys_.push_back(std::uint64_t(format::encode_length(lengths_[base])) << 32 | std::uint32_t(~wdf));
		}
		if (rem_keys_.size() > 1) { std::sort(rem_keys_.begin(), rem_keys_.end()); }
		rem_pairs_.clear();
		std::uint64_t beat = 0;
		for (std::uint64_t k : rem_keys_) {
			const std::uint32_t wdf = ~std::uint32_t(k);
			if (std::uint64_t(wdf) + 1 > beat) {
				beat = std::uint64_t(wdf) + 1;
				rem_pairs_.push_back({wdf, std::uint8_t(k >> 32)});
			}
		}
		std::reverse(rem_pairs_.begin(), rem_pairs_.end());
	}

	// [count blocks bytes][count blocks][tail counts] when counts differ from wdf, then
	// [full position blocks][position blocks bytes][position blocks][tail positions]. Blocks
	// sit back to back; the skip data locates them.
	static positions_layout write_positions(std::string& out, const std::vector<std::uint32_t>& counts, const std::vector<std::uint32_t>& pos,
		bool counts_are_wdf) {
		positions_layout layout;
		std::uint8_t buf[bitpack::max_encoded_bytes];
		if (!counts_are_wdf) {
			const std::size_t full = counts.size() / format::block * format::block;
			std::string count_blocks;
			for (std::size_t b = 0; b < full; b += format::block) {
				layout.count_offsets.push_back(count_blocks.size());
				count_blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(counts.data() + b, buf));
			}
			format::put_leb(out, count_blocks.size());
			out += count_blocks;
			for (std::size_t i = full; i < counts.size(); ++i) { format::put_leb(out, counts[i]); }
		}
		const std::size_t full = pos.size() / format::block * format::block;
		std::string blocks;
		for (std::size_t b = 0; b < full; b += format::block) {
			layout.block_offsets.push_back(blocks.size());
			blocks.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(pos.data() + b, buf));
		}
		layout.blocks_bytes = blocks.size();
		format::put_leb(out, full / format::block);
		format::put_leb(out, blocks.size());
		out += blocks;
		for (std::size_t i = full; i < pos.size(); ++i) { format::put_leb(out, pos[i]); }
		return layout;
	}

	// Stored records: zstd frames back to back, each the payload [count][record lengths]
	// [records] (LEB128 counts and lengths), then one 16-byte index entry per frame (first
	// ordinal u32, uncompressed size u32, offset u64), then the frame count (u32).
	std::string encode_stored() const {
		std::string out, index, raw;
		std::uint32_t frames = 0;
		for (std::size_t first = 0; first < stored_ends_.size();) {
			if (cancel && cancel->requested()) { throw task::cancelled{}; }
			std::size_t last = first;
			const std::uint64_t begin = first ? stored_ends_[first - 1] : 0;
			while (last + 1 < stored_ends_.size() && stored_ends_[last + 1] - begin <= format::stored_block_size) { ++last; }
			raw.clear();
			format::put_leb(raw, last - first + 1);
			for (std::size_t i = first; i <= last; ++i) { format::put_leb(raw, stored_ends_[i] - (i ? stored_ends_[i - 1] : 0)); }
			raw.append(stored_, begin, stored_ends_[last] - begin);
			if (raw.size() > std::numeric_limits<std::uint32_t>::max()) { throw std::invalid_argument("strata: stored record too large"); }
			format::put<std::uint32_t>(index, std::uint32_t(first));
			format::put<std::uint32_t>(index, std::uint32_t(raw.size()));
			format::put<std::uint64_t>(index, out.size());
			out += compress_zstd(raw);
			++frames;
			first = last + 1;
		}
		out += index;
		format::put<std::uint32_t>(out, frames);
		return out;
	}

	// Values bit-packed in 128-value blocks at one width: [width u8][blocks].
	static std::string pack_u32(const std::vector<std::uint32_t>& values) {
		std::uint32_t all = 0;
		for (auto v : values) { all |= v; }
		const unsigned width = 32 - unsigned(std::countl_zero(all));
		std::string out(1, char(width));
		std::uint32_t chunk[format::block];
		std::uint8_t buf[bitpack::packed_bytes(32)];
		for (std::size_t b = 0; b < values.size(); b += format::block) {
			std::size_t n = std::min<std::size_t>(format::block, values.size() - b);
			std::fill(chunk, chunk + format::block, 0u);
			std::copy_n(values.begin() + std::ptrdiff_t(b), n, chunk);
			bitpack::pack(chunk, width, buf);
			out.append(reinterpret_cast<char*>(buf), bitpack::packed_bytes(width));
		}
		return out;
	}

	std::string encode_docids() const { return docids::encode(docids_, memory); }

	// versions: [min u64][width u8][(version - min) in width bits each, little-endian bit order].
	std::string encode_versions() const {
		std::string out;
		const std::uint64_t lo = versions_.empty() ? 0 : *std::min_element(versions_.begin(), versions_.end());
		const std::uint64_t hi = versions_.empty() ? 0 : *std::max_element(versions_.begin(), versions_.end());
		const unsigned width = unsigned(std::bit_width(hi - lo));
		format::put<std::uint64_t>(out, lo);
		out.push_back(char(width));
		std::string bits((versions_.size() * width + 7) / 8, '\0');
		for (std::size_t i = 0; i < versions_.size(); ++i) {
			const std::uint64_t v = versions_[i] - lo;
			for (unsigned b = 0; b < width; ++b) {
				if (v >> b & 1) {
					const std::size_t at = i * width + b;
					bits[at >> 3] = char(std::uint8_t(bits[at >> 3]) | (1u << (at & 7)));
				}
			}
		}
		return out + bits;
	}

	// pk_filter: [smallest key LEB length, bytes][largest key, likewise][fuse filter over
	// the keys' xxHash64].
	std::string encode_pk_filter() const {
		std::string out;
		format::put_leb(out, keys_min_.size());
		out += keys_min_;
		format::put_leb(out, keys_max_.size());
		out += keys_max_;
		return out + fusefilter::filter8::build(key_hashes_).serialize();
	}

	std::vector<format::impact> pairs_;  // write_postings: one block's pairs
	std::vector<piece_base> bases_;       // write_piece_*: the term's pieces
	std::vector<block_meta> piece_meta_;
	std::string piece_blocks_, piece_tail_;
	std::vector<format::impact> all_pairs_, rem_pairs_;
	std::vector<std::uint64_t> rem_keys_;
	std::deque<std::string> arena_;
	std::vector<slot> table_;
	std::deque<term_buffer> buffers_;
	std::vector<term_buffer*> touched_;  // add_tokens: the document's open postings
	std::vector<std::uint32_t> lengths_;
	std::string stored_;                     // stored records, concatenated
	std::vector<std::uint64_t> stored_ends_;  // where each document's record ends
	std::uint32_t max_positions_ = 0;        // the most positions of one term in one document
	std::vector<std::uint64_t> docids_;
	std::vector<std::uint64_t> versions_;
	std::vector<std::uint64_t> key_hashes_;
	std::string keys_min_, keys_max_;
	std::uint64_t total_length_ = 0;
	std::uint64_t columns_charged_ = 0;  // the per-document columns' charged capacity
};

}  // namespace strata
