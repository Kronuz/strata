// basic_index's transactions: apply, stage 1's evaluation and undo log, and the writer calls' lease, chores and fence
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
verdict basic_index<Layout>::apply(const transaction& t, files::position at) {
	const writer_scope leased{*this, opts_.defer_io};
	live();
	const bool logs = logged_ && at == files::position{};
	std::string cmd;
	if constexpr (command::loggable<Layout>) {
		if (logs) {
			if (opts_.log->failed()) {
				fenced_ = true;  // fail-stop (D9): nothing appends until the host reopens the log
				throw index_fenced("strata: the turn log failed; reopen the index");
			}
			if (join_owed_) { throw std::logic_error("strata: a logged index applies after its first commit (D7's join)"); }
			command::encode(cmd, t);
		}
	}
	assert(!logged_ || at == files::position{} || at.index <= logged_->synced.load(std::memory_order_relaxed));  // replay: durable
	turn_guard turn{map_asked_};
	// Stage 1 waits for room: the flush happens before this transaction, so a flush
	// failure never loses an accepted transaction's verdict.
	const auto flush_due = [&] { return needs_flush() || !fits(t) || buffer_size() + t.ops.size() > opts_.buffered_docs_cap; };
	if (!opts_.defer_io && (flush_due() || log_due())) { flush_point(); }
	verdict v;
	if (at != files::position{} && at <= cur_.applied) {
		v.result = verdict::skipped;
		return v;
	}
	// Host mode, after a demote(): no segment read may map inside apply, so the bodies are owed
	// until promote(). The bit keeps demote() out until the turn ends (cold-tier D3, CT-R22).
	if (opts_.defer_io && promote_owed_.load(std::memory_order_acquire)) {
		map_asked_ = turn.owed = true;
		v.result = verdict::map_owed;
		return v;
	}
	assert(!opts_.defer_io || std::all_of(segs_.begin(), segs_.end(), [](const auto& s) { return s->bodied() || s->failed(); }));
	// Host mode: the flush is owed only while the buffer holds something it could write, so an
	// oversize transaction on an empty buffer is invalid, not owed again after every refresh.
	if (opts_.defer_io && flush_due() && (buffered_records() != 0 || holds_rejected())) {
		turn.owed = true;
		v.result = verdict::flush_owed;
		return v;
	}
	if (t.ops.size() > opts_.buffered_docs_cap || !lane_fits(t)) {
		v.result = verdict::invalid;
		return v;
	}
	// Storage admission: the transaction's estimated bytes (and a segment's fixed bytes
	// when it starts a buffer), then any growth of the commit allowance it causes (a
	// first delete in a segment adds a deleted set to the edit), or no_space.
	const std::uint64_t reserve = storage_permit_ ? estimate_bytes(t) + (buffer_size() == 0 ? segment_overhead : 0) : 0;
	const bool drained = std::exchange(drain_ran_, false);
	if (opts_.defer_io) {
		if (reserve && !storage_permit_.try_grow(reserve)) {
			// Owed once per round: the retry after reclaim_for_space() answers no_space.
			if (!drained && drain_pending()) {
				drain_asked_ = true;
				turn.owed = true;
				v.result = verdict::drain_owed;
			} else {
				v.result = verdict::no_space;
			}
			return v;
		}
	} else if (reserve && !storage_permit_.try_grow(reserve) && !(drain() && storage_permit_.try_grow(reserve))) {
		v.result = verdict::no_space;
		return v;
	}
	undo_.clear();
	const std::size_t fields_before = fields_.size();
	try {
		v = evaluate(t);
	} catch (...) {
		// Each step is logged before its change can fail, so the undo log covers whatever
		// stage 1 had done when it threw.
		abandon(reserve);
		throw;
	}
	std::uint64_t allowance = allowance_;
	if (v.result == verdict::accepted && storage_permit_) {
		allowance = commit_allowance();
		if (allowance > allowance_ && !storage_permit_.try_grow(allowance - allowance_)) { v.result = verdict::no_space; }
	}
	if (v.result != verdict::accepted) {
		abandon(reserve);
	} else {
		reserved_ += reserve;
		allowance_ = std::max(allowance, allowance_);
		++wb_->transactions;
		visible_ = wb_->base + wb_->transactions;
		wb_->order.publish();
		invert_pending();
		charge();
		if (t.set_schema) { set_schema(*t.set_schema); }
		if (fields_.size() != fields_before) { ++fields_epoch_; }
		++applied_since_commit_;
		changed();
	}
	undo_.clear();
	if (at != files::position{}) {
		assert(at > cur_.applied);  // the applied position never moves back
		cur_.applied = at;
		changed();  // a rejected transaction moves the position too (SR-D16)
		if (logged_ && v.result == verdict::accepted) { logged_->replayed(visible_); }  // replay: durable
	}
	// The group's end (D7): the cover first (rule 4), then V, before the writer resumes (rule 3).
	// A logged index's V is the watcher's (D4), and the cover precedes the append, so V never
	// passes it.
	publish_cover();
	if (pub_ && !logged_) { pub_->rs->visible.store(visible_, std::memory_order_release); }
	if (logs && v.result == verdict::accepted) { join_log(cmd); }
	if (!opts_.defer_io && chores_owed_.load(std::memory_order_acquire)) { run_chores(); }
	return v;
}

