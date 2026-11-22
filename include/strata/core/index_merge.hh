// basic_index's background merges: scheduling, the merge job, publication and abandonment
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
void basic_index<Layout>::merge(const std::vector<std::uint64_t>& ids) {
	const writer_scope leased{*this};
	live();
	apply_pending();  // the merge drops what the buffer deleted (CR5)
	publish_merges(true, false);
	schedule(ids);
	publish_merges(true, true);
}

template <class Layout>
void basic_index<Layout>::force_merge(std::size_t max_segments) {
	const writer_scope leased{*this};
	apply_pending();
	run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_merges(std::move(c), max_segments); }, true, true);
}

template <class Layout>
void basic_index<Layout>::force_merge_deletes() {
	const writer_scope leased{*this};
	apply_pending();
	run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_deletes_merges(std::move(c)); }, true);
}

template <class Layout>
void basic_index<Layout>::maybe_merge() {
	const writer_scope leased{*this};
	try {
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_merges(std::move(c)); }, false);
	} catch (const map_failure&) {
		// The round is skipped and counted; the next one retries. merge(), force_merge() and
		// force_merge_deletes() rethrow it, as the host asked for those (cold-tier D9).
		load_->skipped.fetch_add(1, std::memory_order_relaxed);
	} catch (const std::system_error& e) {
		if (e.code().value() != ENOSPC) { throw; }
	}
}

template <class Layout>
void basic_index<Layout>::wait_merges() {
	const writer_scope leased{*this};
	live();
	publish_merges(true, false);
}

template <class Layout>
template <class Find>
void basic_index<Layout>::run_merges(Find find, bool wait, bool forced) {
	live();
	if (wait) { publish_merges(true, false); }
	for (;;) {
		std::vector<merge_policy::candidate> c;
		for (const auto& s : segs_) {
			if (merging(*s) || s->damaged) { continue; }
			std::uint64_t deleted = 0;
			for (auto w : s->deleted) { deleted += std::uint64_t(std::popcount(w)); }
			c.push_back({s->ref.id, s->ref.size, s->ref.doc_count, deleted});
		}
		const auto merges = find(std::move(c));
		if (merges.empty()) { return; }
		for (const auto& ids : merges) { schedule(ids, forced); }
		if (!wait && !own_exec_) { return; }  // the index's own executor runs them now
		publish_merges(true, true);
	}
}

template <class Layout>
void basic_index<Layout>::start_merges() {
	exec_ = opts_.background;
	if (!exec_) {
		own_exec_ = std::make_unique<inline_executor>();
		exec_ = own_exec_.get();
	}
	group_ = exec_->open();
	if (!own_exec_) {
		runs_group_ = exec_->open();  // scan runs' builds, beside the merges (D9, D12)
		del_group_ = exec_->open();  // reclaim's deletes, apart from the merges
		del_chan_ = std::make_shared<delete_chan>();
	}
}

template <class Layout>
bool basic_index<Layout>::merging(const open_segment& s) const {
	for (const auto& job : pending_) {
		for (const auto& src : job->sources) {
			if (src.get() == &s) { return true; }
		}
	}
	return false;
}

