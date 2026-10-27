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

// The in-memory segment over a layout: the core's per-document fields and docids column, each
// plugin's builder (layers.md L5), and one sequential pass that writes the file. The keys and
// stored builders live here; the inverted builder is prospect's
// (prospect/writer.hh).

#pragma once

#include "compressor_lz4.h"
#include "compressor_zstd.h"
#include "fusefilter.hh"
#include "hashes.hh"
#include <crc32c/crc32c.hh>
#include "strata/core/borrow.hh"
#include "strata/core/budget.hh"
#include "strata/core/docids.hh"
#include "strata/core/format.hh"
#include "strata/core/keys.hh"
#include "strata/core/reader.hh"
#include "strata/core/section_io.hh"
#include "task.hh"
#include "termdict.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace strata {

// Per-document fields the core hands every builder (layers.md L5).
struct doc_fields {
	std::uint64_t docid;
	std::uint64_t version;  // 0 for a keyless document
	std::string_view key;   // empty for a keyless document
};

namespace detail {
// Appends a section built only to be appended, charged while it lives.
inline void append_built(section_sink& sink, flush_charge* memory, format::section kind, const std::string& bytes) {
	if (memory) { memory->grow(bytes.capacity()); }
	sink.write(kind, bytes);
	if (memory) { memory->shrink(bytes.capacity()); }
}
// The core's sections that frame a segment (F5): the header first; docids and stats (the
// document count, F7) held for the tail; then the table of contents and the footer, which
// records the structures built (F4).
inline void begin_segment(section_sink& sink, const std::array<std::uint8_t, 16>& segment_id) {
	sink.write(format::section::header, format::encode_header(segment_id));
}
inline void write_docids(section_sink& sink, flush_charge* memory, std::span<const std::uint64_t> docids) {
	append_built(sink, memory, format::section::docids, docids::encode(docids, memory));
}
inline void finish_segment(section_sink& sink, std::span<const std::uint64_t> docids, format::structure_bits structures) {
	std::string stats;
	format::put<std::uint64_t>(stats, docids.size());
	sink.write(format::section::stats, stats);
	sink.finish();
	counting_output& o = sink.out();
	o.align(format::section_alignment);
	format::footer f;
	f.toc_offset = o.size();
	std::string toc_bytes = format::encode_toc(sink.toc());
	f.toc_length = toc_bytes.size();
	f.toc_crc = ::kronuz::crc32c::crc32c(toc_bytes.data(), toc_bytes.size());
	f.doc_count = std::uint32_t(docids.size());
	f.min_docid = docids.empty() ? 0 : *std::min_element(docids.begin(), docids.end());
	f.max_docid = docids.empty() ? 0 : *std::max_element(docids.begin(), docids.end());
	f.structures = structures;
	o.write(toc_bytes);
	o.write(format::encode_footer(f));
}
// Charges a column's growth to `now` bytes, when a budget is given.
inline void charge_to(flush_charge* memory, std::uint64_t now, std::uint64_t& charged) {
	if (memory && now > charged) {
		memory->grow(now - charged);
		charged = now;
	}
}
// Bytes copied into chunks of at least `chunk` bytes that never move, so views into them
// stay valid; each chunk is charged as it is allocated.
class stable_bytes {
public:
	explicit stable_bytes(std::size_t chunk) noexcept : chunk_(chunk) {}
	std::string_view copy(std::string_view s, flush_charge* memory) {
		if (chunks_.empty() || chunks_.back().size() + s.size() > chunks_.back().capacity()) {
			chunks_.emplace_back();
			chunks_.back().reserve(std::max(chunk_, s.size()));
			if (memory) { memory->grow(chunks_.back().capacity()); }
		}
		std::string& c = chunks_.back();
		const char* at = c.data() + c.size();
		c.append(s);
		return {at, s.size()};
	}

private:
	std::deque<std::string> chunks_;
	std::size_t chunk_;
};
}  // namespace detail

// What the writer tells each builder before it works: the settings every builder reads as they
// are when a call starts.
struct builder_settings {
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;
	std::string_view key_prefix;
	format::structure_bits structures = 0;
	// The stored blocks' target of uncompressed record bytes and their codec (format-14-kv
	// D1c, D1d); the layout's segment_policy by default.
	std::size_t stored_block = format::max_stored_block;
	format::stored_codec stored_codec = format::zstd_codec(3);
};

