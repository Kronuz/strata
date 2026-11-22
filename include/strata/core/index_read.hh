// basic_index's reads: lookups and scans, reads at a position, the segment reads they share, and the digest and check
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
auto basic_index<Layout>::segments() const -> segment_list {
	lease_.check_read();
	segment_list out;
	out.views_.reserve(segs_.size());
	out.bits_.reserve(segs_.size());
	for (const auto& s : segs_) { out.views_.push_back({s->ref.id, s.get(), s->dead ? &s->deleted : nullptr, s->dead}); }
	if (!wb_ || wb_->pending.empty()) { return out; }
	std::vector<deleted_bits*> own(segs_.size(), nullptr);
	for (const auto& [id, ord] : wb_->pending) {
		const auto it = std::lower_bound(segs_.begin(), segs_.end(), id, [](const auto& x, std::uint64_t y) { return x->ref.id < y; });
		const std::size_t i = std::size_t(it - segs_.begin());
		if (!own[i]) {
			own[i] = &out.bits_.emplace_back((*it)->deleted.clone());
			out.views_[i].deleted = own[i];
		}
		if (!own[i]->test(ord)) {
			own[i]->set(ord);
			++out.views_[i].dead;
		}
	}
	return out;
}

template <class Layout>
std::uint64_t basic_index<Layout>::digest() const {
	const segment_list views = segments();  // the pending deletes applied, as the freeze applies them (D6)
	std::vector<const buffer*> all;  // oldest first
	for_each_buffer([&](const buffer& b) { all.push_back(&b); });
	// A buffer's record counts when it is its buffer's newest and no newer buffer supersedes it.
	const auto live = [&](std::size_t j, wbuf::ref_t r) {
		if (!newest(*all[j], r)) { return false; }
		const record& x = all[j]->lane.at(r);
		for (std::size_t k = j + 1; k < all.size(); ++k) {
			if (!x.key().empty() && find_key_in(*all[k], x.key()) != wbuf::no_ref) { return false; }
			if (all[k]->docids.built() && find_docid_in(*all[k], x.docid) != wbuf::no_ref) { return false; }
		}
		return true;
	};
	return digest_of(views, configured_structures(), cur_.last_docid, cur_.schema_version, meta_, fields_, [&](auto& document_record, auto& emit) {
		const format::structure_bits configured = configured_structures();
		for (std::size_t j = 0; j < all.size(); ++j) {
			const buffer& b = *all[j];
			if (inverting()) {  // the inverter's buffer: the same records as its documents' (F21)
				if (!b.inv) { continue; }
				std::vector<bool> keep(b.inv->buf.doc_count());
				for (std::size_t i = 0; i < b.order.size(); ++i) {
					const record& x = b.lane.at(b.order[i].ref);
					if (!x.tombstone() && x.ordinal() < keep.size()) { keep[x.ordinal()] = live(j, b.order[i].ref); }
				}
				b.inv->buf.digest(configured, [&](ordinal_t o) { return !keep[o]; }, document_record, emit);
				continue;
			}
			for (std::size_t i = 0; i < b.order.size(); ++i) {
				if (!live(j, b.order[i].ref)) { continue; }
				const record& x = b.lane.at(b.order[i].ref);
				document_record(x.docid, x.version, x.key());
				const document whole = x.materialize();
				for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
					if constexpr (requires { typename E::part; E::records(x.docid, static_cast<const typename E::part&>(whole), emit); }) {
						if (configured & E::structure) { E::records(x.docid, static_cast<const typename E::part&>(whole), emit); }
					}
				});
			}
		}
	});
}

