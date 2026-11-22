// basic_index's storage accounting and reclamation: the permits, obsolete files' deletes, and drains
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
std::uint64_t basic_index<Layout>::reclaim(std::uint64_t bytes) {
	const writer_scope leased{*this};
	live();
	if (free_views() || retain_owed_) { retain(true); }
	return reclaim_files(bytes);
}

template <class Layout>
bool basic_index<Layout>::reclaim_for_space() {
	const writer_scope leased{*this};
	live();
	if (std::exchange(drain_asked_, false)) { drain_ran_ = true; }
	return drain();
}

template <class Layout>
void basic_index<Layout>::wait_deletes() {
	const writer_scope leased{*this};
	live();
	finish_deletes();
}

template <class Layout>
void basic_index<Layout>::run_delete(kronuz::journal::IO& io, delete_chan& ch, const std::string& name, std::uint64_t size, std::uint64_t step, const task::cancel_token& stop) {
	std::uint64_t left = size;
	try {
		if (stop.requested()) { throw std::system_error(ECANCELED, std::generic_category(), "strata: delete stopped"); }
		if (left > step) {
			auto f = io.open_existing(name);
			try {
				while (left > step) {
					if (stop.requested()) { throw std::system_error(ECANCELED, std::generic_category(), "strata: delete stopped"); }
					f->truncate(left - step);
					left -= step;
				}
				f->sync();
			} catch (...) {
				try {
					left = f->size();
				} catch (const std::system_error&) {
				}
				throw;
			}
		}
		io.remove(name);
		ch.report(name, std::nullopt);
	} catch (const std::system_error& e) {
		ch.report(name, e.code().value() == ENOENT ? std::nullopt : std::optional<std::uint64_t>(left));
	} catch (...) {
		ch.report(name, left);
	}
}

template <class Layout>
void basic_index<Layout>::collect_deletes() {
	if (!del_chan_ || !del_chan_->ready.load(std::memory_order_acquire)) { return; }
	std::vector<std::string> done;
	std::vector<failed_delete> failed;
	{
		const std::lock_guard lock(del_chan_->mu);
		done.swap(del_chan_->done);
		failed.swap(del_chan_->failed);
		del_chan_->ready.store(0, std::memory_order_relaxed);
	}
	for (const auto& name : done) {
		if (auto it = posted_.find(name); it != posted_.end()) {
			inflight_ -= it->second;
			unlinked_ += it->second;
			posted_.erase(it);
		}
	}
	for (auto it = failed.rbegin(); it != failed.rend(); ++it) { requeue_delete(it->name, it->left); }
}

template <class Layout>
void basic_index<Layout>::requeue_delete(const std::string& name, std::uint64_t left) {
	const auto it = posted_.find(name);
	if (it == posted_.end()) { return; }
	inflight_ -= it->second;
	posted_.erase(it);
	track(name, left);
	reclaim_.push_front(name);
	queued_.insert(name);
	debt_ += left;
}

template <class Layout>
void basic_index<Layout>::finish_deletes() {
	if (!del_group_) { return; }
	del_group_->wait();
	collect_deletes();
	const std::vector<std::pair<std::string, std::uint64_t>> dropped(posted_.begin(), posted_.end());
	for (const auto& [name, size] : dropped) { requeue_delete(name, size); }
}

template <class Layout>
void basic_index<Layout>::end_deletes() noexcept {
	if (del_group_) {
		try {
			del_group_->wait();
		} catch (...) {
		}
	}
	del_group_.reset();
}

template <class Layout>
std::uint64_t basic_index<Layout>::reclaim_posted(std::uint64_t bytes) {
	collect_deletes();
	std::uint64_t freed = 0;
	while (!reclaim_.empty() && freed < bytes) {
		const std::string name = std::move(reclaim_.front());
		reclaim_.pop_front();
		queued_.erase(name);
		std::uint64_t size = 0;
		if (auto it = files_.find(name); it != files_.end()) {
			size = it->second;
			on_disk_ -= size;
			files_.erase(it);
		}
		inflight_ += size;
		posted_[name] = size;
		freed += size;
		try {
			del_group_->post([chan = del_chan_, io = io_, name, size, step = opts_.reclaim_step](const task::cancel_token& stop) { run_delete(*io, *chan, name, size, step, stop); });
		} catch (const std::exception&) {
			requeue_delete(name, size);  // not posted: the file stays queued for a later round
			freed -= size;
			break;
		}
	}
	debt_ -= std::min(debt_, freed);
	if (reclaim_.empty()) { debt_ = 0; }
	settle_storage();
	return freed;
}

