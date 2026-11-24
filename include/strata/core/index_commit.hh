// basic_index's commit: the manifest log's edit, its roll-over and the durable writes behind it
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
bool basic_index<Layout>::merge_output_unsynced() const noexcept {
	return applied_since_commit_ == 0 && std::any_of(segs_.begin(), segs_.end(), [](const auto& s) { return bool(s->unsynced); });
}

template <class Layout>
void basic_index<Layout>::commit() {
	const writer_scope leased{*this};
	live();
	await_log();
	guarded([&] { flush(); });
	publish_merges(false, false);
	if (opts_.auto_merge) { maybe_merge(); }
	prepare_commit();  // the input maps, outside guarded(): a failure leaves the index as refresh() does
	guarded([&] { commit_files(); });
	// The log moved: a failure from here fences, so a throw never hides a durable commit (SR4).
	durable([&] {
		publish_point();
		free_views();
		retain();
		reclaim_files(opts_.reclaim_slice);
	});
	stage_checkpoint();
}

template <class Layout>
void basic_index<Layout>::prepare_commit() {
	for (const auto& s : segs_) {
		if (s->dead == s->ref.doc_count) { continue; }  // commit_files drops it
		if (!s->facts_known) {
			s->learn_facts();
		} else if (s->deletes_dirty || s->failed()) {
			(void)s->seg();
		}
	}
}

template <class Layout>
auto basic_index<Layout>::figures_now() -> std::vector<live_figures> {
	std::sort(deleted_since_.begin(), deleted_since_.end());
	std::vector<live_figures> out(segs_.size());
	std::size_t j = 0;
	for (std::size_t i = 0; i < segs_.size(); ++i) {
		const open_segment& s = *segs_[i];
		const std::uint64_t id = s.ref.id;
		while (j < deleted_since_.size() && deleted_since_[j].first < id) { ++j; }
		live_figures& f = out[i];
		const auto head = m_.segments.find(id);
		if (head == m_.segments.end()) {
			// An installed record keeps its deletes and live length and has no body (D9).
			f.count = s.ref.doc_count - s.dead;
			f.length = !s.dead ? s.total_length : s.deletes_dirty ? live_length(s) : s.ref.live_length;
			continue;
		}
		f.count = head->second.live_count;
		f.length = head->second.live_length;
		for (; j < deleted_since_.size() && deleted_since_[j].first == id; ++j) { f.fresh.push_back(deleted_since_[j].second); }
		if (!f.fresh.empty()) {
			const segment& sg = s.mapped();  // prepare_commit mapped it (D9)
			for (std::uint64_t o : f.fresh) { f.length -= std::min<std::uint64_t>(f.length, sg.length(o)); }
			f.count -= std::min<std::uint64_t>(f.count, f.fresh.size());
		}
		if (f.count != s.ref.doc_count - s.dead) { fence("strata: commit: a segment's deletes are not in the change list"); }
	}
	return out;
}

template <class Layout>
files::log_detail::scalar_values basic_index<Layout>::target_scalars(const std::vector<live_figures>& fig) const {
	namespace sc = files::log_scalar;
	files::log_detail::scalar_values v{};
	v[sc::revision] = m_.revision + (applied_since_commit_ ? 1 : 0);
	v[sc::last_docid] = cur_.last_docid;
	v[sc::next_segment] = cur_.next_segment;
	v[sc::applied_index] = cur_.applied.index;
	v[sc::applied_count] = cur_.applied.applied;
	v[sc::schema_version] = cur_.schema_version;
	v[sc::configured] = cur_.configured;
	v[sc::mask] = cur_.configured;
	for (std::size_t i = 0; i < segs_.size(); ++i) {
		const open_segment& s = *segs_[i];
		v[sc::mask] |= s.structures;
		v[sc::min_version] = v[sc::min_version] ? std::min<std::uint64_t>(v[sc::min_version], s.version) : s.version;
		v[sc::max_version] = std::max<std::uint64_t>(v[sc::max_version], s.version);
		v[sc::doc_count] += s.ref.doc_count;
		v[sc::total_length] += s.total_length;
		v[sc::live_count] += fig[i].count;
		v[sc::live_length] += fig[i].length;
	}
	return v;
}

template <class Layout>
std::vector<std::uint64_t> basic_index<Layout>::target_pins() const {
	std::vector<std::uint64_t> pins = drop_pins_ ? std::vector<std::uint64_t>{} : m_.pins;
	if (pending_pin_ && !std::binary_search(pins.begin(), pins.end(), *pending_pin_)) { pins.insert(std::upper_bound(pins.begin(), pins.end(), *pending_pin_), *pending_pin_); }
	if (pending_unpin_) { std::erase(pins, *pending_unpin_); }
	return pins;
}

template <class Layout>
files::log_segment basic_index<Layout>::entry_of(const open_segment& s, const live_figures& f) {
	files::log_segment e;
	e.id = s.ref.id;
	e.identity = s.ref.identity;
	e.size = s.ref.size;
	e.footer_crc = s.ref.footer_crc;
	e.doc_count = s.ref.doc_count;
	e.live_count = f.count;
	e.live_length = f.length;
	e.deleted = files::log_deleted::of_words(s.deleted.data(), s.deleted.size(), s.ref.doc_count);
	e.values_generation = s.ref.values_generation;
	e.attributes = s.ref.attributes;
	return e;
}