template <class Layout>
basic_check_report<Layout> basic_index<Layout>::check() const {
	basic_check_report<Layout> r;
	for (const auto& s : segs_) { check_segment(s->seg(), &s->deleted, r); }
	if (log_g_) {  // the log replays to the head in memory (manifest-log D3)
		const files::log_read lr = files::read_log(index_detail::read_all(*io_->open_existing(index_detail::manifest_name(log_g_))), log_g_, readable());
		if (!(lr.state == m_)) { throw corrupt_index("the manifest log differs from the head"); }
		// The head's table is the table's prefix, and each segment without uncommitted deletes
		// holds the head's deleted set.
		const auto chain = fields_.chain_at(m_.fields.size());
		if (!chain || m_.fields.mark().chain != *chain) { throw corrupt_index("the field table differs from its log"); }
		for (const auto& s : segs_) {
			const auto it = m_.segments.find(s->ref.id);
			if (it == m_.segments.end() || s->deletes_dirty) { continue; }
			bool same = it->second.deleted.size() == s->dead;
			it->second.deleted.for_each([&](std::uint64_t o) { same = same && s->deleted.test(o); });
			if (!same) { throw corrupt_index("a segment's deleted set differs from its log"); }
		}
	}
	(void)digest();
	return r;
}

template <class Layout>
std::optional<std::pair<std::uint64_t, std::uint64_t>> basic_index<Layout>::find_key(std::string_view key) const {
	lease_.check_read();
	if (const copy c = lookup_key(key)) { return std::pair{c.docid, c.version}; }
	return std::nullopt;
}

template <class Layout>
std::optional<std::string> basic_index<Layout>::get(std::string_view key) const
	requires(segment::template contains<stored_plugin>())
{
	std::optional<std::string> out;
	get(key, [&](std::string_view v) { out.emplace(v); });
	return out;
}

template <class Layout>
template <class F>
bool basic_index<Layout>::get(std::string_view key, F&& f) const
	requires(segment::template contains<stored_plugin>())
{
	lease_.check_read();
	// The buffers newest first: the key's newest record answers, a tombstone "absent" (D5, D6).
	const auto in = [&](const buffer& b) -> int {
		const wbuf::ref_t r = find_key_in(b, key);
		if (r == wbuf::no_ref) { return -1; }
		const record& x = b.lane.at(r);
		if (x.tombstone()) { return 0; }
		f(buffered_stored(b, x));
		return 1;
	};
	if (wb_) {
		if (const int got = in(*wb_); got >= 0) { return got; }
	}
	for (std::size_t i = frozen_.size(); i-- > 0;) {
		if (const int got = in(*frozen_[i]); got >= 0) { return got; }
	}
	return get_segments(owned_segments{segs_}, key, f, pending_index());
}

template <class Layout>
std::optional<std::uint64_t> basic_index<Layout>::find_docid(std::uint64_t docid) const {
	lease_.check_read();
	if (const copy c = locate_docid(docid)) { return c.version; }
	return std::nullopt;
}

template <class Layout>
bool basic_index<Layout>::has_docid(std::uint64_t docid) const {
	lease_.check_read();
	return bool(locate_docid(docid, false));
}

template <class Layout>
constexpr bool basic_index<Layout>::scannable() {
	return segment::template contains<keys_plugin>() && segment::template contains<stored_plugin>() && std::is_same_v<inverter, detail::no_inverter>;
}

template <class Layout>
template <class F>
std::size_t basic_index<Layout>::scan(std::string_view from, F&& f) const
	requires(scannable())
{
	return scan_keys(from, nullptr, ~std::size_t(0), f);
}

template <class Layout>
template <class F>
std::size_t basic_index<Layout>::scan(std::string_view from, std::size_t n, F&& f) const
	requires(scannable())
{
	return scan_keys(from, nullptr, n, f);
}

template <class Layout>
template <class F>
std::size_t basic_index<Layout>::scan(std::string_view from, std::string_view to, F&& f) const
	requires(scannable())
{
	return scan_keys(from, &to, ~std::size_t(0), f);
}

template <class Layout>
template <class F>
std::size_t basic_index<Layout>::scan(std::string_view from, std::string_view to, std::size_t n, F&& f) const
	requires(scannable())
{
	return scan_keys(from, &to, n, f);
}

