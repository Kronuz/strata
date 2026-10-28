// Merging segments (Phase 3 design, "Merges"; layers.md L6): one segment holding every live
// document of its sources, in docid order or in an order the caller gives (a reordering
// merge, docs/design/reordering.md D3), with deleted documents and the terms only they
// held dropped. The core plans the output (its documents, their order, each input's
// ordinal map, and their docids, versions, and keys); each plugin then rebuilds its own
// sections from its readers, in layout order, so a merge writes exactly what a flush of
// the same documents would.
#pragma once

#include "strata/core/reader.hh"
#include "strata/core/writer.hh"
#include "task.hh"

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace strata {

template <class Layout>
struct basic_merge_source {
	const basic_segment<Layout>* seg = nullptr;
	const std::vector<std::uint64_t>* deleted = nullptr;  // bit per ordinal; null: none deleted
	bool live(ordinal_t o) const { return live_bits{deleted ? deleted->data() : nullptr}.live(o); }
};

// A document of a merge's sources: which source, and its ordinal there.
struct merge_ref {
	std::uint32_t source;
	ordinal_t ord;
};

// The merged segment's documents: how many, and each source ordinal's new ordinal.
struct merge_plan {
	std::uint64_t doc_count = 0;
	std::vector<std::vector<ordinal_t>> ordinals;  // [source][old ordinal] = new, or no_more_docs
};

// One input as a plugin's merge sees it (L6).
struct merge_input {
	const core_segment* core = nullptr;                   // docids and document count
	const std::vector<std::uint64_t>* deleted = nullptr;  // bit per ordinal; null: none deleted
	std::span<const ordinal_t> new_ordinal;               // [old ordinal] = new, or no_more_docs
	bool live(ordinal_t o) const { return live_bits{deleted ? deleted->data() : nullptr}.live(o); }
};

// A source's live documents' keys, by ordinal, from the keys reader's `scan` (F8).
using key_map = std::unordered_map<ordinal_t, std::string>;

namespace detail {
// Every key of a merge's inputs in key order, deleted copies included, as (key, input,
// ordinal): one keys cursor per input and a heap of their heads, equal keys by input. A key
// lives until the next call to next().
class key_merger {
public:
	explicit key_merger(std::span<const keys_plugin::reader* const> in) {
		for (std::uint32_t s = 0; s < in.size(); ++s) {
			cursors_.push_back(std::unique_ptr<keys::reader::cursor>(new keys::reader::cursor(in[s]->cursor())));  // elided: no move
			if (cursors_.back()->next()) { push(s); }
		}
	}
	bool next() {
		if (live_) {
			const std::uint32_t s = top_.source;
			if (cursors_[s]->next()) { push(s); }
		}
		if (heap_.empty()) {
			live_ = false;
			return false;
		}
		std::pop_heap(heap_.begin(), heap_.end(), later);
		top_ = heap_.back();
		heap_.pop_back();
		live_ = true;
		return true;
	}
	std::string_view key() const noexcept { return top_.key; }
	std::uint32_t source() const noexcept { return top_.source; }
	ordinal_t ordinal() const noexcept { return cursors_[top_.source]->ordinal(); }

private:
	struct head {
		std::string_view key;
		std::uint32_t source;
	};
	static bool later(const head& a, const head& b) noexcept { return a.key > b.key || (a.key == b.key && a.source > b.source); }
	void push(std::uint32_t s) {
		heap_.push_back({cursors_[s]->key(), s});
		std::push_heap(heap_.begin(), heap_.end(), later);
	}
	std::vector<std::unique_ptr<keys::reader::cursor>> cursors_;  // a cursor neither copies nor moves
	std::vector<head> heap_;
	head top_{};
	bool live_ = false;
};
}  // namespace detail

// What the core hands every plugin's merge (L6): the output's documents in output order.
struct merge_context {
	std::uint8_t region_shift = format::default_region_shift;
	std::uint32_t doc_count = 0;              // documents in the output
	std::span<const merge_input> inputs;
	std::span<const merge_ref> order;         // [new ordinal] = its source
	std::span<const std::uint64_t> docids;    // [new ordinal] = its docid
	std::span<const key_map> keys;            // [input] each live document's key; empty when key-ordered
	bool docid_order = true;                  // false when the output is reordered (P1)
	// The output is in key order, the k-way merge of the inputs' keys (format-14-kv D4):
	// keys stream from the inputs' cursors into the dictionary, no map, and `key_alphabet`
	// holds every byte the live keys use.
	bool key_ordered = false;
	std::string_view key_alphabet;
	std::size_t stored_block = format::max_stored_block;  // the output's stored blocks (D1c, D1d)
	format::stored_codec stored_codec = format::zstd_codec(3);
	const task::cancel_token* cancel = nullptr;
	flush_charge* memory = nullptr;           // the merge's working memory, when budgeted
	scratch* records = nullptr;               // out of docid order: where stored records spill
	std::string_view key_prefix;              // the index's, for the inverted builder's backstop (F8)
	format::structure_bits structures = ~format::structure_bits(0);  // the output's configured bits (F4)
	void check() const {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
	}
	void charge(std::uint64_t n) const {
		if (memory) { memory->grow(n); }
	}
};

// Rebuilt per new ordinal: its docid, its input's version, and its key. In key order the keys
// come from the inputs' cursors, merged again, each live one the next keyed output's, and
// stream into the dictionary; otherwise from the merge's key maps.
inline built keys_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	keys_builder b;
	b.memory = m.memory;
	if (m.key_ordered) {
		b.stream_sorted(m.key_alphabet);
		detail::key_merger keys(in);
		std::size_t i = 0;
		for (; keys.next();) {
			if (!m.inputs[keys.source()].live(keys.ordinal())) { continue; }
			m.check();
			if (i == m.order.size() || m.order[i].source != keys.source() || m.order[i].ord != keys.ordinal()) {
				throw std::logic_error("strata: merge: the key order and the inputs' keys disagree");
			}
			b.add({m.docids[i], in[keys.source()]->version(keys.ordinal()), keys.key()}, false);
			++i;
		}
		for (; i < m.order.size(); ++i) { b.add({m.docids[i], in[m.order[i].source]->version(m.order[i].ord), {}}, false); }  // keyless, last
	} else {
		for (std::size_t i = 0; i < m.order.size(); ++i) {
			const merge_ref& src = m.order[i];
			doc_fields f{m.docids[i], in[src.source]->version(src.ord), {}};
			if (auto k = m.keys[src.source].find(src.ord); k != m.keys[src.source].end()) { f.key = k->second; }
			b.add(f, true);  // a view into the merge's key map
		}
	}
	b.finish(sink);
	return {format::bit::keys};
}