template <class Layout>
void basic_index<Layout>::schedule(const std::vector<std::uint64_t>& ids, bool forced) {
	auto job = std::make_shared<merge_job>();
	job->reserved = segment_overhead;
	std::uint64_t live_docs = 0;
	for (const auto& s : segs_) {
		if (std::find(ids.begin(), ids.end(), s->ref.id) == ids.end() || merging(*s) || s->damaged) { continue; }
		// A map point, before anything is reserved (cold-tier D9): a source that fails its checks
		// is marked and left out; a host error drops the round.
		try {
			(void)s->seg();
		} catch (const corrupt_segment&) {
			s->damaged = true;
			continue;
		} catch (const std::system_error& e) {
			throw map_failure(e.code(), e.what());
		}
		const std::uint64_t docs = s->ref.doc_count;
		job->sources.push_back(s);
		job->deleted.emplace_back(s->deleted.begin(), s->deleted.end());
		job->reserved += s->ref.size + 18 * docs + 16 * (docs / format::block + 1);
		live_docs += docs - s->dead;
	}
	using mode = typename options::reorder_mode;
	job->reorder = (opts_.reordering == mode::force_merge && forced) || (opts_.reordering == mode::merges && (forced || live_docs >= opts_.reorder_min_docs));
	if (job->sources.empty()) { return; }
	if (storage_permit_) {
		job->storage = opts_.storage->open();
		if (!job->storage.try_grow(job->reserved) && !(drain() && job->storage.try_grow(job->reserved))) {
			throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for a merge");
		}
	}
	job->id = cur_.next_segment++;
	job->header = new_identity();
	pending_.reserve(pending_.size() + 1);
	const merge_env ctx{io_, exec_, opts_.rate, opts_.budget, opts_.reorder_settings, configured_structures(),
		opts_.zstd_merged ? std::optional<format::stored_codec>(format::zstd_codec(1)) : std::nullopt, load_};
	group_->post([job, ctx](const task::cancel_token& stop) { run_merge(*job, ctx, stop); });
	pending_.push_back(std::move(job));
}

template <class Layout>
void basic_index<Layout>::run_merge(merge_job& job, const merge_env& ctx, const task::cancel_token& stop) {
	try {
		if (stop.requested()) { throw task::cancelled{}; }
		std::uint64_t live = 0;
		for (std::size_t k = 0; k < job.sources.size(); ++k) {
			std::uint64_t dead = 0;
			for (auto w : job.deleted[k]) { dead += std::uint64_t(std::popcount(w)); }
			live += job.sources[k]->ref.doc_count - dead;
		}
		if (live == 0) {  // nothing to write: publication drops the sources
			job.done.store(true, std::memory_order_release);
			return;
		}
		job.memory = flush_charge(ctx.budget);
		if (!ctx.exec->checkpoint(stop)) { throw task::cancelled{}; }
		std::vector<basic_merge_source<Layout>> sources;
		for (std::size_t k = 0; k < job.sources.size(); ++k) { sources.push_back({&job.sources[k]->mapped(), &job.deleted[k]}); }  // scheduling mapped them (D9)
		const std::string name = index_detail::segment_name(job.id);
		auto file = ctx.io->create_exclusive(name);
		std::string bytes;
		std::shared_ptr<const kronuz::journal::Mapping> mapped;  // the output, mapped; else read into bytes
		merge_file out(*file, ctx, job, stop);
		try {
			const std::size_t chunk = ctx.rate ? ctx.rate->chunk() : std::size_t(1) << 20;
			std::uint64_t used = 0;
			merge_output sink(out, used, chunk);
			std::vector<merge_ref> order;
			// A key-ordered layout's merges order by key themselves and read each source in
			// order, so they take neither a reordering nor a records scratch (format-14-kv D4).
			constexpr bool by_key = segment_policy<Layout>::key_order;
			if constexpr (order_source<Layout>::exists && !by_key) {
				if (job.reorder) {
					merge_scratch forward(ctx, job, used, stop, chunk, index_detail::forward_name(job.id));
					order = order_source<Layout>::order(sources, forward, ctx.reorder, &stop, &job.memory);
				}
			}
			{
				std::optional<merge_scratch> records;
				if (!by_key && (!order.empty() || (sources.size() == 1 && !sources[0].seg->core().docid_ordered()))) {
					records.emplace(ctx, job, used, stop, chunk, index_detail::records_name(job.id));
				}
				merge_spill positions(ctx, job, used, stop, chunk);
				job.plan = detail::merge_layout(std::span<const basic_merge_source<Layout>>(sources), job.header, sink, positions,
					{.cancel = &stop, .memory = &job.memory, .order = order, .records = records ? &*records : nullptr,
						.structures = ctx.structures, .stored_codec = ctx.stored_codec});
			}
			sink.finish();
			job.memory.shrink(job.memory.held());  // the build's buffers are gone
			if (!(mapped = map_output(*file))) {
				bytes = index_detail::read_all(*file);  // through the file it holds: no new descriptor
				job.memory.grow(bytes.capacity());
			}
			if ((mapped ? mapped->bytes().size() : bytes.size()) != out.size()) {
				throw std::system_error(EIO, std::generic_category(), "strata: a merge's output reads back short");
			}
		} catch (...) {
			file.reset();
			ctx.io->remove(name);
			job.removed = out.size();  // the writer's directory sync returns it: one sync path (sync-at-first-commit D4)
			throw;
		}
		const std::string_view view = mapped ? mapped->bytes() : std::string_view(bytes);
		files::segment_ref ref;
		ref.id = job.id;
		ref.identity = job.header;
		ref.size = view.size();
		ref.footer_crc = format::decode_footer(view).toc_crc;
		ref.doc_count = job.plan.doc_count;
		job.output = std::make_unique<open_segment>(ref, ctx.load);
		job.output->adopt(mapped ? std::make_unique<segment_body>(std::move(mapped)) : std::make_unique<segment_body>(std::move(bytes)));  // facts before it is published
		job.output->unsynced = std::move(file);
	} catch (const std::system_error& e) {
		job.error = e.code().value() == ENOSPC ? std::make_exception_ptr(space_error(e.code(), e.what())) : std::current_exception();
	} catch (...) {
		job.error = std::current_exception();
	}
	job.done.store(true, std::memory_order_release);
}