// Keys and versions: the keys plugin's builder (F8, F9). keys streams with the plugins'
// sections; key_index and versions go to the tail.
class keys_builder {
public:
	flush_charge* memory = nullptr;

	void configure(const builder_settings& s) noexcept { memory = s.memory; }

	// Keys from here on arrive in strictly increasing order and stream into the dictionary as
	// they come, none held (a key-ordered merge, format-14-kv D4); `alphabet` holds every byte
	// they use.
	// The dictionary's blocks go straight to `sink`'s keys section, which finish() closes.
	void stream_sorted(std::string_view alphabet, section_sink& sink) {
		if (!versions_.empty()) { throw std::logic_error("strata: stream_sorted after the first document"); }
		section_stream& blocks = sink.begin();
		sorted_ = std::make_unique<keys::sorted_encoder>(alphabet, memory, [&blocks](std::string_view b) { blocks.write(b); });
	}

	// Room for `docs` documents' versions and, when streaming, hashes: a merge knows its output's size.
	void reserve_docs(std::size_t docs) {
		versions_.reserve(docs);
		if (sorted_) { sorted_->reserve(docs); }
	}

	// A borrowed key is held as a view, so its bytes must outlive finish() (the flush's
	// buffer); any other is copied once into the builder's own chunks. A streamed key is
	// held by neither.
	void add(const doc_fields& f, bool borrowed) {
		// A version belongs to a key (F9, F13-R9).
		if (f.key.empty() && f.version != 0) { throw std::invalid_argument("strata: a version without a key"); }
		if (!f.key.empty()) {
			if (f.key.size() > format::max_term_bytes) { throw std::invalid_argument("strata: key longer than the term limit"); }
			if (sorted_) {
				sorted_->add(f.key, ordinal_t(versions_.size()));
			} else {
				const std::string_view k = borrowed ? f.key : owned_.copy(f.key, memory);
				keys_.push_back({k.data(), std::uint32_t(k.size()), ordinal_t(versions_.size())});
			}
		}
		versions_.push_back(f.version);
		detail::charge_to(memory, versions_.capacity() * sizeof(std::uint64_t) + keys_.capacity() * sizeof(keys::entry), charged_);
	}
	// The entries are sorted in place, so a second flush writes the same bytes.
	void finish(section_sink& sink) {
		keys::sections sections;
		if (sorted_) {
			sections = sorted_->finish();
		} else {
			section_stream& blocks = sink.begin();
			sections = keys::encode(keys_, memory, [&blocks](std::string_view b) { blocks.write(b); });
		}
		sink.end(format::section::keys);
		const std::uint64_t built = sections.index.capacity();
		if (memory) { memory->grow(built); }
		detail::append_built(sink, memory, format::section::versions, encode_versions());
		sink.write(format::section::key_index, sections.index);
		if (memory) { memory->shrink(built); }
	}

private:
	// versions: [min u64][width u8][(version - min) in width bits each, little-endian bit order].
	std::string encode_versions() const {
		std::string out;
		std::uint64_t lo = versions_.empty() ? 0 : *std::min_element(versions_.begin(), versions_.end());
		const std::uint64_t hi = versions_.empty() ? 0 : *std::max_element(versions_.begin(), versions_.end());
		unsigned width = unsigned(std::bit_width(hi - lo));
		// The reader requires lo + 2^width - 1 to fit in 64 bits; when it would not, the base is
		// 0 and the width 64, since every version then exceeds 2^63 (F13-R25 item 7).
		if (width && lo > ~std::uint64_t(0) - (width >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << width) - 1)) {
			lo = 0;
			width = 64;
		}
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

	std::vector<std::uint64_t> versions_;
	std::vector<keys::entry> keys_;
	detail::stable_bytes owned_{std::size_t(64) << 10};  // the keys not borrowed
	std::unique_ptr<keys::sorted_encoder> sorted_;     // set by stream_sorted
	std::uint64_t charged_ = 0;
};

// Stored records: the stored plugin's builder.
class stored_builder {
public:
	flush_charge* memory = nullptr;
	const task::cancel_token* cancel = nullptr;  // checked per frame

