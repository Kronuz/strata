// basic_index's lifecycle: create, open and its recovery, heal, warm (demote, promote) and close
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
auto basic_index<Layout>::create(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) -> basic_index {
	basic_index ix = create_deferred(io, opts, uuid);
	ix.commit();  // generation 1 (D3-C)
	return ix;
}

template <class Layout>
auto basic_index<Layout>::create_deferred(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) -> basic_index {
	basic_index ix(io);
	const lease_scope leased{ix.lease_};
	ix.lock_directory(opts);
	ix.clear_uncreated();
	ix.cur_.uuid = uuid;
	ix.m_.uuid = uuid;
	// Every structure the index's layout builds, keys only in a keyed index (F4, F20), and
	// index_sort's key order with keys in a key-ordered layout (format-14-kv D1a).
	ix.cur_.configured = segment::readable & ~(opts.keyed ? 0 : format::bit::keys) & ~format::bit::index_sort;
	if (segment_policy<Layout>::key_order && (ix.cur_.configured & format::bit::keys)) { ix.cur_.configured |= format::bit::index_sort; }
	ix.opts_ = opts;
	if (ix.opts_.defer_io) { ix.opts_.eager_segments = true; }  // a first-read map would be I/O inside apply()
	ix.seed_identities();
	if (opts.budget) { ix.permit_ = opts.budget->open(); }
	if (opts.storage) { ix.storage_permit_ = opts.storage->open(); }
	ix.start_merges();
	ix.attach_log(true);
	return ix;
}

template <class Layout>
auto basic_index<Layout>::open(kronuz::journal::IO& io, const options& opts, std::optional<std::uint64_t> to) -> basic_index {
	basic_index ix(io);
	const lease_scope leased{ix.lease_};
	ix.lock_directory(opts);
	ix.opts_ = opts;
	if (ix.opts_.defer_io) { ix.opts_.eager_segments = true; }  // a first-read map would be I/O inside apply()
	if (opts.budget) { ix.permit_ = opts.budget->open(); }
	if (opts.storage) { ix.storage_permit_ = opts.storage->open(); }
	ix.start_merges();
	ix.recover(to);
	ix.seed_identities();  // after recovery: a seeded base counts from the next generation
	ix.attach_log();
	return ix;
}

template <class Layout>
auto basic_index<Layout>::operator=(basic_index&& o) noexcept -> basic_index& {
	if (this != &o) {
		std::destroy_at(this);
		std::construct_at(this, std::move(o));
	}
	return *this;
}

template <class Layout>
basic_index<Layout>::~basic_index() {
	const lease_scope leased{lease_};
	detach_log();
	end_merges();
	if (pub_) {
		retire_current();
		free_views();
		if (!pub_->retired.empty()) {
			detail::owner_break("strata: an index destroyed while readers hold its views");
			pub_->retired.leak();
			for (auto& t : pub_->tables) { detail::keep_forever(std::move(t.second)); }  // their readers' tables too
			detail::keep_forever(std::shared_ptr<const kronuz::journal::OwnerLock>(std::move(lock_)));
		}
		pub_.reset();
	}
	segs_.clear();  // the segments drop here, under the lease
}

template <class Layout>
void basic_index<Layout>::close() {
	// Refused inside a flush or a scan's callback before anything is reset, and before the
	// lease: a flush's run thread may call it, and a refusal changes nothing.
	require_buffers_unpinned();
	if (scanning_) { throw std::logic_error("strata: the index was closed inside a scan's callback"); }
	const writer_scope leased{*this};
	if (closing()) { throw index_fenced("strata: the index is closing"); }
	leave();  // a failure throws before anything is torn down; the log keeps the group stored
	detach_log();
	end_merges();
	segs_.clear();
	clear_buffer();
	appender_.reset();
	log_.reset();
	permit_ = {};
	fenced_ = true;
	map_asked_ = false;
	if (pub_) {
		retire_current();
		free_views();
		if (!pub_->retired.empty()) {
			pub_->closing = true;
			return;
		}
	}
	storage_permit_ = {};
	lock_.reset();
}

template <class Layout>
bool basic_index<Layout>::try_finish_close(std::optional<std::uint64_t> deadline) {
	const writer_scope leased{*this};
	if (!closing()) {
		if (!fenced_) { throw std::logic_error("strata: try_finish_close() on an open index"); }
		return true;
	}
	free_views();
	if (pub_->retired.empty()) {
		pub_->closing = false;
		storage_permit_ = {};
		lock_.reset();
		return true;
	}
	if (deadline && exec_->now() >= *deadline) {
		std::vector<reader_registry::claim_time> mine;
		for (const auto& c : pub_->rs->registry->claims()) {
			pub_->retired.for_each([&](const view& v) {
				if (c.p == &v) { mine.push_back(c); }
			});
		}
		throw readers_active("strata: readers hold a closing index's views past the deadline", std::move(mine));
	}
	return false;
}