namespace detail {
// Rebuilt record by record in output order. In docid order each input's frames are read
// in turn through one cached frame per input; out of it, with `records`, every input's
// live records are first spilled there, each frame decoded once (reordering review,
// P1-R4 (b)).
template <class Reader>
inline void merge_frames(const merge_context& m, std::span<const Reader* const> in, section_sink& sink, format::section kind) {
	std::vector<std::vector<std::uint64_t>> spilled;  // [input][ordinal] .. [ordinal + 1]: its bytes
	std::string_view spilled_bytes;
	if (m.records && !m.docid_order) {
		spilled.resize(in.size());
		std::uint64_t at = 0;
		detail::frame_reader::frame frame;
		for (std::size_t s = 0; s < in.size(); ++s) {
			const Reader& r = *in[s];
			const std::uint64_t docs = m.inputs[s].core->doc_count();
			spilled[s].assign(docs + 1, at);  // this input's records start at `at`
			m.charge(spilled[s].capacity() * 8);
			const std::uint32_t frames = r.frames();
			ordinal_t next = 0;
			for (std::uint32_t f = 0; f < frames; ++f) {
				m.check();
				r.decode(f, frame);
				for (ordinal_t o = frame.first; o < frame.first + frame.count; ++o) {
					for (; next < o; ++next) { spilled[s][next + 1] = at; }
					if (m.inputs[s].live(o)) {
						const auto rec = frame.record(o);
						m.records->out().write(rec.data(), rec.size());
						at += rec.size();
					}
					spilled[s][o + 1] = at;
					next = o + 1;
				}
			}
			for (; next < docs; ++next) { spilled[s][next + 1] = at; }
		}
		spilled_bytes = m.records->bytes();
		if (spilled_bytes.size() != at) { throw std::runtime_error("strata: merge: the stored records read back short"); }
	}
	std::vector<detail::frame_reader::frame> frames(spilled.empty() ? in.size() : 0);
	frame_builder b;
	b.configure({m.memory, m.cancel, m.key_prefix, m.structures, m.stored_block, m.stored_codec});
	for (const auto& src : m.order) {
		m.check();
		const std::string_view rec = spilled.empty() ? in[src.source]->record(src.ord, frames[src.source])
			: spilled_bytes.substr(spilled[src.source][src.ord], spilled[src.source][src.ord + 1] - spilled[src.source][src.ord]);
		if (rec.size() > format::max_stored_record) { throw std::invalid_argument("strata: stored record too large"); }
		b.add(rec);
	}
	b.finish(sink, kind);
}
}  // namespace detail