template <class Layout>
void basic_index<Layout>::publish_merges(bool wait, bool space_throws) {
	if (!group_ || pending_.empty()) { return; }
	if (wait) { group_->wait(); }
	obsoleted_ = 0;
	std::optional<std::string> space;  // the first refusal's own message: admission's or the disk's
	for (std::size_t i = 0; i < pending_.size();) {
		if (!pending_[i]->done.load(std::memory_order_acquire)) {
			++i;
			continue;
		}
		const auto job = std::move(pending_[i]);
		pending_.erase(pending_.begin() + std::ptrdiff_t(i));
		const job_drop drop{*job};  // whether installed, dropped, or fencing
		if (job->error) {
			job->storage.settle();
			job->memory.settle();
			unlinked_ += job->removed;
			try {
				std::rethrow_exception(job->error);
			} catch (const space_error& e) {
				if (!space) { space = e.what(); }
				continue;
			} catch (const task::cancelled&) {
				continue;
			} catch (const corrupt_segment& e) {
				if (mark_damaged(*job)) { continue; }
				fence(std::string("strata: a merge's output is corrupt: ") + e.what());
			} catch (const std::system_error& e) {
				fence(std::string("strata: durability failure: ") + e.what());
			} catch (const std::exception& e) {
				fence(std::string("strata: merge failure: ") + e.what());
			}
		}
		install_merge(*job);
	}
	// Published before retain() when publishable, so the view it retires can be freed first.
	publish_point();
	// What the installs obsoleted is owed; retain() queues it, so a file a pending merge, a
	// kept manifest, or a view names is never queued, and this call pays up to the cap.
	if (obsoleted_) {
		debt_ += std::exchange(obsoleted_, 0);
		retain();
	}
	if (debt_) { reclaim_files(std::min(debt_, opts_.reclaim_cap)); }
	collect_deletes();
	settle_storage();
	if (space && space_throws) { throw space_error(std::make_error_code(std::errc::no_space_on_device), *space); }
}

template <class Layout>
bool basic_index<Layout>::mark_damaged(const merge_job& job) {
	bool any = false;
	for (const auto& src : job.sources) {
		if (std::find(segs_.begin(), segs_.end(), src) == segs_.end()) { continue; }
		try {
			basic_check_report<Layout> r;
			check_segment(src->mapped(), &src->deleted, r);
		} catch (const corrupt_segment&) {
			src->damaged = true;
			any = true;
		}
	}
	return any;
}