template <class Layout>
void basic_index<Layout>::abandon(std::uint64_t reserve) {
	rollback();
	pending_docs_.clear();
	storage_permit_.shrink(reserve);
	charge();
	undo_.clear();
}

template <class Layout>
void basic_index<Layout>::check_in() noexcept {
	do {
		const chore_scope chores{lease_};
		if (!chores) {
			// Dekker with writer_scope (CR4): a holder that hands off after this store sees it; one
			// that handed off before lets the retry take the lease.
			chores_owed_.store(true, std::memory_order_seq_cst);
			const chore_scope retry{lease_};
			if (!retry) { return; }
			run_chores();
		} else {
			run_chores();
		}
	} while (chores_owed_.load(std::memory_order_seq_cst));  // a check-in that met these chores
}

template <class Layout>
void basic_index<Layout>::live() const {
#ifndef NDEBUG
	if (guard_word_ && !guard_held_(guard_word_->load(std::memory_order_acquire))) { detail::owner_break("strata: a catalog index was called with no pin held"); }
#endif
	if (fenced_) { throw index_fenced("strata: the writer is fenced; reopen the index"); }
	if (scanning_) { throw std::logic_error("strata: the index changed inside a scan's callback"); }
}

template <class Layout>
template <class F>
void basic_index<Layout>::guarded(F&& f) {
	try {
		const detail::no_input_map_scope noinput;
		f();
	} catch (const space_error& e) {
		throw std::system_error(e.code(), e.what());
	} catch (const std::system_error& e) {
		fenced_ = true;
		throw index_fenced(std::string("strata: durability failure: ") + e.what());
	}
}

template <class Layout>
void basic_index<Layout>::run_chores() noexcept {
	chores_owed_.exchange(false, std::memory_order_acq_rel);
	if (fenced_ || closing()) { return; }
	try {
		if (!pending_.empty()) {
			guarded([&] { publish_merges(false, false); });
			publish_point();
		}
		if (free_views()) { retain(true); }
	} catch (...) {
		fenced_ = true;
	}
}

template <class Layout>
[[noreturn]] void basic_index<Layout>::fence(const std::string& why) {
	fenced_ = true;
	throw index_fenced(why);
}

template <class Layout>
void basic_index<Layout>::rollback() {
	buffer* b = wb_.get();
	for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
		switch (it->type) {
		case undo_step::key_slot: b->keys.restore(it->place); break;
		case undo_step::docid_slot: b->docids.restore(it->place); break;
		case undo_step::pending:
			b->pending_keys.erase(pending_key(b->pending.back().first, b->pending.back().second));
			b->pending.pop_back();
			break;
		case undo_step::segment_dirty: segs_[it->a]->deletes_dirty = it->prev_dirty; break;
		case undo_step::last_docid: cur_.last_docid = it->prev_value; break;
		case undo_step::first_docid_only: b->first_docid_only.store(it->prev_value, std::memory_order_release); break;
		case undo_step::fields: fields_.truncate(it->a); break;
		case undo_step::meta:
			if (it->prev_meta) {
				meta_[it->key] = *it->prev_meta;
			} else {
				meta_.erase(it->key);
			}
			if (!it->prev_dirty) { meta_since_.erase(it->key); }
			break;
		}
	}
	if (b) {
		// With readers, a rejected record may be under a reader that loaded its slot before the
		// restore: it stays in the lane, unreferenced, and its bytes stay counted, so the flush
		// threshold bounds them and the freeze reclaims them (D4, CR1).
		const bool keep = pub_ && live_reads();
		if (!keep) {
			b->lane.truncate(mark_.lane);  // the rejected records and their document copies
			b->bytes = mark_.bytes;
		}
		b->order.rewind(mark_.records);
		b->docs = mark_.docs;
	}
}