template <class Layout>
std::uint64_t basic_index<Layout>::reclaim_files(std::uint64_t bytes) {
	if (del_group_) { return reclaim_posted(bytes); }
	std::uint64_t freed = 0;
	while (!reclaim_.empty() && freed < bytes) {
		const std::string name = reclaim_.front();
		auto it = files_.find(name);
		std::uint64_t size = it == files_.end() ? 0 : it->second;
		std::unique_ptr<kronuz::journal::File> f;
		std::uint64_t cut = 0;
		try {
			if (size > opts_.reclaim_step) {
				// Truncated bytes count as free once the file's sync makes the truncation durable.
				f = io_->open_existing(name);
				while (size > opts_.reclaim_step && freed + cut < bytes) {
					size -= opts_.reclaim_step;
					f->truncate(size);
					track(name, size);
					truncated_ += opts_.reclaim_step;
					cut += opts_.reclaim_step;
				}
				f->sync();
				truncated_ -= cut;
				freed += cut;
				if (size > opts_.reclaim_step) { break; }
			}
			remove_file(name);
		} catch (const std::system_error& e) {
			// A failed round releases what it counted as truncated; the file's size is
			// whatever the truncations left.
			if (cut) {
				truncated_ -= cut;
				try {
					track(name, f->size());
				} catch (const std::system_error&) {
				}
			}
			// The commits stand; the file stays queued for a later round, unless it is gone.
			if (e.code().value() != ENOENT) { break; }
			forget_file(name);
		}
		freed += size;
		queued_.erase(name);
		reclaim_.pop_front();
	}
	debt_ -= std::min(debt_, freed);
	if (reclaim_.empty()) { debt_ = 0; }
	settle_storage();
	return freed;
}

template <class Layout>
void basic_index<Layout>::track(const std::string& name, std::uint64_t size) {
	auto& at = files_[name];
	on_disk_ += size - at;
	at = size;
}

template <class Layout>
void basic_index<Layout>::remove_file(const std::string& name) {
	io_->remove(name);
	forget_file(name);
}

template <class Layout>
void basic_index<Layout>::forget_file(const std::string& name) {
	if (auto it = files_.find(name); it != files_.end()) {
		on_disk_ -= it->second;
		unlinked_ += it->second;
		files_.erase(it);
	}
}

template <class Layout>
void basic_index<Layout>::sync_dir() {
	if (inherited_owed_) { return sync_inherited(); }
	io_->sync_directory();
	unlinked_ = 0;
}

template <class Layout>
void basic_index<Layout>::sync_inherited() {
	durable([&] {
		std::unique_ptr<kronuz::journal::File> opened;  // a demotion closed the log
		kronuz::journal::File* f = log_.get();
		if (!f) { f = (opened = io_->open_existing(index_detail::manifest_name(log_g_))).get(); }
		io_->sync_files_and_directory({&f, 1});
	});
	unlinked_ = 0;
	inherited_owed_ = false;
}

template <class Layout>
bool basic_index<Layout>::inherited(const std::string& name) const {
	const auto p = index_detail::parse_name(name);
	return p && (p->type == index_detail::parsed_name::manifest || (p->type == index_detail::parsed_name::segment && p->a < inherited_below_));
}

template <class Layout>
std::uint64_t basic_index<Layout>::commit_allowance() const {
	std::uint64_t n = 256 + 64 * files::log_scalar::count + 10 * deleted_since_.size();
	for (const auto& s : segs_) {
		if (!m_.segments.count(s->ref.id)) { n += 96 + (s->ref.doc_count + 7) / 8; }
	}
	for (const auto& k : meta_since_) {
		const auto it = meta_.find(k);
		n += 20 + k.size() + (it != meta_.end() ? it->second.size() : 0);
	}
	if (fields_.size() > m_.fields.size()) { n += 32 + 8 * (fields_.size() - m_.fields.size()) + fields_.name_bytes() - m_.fields.name_bytes(); }
	n += (n / 426 + 2) * 512;  // framing: a block header and a fragment per 426 bytes, padding
	if (rollover_owed_) { n += files::snapshot_bytes(m_) + n; }  // a demoted log's descriptor reopens to append
	return n;
}

template <class Layout>
bool basic_index<Layout>::drain_pending() {
	collect_deletes();
	return retain_owed_ || !reclaim_.empty() || unlinked_ || inflight_;  // an open's retention runs in the drain
}

template <class Layout>
bool basic_index<Layout>::drain() {
	collect_deletes();
	if (retain_owed_) { retain(); }
	if (reclaim_.empty() && !unlinked_ && !inflight_) { return false; }
	reclaim();
	finish_deletes();
	if (unlinked_) {
		try {
			sync_dir();
		} catch (const std::system_error&) {
			return false;  // the removals stay counted; a later sync frees them
		}
		settle_storage();
	}
	return true;
}

template <class Layout>
void basic_index<Layout>::settle_storage() {
	if (!storage_permit_) { return; }
	allowance_ = commit_allowance();
	const std::uint64_t want = on_disk_ + unlinked_ + inflight_ + truncated_ + reserved_ + allowance_ + held_;
	if (want > storage_permit_.bytes()) {
		storage_permit_.grow(want - storage_permit_.bytes());
	} else {
		storage_permit_.shrink(storage_permit_.bytes() - want);
	}
}

template <class Layout>
std::uint64_t basic_index<Layout>::estimate_bytes(const transaction& t) {
	std::uint64_t n = 64;
	for (const auto& o : t.ops) {
		n += 16 + o.key.size() + o.meta_key.size() + o.meta_value.size();
		n += 64 + o.doc.disk_estimate();  // a per-document overhead, and the parts
	}
	return n;
}

}  // namespace strata
