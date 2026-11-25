// basic_index's write buffer: the lookups of its copies, its memory accounting, and whether a transaction fits
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
std::size_t basic_index<Layout>::buffered_count() const noexcept {
	lease_.check_read();
	return buffer_size();
}

template <class Layout>
std::string_view basic_index<Layout>::stored_bytes(const document& d) noexcept {
	if constexpr (std::is_base_of_v<stored_part, document>) {
		return static_cast<const stored_part&>(d).stored;
	} else {
		return {};
	}
}

template <class Layout>
template <class E>
constexpr bool basic_index<Layout>::inline_kind() noexcept {
	if constexpr (!requires { typename E::part; }) {
		return true;
	} else {
		constexpr bool opts_in = requires(const typename E::part& p) { { E::inline_part(p) } -> std::convertible_to<bool>; };
		static_assert(!opts_in || basic_segment_writer<Layout>::template views_stored<E>(), "strata: a plugin with inline_part derives from stored_plugin and keeps its builder");
		return opts_in || (std::is_same_v<typename E::part, stored_part> && basic_segment_writer<Layout>::template views_stored<E>());
	}
}

template <class Layout>
constexpr bool basic_index<Layout>::inline_layout() noexcept {
	return []<class... E>(std::type_identity<layout<E...>>) { return (inline_kind<E>() && ...); }(std::type_identity<Layout>{});
}

template <class Layout>
bool basic_index<Layout>::inline_parts(const document& d) noexcept {
	if constexpr (!inline_layout()) {
		return false;
	} else {
		bool ok = true;
		for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
			if constexpr (requires(const typename E::part& p) { E::inline_part(p); }) { ok = ok && E::inline_part(static_cast<const typename E::part&>(d)); }
		});
		return ok;
	}
}

template <class Layout>
constexpr bool basic_index<Layout>::has_buffer_hooks() noexcept {
	bool any = false;
	for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
		if constexpr (requires { typename E::part; }) {
			if constexpr (requires(typename E::part& p) { E::buffered(p); } || requires(typename E::part& p, const field_resolution& r) { E::resolve(p, r); }) { any = true; }
		}
	});
	return any;
}

template <class Layout>
std::uint64_t basic_index<Layout>::map_bytes() const {
	std::uint64_t n = 0;
	for_each_buffer([&](const buffer& b) { const wbuf::run_set* rs = b.runs.load(std::memory_order_relaxed);
		n += b.keys.bytes() + b.docids.bytes() + b.order.bytes() + (rs ? rs->bytes() : 0); });
	return n;
}

template <class Layout>
std::uint64_t basic_index<Layout>::buffered_bytes() const noexcept {
	std::uint64_t n = 0;
	for_each_buffer([&](const buffer& b) { n += (inverting() ? (b.inv ? b.inv->held.held() : 0) : b.bytes) + b.lane.bytes(); });
	return n;
}

template <class Layout>
void basic_index<Layout>::charge() {
	if (!permit_) { return; }
	const std::uint64_t want = buffered_bytes() + map_bytes();
	if (want > permit_.bytes()) {
		permit_.grow(want - permit_.bytes());
	} else {
		permit_.shrink(permit_.bytes() - want);
	}
}

template <class Layout>
std::uint64_t basic_index<Layout>::buffer_limit() const noexcept {
	if (!opts_.budget) { return opts_.buffer_share; }
	return std::min(opts_.buffer_share, opts_.budget->capacity() / (100 + flush_working_pct) * 100);
}

template <class Layout>
bool basic_index<Layout>::needs_flush() const {
	if (buffered_records() == 0 && !holds_rejected()) { return false; }
	if (buffered_bytes() >= buffer_limit() || map_bytes() >= opts_.buffer_share / 4) { return true; }
	if (!opts_.budget || !opts_.budget->pressure()) { return false; }
	const std::uint64_t div = opts_.pressure_flush_divisor;
	const std::uint64_t share = std::min(opts_.buffer_share, opts_.budget->capacity());
	return !div || buffered_bytes() + map_bytes() >= share / div;
}

template <class Layout>
template <class F>
void basic_index<Layout>::for_each_buffer(F&& f) const {
	for (const auto& b : frozen_) { f(*b); }
	if (wb_) { f(*wb_); }
}