template <class Layout>
bool basic_index<Layout>::needs_heal() const noexcept {
	return std::any_of(segs_.begin(), segs_.end(), [](const auto& s) { return s->damaged || s->failed(); });
}

template <class Layout>
std::vector<std::uint64_t> basic_index<Layout>::heal() {
	const writer_scope leased{*this};
	live();
	std::vector<std::uint64_t> damaged;
	for (auto& s : segs_) {
		if (s->unsynced) {  // built in memory, never read from disk
			if (s->damaged) { damaged.push_back(s->ref.id); }
			continue;
		}
		if (!s->damaged && !s->failed() && !s->bodied()) { continue; }
		try {
			const segment& sg = s->seg();  // a marked record throws corrupt_segment
			if (s->damaged) {  // a merge found it inconsistent: the checksums alone do not clear it
				basic_check_report<Layout> r;
				check_segment(sg, &s->deleted, r);
			} else {
				sg.verify_all();
			}
			s->damaged = false;
			continue;
		} catch (const corrupt_segment&) {
		}
		try {
			auto fresh = read_checked([&] { return load_segment(*io_, vouch_of(m_), s->ref, true, load_); }, nullptr);
			if (s->damaged) {
				basic_check_report<Layout> r;
				check_segment(fresh->mapped(), &s->deleted, r);
			}
			// The record's own state carries over; the facts are the fresh body's.
			fresh->deleted = std::move(s->deleted);
			fresh->dead = s->dead;
			fresh->deletes_dirty = s->deletes_dirty;
			fresh->ref = s->ref;
			s = std::move(fresh);  // a view keeps the failing object until it is freed (D11)
			load_->retries.fetch_add(1, std::memory_order_relaxed);
			changed();
		} catch (const corrupt_index&) {
			damaged.push_back(s->ref.id);
		} catch (const corrupt_segment&) {
			damaged.push_back(s->ref.id);
		} catch (const std::system_error&) {
			damaged.push_back(s->ref.id);
		}
	}
	publish_point();
	return damaged;
}

template <class Layout>
bool basic_index<Layout>::demote() {
	const writer_scope leased{*this};
	live();
	if (!warm_quiescent()) { return false; }
	std::vector<std::shared_ptr<open_segment>> twins;
	twins.reserve(segs_.size());
	for (auto& s : segs_) { twins.push_back(s->twin()); }
	const bool owed = opts_.defer_io && std::any_of(twins.begin(), twins.end(), [](const auto& t) { return !t->failed(); });
	segs_ = std::move(twins);
	appender_.reset();  // warm holds the directory alone: the next commit opens the log again
	log_.reset();
	if (owed) { promote_owed_.store(true, std::memory_order_release); }
	changed();
	publish_point();  // the new view over the twins; it frees the retired views no reader holds
	return true;
}

template <class Layout>
bool basic_index<Layout>::close_ready() {
	const writer_scope leased{*this};
	live();
	if (!warm_quiescent() || applied_since_commit_ != 0) { return false; }
	collect_deletes();
	return ledger_.posted().empty() && reclaim_.empty() && debt_ == 0;
}

template <class Layout>
void basic_index<Layout>::promote() {
	const writer_scope leased{*this};
	live();
	if (!promote_owed_.load(std::memory_order_acquire)) { return; }
	for (const auto& s : segs_) {
		if (s->bodied()) { continue; }
		try {
			(void)s->seg();
		} catch (const corrupt_segment&) {  // marked, by this map or a reader's: done (D9)
		}
	}
	promote_owed_.store(false, std::memory_order_release);
}

template <class Layout>
bool basic_index<Layout>::warm_quiescent() const noexcept {
	if (buffered_records() != 0 || !pending_.empty() || map_asked_) { return false; }
	return std::none_of(segs_.begin(), segs_.end(), [](const auto& s) { return bool(s->unsynced); });
}

template <class Layout>
bool basic_index<Layout>::uncreated(const std::vector<std::string>& names) {
	for (const auto& n : names) {
		auto p = index_detail::parse_name(n);
		if (p && !(p->a == 1 && p->type == index_detail::parsed_name::manifest_tmp)) { return false; }
	}
	return true;
}