template <class Layout>
template <class Segs, class Buffer>
std::uint64_t basic_index<Layout>::digest_of(const Segs& segs, format::structure_bits configured, std::uint64_t last_docid, std::uint64_t schema_version,
	const std::map<std::string, std::string>& meta, const field_table& table, Buffer&& buffer) {
	std::uint64_t sum = 0;
	auto emit = [&](std::string_view rec) { sum += xxh64::hash(rec); };
	std::set<std::uint64_t> docids;
	std::set<std::string> keys;
	auto document_record = [&](std::uint64_t docid, std::uint64_t version, std::string_view key) {
		if (docid == 0 || docid > last_docid || !docids.insert(docid).second) { throw std::logic_error("strata: digest: docid invariant"); }
		if (!key.empty() && !keys.insert(std::string(key)).second) { throw std::logic_error("strata: digest: key invariant"); }
		digest_record rec('D');
		rec.u64(docid);
		rec.u64(version);
		rec.bytes(key);
		emit(rec.data());
	};
	for (std::size_t i = 0; i < segs.size(); ++i) {
		const segment_view s = segs[i];
		auto live = [&](ordinal_t o) { return s.live(o); };
		const segment& sg = *s.seg();
		const core_segment& core = sg.core();
		std::vector<std::string> key_of(core.doc_count());  // walked through the keys reader's checks (L3, MR2)
		sg.template get<keys_plugin>().each_key([&](std::string_view key, ordinal_t o) { key_of[o] = key; });
		for (ordinal_t o = 0; o < core.doc_count(); ++o) {
			if (live(o)) { document_record(core.docid(o), sg.template get<keys_plugin>().version(o), key_of[o]); }
		}
		for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
			if constexpr (requires { E::records(sg.template get<E>(), core, live, emit); }) {
				if (configured & E::structure) { E::records(sg.template get<E>(), core, live, emit); }
			}
		});
	}
	buffer(document_record, emit);
	digest_record last('L');
	last.u64(last_docid);
	emit(last.data());
	digest_record schema('S');
	schema.u64(schema_version);
	emit(schema.data());
	for (const auto& [k, v] : meta) {
		digest_record m('M');
		m.bytes(k);
		m.bytes(v);
		emit(m.data());
	}
	for (std::size_t id = 0; id < table.size(); ++id) {  // the field table, in id order
		digest_record f('F');
		f.u32(std::uint32_t(id));
		f.bytes(table[id].name);
		f.u32(std::uint32_t(table[id].role) | std::uint32_t(table[id].postings) << 8 | std::uint32_t(table[id].norms) << 16);
		emit(f.data());
	}
	return sum;
}

template <class Layout>
bool basic_index<Layout>::entry_from(const buffer& b, const wbuf::pos_entry& e, std::uint64_t prefix, std::string_view key) {
	const std::uint64_t p = e.prefix();
	return p != prefix ? p > prefix : std::string_view(b.lane.at(e.ref).key()) >= key;
}

template <class Layout>
bool basic_index<Layout>::maintain_runs() const {
	if constexpr (!scannable()) {
		return false;
	} else {
		bool any = false;
		for (const auto& b : frozen_) { any |= maintain_runs(b); }
		if (wb_) { any |= maintain_runs(wb_); }
		return any;
	}
}