	void configure(const builder_settings& s) {
		if (s.stored_block == 0 || s.stored_block > format::max_stored_block) { throw std::invalid_argument("strata: stored block size"); }
		if (s.stored_codec.kind == format::codec::zstd ? (s.stored_codec.level < 1 || s.stored_codec.level > 22) : s.stored_codec.level != 0) {
			throw std::invalid_argument("strata: stored codec");
		}
		memory = s.memory;
		cancel = s.cancel;
		block_ = s.stored_block;
		codec_ = s.stored_codec;
	}
	// Sizes the builder for `docs` records of `bytes` in all, so adding them allocates no more.
	void reserve(std::size_t docs, std::size_t bytes) {
		stored_ends_.reserve(docs);
		if (lease_) {
			views_.reserve(docs);
		} else {
			stored_.reserve(bytes);
		}
	}
	// Keeps each record as a view into the leased container, not a copy: the records of parts
	// that lie in the lease's range, which the lease holds unchanged for as long as the builder
	// lives (core/borrow.hh). Taken before the first record.
	void borrow(borrow_gate::lease& lease) {
		if (!stored_ends_.empty()) { throw std::logic_error("strata: stored records borrowed after copying began"); }
		lease_ = std::move(lease);
	}
	void add(ordinal_t, const doc_fields&, const stored_plugin::part& p) {
		if (!lease_) {
			copy(p.stored);
			return;
		}
		if (!lease_.owns(&p)) { throw std::logic_error("strata: a stored record outside the container it borrows from"); }
		views_.push_back(p.stored);
		total_ += p.stored.size();
		stored_ends_.push_back(total_);
		charge();
	}

	// Copies a record; a builder that borrows takes its records from parts alone.
	void add(std::string_view stored) {
		if (lease_) { throw std::logic_error("strata: a copied stored record in a builder that borrows"); }
		if (sink_) {
			gather(stored);
			return;
		}
		copy(stored);
	}
	// Streams the section from here on: each block of records goes to `sink` as it fills, so the
	// builder holds one block, not the section. For records added as copies, before the first.
	void stream_to(section_sink& sink) {
		if (!stored_ends_.empty() || lease_) { throw std::logic_error("strata: stored streaming after the first record"); }
		sink_ = &sink;
		section_ = &sink.begin();
	}
	void finish(section_sink& sink) {
		if (sink_) {
			if (&sink != sink_) { throw std::logic_error("strata: stored streamed to another sink"); }
			close_block();
			finish_index();
			sink.end(format::section::stored);
			return;
		}
		section_stream& st = sink.begin();
		encode_stored(st);
		sink.end(format::section::stored);
	}

private:
	// Stored records (format-14-kv D1d): frames back to back, each [codec u8][payload], the
	// payload decoding to [count][record lengths][records] (LEB128 counts and lengths) of at
	// most the block target's record bytes and as many records, or one record past it; then one 16-byte index
	// entry per frame (first ordinal u32, uncompressed size u32, offset u64), then the frame
	// count (u32). A codec that saves less than an eighth stores the payload raw.
	void encode_stored(section_stream& st) {
		section_ = &st;
		index_.clear(), offset_ = 0, frames_ = 0;  // a writer may flush more than once (keys.hh:187)
		std::string raw;
		for (std::size_t first = 0; first < stored_ends_.size();) {
			if (cancel && cancel->requested()) { throw task::cancelled{}; }
			std::size_t last = first;
			const std::uint64_t begin = first ? stored_ends_[first - 1] : 0;
			// A block closes at the target's record bytes or at as many records, so records of zero
			// bytes cannot make one unbounded: a frame decodes to at most about twice the target.
			while (last + 1 < stored_ends_.size() && last + 1 - first < block_ && stored_ends_[last + 1] - begin <= block_) { ++last; }
			raw.clear();
			format::put_leb(raw, last - first + 1);
			for (std::size_t i = first; i <= last; ++i) { format::put_leb(raw, stored_ends_[i] - (i ? stored_ends_[i - 1] : 0)); }
			if (lease_) {
				for (std::size_t i = first; i <= last; ++i) { raw.append(views_[i]); }
			} else {
				raw.append(stored_, begin, stored_ends_[last] - begin);
			}
			put_frame(first, raw);
			first = last + 1;
		}
		finish_index();
	}
	// Adds a record to the block being gathered, closing the block first when it would not fit.
	void gather(std::string_view rec) {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
		if (pending_count_ && (pending_count_ >= block_ || pending_.size() + rec.size() > block_)) { close_block(); }
		pending_lens_.push_back(rec.size());
		pending_.append(rec);
		++pending_count_;
	}
	void close_block() {
		if (!pending_count_) { return; }
		std::string& raw = block_raw_;
		raw.clear();
		format::put_leb(raw, pending_count_);
		for (const std::uint64_t n : pending_lens_) { format::put_leb(raw, n); }
		raw.append(pending_);
		put_frame(first_ordinal_, raw);
		first_ordinal_ += pending_count_;
		pending_count_ = 0;
		pending_lens_.clear();
		pending_.clear();
		detail::charge_to(memory, pending_.capacity() + block_raw_.capacity() + packed_.capacity() + pending_lens_.capacity() * sizeof(std::uint64_t), charged_);
	}
	// One frame to the section and its entry to the index.
	void put_frame(std::size_t first, std::string_view raw) {
		if (raw.size() > format::max_stored_frame) { throw std::invalid_argument("strata: stored record too large"); }
		format::put<std::uint32_t>(index_, std::uint32_t(first));
		format::put<std::uint32_t>(index_, std::uint32_t(raw.size()));
		format::put<std::uint64_t>(index_, offset_);
		offset_ += append_frame(*section_, raw);
		++frames_;
	}
	void finish_index() {
		section_->write(index_);
		std::string count;
		format::put<std::uint32_t>(count, frames_);
		section_->write(count);
	}
	// Writes a block's frame; returns its bytes.
	std::size_t append_frame(section_stream& out, std::string_view raw) {
		const bool lz4 = codec_.kind == format::codec::lz4;
		std::size_t n = 0;
		if (codec_.kind != format::codec::raw) {
			packed_.resize(lz4 ? lz4_block_bound(raw.size()) : zstd_frame_bound(raw.size()));
			n = lz4 ? compress_lz4_block(raw, packed_.data(), packed_.size()) : compress_zstd_into(raw, packed_.data(), packed_.size(), codec_.level);
			if (n == 0) { throw std::runtime_error("strata: stored block compression failed"); }
		}
		if (n == 0 || n > raw.size() - raw.size() / 8) {
			const char tag = char(format::codec::raw);
			out.write(std::string_view(&tag, 1));
			out.write(raw);
			return 1 + raw.size();
		}
		const char tag = char(codec_.kind);
		out.write(std::string_view(&tag, 1));
		out.write(std::string_view(packed_.data(), n));
		return 1 + n;
	}