template <class Layout>
void basic_index<Layout>::check_directory_trust(const options& opts) {
	const auto trust = io_->directory_trust();
	if (trust.trusted()) { return; }
	if (opts.directory_trust == options::trust_policy::refuse) {
		throw std::system_error(std::make_error_code(std::errc::permission_denied),
			!trust.known ? "strata: the directory's ownership and mode are unknown"
			: trust.foreign_owner ? "strata: the directory is owned by another user" : "strata: the directory is writable by group or others");
	}
	trust_warning_ = strata::directory_trust_warning{trust.known, trust.foreign_owner, trust.shared_writable};
}

template <class Layout>
void basic_index<Layout>::lock_directory(const options& opts) {
	check_directory_trust(opts);
	const bool refuse = opts.nonlocal == options::nonlocal_policy::refuse;
	kronuz::journal::FilesystemClass fs;
	try {
		fs = io_->directory_filesystem();
	} catch (const std::system_error& e) {
		if (refuse) { throw std::system_error(e.code(), std::string("strata: the directory's filesystem is unknown: ") + e.what()); }
		fs = {false, std::string("unknown (") + e.what() + ")"};
	}
	const bool fuse = fs.name.starts_with("fuse") || fs.name.starts_with("macfuse") || fs.name.starts_with("osxfuse");
	if (!fs.local && refuse && !fuse) {
		throw std::system_error(std::make_error_code(std::errc::not_supported), "strata: " + fs.name + " is not a known-local filesystem");
	}
	try {
		lock_ = io_->acquire_directory_owner();
	} catch (const std::system_error& e) {
		if (e.code() != std::errc::not_supported || refuse) { throw; }  // unlocked: refuse's to stop
		fs_warning_ = strata::filesystem_warning{fs.name, fs.local, false};
		return;
	}
	if (!fs.local) { fs_warning_ = strata::filesystem_warning{fs.name, false, true}; }
}

template <class Layout>
void basic_index<Layout>::clear_uncreated() {
	std::vector<std::string> names;
	{
		auto cursor = io_->scan_directory();
		while (auto n = cursor->next()) { names.push_back(*n); }
	}
	if (!uncreated(names)) { throw std::system_error(std::make_error_code(std::errc::file_exists), "strata: the directory holds an index"); }
	bool removed = false;
	for (const auto& n : names) {
		if (index_detail::parse_name(n)) {
			io_->remove(n);
			removed = true;
		}
	}
	if (removed) { sync_dir(); }
}