template <class Layout>
bool basic_index<Layout>::maintain_runs(const std::shared_ptr<buffer>& sb) const {
	buffer& b = *sb;
	bool installed = take_runs(b, b.fresh);
	installed |= take_runs(b, b.compaction);
	const wbuf::run_set* cur = b.runs.load(std::memory_order_relaxed);
	if (!b.fresh.live && b.wants_runs.load(std::memory_order_relaxed)) {
		const std::size_t n1 = b.order.published(), from = cur ? cur->cover : 0;
		if (n1 > from + wbuf::tail_build) {
			b.wants_runs.store(false, std::memory_order_relaxed);
			std::vector<wbuf::level> lv;
			if (cur) { lv = cur->levels; }
			start_runs(sb, b.fresh, [sb, &j = b.fresh, lv = std::move(lv), from, n1, cap = run_cap_]() mutable {
				finish_job(j, [&] {
					std::vector<wbuf::pos_entry> fresh;
					fresh.reserve(n1 - from);
					for (std::size_t i = from; i < n1; ++i) {
						if (keyed(*sb, sb->order[i])) { fresh.push_back(sb->order[i]); }
					}
					return wbuf::fresh_splice(std::move(lv), std::move(fresh), n1, cap, entry_less{sb.get()});
				});
			});
			if (take_runs(b, b.fresh)) {
				installed = true;
				cur = b.runs.load(std::memory_order_relaxed);
			}
		}
	}
	if (cur && !b.compaction.live) {
		std::size_t nb = 0;  // the leading levels past the fresh jobs' cap
		while (nb < cur->levels.size() && cur->levels[nb]->size() > run_cap_) { ++nb; }
		std::vector<wbuf::level> big(cur->levels.begin(), cur->levels.begin() + std::ptrdiff_t(nb));
		if (wbuf::counter_merges(big)) {
			start_runs(sb, b.compaction, [sb, &j = b.compaction, big = std::move(big)] {
				finish_job(j, [&] { return wbuf::compact_splice(big, entry_less{sb.get()}); });
			});
			installed |= take_runs(b, b.compaction);
		}
	}
	return installed;
}

template <class Layout>
template <class Build>
void basic_index<Layout>::finish_job(wbuf::run_job& j, Build&& build) noexcept {
	try {
		j.out = build();
	} catch (...) {
		j.out = {};
		j.out.failed = true;
	}
	j.ready.store(true, std::memory_order_release);
}

template <class Layout>
template <class Job>
void basic_index<Layout>::start_runs(const std::shared_ptr<buffer>& sb, wbuf::run_job& j, Job&& job) const {
	if (!runs_group_) {
		j.live = true;
		job();
		return;
	}
	runs_group_->post([sb, job = std::forward<Job>(job)](const task::cancel_token&) mutable { job(); });
	j.live = true;  // a post that throws leaves it clear, so the buffer can post again
}

template <class Layout>
bool basic_index<Layout>::take_runs(buffer& b, wbuf::run_job& j) const {
	if (!j.live || !j.ready.load(std::memory_order_acquire)) { return false; }
	j.live = false;
	j.ready.store(false, std::memory_order_relaxed);
	const wbuf::splice sp = std::exchange(j.out, {});
	const wbuf::run_set* cur = b.runs.load(std::memory_order_relaxed);
	auto next = wbuf::apply_splice(cur, sp);
	if (!next) { return false; }
	b.runs.store(next.release(), std::memory_order_release);
	std::unique_ptr<const wbuf::run_set> old(cur);
	if (old && pub_ && live_reads()) { b.retired_runs.push_back(std::shared_ptr<const void>(std::move(old))); }
	return true;
}

template <class Layout>
template <class F>
std::size_t basic_index<Layout>::scan_keys(std::string_view from, const std::string_view* to, std::size_t n, F& f) const {
	lease_.check_read();
	struct guard {
		std::uint32_t& depth;
		explicit guard(std::uint32_t& d) : depth(d) { ++depth; }
		~guard() { --depth; }
	} in_scan(scanning_);
	if (!n || (to && *to <= from)) { return 0; }
	// The lease holder's scan asks for its own builds and installs what finished first.
	for_each_buffer([&](const buffer& b) {
		const wbuf::run_set* rs = b.runs.load(std::memory_order_relaxed);
		if (b.order.published() > (rs ? rs->cover : 0) + wbuf::tail_build) { b.wants_runs.store(true, std::memory_order_relaxed); }
	});
	maintain_runs();
	std::vector<buffer_part> parts;  // oldest first
	parts.reserve(frozen_.size() + 1);
	for_each_buffer([&](const buffer& b) { parts.push_back(part_at(b, ~std::uint64_t(0), from, to)); });
	return scan_segments(owned_segments{segs_}, std::span<buffer_part>(parts), pending_index(), from, to, n, f);
}