inline built stored_plugin::merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
	detail::merge_frames(m, in, sink, format::section::stored);
	return {format::bit::stored};
}

// A key-ordered merge's output order, and the bytes its live keys use.
struct key_merge_order {
	std::vector<merge_ref> order;
	std::string alphabet;
};

// Where a reordering merge's order comes from (L6, reordering review P1-R4 (d)): a layout
// names at most one source, which the core calls before any plugin's merge. None by default.
template <class Layout>
struct order_source {
	static constexpr bool exists = false;
	struct settings {};  // the index options' share of it
};

// The document layout's: every merge's output in key order (format-14-kv D4), the k-way merge
// of its sources' keys, one heap step a record and no sort; then the keyless documents by
// docid. Each key-ordered source's ordinals ascend in it, so its stored blocks are read in
// order. Its live keys' bytes come back as the output dictionary's alphabet.
template <>
struct order_source<document_layout> {
	static constexpr bool exists = true;
	static constexpr bool by_key = true;  // every merge, not only reordering ones
	struct settings {};
	static key_merge_order order(std::span<const basic_merge_source<document_layout>> sources, const task::cancel_token* cancel, flush_charge* memory) {
		key_merge_order out;
		std::vector<const keys_plugin::reader*> in;
		std::uint64_t live = 0;
		for (const auto& src : sources) {
			in.push_back(&src.seg->template get<keys_plugin>());
			live += src.seg->core().doc_count();
		}
		out.order.reserve(live);
		if (memory) { memory->grow(out.order.capacity() * sizeof(merge_ref)); }
		std::array<bool, 256> used{};
		std::vector<std::vector<bool>> keyed(sources.size());
		for (std::size_t s = 0; s < sources.size(); ++s) { keyed[s].assign(sources[s].seg->core().doc_count(), false); }
		detail::key_merger keys(in);
		for (std::uint64_t n = 0; keys.next(); ++n) {
			if (cancel && (n & 4095) == 0 && cancel->requested()) { throw task::cancelled{}; }
			keyed[keys.source()][keys.ordinal()] = true;
			if (!sources[keys.source()].live(keys.ordinal())) { continue; }
			for (char c : keys.key()) { used[std::uint8_t(c)] = true; }
			out.order.push_back({keys.source(), keys.ordinal()});
		}
		struct keyless {
			std::uint64_t docid;
			merge_ref ref;
		};
		std::vector<keyless> rest;
		for (std::uint32_t s = 0; s < sources.size(); ++s) {
			for (ordinal_t o = 0; o < sources[s].seg->core().doc_count(); ++o) {
				if (!keyed[s][o] && sources[s].live(o)) { rest.push_back({sources[s].seg->core().docid(o), {s, o}}); }
			}
		}
		if (memory) { memory->grow(rest.capacity() * sizeof(keyless)); }
		std::sort(rest.begin(), rest.end(), [](const keyless& a, const keyless& b) { return a.docid < b.docid; });
		for (const auto& k : rest) { out.order.push_back(k.ref); }
		out.alphabet = keys::alphabet_of(used);
		return out;
	}
};