template <class Layout>
void basic_index<Layout>::recover(std::optional<std::uint64_t> to) {
	std::vector<std::string> names;
	{
		auto cursor = io_->scan_directory();
		while (auto n = cursor->next()) { names.push_back(*n); }
	}
	std::uint64_t top = 0, max_segment = 0;
	std::vector<std::uint64_t> logs;
	for (const auto& n : names) {
		auto p = index_detail::parse_name(n);
		if (!p) { continue; }
		ledger_.track(n, read_checked([&] { return io_->open_existing(n)->size(); }, &load_->retries));  // an EIO twice is damage
		switch (p->type) {
		case index_detail::parsed_name::manifest:
			logs.push_back(p->a);
			top = std::max(top, p->a);
			break;
		case index_detail::parsed_name::manifest_tmp: top = std::max(top, p->a); break;
		case index_detail::parsed_name::segment:
		case index_detail::parsed_name::spill:
		case index_detail::parsed_name::scratch: max_segment = std::max(max_segment, p->a); break;
		}
	}
	if (logs.empty()) {
		if (uncreated(names)) { throw index_not_created("strata: no index was committed here"); }
		throw corrupt_index("no manifest log");
	}
	std::sort(logs.begin(), logs.end());
	std::uint64_t chosen = logs.back();
	if (to) {
		const auto it = std::upper_bound(logs.begin(), logs.end(), *to);
		if (it == logs.begin()) { throw files::generation_not_retained(*to); }
		chosen = *std::prev(it);
	}
	auto read = [&](std::uint64_t g, std::optional<std::uint64_t> upto) {
		return read_checked([&] { return files::read_log(index_detail::read_all(*io_->open_existing(index_detail::manifest_name(g))), g, readable(), upto); }, &load_->retries);
	};
	files::log_read lr = read(chosen, to);
	m_ = std::move(lr.state);
	check_readable(m_.configured);
	// Lazy: the names and sizes the scan recorded stand for the segment files (cold-tier D11).
	segs_ = load_files(*io_, m_, false, load_, opts_.eager_segments ? nullptr : &ledger_.files());
	meta_ = m_.metadata;
	fields_ = m_.fields;
	cur_ = {m_.uuid, m_.last_docid, std::max(m_.next_segment, max_segment + 1), m_.schema_version, m_.configured, {m_.applied_index, m_.applied_count}};
	if (opts_.keyed && !(cur_.configured & format::bit::keys)) { throw std::invalid_argument("strata: the index is keyless"); }
	log_g_ = chosen;
	log_end_ = lr.end;
	log_edits_ = lr.edits;
	// A log that does not end at its last whole record replayed (a torn tail, or records past
	// `to`) is never appended to: the next commit rolls over (D6.4, D7), skipping the torn
	// record's generation (D6.7). Counters start above every name on disk (D3).
	rollover_owed_ = lr.dirty || lr.end != ledger_.size_of(index_detail::manifest_name(chosen));
	next_generation_ = std::max(m_.generation + 1 + (rollover_owed_ ? 1 : 0), top + 1);
	std::optional<std::uint64_t> newest;
	if (to) {
		// The newest generation the logs hold whole, read again when `to` is below it.
		try {
			newest = chosen == logs.back() && lr.end == ledger_.size_of(index_detail::manifest_name(chosen)) ? m_.generation : read(logs.back(), std::nullopt).state.generation;
		} catch (const files::log_damage& e) {
			newest = e.generation;  // the damaged edit was a commit, now lost (Q2: "damage at generation x")
		} catch (const std::exception&) {
			newest = top;
		}
	}
	if (newest && *newest > m_.generation) {
		fallback_ = fallback_report{*to, *newest, *newest - *to};
		rollover_owed_ = true;
		next_generation_ = std::max(next_generation_, *newest + 1);  // above every generation the logs hold
		prepare_commit();  // an open's failure to map fails the open
		commit_files();
	} else {
		// A crash's unsynced edit or rollover rename stays owed until the first commit or
		// directory sync (sync-at-first-commit D3, D4).
		log_ = io_->open_existing(index_detail::manifest_name(chosen));
		inherited_owed_ = true;
		inherited_below_ = cur_.next_segment;
		appender_.emplace(*log_, m_.uuid, log_g_, log_end_);
	}
	kept_files_[m_.generation] = names_of(m_, log_g_);
	retain_owed_ = true;
	// Orphans leave only after a successful open, under the owner lock; their removal is durable
	// at the next directory sync (D8).
	for (const auto& n : names) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest_tmp) { remove_file(n); }
	}
	settle_storage();  // the permit holds the files on record and the commit allowance
}

template <class Layout>
template <class F>
auto basic_index<Layout>::read_checked(F&& f, std::atomic<std::uint64_t>* retries) -> decltype(f()) {
	try {
		return f();
	} catch (const corrupt_index&) {
		if (retries) { retries->fetch_add(1, std::memory_order_relaxed); }
	} catch (const corrupt_segment&) {
		if (retries) { retries->fetch_add(1, std::memory_order_relaxed); }
	} catch (const std::system_error& e) {
		if (retries && e.code().value() == EIO) { retries->fetch_add(1, std::memory_order_relaxed); }  // the host's errors are not the medium's
	}
	try {
		return f();
	} catch (const std::system_error& e) {
		// Only a read error is the medium's; a lack of descriptors, memory, or permission is
		// the host's, and a healthy shard must not read as damaged.
		if (e.code().value() != EIO) { throw; }
		throw corrupt_index(std::string("unreadable: ") + e.what());
	}
}

template <class Layout>
void basic_index<Layout>::check_readable(std::uint64_t configured) {
	for (format::structure_bits b = configured; b; b &= b - 1) {
		const format::structure_bits owner = format::requires_of(b & -b);
		if (owner && !(configured & owner)) { throw corrupt_index("configured structure bit " + std::to_string(std::countr_zero(b)) + " without its owner"); }
	}
}

template <class Layout>
files::segment_ref basic_index<Layout>::ref_of(const files::log_segment& s) {
	files::segment_ref r;
	r.id = s.id;
	r.identity = s.identity;
	r.size = s.size;
	r.footer_crc = s.footer_crc;
	r.doc_count = s.doc_count;
	r.live_count = s.live_count;
	r.live_length = s.live_length;
	r.values_generation = s.values_generation;
	r.attributes = s.attributes;
	return r;
}