template <class Layout>
wbuf::ref_t basic_index<Layout>::find_key_in(const buffer& b, std::string_view key) {
	return b.keys.find(index_detail::key_hash(key), [&](wbuf::ref_t r) { return std::string_view(b.lane.at(r).key()) == key; });
}

template <class Layout>
wbuf::ref_t basic_index<Layout>::probe_docid(const buffer& b, std::uint64_t docid) {
	return b.docids.find(index_detail::docid_hash(docid), [&](wbuf::ref_t r) { return b.lane.at(r).docid == docid; });
}

template <class Layout>
wbuf::ref_t basic_index<Layout>::find_docid_in(const buffer& b, std::uint64_t docid) const {
	build_docids(b);
	return probe_docid(b, docid);
}

template <class Layout>
void basic_index<Layout>::build_docids(const buffer& b) const {
	if (b.docids.built()) { return; }
	b.docids.reserve(b.order.size());
	for (std::size_t i = 0; i < b.order.size(); ++i) { store_docid(b, b.order[i].ref); }
}

template <class Layout>
wbuf::version_map::place basic_index<Layout>::store_docid(const buffer& b, wbuf::ref_t r) const {
	const std::uint64_t d = b.lane.at(r).docid;
	const std::uint32_t h = index_detail::docid_hash(d);
	const auto p = b.docids.locate(h, [&](wbuf::ref_t q) { return b.lane.at(q).docid == d; });
	b.docids.store(p, h, r);
	return p;
}

template <class Layout>
bool basic_index<Layout>::newest(const buffer& b, wbuf::ref_t r) const {
	const record& x = b.lane.at(r);
	if (x.tombstone()) { return false; }
	return x.key().empty() ? find_docid_in(b, x.docid) == r : find_key_in(b, x.key()) == r;
}

template <class Layout>
auto basic_index<Layout>::buffered_copy(const buffer& b, typename copy::where_t w, std::size_t at, wbuf::ref_t r) const -> copy {
	const record& x = b.lane.at(r);
	if (x.tombstone()) { return {}; }
	return {w, at, r, 0, x.docid, x.version};
}

template <class Layout>
auto basic_index<Layout>::locate_docid(std::uint64_t docid, bool build) const -> copy {
	const auto find = [&](const buffer& b) {
		if (build || b.docids.built()) { return find_docid_in(b, docid); }
		for (std::size_t i = b.order.size(); i-- > 0;) {
			if (b.lane.at(b.order[i].ref).docid == docid) { return b.order[i].ref; }
		}
		return wbuf::no_ref;
	};
	if (wb_) {
		if (const auto r = find(*wb_); r != wbuf::no_ref) { return buffered_copy(*wb_, copy::active, 0, r); }
	}
	for (std::size_t i = frozen_.size(); i-- > 0;) {
		if (const auto r = find(*frozen_[i]); r != wbuf::no_ref) { return buffered_copy(*frozen_[i], copy::frozen, i, r); }
	}
	const pending_set* pending = pending_index();
	for (std::size_t i = segs_.size(); i-- > 0;) {
		const auto& s = *segs_[i];
		const segment& sg = s.seg();  // a map point under the lease (cold-tier D9)
		if (auto ord = sg.core().ordinal_of(docid); ord && !((s.deleted[*ord >> 6] >> (*ord & 63)) & 1)) {
			if (pending_has(pending, s.ref.id, *ord)) { return {}; }
			return {copy::segment, i, wbuf::no_ref, *ord, docid, sg.template get<keys_plugin>().version(*ord)};
		}
	}
	return {};
}

template <class Layout>
auto basic_index<Layout>::lookup_key(std::string_view key) const -> copy {
	if (wb_) {
		if (const auto r = find_key_in(*wb_, key); r != wbuf::no_ref) { return buffered_copy(*wb_, copy::active, 0, r); }
	}
	for (std::size_t i = frozen_.size(); i-- > 0;) {
		if (const auto r = find_key_in(*frozen_[i], key); r != wbuf::no_ref) { return buffered_copy(*frozen_[i], copy::frozen, i, r); }
	}
	const pending_set* pending = pending_index();
	const std::uint64_t hash = xxh64::hash(key);  // one hash, one filter probe per segment (CR7)
	for (std::size_t i = segs_.size(); i-- > 0;) {
		const auto& s = *segs_[i];
		const segment& sg = s.seg();  // a map point under the lease (cold-tier D9)
		const auto& keys = sg.template get<keys_plugin>();
		if (const auto ord = keys.find(key, live_bits{s.deleted.data()}, hash)) {
			if (pending_has(pending, s.ref.id, *ord)) { return {}; }
			return {copy::segment, i, wbuf::no_ref, *ord, sg.core().docid(*ord), keys.version(*ord)};
		}
	}
	return {};
}