template <class Layout>
auto basic_index<Layout>::key_at(const buffer& b, std::string_view key, std::uint64_t r) -> const record* {
	for (wbuf::ref_t x = find_key_in(b, key); x != wbuf::no_ref;) {
		const record& rec = b.lane.at(x);
		if (position_of(b, rec) <= r) { return &rec; }
		x = rec.prev;
	}
	return nullptr;
}

template <class Layout>
bool basic_index<Layout>::docid_at(const buffer& b, std::uint64_t docid, std::uint64_t r) {
	for (wbuf::ref_t x = probe_docid(b, docid); x != wbuf::no_ref;) {
		const record& rec = b.lane.at(x);
		if (rec.docid != docid) { return false; }
		if (position_of(b, rec) <= r) { return true; }
		x = rec.docid_prev() != wbuf::no_ref ? rec.docid_prev() : rec.prev;
	}
	return false;
}

template <class Layout>
bool basic_index<Layout>::superseded(const buffer* active, const segment_view& s, ordinal_t ord, std::uint64_t r) {
	if (!active || r < active->first_docid_only.load(std::memory_order_acquire)) { return false; }
	return docid_at(*active, s.seg()->core().docid(ord), r);
}

template <class Layout>
template <class F>
bool basic_index<Layout>::get_at(const view& v, std::string_view key, std::uint64_t r, F& f)
	requires(segment::template contains<stored_plugin>())
{
	const auto in = [&](const buffer& b) -> int {
		const record* x = key_at(b, key, r);
		if (!x) { return -1; }
		if (x->tombstone()) { return 0; }
		f(x->stored());
		return 1;
	};
	if (v.active_) {
		if (const int got = in(*v.active_); got >= 0) { return got; }
	}
	for (std::size_t i = v.frozen_.size(); i-- > 0;) {
		if (const int got = in(*v.frozen_[i]); got >= 0) { return got; }
	}
	const buffer* a = v.active_.get();
	return get_segments(v.views_, key, f, nullptr, [&](const segment_view& s, ordinal_t o) { return superseded(a, s, o, r); });
}

template <class Layout>
auto basic_index<Layout>::part_at(const buffer& b, std::uint64_t r, std::string_view from, const std::string_view* to) -> buffer_part {
	buffer_part p{&b, r, b.runs.load(std::memory_order_acquire), {}, 0};
	const std::size_t cover = p.runs ? p.runs->cover : 0;
	std::size_t end = b.order.published();
	for (std::size_t lo = cover; lo < end;) {  // positions never decrease along the index
		const std::size_t mid = lo + (end - lo) / 2;
		if (position_of(b, b.lane.at(b.order[mid].ref)) <= r) {
			lo = mid + 1;
		} else {
			end = mid;
		}
	}
	if (end > cover + wbuf::tail_build && !b.wants_runs.load(std::memory_order_relaxed)) { b.wants_runs.store(true, std::memory_order_relaxed); }
	const std::uint64_t pf = wbuf::key_head(from), pt = to ? wbuf::key_head(*to) : 0;
	for (std::size_t i = cover; i < end; ++i) {
		const wbuf::pos_entry e = b.order[i];
		if (keyed(b, e) && entry_from(b, e, pf, from) && !(to && entry_from(b, e, pt, *to))) { p.tail.push_back(e); }
	}
	const entry_less less{&b};
	if (p.tail.size() > 2 * wbuf::tail_select) {  // a seek and a few nexts need only the smallest
		std::nth_element(p.tail.begin(), p.tail.begin() + wbuf::tail_select, p.tail.end(), less);
		std::sort(p.tail.begin(), p.tail.begin() + wbuf::tail_select, less);
		p.sorted = wbuf::tail_select;
	} else {
		std::sort(p.tail.begin(), p.tail.end(), less);
		p.sorted = p.tail.size();
	}
	return p;
}