template <class Layout>
files::log_edit basic_index<Layout>::edit_now(const files::log_detail::scalar_values& target, const std::vector<std::uint64_t>& pins, const std::vector<live_figures>& fig) const {
	files::log_edit e;
	e.generation = m_.generation + 1;
	if (attributes_owed_ && *attributes_owed_ != m_.attributes) { e.attributes = *attributes_owed_; }
	const auto head = files::log_detail::scalars_of(m_);
	for (unsigned i = 0; i < files::log_scalar::count; ++i) {
		if (target[i] != head[i]) {
			e.scalars |= 1u << i;
			e.values[i] = target[i];
		}
	}
	std::set_difference(pins.begin(), pins.end(), m_.pins.begin(), m_.pins.end(), std::back_inserter(e.pins_added));
	std::set_difference(m_.pins.begin(), m_.pins.end(), pins.begin(), pins.end(), std::back_inserter(e.pins_removed));
	for (const auto& [id, seg] : m_.segments) {
		const auto it = std::lower_bound(segs_.begin(), segs_.end(), id, [](const auto& s, std::uint64_t x) { return s->ref.id < x; });
		if (it == segs_.end() || (*it)->ref.id != id) { e.segments_removed.push_back(id); }
	}
	for (std::size_t i = 0; i < segs_.size(); ++i) {
		const open_segment& s = *segs_[i];
		if (!m_.segments.count(s.ref.id)) {
			e.segments_added.push_back(entry_of(s, fig[i]));
		} else if (!fig[i].fresh.empty()) {
			e.deletes.push_back({s.ref.id, fig[i].fresh, fig[i].count, fig[i].length});
		}
	}
	for (const auto& k : meta_since_) {
		const auto c = meta_.find(k);
		const auto d = m_.metadata.find(k);
		if (c != meta_.end()) {
			if (d == m_.metadata.end() || d->second != c->second) { e.metadata_set.emplace_back(k, c->second); }
		} else if (d != m_.metadata.end()) {
			e.metadata_removed.push_back(k);
		}
	}
	if (fields_.size() > m_.fields.size()) {
		e.fields_from = m_.fields.size();
		for (std::size_t i = m_.fields.size(); i < fields_.size(); ++i) { e.fields.push_back(fields_[i]); }
	}
	return e;
}

template <class Layout>
files::log_state basic_index<Layout>::state_now(std::uint64_t g, const files::log_detail::scalar_values& target, std::vector<std::uint64_t> pins, const std::vector<live_figures>& fig) const {
	files::log_state c;
	c.uuid = cur_.uuid;
	c.generation = g;
	for (unsigned i = 0; i < files::log_scalar::count; ++i) { c.*files::log_scalars[i] = target[i]; }
	c.pins = std::move(pins);
	for (std::size_t i = 0; i < segs_.size(); ++i) { c.segments.emplace_hint(c.segments.end(), segs_[i]->ref.id, entry_of(*segs_[i], fig[i])); }
	c.metadata = meta_;
	c.fields = fields_;
	c.attributes = attributes_owed_ ? *attributes_owed_ : m_.attributes;
	files::recount(c);
	return c;
}

template <class Layout>
void basic_index<Layout>::commit_files() {
	// A segment whose documents are all deleted leaves without a merge; retention frees
	// its files.
	if (std::erase_if(segs_, [](const auto& s) { return s->dead == s->ref.doc_count; })) { changed(); }
	for (const auto& s : segs_) {
		if (!s->facts_known) { fence("strata: commit: a segment's facts are unknown"); }
	}
	// The log lists segments by increasing id, and its reader refuses any other order: a
	// regression fences before anything is written, whichever caller commits.
	for (std::size_t i = 1; i < segs_.size(); ++i) {
		if (segs_[i]->ref.id <= segs_[i - 1]->ref.id) { fence("strata: commit: segments out of id order"); }
	}
	std::vector<kronuz::journal::File*> files;
	for (auto& s : segs_) {
		if (s->unsynced) { files.push_back(s->unsynced.get()); }
	}
	const std::vector<live_figures> fig = figures_now();
	const auto target = target_scalars(fig);
	std::vector<std::uint64_t> pins = target_pins();
	reopen_log();
	const bool forced = rollover_owed_ || !appender_;
	std::optional<files::log_edit> edit;
	std::string record;
	bool roll = forced;
	files::log_checked checked;
	if (!forced) {
		edit = edit_now(target, pins, fig);
		try {
			checked = files::check_edit(m_, *edit);  // the writer's edit is held to the reader's checks
		} catch (const corrupt_index& e) {
			fence(std::string("strata: commit: an edit the log refuses: ") + e.what());
		}
		record = files::encode_edit(*edit);
		roll = !files::log_within(checked.after, appender_->end_after(record.size()), log_edits_ + 1);
	}
	if (!roll || !roll_over(files, state_now(std::max(next_generation_, m_.generation + 1), target, pins, fig), forced)) {
		// The writer holds the reader's bound: past it, ENOSPC is the caller's, not an edit.
		if (roll && !files::log_within(checked.after, appender_->end_after(record.size()), log_edits_ + 1, 2)) {
			throw space_error(std::make_error_code(std::errc::no_space_on_device), "strata: commit: no space for the manifest log's rollover");
		}
		append_edit(files, *edit, record);
	}
	// Durable: the index's state follows the head, or the writer fences (SR4).
	durable([&] {
		for (std::size_t i = 0; i < segs_.size(); ++i) {
			segs_[i]->unsynced.reset();
			segs_[i]->ref.live_count = fig[i].count;  // only the fields a commit changes: a merge reads the others
			segs_[i]->ref.live_length = fig[i].length;
			segs_[i]->deletes_dirty = false;
		}
		deleted_since_.clear();
		meta_since_.clear();
		applied_since_commit_ = 0;
		pending_pin_.reset();
		pending_unpin_.reset();
		drop_pins_ = false;
		attributes_owed_.reset();
		kept_files_[m_.generation] = names_of(m_, log_g_);
	});
}

