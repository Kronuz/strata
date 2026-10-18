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
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace strata {

using docset::no_more_docs;

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
class section_stream {
public:
	explicit section_stream(counting_output& out, std::uint8_t region_shift = format::default_region_shift)
		: out_(out), offset_(out.size()), shift_(region_shift), size_(std::size_t(1) << region_shift) {}
	void write(std::string_view bytes) {
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
		length_ += bytes.size();
	}
	std::uint64_t size() const noexcept { return length_; }
	format::section_entry finish(format::section kind) {
		if (region_fill_) { regions_.push_back(region_.value()); }
		return {kind, offset_, length_, crc_.value(), std::move(regions_), shift_};
	}

private:
	counting_output& out_;
	std::uint64_t offset_, length_ = 0;
	std::uint8_t shift_;
	std::size_t size_;
	hashes::Crc32c crc_, region_;
	std::size_t region_fill_ = 0;
	std::vector<std::uint32_t> regions_;
};

// One term's postings as a flush or a merge produces them: ordinals ascending, a wdf and a
// position count per ordinal, and every position as deltas within its document.
struct term_postings {
	std::vector<std::uint32_t> ords, wdfs, counts, positions;
	std::uint64_t cf = 0;
	bool all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
	void clear() {
		ords.clear(), wdfs.clear(), counts.clear(), positions.clear();
		cf = 0;
		all_zero_wdf = true, any_positions = false, counts_are_wdf = true;
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
			tp.clear();
			append_postings(buffers_[buffer], 0, tp);
			return key;
		});
	}

	// One segment from writers holding contiguous docid runs in increasing order (a parallel
	// flush's runs, D18): the bytes one writer given every document in order writes. The
	// terms are encoded on `threads` threads in 256 groups by hash, then placed in order.
	static std::string flush_runs(std::span<const segment_writer> runs, const std::array<std::uint8_t, 16>& segment_id, std::size_t threads,
		flush_charge* memory = nullptr) {
		segment_writer w;
		w.memory = memory;
		w.region_shift = runs.empty() ? format::default_region_shift : runs[0].region_shift;
		w.wide_skip_offsets = !runs.empty() && runs[0].wide_skip_offsets;
		std::vector<ordinal_t> base;
		for (const auto& r : runs) {
			base.push_back(ordinal_t(w.doc_count()));
			w.append_documents(r);
		}
		struct ref {
			std::string_view key;
			std::uint32_t run, buffer;
		};
		// Groups by key range, split at a sample of the runs' terms: each group's terms follow
		// the previous group's, so groups are placed as they finish.
		std::vector<std::string_view> sample;
		for (const auto& r : runs) {
			std::size_t i = 0;
			for (const auto& slot : r.table_) {
				if (slot.key && (i++ & 15) == 0) { sample.push_back({slot.key, slot.length}); }
			}
		}
		std::sort(sample.begin(), sample.end());
		sample.erase(std::unique(sample.begin(), sample.end()), sample.end());
		const std::size_t groups = std::min<std::size_t>(256, sample.size() + 1);
		std::vector<std::string_view> splitters;
		for (std::size_t g = 1; g < groups; ++g) { splitters.push_back(sample[sample.size() * g / groups]); }
		std::vector<std::vector<std::vector<ref>>> by_run(runs.size(), std::vector<std::vector<ref>>(groups));
		std::vector<std::array<bool, 256>> used(runs.size());
		parallel(runs.size(), threads, [&](std::size_t k) {
			for (const auto& slot : runs[k].table_) {
				if (!slot.key) { continue; }
				const std::string_view key(slot.key, slot.length);
				const auto g = std::size_t(std::upper_bound(splitters.begin(), splitters.end(), key) - splitters.begin());
				by_run[k][g].push_back({key, std::uint32_t(k), slot.buffer});
				for (char c : key) { used[k][std::uint8_t(c)] = true; }
			}
		});
		std::string alphabet;
		for (unsigned c = 0; c < 256; ++c) {
			if (std::any_of(used.begin(), used.end(), [&](const auto& u) { return u[c]; })) { alphabet.push_back(char(c)); }
		}
		std::string file;
		string_output out(file, memory);
		string_spill positions(memory);
		w.flush_grouped(segment_id, out, positions, alphabet, groups, threads, [&](std::size_t g, auto&& emit) {
			std::vector<ref> refs;
			for (auto& r : by_run) {
				refs.insert(refs.end(), r[g].begin(), r[g].end());
				std::vector<ref>().swap(r[g]);
			}
			std::sort(refs.begin(), refs.end(), [](const ref& a, const ref& b) { return a.key != b.key ? a.key < b.key : a.run < b.run; });
			term_postings tp;
			for (std::size_t i = 0, j; i < refs.size(); i = j) {
				tp.clear();
				for (j = i; j < refs.size() && refs[j].key == refs[i].key; ++j) { runs[refs[j].run].append_postings(runs[refs[j].run].buffers_[refs[j].buffer], base[refs[j].run], tp); }
				emit(refs[i].key, tp);
			}
		});
		return file;
	}

	// Writes the segment with terms from next(tp), which fills tp and returns the term, in
	// strictly increasing order, or nothing at the end. Documents come from add() or
	// add_document(); a term's ordinals name them. `alphabet` holds every byte the terms
	// use (the dictionary codes its keys over it; empty: all 256).
	template <class Next>
	void flush_from(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill, std::string_view alphabet, Next&& next) {
		write_segment(segment_id, sink, positions_spill, alphabet, [&](auto&& place) {
			term_postings tp;
			encoded_term e;
			std::string last_key;
			bool first_term = true;
			std::uint64_t scratch = 0;  // the per-term buffers' charged capacity
			while (const auto got = next(tp)) {
				const std::string_view key = *got;
				if (!first_term && key <= std::string_view(last_key)) { throw std::invalid_argument("strata: terms must increase"); }
				first_term = false;
				last_key.assign(key);
				encode_term(tp, e);
				place(key, e.info, e.postings, e.positions, e.max_count);
				if (memory) {
					const std::uint64_t now = (tp.ords.capacity() + tp.wdfs.capacity() + tp.counts.capacity() + tp.positions.capacity()) * sizeof(std::uint32_t) +
						e.all.capacity() * sizeof(format::impact) + e.postings.capacity() + e.positions.capacity();
					if (now > scratch) {
						memory->grow(now - scratch);
						scratch = now;
					}
				}
			}
		});
	}

	// The same segment with its terms encoded on `threads` threads: source(g, emit) calls
	// emit(key, tp) for group g's terms (g < groups) in increasing order, each group's above
	// the previous group's, and this thread places each group's terms once it is encoded.
	template <class Source>
	void flush_grouped(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill, std::string_view alphabet,
		std::size_t groups, std::size_t threads, Source&& source) {
		struct entry {
			std::string_view key;
			format::term_info info;
			std::size_t postings, postings_length, positions, positions_length;
			std::uint32_t max_count;
		};
		struct encoded_group {
			std::vector<entry> terms;
			std::string postings, positions;
			bool done = false;
			std::exception_ptr error;
		};
		std::vector<encoded_group> encoded(groups);
		std::mutex mu;
		std::condition_variable cv;
		std::atomic<std::size_t> claimed{0};
		std::atomic<bool> stop{false};
		auto work = [&] {
			encoded_term e;
			for (std::size_t g; !stop.load(std::memory_order_relaxed) && (g = claimed.fetch_add(1, std::memory_order_relaxed)) < groups;) {
				encoded_group& out = encoded[g];
				std::exception_ptr error;
				try {
					source(g, [&](std::string_view key, const term_postings& tp) {
						encode_term(tp, e);
						out.terms.push_back({key, std::move(e.info), out.postings.size(), e.postings.size(), out.positions.size(), e.positions.size(), e.max_count});
						out.postings += e.postings;
						out.positions += e.positions;
					});
				} catch (...) {
					error = std::current_exception();
					stop = true;
				}
				{
					std::lock_guard l(mu);
					out.error = error;
					out.done = true;
				}
				cv.notify_all();
			}
		};
		struct workers {
			std::vector<std::thread> pool;
			std::atomic<bool>& stop;
			~workers() {
				stop = true;
				for (auto& t : pool) { t.join(); }
			}
		} running{{}, stop};
		for (std::size_t k = 0; k < std::max<std::size_t>(1, std::min(threads, groups)); ++k) { running.pool.emplace_back(work); }
		write_segment(segment_id, sink, positions_spill, alphabet, [&](auto&& place) {
			std::string_view last;
			bool first = true;
			for (std::size_t g = 0; g < groups; ++g) {
				encoded_group& grp = encoded[g];
				{
					std::unique_lock l(mu);
					cv.wait(l, [&] { return grp.done; });
				}
				if (grp.error) { std::rethrow_exception(grp.error); }
				const std::uint64_t held = grp.terms.capacity() * sizeof(entry) + grp.postings.capacity() + grp.positions.capacity();
				if (memory) { memory->grow(held); }
				for (entry& t : grp.terms) {
					if (!first && t.key <= last) { throw std::invalid_argument("strata: terms must increase"); }
					first = false;
					last = t.key;
					place(t.key, std::move(t.info), std::string_view(grp.postings).substr(t.postings, t.postings_length),
						std::string_view(grp.positions).substr(t.positions, t.positions_length), t.max_count);
				}
				grp = {};
				if (memory) { memory->shrink(held); }
			}
		});
	}

	// Runs f(0) to f(n - 1) on up to `threads` threads, this one among them; rethrows the first
	// exception once all have ended.
	template <class F>
	static void parallel(std::size_t n, std::size_t threads, F&& f) {
		std::atomic<std::size_t> next{0};
		std::vector<std::exception_ptr> errors(std::max<std::size_t>(1, std::min(threads, n)));
		auto work = [&](std::size_t k) {
			try {
				for (std::size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) { f(i); }
			} catch (...) { errors[k] = std::current_exception(); }
		};
		std::vector<std::thread> pool;
		for (std::size_t k = 1; k < errors.size(); ++k) { pool.emplace_back(work, k); }
		work(0);
		for (auto& t : pool) { t.join(); }
		for (auto& e : errors) {
			if (e) { std::rethrow_exception(e); }
		}
	}

	// The segment's sections around its terms: terms(place) calls place(key, info, postings,
	// positions, max_count) for every term in increasing order.
	template <class Terms>
	void write_segment(const std::array<std::uint8_t, 16>& segment_id, output& sink, spill& positions_spill, std::string_view alphabet, Terms&& terms) {
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
		terms([&](std::string_view key, format::term_info info, std::string_view postings_bytes, std::string_view positions_bytes, std::uint32_t max_count) {
			max_positions_ = std::max(max_positions_, max_count);
			if (!(info.flags & format::no_positions)) {
				info.positions = positions.size();
				positions.write(positions_bytes);
			}
			if (!(info.flags & format::single_posting)) {
				info.postings = postings.size();
				postings.write(postings_bytes);
			}
			dict.add(key, format::encode_term_info(info, run, dict.next_is_restart()));
		});
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

	// One term's encoded postings and positions: its term info lacks only the offsets of its
	// bytes in the postings and positions sections, which placement sets.
	struct encoded_term {
		format::term_info info;
		std::string postings, positions;
		std::uint32_t max_count = 0;      // the most positions of the term in one document
		std::vector<format::impact> all;    // scratch: a long list's (wdf, length) pairs
		std::vector<format::impact> pairs;  // scratch: one block's pairs
	};

	// Encodes a term's postings; reads only the per-document lengths, so groups of terms
	// encode on several threads at once.
	void encode_term(const term_postings& tp, encoded_term& e) const {
		const auto& ords = tp.ords;
		const auto& wdfs = tp.wdfs;
		const auto& counts = tp.counts;
		const std::uint32_t df = std::uint32_t(ords.size());
		if (df == 0 || wdfs.size() != df || counts.size() != df) { throw std::invalid_argument("strata: malformed term postings"); }
		e.max_count = 0;
		for (auto c : counts) { e.max_count = std::max(e.max_count, c); }
		format::term_info info;
		info.df = df;
		info.cf = tp.cf;
		if (tp.all_zero_wdf) { info.flags |= format::no_wdf; }
		if (!tp.any_positions) { info.flags |= format::no_positions; }
		if (tp.counts_are_wdf) { info.flags |= format::counts_are_wdf; }
		if (df >= format::block) {  // the term info stores whole-list pairs only with skip blocks
			e.all.clear();
			for (std::size_t i = 0; i < ords.size(); ++i) { e.all.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			info.impacts = format::competitive(e.all);
		}
		positions_layout layout;
		e.positions.clear();
		if (tp.any_positions) {
			layout = write_positions(e.positions, counts, tp.positions, tp.counts_are_wdf);
			info.positions_length = e.positions.size();
		}
		e.postings.clear();
		if (df == 1) {
			info.flags |= format::single_posting;
			info.postings = ords[0];
			info.single_wdf = wdfs[0];
		} else {
			write_postings(e.postings, ords, wdfs, !tp.all_zero_wdf, counts, tp.any_positions ? &layout : nullptr, !tp.counts_are_wdf, e.pairs);
			info.postings_length = e.postings.size();
		}
		e.info = std::move(info);
	}

	// Appends a buffered term's postings to tp, its ordinals offset by `base`.
	static void append_postings(const term_buffer& t, ordinal_t base, term_postings& tp) {
		const std::size_t at = tp.ords.size();
		decode_append(t.ords, tp.ords, t.df);
		std::uint32_t ord = 0;
		for (std::size_t i = at; i < tp.ords.size(); ++i) {
			ord += tp.ords[i];
			tp.ords[i] = ord - 1 + base;  // stored as ordinal + 1 deltas
		}
		decode_append(t.wdfs, tp.wdfs, t.df);
		if (t.counts_are_wdf) {
			tp.counts.insert(tp.counts.end(), tp.wdfs.begin() + std::ptrdiff_t(at), tp.wdfs.end());
		} else {
			decode_append(t.counts, tp.counts, t.df);
		}
		if (t.any_positions) {
			std::size_t total = 0;
			for (std::size_t i = at; i < tp.counts.size(); ++i) { total += tp.counts[i]; }
			decode_append(t.positions, tp.positions, total);
		}
		tp.cf += t.cf;
		tp.all_zero_wdf = tp.all_zero_wdf && t.all_zero_wdf;
		tp.any_positions = tp.any_positions || t.any_positions;
		tp.counts_are_wdf = tp.counts_are_wdf && t.counts_are_wdf;
	}

	// Appends another writer's per-document columns, as if its documents were added here
	// after this writer's; its terms stay with it (flush_runs reads them).
	void append_documents(const segment_writer& o) {
		if (lengths_.size() + o.lengths_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (!permuted && !docids_.empty() && !o.docids_.empty() && o.docids_.front() <= docids_.back()) {
			throw std::invalid_argument("strata: docids must increase within a segment");
		}
		const std::uint64_t stored_base = stored_.size();
		lengths_.insert(lengths_.end(), o.lengths_.begin(), o.lengths_.end());
		stored_ += o.stored_;
		for (const auto end : o.stored_ends_) { stored_ends_.push_back(stored_base + end); }
		docids_.insert(docids_.end(), o.docids_.begin(), o.docids_.end());
		versions_.insert(versions_.end(), o.versions_.begin(), o.versions_.end());
		key_hashes_.insert(key_hashes_.end(), o.key_hashes_.begin(), o.key_hashes_.end());
		if (!o.key_hashes_.empty()) {
			if (keys_min_.empty() && keys_max_.empty()) {
				keys_min_ = o.keys_min_;
				keys_max_ = o.keys_max_;
			} else {
				if (o.keys_min_ < keys_min_) { keys_min_ = o.keys_min_; }
				if (o.keys_max_ > keys_max_) { keys_max_ = o.keys_max_; }
			}
		}
		total_length_ += o.total_length_;
		if (memory) {
			const std::uint64_t now = lengths_.capacity() * sizeof(std::uint32_t) +
				(stored_ends_.capacity() + docids_.capacity() + versions_.capacity() + key_hashes_.capacity()) * sizeof(std::uint64_t) + stored_.capacity();
			if (now > columns_charged_) {
				memory->grow(now - columns_charged_);
				columns_charged_ = now;
			}
		}
	}

	static void decode_append(const std::string& stream, std::vector<std::uint32_t>& out, std::size_t n) {
		const std::size_t at = out.size();
		out.resize(at + n);
		const char* p = stream.data();
		const char* end = p + stream.size();
		for (std::size_t i = at; i < out.size(); ++i) { out[i] = std::uint32_t(format::get_leb(&p, end)); }
	}

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

	struct block_meta {
		ordinal_t last;
		std::uint64_t bytes;
		std::uint64_t first_position;  // index into the term's position stream
		std::vector<format::impact> impacts;
		std::uint64_t pos_offset = 0;    // byte offset of the position block holding first_position
		std::uint64_t count_offset = 0;  // byte offset of the block's count block
		bool bitset = false;             // ordinals as a bitset instead of FOR deltas
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
		const std::vector<std::uint32_t>& counts, const positions_layout* layout, bool with_counts, std::vector<format::impact>& block_pairs) const {
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
			block_pairs.clear();
			for (std::size_t i = b; i < b + format::block; ++i) { block_pairs.push_back({wdfs[i], format::encode_length(lengths_[ords[i]])}); }
			meta.push_back({ords[b + format::block - 1], blocks.size() - start, position, format::competitive(block_pairs)});
			meta.back().bitset = bitset;
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
		// Level 0, grouped by 32 blocks. Level 1 is a fixed-width array, one entry per group,
		// so a reader locates groups without parsing: last ordinal (u32), the offset of its
		// competitive pairs in the pairs area (u32), its level-0 and blocks offsets, and for
		// terms with positions its first position index and position and count block
		// offsets, each u32, or u64 when the term flags itself wide. Level-0 positional fields (present only with positions, counts only
		// when they differ from wdf) are deltas restarting per group.
		const bool with_positions = layout != nullptr;
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
				format::put_leb(group, meta[i].bytes << 1 | std::uint64_t(meta[i].bitset));
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
			format::put_leb(out, layout->offset_of(position));
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