template <class Layout>
template <class F>
std::size_t basic_index<Layout>::scan_at(const view& v, std::uint64_t r, std::string_view from, const std::string_view* to, std::size_t n, F& f)
	requires(snapshot_scannable())
{
	if (!n || (to && *to <= from)) { return 0; }
	std::vector<buffer_part> parts;  // oldest first
	parts.reserve(v.frozen_.size() + 1);
	for (const auto& b : v.frozen_) { parts.push_back(part_at(*b, r, from, to)); }
	if (v.active_) { parts.push_back(part_at(*v.active_, r, from, to)); }
	const buffer* a = v.active_.get();
	return scan_segments(v.views_, std::span<buffer_part>(parts), nullptr, from, to, n, f, [&](const segment_view& s, ordinal_t o) { return superseded(a, s, o, r); });
}

template <class Layout>
template <class Segs, class F, class Gone>
bool basic_index<Layout>::get_segments(const Segs& segs, std::string_view key, F& f, const pending_set* pending, Gone&& gone) {
	const std::uint64_t hash = xxh64::hash(key);
	for (std::size_t i = segs.size(); i-- > 0;) {
		const segment_view s = segs[i];
		if (const auto ord = s.seg()->template get<keys_plugin>().find(key, live_bits{s.deleted ? s.deleted->data() : nullptr}, hash)) {
			if (pending_has(pending, s.id, *ord) || gone(s, *ord)) { return false; }
			index_detail::lent_frame lent;
			f(s.seg()->template get<stored_plugin>().stored(*ord, lent.frame()));
			return true;
		}
	}
	return false;
}