	void copy(std::string_view stored) {
		stored_ += stored;
		stored_ends_.push_back(stored_.size());
		charge();
	}
	void charge() {
		detail::charge_to(memory, stored_ends_.capacity() * sizeof(std::uint64_t) + views_.capacity() * sizeof(std::string_view) + stored_.capacity(), charged_);
	}

	borrow_gate::lease lease_;                // set: the records are views, not copies
	std::vector<std::string_view> views_;     // borrowed records, one per document
	std::uint64_t total_ = 0;                 // their bytes so far
	std::string stored_;                      // stored records, concatenated
	std::vector<std::uint64_t> stored_ends_;  // where each document's record ends
	std::uint64_t charged_ = 0;
	std::size_t block_ = format::max_stored_block;
	format::stored_codec codec_ = format::zstd_codec(3);
	std::string packed_;  // a block's compressed bytes, reused
	// Streaming (stream_to): the block being gathered, and the frames written so far.
	section_sink* sink_ = nullptr;
	section_stream* section_ = nullptr;
	std::string pending_, block_raw_, index_;
	std::vector<std::uint64_t> pending_lens_;
	std::uint64_t pending_count_ = 0, first_ordinal_ = 0, offset_ = 0;
	std::uint32_t frames_ = 0;
};

template <class Layout>
class basic_segment_writer;