template <class Layout>
auto basic_index<Layout>::load_files(kronuz::journal::IO& io, const files::log_state& m, bool whole,
	const std::shared_ptr<load_context>& load, const std::map<std::string, std::uint64_t>* sizes) -> std::vector<std::shared_ptr<open_segment>> {
	std::vector<std::shared_ptr<open_segment>> out;
	out.reserve(m.segments.size());
	for (const auto& [id, seg] : m.segments) {
		const files::segment_ref r = ref_of(seg);
		std::shared_ptr<open_segment> s;
		if (sizes) {
			s = read_checked([&] { return load_record(r, vouch_of(m), *sizes, load); }, &load->retries);
		} else {
			s = read_checked([&] { return load_segment(io, vouch_of(m), r, whole, load); }, &load->retries);
		}
		fill_deleted(seg.deleted, *s);
		out.push_back(std::move(s));
	}
	return out;
}

template <class Layout>
void basic_index<Layout>::fill_deleted(const files::log_deleted& d, open_segment& s) {
	if (d.empty()) { return; }
	std::uint64_t* bits = s.deleted.mutable_words();
	if (d.dense()) {
		std::memcpy(bits, d.words().data(), d.words().size() * sizeof(std::uint64_t));  // words_for(doc_count) of them, as the record's
	} else {
		d.for_each([&](std::uint64_t o) { bits[o >> 6] |= std::uint64_t(1) << (o & 63); });
	}
	s.dead = d.size();
}

template <class Layout>
auto basic_index<Layout>::load_segment(kronuz::journal::IO& io, const vouch& v, const files::segment_ref& r, bool whole, const std::shared_ptr<load_context>& load) -> std::unique_ptr<open_segment> {
	auto s = std::make_unique<open_segment>(r, load, v);
	s->adopt(open_body(io, r, s->claim, whole));
	return s;
}

template <class Layout>
auto basic_index<Layout>::load_record(const files::segment_ref& r, const vouch& v, const std::map<std::string, std::uint64_t>& sizes,
	const std::shared_ptr<load_context>& load) -> std::unique_ptr<open_segment> {
	const std::string name = index_detail::segment_name(r.id);
	const auto it = sizes.find(name);
	if (it == sizes.end()) { throw corrupt_index("missing file: " + name); }
	if (it->second != r.size) { throw corrupt_index("file size differs from the manifest: " + name); }
	return std::make_unique<open_segment>(r, load, v);
}

template <class Layout>
auto basic_index<Layout>::open_body(kronuz::journal::IO& io, const files::segment_ref& r, const vouch& v, bool whole) -> std::unique_ptr<segment_body> {
	try {
		return open_body_of(io, r, v, whole);
	} catch (const unsupported_segment& e) {
		throw corrupt_segment(e.what());
	}
}

template <class Layout>
auto basic_index<Layout>::open_body_of(kronuz::journal::IO& io, const files::segment_ref& r, const vouch& v, bool whole) -> std::unique_ptr<segment_body> {
	const std::string name = index_detail::segment_name(r.id);
	std::unique_ptr<kronuz::journal::File> f;
	try {
		f = io.open_existing(name);
	} catch (const std::system_error& e) {
		if (e.code().value() != ENOENT) { throw; }
		throw corrupt_index("missing file: " + name);
	}
	auto mapped = f->map();
	std::string bytes;
	if (!mapped) { bytes = index_detail::read_all(*f); }
	const std::string_view view = mapped ? mapped->bytes() : std::string_view(bytes);
	if (view.size() != r.size) { throw corrupt_index("file size differs from the manifest: " + name); }
	(void)format::read_version(view);
	if (format::decode_footer(view).toc_crc != r.footer_crc) { throw corrupt_index("segment differs from its manifest entry"); }
	// The header's identity is the entry's (I13, F13-R41): a file of another producer under
	// this name is never taken for the one the manifest names.
	if (std::memcmp(view.data() + format::identity_offset, r.identity.data(), 16) != 0) { throw corrupt_index("segment identity differs from its manifest entry"); }
	auto b = mapped ? std::make_unique<segment_body>(std::move(mapped)) : std::make_unique<segment_body>(std::move(bytes));
	if (b->seg.core().doc_count() != r.doc_count) { throw corrupt_index("segment document count"); }
	// The manifest vouches for its segments: one outside its mask or version range is damage.
	const std::uint32_t ver = b->seg.core().version();
	if ((b->seg.core().structures() & ~v.mask) || ver < v.min_version || ver > v.max_version) {
		throw corrupt_segment("segment outside its manifest's structures or versions");
	}
	if (whole) { b->seg.verify_all(); }
	return b;
}

}  // namespace strata