template <class Layout>
template <class Segs, class F, class Gone>
std::size_t basic_index<Layout>::scan_segments(const Segs& segs, std::span<buffer_part> buffers, const pending_set* pending, std::string_view from, const std::string_view* to, std::size_t n, F& f, Gone&& gone) {
	if (!n || (to && *to <= from)) { return 0; }
	struct seg_source {
		segment_view seg;
		keys::reader::range cur;
		typename stored_plugin::reader::stored_frame frame;
	};
	const auto skip_dead = [&](seg_source& s) {
		while (s.cur.valid() && ((s.seg.dead && s.seg.deleted->test(s.cur.ordinal())) || pending_has(pending, s.seg.id, s.cur.ordinal()))) { s.cur.next(); }
	};
	std::vector<seg_source> segsrc;  // by segment, oldest first
	segsrc.reserve(segs.size());
	for (std::size_t i = 0; i < segs.size(); ++i) {
		const segment_view sv = segs[i];
		const auto& k = sv.seg()->template get<keys_plugin>();
		if (!k.key_count() || k.max_key() < from || (to && k.min_key() >= *to)) { continue; }
		segsrc.push_back({sv, k.range(from), {}});
		skip_dead(segsrc.back());
	}
	struct source {
		std::uint64_t pre = 0;
		std::string_view key;
		bool valid = false;
		seg_source* seg = nullptr;
		buffer_part* part = nullptr;  // a buffer's level or tail:
		const std::vector<wbuf::pos_entry>* a = nullptr;
		std::size_t at = 0;
		const record* rec = nullptr;
	};
	const auto settle = [](source& s) {
		if (s.seg) {
			s.valid = s.seg->cur.valid();
			if (s.valid) {
				s.key = s.seg->cur.key();
				s.pre = wbuf::key_head(s.key);
			}
			return;
		}
		const buffer& b = *s.part->b;
		for (s.valid = false; s.at < s.a->size(); ++s.at) {
			if (s.a == &s.part->tail && s.at == s.part->sorted) { s.part->finish(); }
			const wbuf::pos_entry& e = (*s.a)[s.at];
			const record& x = b.lane.at(e.ref);
			if (position_of(b, x) <= s.part->r) {  // run entries may be newer than r
				s.valid = true;
				s.key = x.key();
				s.pre = e.prefix();
				s.rec = &x;
				return;
			}
		}
	};
	std::vector<source> src;  // the segments oldest first, then each buffer's levels and tail
	src.reserve(segsrc.size() + 8 * buffers.size());
	for (auto& s : segsrc) {
		src.push_back({.seg = &s});
		settle(src.back());
	}
	const std::uint64_t pf = wbuf::key_head(from);
	for (buffer_part& p : buffers) {
		if (p.runs) {
			for (const auto& l : p.runs->levels) {
				const auto it = std::partition_point(l->begin(), l->end(), [&](const wbuf::pos_entry& e) { return !entry_from(*p.b, e, pf, from); });
				src.push_back({.part = &p, .a = l.get(), .at = std::size_t(it - l->begin())});
				settle(src.back());
			}
		}
		src.push_back({.part = &p, .a = &p.tail});
		settle(src.back());
	}
	const auto cmp = [](const source& x, const source& y) { return x.pre != y.pre ? (x.pre < y.pre ? -1 : 1) : x.key.compare(y.key); };
	const auto newer = [](const source& x, const source& y) {  // within one buffer
		return x.rec->position != y.rec->position ? x.rec->position > y.rec->position : (*x.a)[x.at].ref > (*y.a)[y.at].ref;
	};
	std::vector<std::size_t> tied;
	tied.reserve(src.size());
	std::size_t count = 0;
	while (count < n) {
		tied.clear();
		const source* min = nullptr;
		for (std::size_t i = 0; i < src.size(); ++i) {
			if (!src[i].valid) { continue; }
			const int c = min ? cmp(src[i], *min) : -1;
			if (c < 0) {
				min = &src[i];
				tied.clear();
			}
			if (c <= 0) { tied.push_back(i); }
		}
		if (!min || (to && min->key >= *to)) { break; }
		// The newest: the last tied source's buffer or segment; within a buffer, its newest record.
		std::size_t pick = tied.back();
		if (src[pick].part) {
			for (const std::size_t i : tied) {
				if (src[i].part == src[pick].part && newer(src[i], src[pick])) { pick = i; }
			}
		}
		const std::string_view key = min->key;
		const std::uint64_t pre = min->pre;
		bool emit = true;
		std::string_view value;
		std::uint64_t version = 0;
		if (seg_source* s = src[pick].seg) {
			const ordinal_t ord = s->cur.ordinal();
			emit = !gone(s->seg, ord);  // the docid check, emitted or not (D9, WB-22)
			if (emit) {
				value = s->seg.seg()->template get<stored_plugin>().stored(ord, s->frame);
				version = s->seg.seg()->template get<keys_plugin>().version(ord);
			}
		} else {
			const record& x = *src[pick].rec;
			emit = !x.tombstone();
			if (emit) {
				value = x.stored();
				version = x.version;
			}
		}
		bool more = true;
		if (emit) {
			++count;
			if constexpr (std::is_void_v<std::invoke_result_t<F&, std::string_view, std::string_view, std::uint64_t>>) {
				f(key, value, version);
			} else {
				more = f(key, value, version);
			}
		}
		if (!more) { break; }
		// Every source holding the key moves past it: the buffers' first, while `key` (perhaps a
		// segment cursor's) is still valid.
		for (const std::size_t i : tied) {
			source& s = src[i];
			if (s.seg) { continue; }
			while (s.valid && s.pre == pre && s.key == key) {
				++s.at;
				settle(s);
			}
		}
		for (const std::size_t i : tied) {
			source& s = src[i];
			if (!s.seg) { continue; }
			s.seg->cur.next();
			skip_dead(*s.seg);
			settle(s);
		}
	}
	return count;
}

}  // namespace strata