// The in-memory segment of a layout: per-document fields and docids in the core, everything
// else in the plugins' builders, which the layout's entries name (`P::builder`).
template <class... Entries>
class basic_segment_writer<layout<Entries...>> {
	using policy = segment_policy<layout<Entries...>>;
	static constexpr bool keyed = (std::is_same_v<Entries, keys_plugin> || ...);
	// The bits this writer may build: its entries', and index_sort's key order for a
	// key-ordered layout (format-14-kv D1a).
	static constexpr format::structure_bits buildable =
		(format::structure_bits(0) | ... | Entries::structures) | (policy::key_order && keyed ? format::bit::index_sort : 0);

public:
	// Documents arrive in output order with distinct docids in any order (a reordering
	// merge, reordering.md D2); flushes leave it off, so docids increase. A key-ordered
	// segment (index_sort set) takes docids in any order and checks key order instead.
	bool permuted = false;
	// The verification region size of every section, as a power of two.
	std::uint8_t region_shift = format::default_region_shift;
	// The run's draw on the indexing budget, if any: the builders' buffers and the flush's
	// scratch and output report their allocations.
	flush_charge* memory = nullptr;
	// When given, the stored section checks it per frame and throws task::cancelled.
	const task::cancel_token* cancel = nullptr;
	// The index's key prefix, for the inverted builder's backstop (F8); empty for none.
	std::string key_prefix;
	// The structures the segment holds, set from the index's configuration, never from its
	// documents (F4). With index_sort the documents arrive in key order: keyed ones by
	// strictly increasing key, then keyless ones by increasing docid.
	format::structure_bits structures = buildable;
	// The stored blocks' target and codec (format-14-kv D1c, D1d).
	std::size_t stored_block = policy::stored_block;
	format::stored_codec stored_codec = policy::stored_codec;

	// Plugin P's builder.
	template <class P>
	typename P::builder& builder() noexcept {
		return std::get<typename P::builder>(builders_);
	}
	std::size_t doc_count() const noexcept { return docids_.size(); }