// How a merge runs (L6, F13-R11): designated initializers only, in declaration order, so a
// removed field fails to compile; positional initialization still compiles (review rule).
struct merge_options {
	std::uint8_t region_shift = format::default_region_shift;
	const task::cancel_token* cancel = nullptr;  // checked per document, per term, and per stored frame
	flush_charge* memory = nullptr;              // charged with the merge's working memory
	std::span<const merge_ref> order;            // [new ordinal] = the source document; empty: docid order, but
	                                             // a single source out of docid order keeps its order
	scratch* records = nullptr;                  // out of docid order: where each source's live records spill
	std::string_view key_prefix;                 // the index's, only for the inverted builder's backstop (F8)
	format::structure_bits structures = ~format::structure_bits(0);  // the index's configured bits (F4); default: every one of the layout's
	std::optional<format::stored_codec> stored_codec;  // the output's stored codec; none: the layout's (format-14-kv Q2's preset)
};

namespace detail {

// The core of a merge (L6) over a layout: the plan, then each layout entry in order.
template <class... Entries>
merge_plan merge_layout(std::span<const basic_merge_source<layout<Entries...>>> sources, const std::array<std::uint8_t, 16>& segment_id, output& out,
	spill& positions_spill, const merge_options& options) {
	using segment_type = basic_segment<layout<Entries...>>;
	using policy = segment_policy<layout<Entries...>>;
	const std::uint8_t region_shift = options.region_shift;
	const task::cancel_token* const cancel = options.cancel;
	flush_charge* const memory = options.memory;
	constexpr bool keyed = (std::is_same_v<Entries, keys_plugin> || ...);
	// A key-ordered layout's merge with keys and index_sort configured orders its output by
	// key itself (format-14-kv D4): an order given is refused.
	bool key_ordered = false;
	key_merge_order by_key;
	if constexpr (requires { order_source<layout<Entries...>>::by_key; }) {
		static_assert(policy::key_order && keyed);
		const format::structure_bits need = format::bit::keys | format::bit::index_sort;
		if ((options.structures & need) == need) {
			if (!options.order.empty()) { throw std::invalid_argument("strata: a key-ordered merge takes no order"); }
			by_key = order_source<layout<Entries...>>::order(sources, cancel, memory);
			key_ordered = true;
		}
	}
	const std::span<const merge_ref> order = key_ordered ? std::span<const merge_ref>(by_key.order) : options.order;
	auto check = [&] {
		if (cancel && cancel->requested()) { throw task::cancelled{}; }
	};
	auto charge = [&](std::uint64_t n) {
		if (memory) { memory->grow(n); }
	};
	check();
	merge_plan plan;
	plan.ordinals.resize(sources.size());
	// Each live document's key, if any, walked through the keys reader's checks (L3, MR2); a
	// key-ordered merge streams them instead.
	std::vector<key_map> keys(key_ordered ? 0 : sources.size());
	if constexpr (keyed) {
		for (std::size_t s = 0; s < keys.size(); ++s) {
			const segment_type& seg = *sources[s].seg;
			seg.template get<keys_plugin>().each_key([&](std::string_view key, ordinal_t o) {
				if (sources[s].live(o)) {
					keys[s].emplace(o, key);
					charge(64 + key.size());  // a map node and its key
				}
			});
		}
	}
	// Documents in docid order.
	struct doc {
		std::uint64_t docid;
		merge_ref ref;
	};
	std::vector<doc> docs;
	for (std::size_t s = 0; s < sources.size(); ++s) {
		const segment_type& seg = *sources[s].seg;
		plan.ordinals[s].assign(seg.core().doc_count(), no_more_docs);
		for (ordinal_t o = 0; o < seg.core().doc_count(); ++o) {
			if (sources[s].live(o)) { docs.push_back({seg.core().docid(o), {std::uint32_t(s), o}}); }
		}
		charge(plan.ordinals[s].capacity() * sizeof(ordinal_t));
	}
	charge(docs.capacity() * sizeof(doc));
	// A single permuted source keeps its order (docs are still in its ordinal order here).
	const bool keep_source_order = order.empty() && sources.size() == 1 && !sources[0].seg->core().docid_ordered();
	std::vector<doc> by_docid;
	if (keep_source_order) { by_docid = docs, charge(by_docid.capacity() * sizeof(doc)); }
	auto& sorted = keep_source_order ? by_docid : docs;
	std::sort(sorted.begin(), sorted.end(), [](const doc& a, const doc& b) { return a.docid < b.docid; });
	for (std::size_t i = 1; i < sorted.size(); ++i) {
		if (sorted[i - 1].docid == sorted[i].docid) { throw std::logic_error("strata: merge: two live copies of a docid"); }
	}
	if (!order.empty()) {
		if (order.size() != docs.size()) { throw std::invalid_argument("strata: merge order must name every live document once"); }
		docs.clear();
		for (const auto& r : order) {
			if (r.source >= sources.size() || r.ord >= sources[r.source].seg->core().doc_count() || !sources[r.source].live(r.ord) ||
				plan.ordinals[r.source][r.ord] != no_more_docs) {
				throw std::invalid_argument("strata: merge order must name every live document once");
			}
			plan.ordinals[r.source][r.ord] = 0;  // seen; numbered below
			docs.push_back({sources[r.source].seg->core().docid(r.ord), r});
		}
	}
	// Ordinals are 32-bit and the largest value means no more documents.
	if (docs.size() > std::size_t(no_more_docs) - 1) { throw std::length_error("strata: segment document limit"); }
	const bool docid_order = order.empty() && !keep_source_order;
	// The output in output order: its sources and docids (with `docs`, main's 32 bytes a document).
	std::vector<merge_ref> refs;
	std::vector<std::uint64_t> docids;
	refs.reserve(docs.size()), docids.reserve(docs.size());
	charge(refs.capacity() * sizeof(merge_ref) + docids.capacity() * sizeof(std::uint64_t));
	for (std::size_t i = 0; i < docs.size(); ++i) {
		check();
		const auto& d = docs[i];
		if (d.docid == 0) { throw std::invalid_argument("strata: docids must increase within a segment"); }
		plan.ordinals[d.ref.source][d.ref.ord] = ordinal_t(i);
		refs.push_back(d.ref);
		docids.push_back(d.docid);
	}
	plan.doc_count = docs.size();
	std::vector<doc>().swap(docs);
	std::vector<doc>().swap(by_docid);

	std::vector<merge_input> inputs(sources.size());
	for (std::size_t s = 0; s < sources.size(); ++s) { inputs[s] = {&sources[s].seg->core(), sources[s].deleted, plan.ordinals[s]}; }
	merge_context m;
	m.region_shift = region_shift;
	m.doc_count = std::uint32_t(refs.size());
	m.inputs = inputs;
	m.order = refs;
	m.docids = docids;
	m.keys = keys;
	m.docid_order = docid_order;
	m.key_ordered = key_ordered;
	m.key_alphabet = by_key.alphabet;
	m.stored_block = policy::stored_block;
	m.stored_codec = options.stored_codec.value_or(policy::stored_codec);
	m.cancel = cancel;
	m.memory = memory;
	m.records = options.records;
	m.key_prefix = options.key_prefix;
	m.structures = options.structures;

	section_sink sink(out, region_shift, &positions_spill, memory);
	begin_segment(sink, segment_id);
	format::structure_bits structures = 0;
	auto merge_entry = [&]<class E>(std::type_identity<E>) {
		{
			if (!(options.structures & E::structure)) { return; }  // not configured: none of its sections (F4)
			if constexpr (opt_in_of<E>() != 0) {  // an opt-in plugin merges what every source holds
				for (const auto& src : sources) {
					if (!(src.seg->core().structures() & E::structure)) { return; }
				}
			}
			std::vector<const typename E::reader*> readers;
			readers.reserve(sources.size());
			for (const auto& src : sources) { readers.push_back(&src.seg->template get<E>()); }
			built b = E::merge(m, readers, sink);
			structures |= b.structures;
		}
	};
	(merge_entry(std::type_identity<Entries>{}), ...);
	if (key_ordered) {
		sink.write(format::section::index_sort, format::encode_key_sort());  // pinned: the tail
		structures |= format::bit::index_sort;
	}
	write_docids(sink, memory, docids);  // the core's, in the tail by rank (F5); after the plugins, off their peak (CR8)
	finish_segment(sink, docids, structures);
	return plan;
}

}  // namespace detail

}  // namespace strata