template <class Layout>
void basic_index<Layout>::remove_copy(const copy& c) {
	if (c.where != copy::segment) { return; }
	auto& s = *segs_[c.at];
	buffer& b = *wb_;
	assert(s.ref.id < (std::uint64_t(1) << 32));
	reserve_undo(2);  // each step is logged before the change it undoes can fail
	b.pending.push_back({s.ref.id, c.ord});
	undo_.push_back({undo_step::pending});
	b.pending_keys.insert(pending_key(s.ref.id, c.ord));
	undo_.push_back({undo_step::segment_dirty, {}, c.at, {}, 0, {}, s.deletes_dirty});
	s.deletes_dirty = true;  // its deleted set changes at the next freeze
}

template <class Layout>
wbuf::ref_t basic_index<Layout>::push_record(buffer& b, std::uint64_t docid, std::uint64_t version, const document* doc, std::string_view key, bool tomb, bool by_docid) {
	const std::uint32_t kh = key.empty() ? 0 : index_detail::key_hash(key);
	wbuf::version_map::place kp;
	if (!key.empty()) { kp = b.keys.locate(kh, [&](wbuf::ref_t q) { return std::string_view(b.lane.at(q).key()) == key; }); }
	wbuf::ref_t prev = wbuf::no_ref;
	if (!key.empty()) {
		prev = wbuf::version_map::ref_of(kp);
	} else if (b.docids.built()) {
		prev = probe_docid(b, docid);
	}
	// A replace_docid reserves the docid prev field; it names the docid's record when that is
	// not prev (D3, WB-21). Its map exists: writable_buffer built it.
	wbuf::ref_t dprev = wbuf::no_ref;
	std::uint8_t flags = tomb ? record::tombstone_flag : 0;
	if (by_docid) {
		flags |= record::docid_prev_flag;
		if (const wbuf::ref_t d = probe_docid(b, docid); d != prev) { dprev = d; }
	}
	std::uint32_t ordinal = 0;
	if (inverting() && !tomb) {
		flags |= record::ordinal_flag;
		ordinal = std::uint32_t((b.inv ? b.inv->buf.doc_count() : 0) + pending_docs_.size());
	}
	// The record keeps the key and the stored bytes in itself; a document with a part the segment
	// writer reads beyond them, or one past a chunk, is held whole (the record's fallback).
	const std::uint32_t pos = b.transactions + 1;
	const wbuf::ref_t r = [&] {
		if (tomb || inverting()) {
			return record::emplace(b.lane, docid, version, pos, prev, flags, dprev, ordinal, true, key, {}, [&] {
				document d;
				d.key.assign(key);
				return d;
			});
		}
		std::optional<document> buffered;  // the document as the buffer keeps it, when a hook rewrites a part
		if (has_buffer_hooks()) {
			buffered = *doc;
			for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
				if constexpr (requires(typename E::part& p) { E::buffered(p); }) { E::buffered(static_cast<typename E::part&>(*buffered)); }
				// A part's references become ids: the buffer stores what the transaction resolved.
				if constexpr (requires(typename E::part& p, const field_resolution& r) { E::resolve(p, r); }) {
					if (fields_res_) { E::resolve(static_cast<typename E::part&>(*buffered), *fields_res_); }
				}
			});
		}
		const document& src = buffered ? *buffered : *doc;
		return record::emplace(b.lane, docid, version, pos, prev, flags, dprev, ordinal, inline_parts(src), src.key, stored_bytes(src), [&] { return buffered ? std::move(*buffered) : document(*doc); });
	}();
	reserve_undo(2);  // so logging the slots below cannot throw after a store
	if (!key.empty()) {
		b.keys.store(kp, kh, r);
		undo_.push_back({undo_step::key_slot, kp});
	}
	if (b.docids.built()) { undo_.push_back({undo_step::docid_slot, store_docid(b, r)}); }
	b.order.push_back(wbuf::make_entry(b.lane.at(r).key(), r));
	if (!tomb) { ++b.docs; }
	const record& x = b.lane.at(r);
	if (x.is_whole()) {  // what a whole record holds beyond the lane
		assert(doc_bytes(x.whole_document()) >= x.stride());
		b.bytes += doc_bytes(x.whole_document()) - x.stride();
	}
	return r;
}