	// The core's step for one document: the limits, docid order, the docids column, and the
	// key and version (F8, F9); returns its ordinal. A borrowed key's bytes outlive the flush
	// (keys_builder::add). The plugins' parts follow, through add() or the builders.
	ordinal_t begin(const doc_fields& f, bool borrowed) {
		sync();
		// Ordinals are 32-bit and the largest value means no more documents.
		if (docids_.size() >= std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
		if (structures & format::bit::index_sort) {
			check_key_order(f);
		} else if (f.docid == 0 || (!permuted && !docids_.empty() && f.docid <= docids_.back())) {
			throw std::invalid_argument("strata: docids must increase within a segment");
		}
		// Only configured builders collect: a keyless segment holds no versions it never writes
		// (CR8). A version belongs to a key (F9).
		if constexpr (keyed) {
			if (structures & keys_plugin::structure) {
				builder<keys_plugin>().add(f, borrowed);
			} else {
				refuse_keyless(f);
			}
		} else {
			refuse_keyless(f);
		}
		docids_.push_back(f.docid);
		detail::charge_to(memory, docids_.capacity() * sizeof(std::uint64_t), charged_);
		return ordinal_t(docids_.size() - 1);
	}

	// A document: begin(), then each plugin's part of `doc`, in layout order. A part for a
	// structure the segment does not build is dropped, or refused where the plugin says so.
	template <class Doc>
	ordinal_t add(const doc_fields& f, const Doc& doc, bool borrowed) {
		(refuse_part<Entries>(doc), ...);
		const ordinal_t ord = begin(f, borrowed);
		(add_part<Entries>(ord, f, doc), ...);
		return ord;
	}

	// Sizes the writer for a segment of `docs` documents whose stored records hold
	// `stored_bytes`, so adding them allocates no more.
	void reserve(std::size_t docs, std::size_t stored_bytes) {
		docids_.reserve(docs);
		(reserve_one<Entries>(docs, stored_bytes), ...);
	}
	// Takes the stored records as views into the container `lease` pins, not copies; they stay
	// valid because the builder keeps the lease. Call before the first add(); a layout with no
	// stored records leaves the lease with the caller.
	void borrow_stored(borrow_gate::lease& lease) { (borrow_one<Entries>(lease), ...); }

	// Writes the segment file and returns its bytes.
	std::string flush(const std::array<std::uint8_t, 16>& segment_id) {
		std::string file;
		string_output out(file, memory);
		string_spill positions(memory);
		flush(segment_id, out, positions);
		return file;
	}

	// Streams the segment file to out in one pass. A section produced alongside another (the
	// positions beside the postings) goes to `held` until the other ends.
	void flush(const std::array<std::uint8_t, 16>& segment_id, output& out, spill& held) {
		write(segment_id, out, held, [](auto& b, section_sink& s) { b.finish(s); });
	}

	// The file in F5's order: the header; each configured builder's sections, in layout order
	// (`finish(builder, sink)` writes them; the streamed ones in rank order); the docids and the
	// pinned tail by rank; then the table of contents and footer, with the structures (F4).
	template <class Finish>
	void write(const std::array<std::uint8_t, 16>& segment_id, output& out, spill& held, Finish&& finish) {
		sync();
		if ((structures & ~buildable) || !(consistent<Entries>() && ...)) { throw std::invalid_argument("strata: segment structures"); }
		section_sink sink(out, region_shift, &held, memory);
		detail::begin_segment(sink, segment_id);
		(finish_entry<Entries>(sink, finish), ...);
		if (structures & format::bit::index_sort) { sink.write(format::section::index_sort, format::encode_key_sort()); }  // pinned: the tail
		detail::write_docids(sink, memory, docids_);
		detail::finish_segment(sink, docids_, structures);
	}

protected:
	// Builders read the writer's settings as they are when each call starts.
	void sync() {
		if ((structures & format::bit::index_sort) && !(structures & format::bit::keys)) { throw std::invalid_argument("strata: index_sort's key order without keys"); }
		const builder_settings s{memory, cancel, key_prefix, structures, stored_block, stored_codec};
		(builder<Entries>().configure(s), ...);
	}

private:
	// Key order (format-14-kv D1a): keys strictly increase, keyless documents follow every
	// keyed one, and their docids increase.
	void check_key_order(const doc_fields& f) {
		if (f.docid == 0) { throw std::invalid_argument("strata: docids must increase within a segment"); }
		if (!f.key.empty()) {
			if (keyless_ || (!docids_.empty() && f.key <= std::string_view(last_key_))) { throw std::invalid_argument("strata: documents out of key order"); }
			last_key_.assign(f.key);
		} else {
			if (keyless_ && f.docid <= docids_.back()) { throw std::invalid_argument("strata: keyless documents out of docid order"); }
			keyless_ = true;
		}
	}

	static void refuse_keyless(const doc_fields& f) {
		if (!f.key.empty()) { throw std::invalid_argument("strata: a key in a segment without keys"); }
		if (f.version != 0) { throw std::invalid_argument("strata: a version without a key"); }
	}
	template <class E, class Doc>
	void refuse_part(const Doc& doc) const {
		if constexpr (requires { typename E::part; E::refuse(static_cast<const typename E::part&>(doc), true); }) {
			E::refuse(static_cast<const typename E::part&>(doc), (structures & E::structure) != 0);
		}
	}
	template <class E, class Doc>
	void add_part(ordinal_t ord, const doc_fields& f, const Doc& doc) {
		if constexpr (requires { typename E::part; }) {
			if (structures & E::structure) { builder<E>().add(ord, f, static_cast<const typename E::part&>(doc)); }
		}
	}
	template <class E>
	bool consistent() const {
		if constexpr (requires { E::consistent(structures); }) {
			return E::consistent(structures);
		} else {
			return true;
		}
	}
	template <class E>
	void reserve_one(std::size_t docs, std::size_t bytes) {
		if constexpr (requires(typename E::builder& b) { b.reserve(docs, bytes); }) {
			if (structures & E::structure) { builder<E>().reserve(docs, bytes); }
		}
	}
	template <class E>
	void borrow_one(borrow_gate::lease& lease) {
		if constexpr (requires(typename E::builder& b) { b.borrow(lease); }) {
			if (structures & E::structure) { builder<E>().borrow(lease); }
		}
	}
	template <class E, class Finish>
	void finish_entry(section_sink& sink, Finish& finish) {
		if (structures & E::structure) { finish(builder<E>(), sink); }
	}

	std::tuple<typename Entries::builder...> builders_;
	std::vector<std::uint64_t> docids_;
	std::uint64_t charged_ = 0;  // the docids column's charged capacity
	std::string last_key_;       // with index_sort: the last key taken
	bool keyless_ = false;       // with index_sort: a keyless document was taken
};

}  // namespace strata
