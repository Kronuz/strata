// basic_index's checkpoints and retention: pins, install from another index, and the files each kept generation keeps
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
std::uint64_t basic_index<Layout>::checkpoint() {
	const writer_scope leased{*this};
	live();
	commit();
	const std::uint64_t generation = m_.generation;
	// The pin lives in the next manifest; committing again makes it durable.
	pending_pin_ = generation;
	commit();
	return generation;
}

template <class Layout>
void basic_index<Layout>::release(std::uint64_t generation) {
	const writer_scope leased{*this};
	live();
	pending_unpin_ = generation;
	commit();
}

template <class Layout>
void basic_index<Layout>::install(kronuz::journal::IO& source, std::uint64_t generation) {
	const writer_scope leased{*this};
	live();
	const files::log_state theirs = read_generation(source, generation);
	check_readable(theirs.configured);
	auto got = load_files(source, theirs, true, load_);
	// The copy, and the rollover's snapshot of it.
	std::uint64_t need = files::snapshot_bytes(theirs) + 160 * theirs.segments.size();
	for (const auto& s : got) { need += s->ref.size; }
	if (storage_permit_ && !storage_permit_.try_grow(need) && !(drain() && storage_permit_.try_grow(need))) {
		throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for an install");
	}
	hold held{this};
	if (storage_permit_) { held.add(need); }
	abandon_merges();  // their sources leave with the old content
	std::vector<files::segment_ref> mines;
	guarded([&] {
		for (auto& s : got) {
			files::segment_ref mine = s->ref;
			mine.id = cur_.next_segment++;
			write_new(index_detail::segment_name(mine.id), s->bytes());  // loaded whole: its body is here
			mines.push_back(mine);
		}
	});
	// Every copy is written and synced and nothing here has changed, so a host error reopening
	// one (EMFILE, ENOMEM) leaves the index as it was, unfenced; the copies are orphans.
	for (std::size_t i = 0; i < got.size(); ++i) {
		auto& s = got[i];
		if (!opts_.eager_segments) {  // an unmapped record with the facts of the incoming body (cold-tier D9)
			auto mine = std::make_unique<open_segment>(mines[i], load_, vouch_of(theirs));
			mine->structures = s->structures;
			mine->version = s->version;
			mine->total_length = s->total_length;
			mine->facts_known = true;
			mine->deleted = std::move(s->deleted);
			mine->dead = s->dead;
			s = std::move(mine);
		} else if (s->body().mapping) {  // map the copy this index owns: the source may reclaim its file
			auto mine = read_checked([&] { return load_segment(*io_, vouch_of(theirs), mines[i], false, load_); }, &load_->retries);
			mine->deleted = std::move(s->deleted);
			mine->dead = s->dead;
			s = std::move(mine);
		} else {
			s->ref = mines[i];
		}
	}
	guarded([&] {
		segs_ = std::move(got);
		clear_buffer();
		reserved_ = 0;  // the discarded buffer's reservations
		if (permit_) { permit_.settle(); }
		meta_ = theirs.metadata;
		fields_ = theirs.fields;
		++fields_epoch_;
		cur_.uuid = theirs.uuid;
		cur_.last_docid = theirs.last_docid;
		cur_.schema_version = theirs.schema_version;
		cur_.applied = {theirs.applied_index, theirs.applied_count};
		cur_.configured = theirs.configured;
		// The content is replaced whole: a rollover, the pins dropped, the change list moot.
		rollover_owed_ = true;
		drop_pins_ = true;
		deleted_since_.clear();
		meta_since_.clear();
		applied_since_commit_ = 1;
		changed();
		commit_files();
	});
	durable([&] {  // the log moved, as after a commit
		publish_point();  // earlier views keep the old segments until freed (D11)
		free_views();
		retain();
	});
}

template <class Layout>
void basic_index<Layout>::retain(bool) {
	retain_owed_ = false;
	retention_blocked_.reset();
	if (!m_.generation) { return; }  // nothing committed yet
	std::map<std::uint64_t, bool> keep;  // generation, pinned
	for (std::uint64_t k = 1; k <= opts_.retained_predecessors && k < m_.generation; ++k) { keep[m_.generation - k] = false; }
	for (std::uint64_t p : m_.pins) { keep[p] = true; }
	keep[m_.generation] = true;
	std::erase_if(kept_files_, [&](const auto& e) { return !keep.count(e.first); });
	std::set<std::string> referenced;
	for (const auto& [g, pinned] : keep) {
		bool blocked = false;
		const auto* names = kept_files(g, pinned, blocked);
		if (blocked) {  // a kept generation it cannot read: remove nothing this round
			retention_blocked_ = g;
			return;
		}
		if (names) { referenced.insert(names->begin(), names->end()); }
	}
	// Flushed segments awaiting a commit are on disk but not yet named by a log.
	for (const auto& s : segs_) { referenced.insert(index_detail::segment_name(s->ref.id)); }
	// A pending merge's sources stay mapped until it publishes: none is truncated before then.
	for (const auto& job : pending_) {
		for (const auto& src : job->sources) { referenced.insert(index_detail::segment_name(src->ref.id)); }
	}
	// So do a view's until it is freed (D7): a file a reader maps is never truncated.
	for_each_view([&](const view& v) {
		for (const auto& sv : v.views_) { referenced.insert(index_detail::segment_name(sv.id)); }
	});
	std::vector<std::string> out;
	for (const auto& [n, size] : files_) {
		if (!referenced.count(n) && !queued_.count(n)) { out.push_back(n); }
	}
	// A file the open's head dropped leaves only once that head is durable (D4).
	if (inherited_owed_ && std::any_of(out.begin(), out.end(), [&](const std::string& n) { return inherited(n); })) { sync_inherited(); }
	for (auto& n : out) {
		queued_.insert(n);
		reclaim_.push_back(std::move(n));
	}
}

template <class Layout>
const std::vector<std::string>* basic_index<Layout>::kept_files(std::uint64_t g, bool pinned, bool& blocked) {
	if (auto it = kept_files_.find(g); it != kept_files_.end()) { return &it->second; }
	std::optional<std::uint64_t> holder;
	for (const auto& [n, size] : files_) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest && p->a <= g && (!holder || p->a > *holder)) { holder = p->a; }
	}
	if (!holder) { return nullptr; }
	try {
		auto f = io_->open_existing(index_detail::manifest_name(*holder));
		const files::log_read lr = files::read_log(index_detail::read_all(*f), *holder, readable(), g);
		return &(kept_files_[g] = names_of(lr.state, *holder));
	} catch (const files::generation_not_retained&) {
		blocked = pinned;
	} catch (const std::exception&) {
		blocked = true;
	}
	return nullptr;
}

template <class Layout>
std::vector<std::string> basic_index<Layout>::names_of(const files::log_state& s, std::uint64_t log) {
	std::vector<std::string> names{index_detail::manifest_name(log)};
	for (const auto& [id, seg] : s.segments) { names.push_back(index_detail::segment_name(id)); }
	return names;
}

}  // namespace strata