template <class Layout>
void basic_index<Layout>::reserve_undo(std::size_t n) {
	if (undo_.capacity() - undo_.size() < n) { undo_.reserve(std::max(undo_.capacity() * 2, undo_.size() + n)); }
}

template <class Layout>
void basic_index<Layout>::note_docid_only(buffer& b) {
	const std::uint64_t at = b.base + b.transactions + 1, was = b.first_docid_only.load(std::memory_order_relaxed);
	if (was <= at) { return; }
	undo_.push_back({undo_step::first_docid_only, {}, 0, {}, was});
	b.first_docid_only.store(at, std::memory_order_release);
}

template <class Layout>
void basic_index<Layout>::bump_last_docid(std::uint64_t docid) {
	if (docid <= cur_.last_docid) { return; }
	undo_.push_back({undo_step::last_docid, {}, 0, {}, cur_.last_docid});
	cur_.last_docid = docid;
}

template <class Layout>
auto basic_index<Layout>::writable_buffer(const transaction& t) -> buffer& {
	if (!wb_) { wb_ = std::make_shared<buffer>(visible_, opts_.lane_chunks_cap); }
	buffer& b = *wb_;
	mark_ = {b.order.size(), b.docs, b.bytes, b.lane.tell()};  // before anything that can throw: a rollback restores it
	bool docids = false;
	for (const auto& o : t.ops) {
		const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
		docids = docids || o.type == operation::replace_docid || o.type == operation::delete_docid || (writes && o.doc.key.empty());
	}
	b.keys.reserve(2 * t.ops.size());
	if (docids) { build_docids(b); }
	if (b.docids.built()) { b.docids.reserve(2 * t.ops.size()); }
	return b;
}