template <class Layout>
void basic_index<Layout>::install_merge(merge_job& job) {
	std::vector<std::size_t> at;
	for (const auto& src : job.sources) {
		const auto it = std::find(segs_.begin(), segs_.end(), src);
		if (it == segs_.end()) { break; }
		at.push_back(std::size_t(it - segs_.begin()));
	}
	const std::string name = index_detail::segment_name(job.id);
	auto reclaim_output = [&] {
		if (!job.output) { return; }
		track(name, job.output->ref.size);
		obsoleted_ += job.output->ref.size;
		job.output.reset();
		reclaim_.push_back(name);
		queued_.insert(name);
	};
	std::vector<ordinal_t> carried;
	if (at.size() == job.sources.size() && job.output) {
		for (std::size_t k = 0; k < at.size(); ++k) {
			const auto& now = segs_[at[k]]->deleted;
			const auto& then = job.deleted[k];
			const auto& to = job.plan.ordinals[k];
			for (std::size_t w = 0; w < now.size(); ++w) {
				for (std::uint64_t fresh = now[w] & ~then[w]; fresh; fresh &= fresh - 1) {
					const ordinal_t o = to[w * 64 + std::size_t(std::countr_zero(fresh))];
					if (o == no_more_docs) {
						reclaim_output();
						settle_storage();
						job.storage.settle();
						job.memory.settle();
						fence("strata: merge publication: a delete names a document the merge dropped");
					}
					carried.push_back(o);
				}
			}
		}
	}
	if (at.size() != job.sources.size()) {
		reclaim_output();
	} else {
		if (job.output) {
			auto& out = *job.output;
			for (ordinal_t o : carried) { out.deleted.set(o); }
			out.dead = carried.size();
			out.deletes_dirty = !carried.empty();
		}
		carry_pending(job);
		changed();
		std::sort(at.begin(), at.end());
		for (std::size_t k = at.size(); k-- > 0;) {
			obsoleted_ += segs_[at[k]]->ref.size;
			segs_.erase(segs_.begin() + std::ptrdiff_t(at[k]));
		}
		if (job.output) {
			// Its id was taken at scheduling, below any flushed since: it goes in id order,
			// which the manifest requires.
			track(name, job.output->ref.size);
			const auto at_id = std::lower_bound(segs_.begin(), segs_.end(), job.id, [](const auto& s, std::uint64_t id) { return s->ref.id < id; });
			segs_.emplace(at_id, std::move(job.output));
		}
	}
	settle_storage();
	job.storage.settle();
	job.memory.settle();  // published: the segment's bytes are the index's now (D17)
}

template <class Layout>
void basic_index<Layout>::carry_pending(const merge_job& job) {
	if (!wb_ || wb_->pending.empty()) { return; }
	bool moved = false;
	for (auto& [id, ord] : wb_->pending) {
		for (std::size_t k = 0; k < job.sources.size(); ++k) {
			if (job.sources[k]->ref.id != id) { continue; }
			const ordinal_t o = job.output ? job.plan.ordinals[k][ord] : no_more_docs;
			if (o == no_more_docs) { fence("strata: merge publication: a pending delete names a document the merge dropped"); }
			id = job.id;
			ord = o;
			moved = true;
			break;
		}
	}
	if (!moved) { return; }
	wb_->pending_keys.clear();
	for (const auto& [id, ord] : wb_->pending) { wb_->pending_keys.insert(pending_key(id, ord)); }
}

template <class Layout>
void basic_index<Layout>::end_merges() noexcept {
	runs_group_.reset();
	group_.reset();
	end_deletes();
	for (const auto& job : pending_) { const job_drop drop{*job}; }
	pending_.clear();
}

template <class Layout>
void basic_index<Layout>::abandon_merges() {
	if (!group_) { return; }
	group_->cancel();
	for (const auto& job : pending_) {
		const job_drop drop{*job};
		if (job->done.load(std::memory_order_acquire) && job->output) {
			const std::string name = index_detail::segment_name(job->id);
			track(name, job->output->ref.size);
			job->output.reset();
			reclaim_.push_back(name);
			queued_.insert(name);
		}
		job->storage.settle();
		job->memory.settle();
	}
	pending_.clear();
	settle_storage();
}

}  // namespace strata