template <class Layout>
std::string_view basic_index<Layout>::key_of_copy(const copy& c) const {
	if (c.where == copy::active) { return wb_->lane.at(c.ref).key(); }
	if (c.where == copy::frozen) { return frozen_[c.at]->lane.at(c.ref).key(); }
	return {};
}

template <class Layout>
std::size_t basic_index<Layout>::buffer_size() const noexcept {
	std::size_t n = 0;
	for_each_buffer([&](const buffer& b) { n += b.docs; });
	return n;
}

template <class Layout>
std::size_t basic_index<Layout>::buffered_records() const noexcept {
	std::size_t n = 0;
	for_each_buffer([&](const buffer& b) { n += b.order.size(); });
	return n;
}

template <class Layout>
std::string_view basic_index<Layout>::buffered_stored(const buffer& b, const record& x) const
	requires(segment::template contains<stored_plugin>())
{
	if (!inverting()) { return x.stored(); }
	const std::size_t n = b.inv ? b.inv->buf.doc_count() : 0;
	return x.ordinal() < n ? b.inv->buf.stored(x.ordinal()) : std::string_view(static_cast<const typename stored_plugin::part&>(*pending_docs_[x.ordinal() - n].doc).stored);
}

template <class Layout>
void basic_index<Layout>::invert_pending() {
	if (pending_docs_.empty()) { return; }
	try {
		auto& inv = wb_->inv;
		if (!inv) {
			inv = std::make_unique<active_buffer>();
			inv->buf.max_run_bytes = max_run_bytes_;
		}
		auto& a = *inv;
		for (const pending_doc& p : pending_docs_) {
			// An inverter that reads field references takes the transaction's resolution.
			const field_resolution res{pending_declared_, pending_known_, pending_key_field_};
			ordinal_t ord;
			if constexpr (requires { a.buf.add(p.docid, p.version, p.key, *p.doc, res); }) {
				ord = a.buf.add(p.docid, p.version, p.key, *p.doc, res);
			} else {
				ord = a.buf.add(p.docid, p.version, p.key, *p.doc);
			}
			if (ord && p.docid <= a.prev_docid) { a.ordered = false; }
			a.prev_docid = p.docid;
		}
	} catch (...) {
		pending_docs_.clear();
		fenced_ = true;
		throw;
	}
	pending_docs_.clear();
}

template <class Layout>
bool basic_index<Layout>::lane_fits(const transaction& t) const noexcept {
	std::size_t widest = record::whole_stride(record::widest_flags);
	for (const auto& o : t.ops) {
		const std::size_t k = std::max(o.key.size(), o.doc.key.size()), s = stored_bytes(o.doc).size();
		const std::size_t n = record::inline_stride(record::widest_flags, k, s);
		// record::emplace holds a record past a chunk whole; one near the edge may be either.
		widest = std::max(widest, n <= wbuf::min_chunk ? n : record::inline_stride(0, k, s) <= wbuf::min_chunk ? wbuf::min_chunk : 0);
	}
	const std::size_t records = 2 * t.ops.size();
	return wb_ ? wb_->lane.room_for(records, widest) : wbuf::lane<record>(opts_.lane_chunks_cap).room_for(records, widest);
}

template <class Layout>
bool basic_index<Layout>::fits(const transaction& t) const noexcept {
	if (!lane_fits(t)) { return false; }  // a full chunk directory freezes (D3)
	if (!wb_ || !wb_->inv) { return true; }
	return wb_->inv->buf.fits(t);
}

template <class Layout>
void basic_index<Layout>::require_buffers_unpinned() const {
	for_each_buffer([](const buffer& b) { b.lane.require_unpinned(); });
}

template <class Layout>
void basic_index<Layout>::clear_buffer() {
	require_buffers_unpinned();
	wb_.reset();
	frozen_.clear();
	pending_docs_.clear();
}

}  // namespace strata