template <class Layout>
verdict basic_index<Layout>::evaluate(const transaction& t) {
	verdict v;
	buffer& b = writable_buffer(t);
	if (t.expected_schema && *t.expected_schema != cur_.schema_version) {
		v.result = verdict::schema_conflict;
		v.current_version = cur_.schema_version;
		return v;
	}
	// The fields (D2): the host's mark first, then each declaration by name, then the resolution
	// the parts' references are checked against.
	if (t.expected_fields) {
		const auto chain = fields_.chain_at(t.expected_fields->size);
		if (!chain || *chain != t.expected_fields->chain) {
			v.result = verdict::fields_conflict;
			v.current_fields = fields_.mark();
			return v;
		}
	}
	std::vector<std::uint32_t> declared_ids;
	declared_ids.reserve(t.fields.size());
	bool logged = false;
	for (std::size_t i = 0; i < t.fields.size(); ++i) {
		const field_declaration& d = t.fields[i];
		auto refuse = [&] {
			v.result = verdict::invalid;
			v.field = i;
			return v;
		};
		if (field_problem(d)) { return refuse(); }
		if (const auto id = fields_.find(d.name)) {
			if (fields_[*id] != d) { return refuse(); }  // a different declaration of a known name
			declared_ids.push_back(*id);
			continue;
		}
		// One key field, in a keyed index (D7).
		if (d.role == field_role::key && (!(cur_.configured & format::bit::keys) || fields_.key_field())) { return refuse(); }
		if (!logged) {
			undo_.push_back({undo_step::fields, {}, fields_.size()});  // logged before the table changes
			logged = true;
		}
		declared_ids.push_back(fields_.append(d));
	}
	const field_resolution resolution{declared_ids, t.expected_fields ? t.expected_fields->size : 0, fields_.key_field()};
	pending_declared_ = declared_ids;
	pending_known_ = resolution.known;
	pending_key_field_ = resolution.key;
	struct resolution_scope {
		const field_resolution*& slot;
		~resolution_scope() { slot = nullptr; }
	} resolution_guard{fields_res_};
	fields_res_ = &resolution;
	for (std::size_t i = 0; i < t.ops.size(); ++i) {
		const operation& o = t.ops[i];
		v.op = i;
		auto reject = [&](verdict::outcome why, std::uint64_t current = 0) {
			v.result = why;
			v.current_version = current;
			return v;
		};
		switch (o.type) {
		case operation::set_meta:
		case operation::delete_meta: {
			if (o.meta_key.starts_with(files::reserved_meta_prefix)) { return reject(verdict::invalid); }  // the engine's (F20)
			undo_step u{undo_step::meta};
			u.key = o.meta_key;
			u.prev_dirty = meta_since_.count(o.meta_key) != 0;
			if (auto it = meta_.find(o.meta_key); it != meta_.end()) { u.prev_meta = it->second; }
			undo_.push_back(std::move(u));
			if (o.type == operation::set_meta) {
				meta_[o.meta_key] = o.meta_value;
			} else {
				meta_.erase(o.meta_key);
			}
			meta_since_.insert(o.meta_key);  // the change list's key (D3)
			continue;
		}
		default:
			break;
		}
		const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
		bool key_valid = true;
		std::optional<std::string_view> doc_key;  // into the operation, which outlives stage 2
		if (writes) {
			if (!valid(o.doc)) { return reject(verdict::invalid); }
			doc_key = key_of(o.doc, key_valid);
			if (!key_valid) { return reject(verdict::invalid); }
		}
		// The key the operation addresses and its live copy.
		std::optional<std::string_view> key;
		if (o.type == operation::replace_key || o.type == operation::delete_key) {
			if (!(cur_.configured & format::bit::keys) || o.key.empty()) { return reject(verdict::invalid); }
			key = o.key;
			if (writes && doc_key != key) { return reject(verdict::invalid); }
		} else if (writes) {
			key = doc_key;
		}
		copy by_key;
		if (key) { by_key = lookup_key(*key); }
		copy by_docid;
		if (o.type == operation::replace_docid || o.type == operation::delete_docid) {
			if (o.docid == 0) { return reject(verdict::invalid); }
			by_docid = locate_docid(o.docid);
			// A key and a docid naming different live documents reject.
			if (by_key && by_docid && by_key.docid != o.docid) { return reject(verdict::invalid); }
			if (by_key && !by_docid) { return reject(verdict::invalid); }
		}
		// The current version of the addressed document.
		const copy& target = by_key ? by_key : by_docid;
		const std::uint64_t current = target.version;
		const bool exists = bool(target);
		if (o.type == operation::add && exists) { return reject(verdict::version_conflict, current); }
		if (o.expected_version) {
			// A version belongs to a key: a conditional operation without one is invalid.
			if (!key) { return reject(verdict::invalid); }
			if (*o.expected_version == 0 ? exists : (!exists || *o.expected_version != current)) { return reject(verdict::version_conflict, current); }
		}
		// The superseded copy's key, when known: a tombstone for it shadows the copy (D6).
		const std::string_view old_key = by_key ? *key : key_of_copy(target);
		// A segment copy found by docid alone: readers at or above this position check docids (WB-22).
		if (exists && target.where == copy::segment && !by_key) { note_docid_only(b); }
		if (o.type == operation::delete_key || o.type == operation::delete_docid) {
			if (exists) {
				remove_copy(target);
				push_record(b, target.docid, target.version, nullptr, old_key, true);
				v.docids.push_back(target.docid);
			} else {
				v.docids.push_back(0);
			}
			v.versions.push_back(0);
			continue;
		}
		// add or replace: the docid kept, given, or assigned.
		std::uint64_t docid;
		if (o.type == operation::replace_docid) {
			docid = o.docid;
		} else if (by_key) {
			docid = by_key.docid;
		} else {
			docid = cur_.last_docid + 1;
		}
		if (exists) {
			remove_copy(target);
			if (!old_key.empty() && (!key || old_key != *key)) { push_record(b, target.docid, target.version, nullptr, old_key, true); }
		}
		// A version belongs to a key: a document without one has version 0 (F9).
		const std::uint64_t version = !key ? 0 : exists ? current + 1 : 1;
		bump_last_docid(docid);
		const std::string_view new_key = key ? std::string_view(o.doc.key) : std::string_view();
		push_record(b, docid, version, &o.doc, new_key, false, o.type == operation::replace_docid);
		if (inverting()) { pending_docs_.push_back({docid, version, new_key, &o.doc}); }
		v.docids.push_back(docid);
		v.versions.push_back(version);
	}
	v.op = 0;
	return v;
}

}  // namespace strata