template <class Layout>
template <class F>
void basic_index<Layout>::durable(F&& f) {
	try {
		f();
	} catch (const index_fenced&) {
		throw;
	} catch (const std::exception& e) {
		fenced_ = true;  // first: the message's allocation may fail too
		fence(std::string("strata: durability failure: ") + e.what());
	}
}

template <class Layout>
bool basic_index<Layout>::roll_over(std::vector<kronuz::journal::File*> files, files::log_state c, bool forced) {
	const std::uint64_t g = c.generation;
	next_generation_ = g + 1;  // a failed attempt's name is never reused
	const std::string name = index_detail::manifest_name(g), tmp = name + ".tmp";
	const std::string bytes = files::frame_snapshot(c);
	std::unique_ptr<kronuz::journal::File> log;
	try {
		log = write_unsynced(tmp, bytes);
	} catch (const space_error&) {
		if (forced) { throw; }
		rollover_owed_ = true;  // the allowance holds S from now on (D8)
		return false;
	}
	files.push_back(log.get());
	durable([&] {
		io_->order_files_and_directory(files);
		io_->replace(tmp, name);
		files_.erase(tmp);
		files_[name] = bytes.size();
		sync_dir();
	});
	// Durable: the new log is the head's.
	appender_.reset();
	log_ = std::move(log);
	appender_.emplace(*log_, c.uuid, g, bytes.size());
	m_ = std::move(c);
	log_g_ = g;
	log_end_ = bytes.size();
	log_edits_ = 0;
	rollover_owed_ = false;
	return true;
}

template <class Layout>
void basic_index<Layout>::commit_empty() {
	files::log_state c = m_;
	c.generation = std::max(next_generation_, m_.generation + 1);
	c.next_segment = cur_.next_segment;
	c.configured = c.mask = cur_.configured;
	files::recount(c);
	roll_over({}, std::move(c), true);
	kept_files_[m_.generation] = names_of(m_, log_g_);
}

template <class Layout>
void basic_index<Layout>::append_edit(std::vector<kronuz::journal::File*>& files, const files::log_edit& e, const std::string& record) {
	// The inherited tail and names go ahead of the edit: one record in flight (D3).
	if (inherited_owed_) { files.push_back(log_.get()); }
	if (!files.empty()) { io_->order_files_and_directory(files); }
	try {
		appender_->write(record);
	} catch (const std::system_error& x) {
		if (x.code().value() == ENOSPC && !appender_->broken()) { throw space_error(x.code(), x.what()); }
		throw;
	} catch (const std::exception& x) {
		if (appender_->broken()) {
			fenced_ = true;  // first: the message's allocation may fail too
			fence(std::string("strata: durability failure: ") + x.what());
		}
		throw;  // truncated back: the index is as it was
	}
	durable([&] { appender_->sync(); });
	inherited_owed_ = false;  // ordered ahead of the edit, durable with it
	try {
		log_end_ = appender_->end();
		track(index_detail::manifest_name(log_g_), log_end_);
		files::apply(m_, e);
	} catch (...) {
		fenced_ = true;
		fence("strata: commit: the head cannot follow its durable edit");
	}
	++log_edits_;
}

template <class Layout>
void basic_index<Layout>::reopen_log() {
	if (appender_ || !log_g_ || rollover_owed_) { return; }
	log_ = io_->open_existing(index_detail::manifest_name(log_g_));
	appender_.emplace(*log_, m_.uuid, log_g_, log_end_);
}

template <class Layout>
std::uint64_t basic_index<Layout>::live_length(const open_segment& s) const {
	std::uint64_t n = 0;
	const segment& sg = s.mapped();  // prepare_commit mapped it (D9)
	for (ordinal_t o = 0; o < s.ref.doc_count; ++o) {
		if (!((s.deleted[o >> 6] >> (o & 63)) & 1)) { n += sg.length(o); }
	}
	return n;
}

}  // namespace strata
