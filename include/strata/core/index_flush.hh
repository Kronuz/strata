// basic_index's freeze and flush: the buffer written as segments, their files and identities
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
void basic_index<Layout>::refresh() {
	const writer_scope leased{*this};
	live();
	guarded([&] { flush(); });
	publish_merges(false, false);
	if (opts_.auto_merge) { maybe_merge(); }
	publish_point();
}

template <class Layout>
auto basic_index<Layout>::streamed_segment(std::unique_ptr<kronuz::journal::File> file, files::segment_ref ref, flush_charge* held) -> std::unique_ptr<open_segment> {
	auto seg = std::make_unique<open_segment>(ref, load_);
	std::shared_ptr<const kronuz::journal::Mapping> m = map_output(*file);
	if (m && m->bytes().size() == ref.size) {
		seg->ref.footer_crc = format::decode_footer(m->bytes()).toc_crc;
		seg->adopt(std::make_unique<segment_body>(std::move(m)));
	} else {
		std::string bytes = index_detail::read_all(*file);  // a backend that cannot map
		seg->ref.footer_crc = format::decode_footer(bytes).toc_crc;
		if (held) { held->grow(bytes.capacity()); }
		seg->adopt(std::make_unique<segment_body>(std::move(bytes)));
	}
	seg->unsynced = std::move(file);
	return seg;
}

template <class Layout>
std::shared_ptr<const kronuz::journal::Mapping> basic_index<Layout>::map_output(kronuz::journal::File& f) noexcept {
	try {
		return f.map();
	} catch (...) {
		return nullptr;
	}
}

template <class Layout>
std::unique_ptr<kronuz::journal::File> basic_index<Layout>::write_unsynced(const std::string& name, std::string_view bytes) {
	auto f = io_->create_exclusive(name);
	try {
		index_detail::write_all(*f, bytes);
	} catch (const std::system_error& e) {
		if (e.code().value() != ENOSPC) { throw; }
		f.reset();
		io_->remove(name);
		sync_dir();  // the partial file's space returns once its removal is durable
		throw space_error(e.code(), e.what());
	}
	ledger_.track(name, bytes.size());
	return f;
}

template <class Layout>
std::array<std::uint8_t, 16> basic_index<Layout>::new_identity() {
	std::uint64_t z = identity_base_ + identities_++;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	std::array<std::uint8_t, 16> id{};
	std::memcpy(id.data(), &producer_term_, 8);
	std::memcpy(id.data() + 8, &z, 8);
	return id;
}

template <class Layout>
void basic_index<Layout>::seed_identities() {
	if (opts_.identity_seed) {
		identity_base_ = *opts_.identity_seed + ((next_generation_ - 1) << 32);
	} else {
		std::random_device rd;
		identity_base_ = std::uint64_t(rd()) << 32 ^ rd();
	}
	identities_ = 0;
}

template <class Layout>
void basic_index<Layout>::freeze() {
	if (holds_rejected()) { wb_.reset(); }  // views that hold it keep it alive
	if (!wb_ || wb_->order.size() == 0) { return; }
	assert(pending_docs_.empty());
	apply_pending();
	frozen_.push_back(std::move(wb_));
}

template <class Layout>
void basic_index<Layout>::apply_pending() {
	if (!wb_) { return; }
	if (!wb_->pending.empty()) { await_log(); }
	buffer& b = *wb_;
	for (const auto& [id, ord] : b.pending) {
		const auto it = std::lower_bound(segs_.begin(), segs_.end(), id, [](const auto& s, std::uint64_t x) { return s->ref.id < x; });
		if (it == segs_.end() || (*it)->ref.id != id) { fence("strata: freeze: a pending delete names no segment"); }
		auto& s = **it;
		if (!s.deleted.test(ord)) {
			deleted_since_.emplace_back(id, ord);  // the change list's one site (manifest-log D3)
			s.deleted.set(ord);  // a clone when a view shares them (shared-reader D3)
			++s.dead;
		}
		s.deletes_dirty = true;
	}
	b.pending.clear();
	b.pending_keys.clear();
}

template <class Layout>
void basic_index<Layout>::flush() {
	await_log();
	if (!m_.generation && buffer_size()) { commit_empty(); }  // the first manifest before any data file
	freeze();
	while (!frozen_.empty()) {
		const std::size_t first = segs_.size();
		if (inverting()) {
			flush_active(*frozen_.front());
		} else {
			flush_documents(*frozen_.front());
		}
		install_flushed(first);
	}
	if (permit_) { permit_.settle(); }
	reserved_ = 0;  // the buffer's bytes are on disk now
	settle_storage();
}

template <class Layout>
void basic_index<Layout>::install_flushed(std::size_t first) {
	require_buffers_unpinned();
	frozen_.erase(frozen_.begin());
	if (!frozen_.empty() || (wb_ && wb_->order.size())) {
		for (std::size_t i = first; i < segs_.size(); ++i) {
			auto& s = *segs_[i];
			const auto& core = s.mapped().core();  // the flush's own segment: its body is here
			const auto& keys = s.mapped().template get<keys_plugin>();
			const auto drop = [&](ordinal_t o) {
				if (s.deleted.test(o)) { return; }
				s.deleted.set(o);
				++s.dead;
				s.deletes_dirty = true;
			};
			for (auto r = keys.range({}); r.valid(); r.next()) {
				bool gone = false;
				for_each_buffer([&](const buffer& b) { gone = gone || find_key_in(b, r.key()) != wbuf::no_ref; });
				if (gone) { drop(r.ordinal()); }
			}
			// A newer record with the docid replaced or deleted the document, keyless or not.
			for (ordinal_t o = 0; o < core.doc_count(); ++o) {
				bool gone = false;
				for_each_buffer([&](const buffer& b) { gone = gone || (b.docids.built() && find_docid_in(b, core.docid(o)) != wbuf::no_ref); });
				if (gone) { drop(o); }
			}
		}
	}
	changed();
}

template <class Layout>
void basic_index<Layout>::flush_active(const buffer& b) {
	if (!b.inv || !b.inv->buf.doc_count()) { return; }
	auto& a = *b.inv;
	flush_charge run(opts_.budget);  // the flush's sort, scratch, and output, until published
	std::vector<ordinal_t> order;
	for (std::size_t i = 0; i < b.order.size(); ++i) {
		if (newest(b, b.order[i].ref)) { order.push_back(b.lane.at(b.order[i].ref).ordinal()); }
	}
	const bool as_is = order.size() == a.buf.doc_count() && a.ordered;
	if (as_is) {
		order.clear();
	} else {
		std::sort(order.begin(), order.end(), [&](ordinal_t x, ordinal_t y) { return a.buf.docid(x) < a.buf.docid(y); });
		run.grow(order.capacity() * sizeof(ordinal_t));
		if (order.empty()) { return; }
	}
	const std::uint64_t id = cur_.next_segment++;  // never reused, even after a failure
	const std::array<std::uint8_t, 16> header = new_identity();
	// The segment goes to its file as it is written. ENOSPC removes the partial file and
	// keeps the buffer, as write_unsynced does.
	const std::string name = index_detail::segment_name(id);
	auto file = io_->create_exclusive(name);
	index_detail::file_sink sink(*file);
	try {
		index_detail::io_spill positions(*io_, index_detail::spill_name(id), &run);
		a.buf.flush(header, order, builder_settings{&run, nullptr, configured_structures(), segment_policy<Layout>::stored_block, segment_policy<Layout>::stored_codec}, sink, positions);
		sink.finish();
	} catch (const std::system_error& e) {
		if (e.code().value() != ENOSPC) { throw; }
		file.reset();
		io_->remove(name);
		sync_dir();
		throw space_error(e.code(), e.what());
	}
	ledger_.track(name, sink.size());
	files::segment_ref ref;
	ref.id = id;
	ref.identity = header;
	ref.size = sink.size();
	ref.doc_count = as_is ? a.buf.doc_count() : order.size();
	segs_.emplace_back(streamed_segment(std::move(file), ref));
	index_ranges();
}

template <class Layout>
void basic_index<Layout>::flush_documents(buffer& b) {
	// The owner's lease from before `docs` points into the buffer to the end of the flush.
	const auto owner = b.lane.pin();
	flush_charge sorting(opts_.budget);
	std::vector<const record*> docs;
	for (std::size_t i = 0; i < b.order.size(); ++i) {
		if (newest(b, b.order[i].ref)) { docs.push_back(&b.lane.at(b.order[i].ref)); }
	}
	sorting.grow(docs.capacity() * sizeof(docs[0]));
	if (!docs.empty()) {
		if (configured_structures() & format::bit::index_sort) {
			std::sort(docs.begin(), docs.end(), [](const record* a, const record* c) {
				const std::string_view x = a->key(), y = c->key();
				if (x.empty() != y.empty()) { return y.empty(); }
				return x.empty() ? a->docid < c->docid : x < y;
			});
		} else {
			std::sort(docs.begin(), docs.end(), [](const record* a, const record* c) { return a->docid < c->docid; });
		}
		// Contiguous runs, one segment each, inverted and encoded in parallel (D18).
		const std::size_t runs = std::max<std::size_t>(1, std::min(opts_.flush_threads, docs.size() / std::max<std::size_t>(1, opts_.flush_min_docs)));
		std::vector<std::uint64_t> ids(runs);
		std::vector<std::array<std::uint8_t, 16>> identities(runs);
		for (std::size_t k = 0; k < runs; ++k) {
			ids[k] = cur_.next_segment++;  // never reused, even after a failure
			identities[k] = new_identity();
		}
		// Each run streams to a file of its own, created here and written by the run's thread.
		std::vector<std::string> names(runs);
		std::vector<std::unique_ptr<kronuz::journal::File>> files(runs);
		std::vector<std::uint64_t> sizes(runs);
		std::vector<std::exception_ptr> errors(runs);
		std::vector<flush_charge> charges(runs);  // each run's, held until its segment is published
		auto stream = [&](std::size_t k) {
			try {
				const std::size_t begin = docs.size() * k / runs, end = docs.size() * (k + 1) / runs;
				flush_charge charge(opts_.budget);  // on the run's own thread
				{
					// The writer's views into the buffer (stored records, keys) live as long as it does,
					// and the lease, declared before it, outlives it: the buffer cannot change under them.
					auto lease = b.lane.pin();
					basic_segment_writer<Layout> w;
					if (opts_.budget) { w.memory = &charge; }
					w.structures = configured_structures();
					w.borrow_stored(lease);
					w.reserve(end - begin, 0);  // the stored bytes stay in the buffer
					for (std::size_t i = begin; i < end; ++i) {
						const record* x = docs[i];
						if constexpr (inline_layout()) {
							if (!x->is_whole()) {
								w.add_stored_view({x->docid, x->version, x->key()}, x->stored());
								continue;
							}
						}
						w.add({x->docid, x->version, x->key()}, x->whole_document(), true);
					}
					index_detail::file_sink sink(*files[k]);
					index_detail::io_spill positions(*io_, index_detail::spill_name(ids[k]), &charge);
					w.flush(identities[k], sink, positions);
					sink.finish();
					sizes[k] = sink.size();
				}
				charge.shrink(charge.held());  // the writer's buffers are gone
				charges[k] = std::move(charge);
			} catch (...) { errors[k] = std::current_exception(); }
		};
		// Every run's file is written before any segment is published: a write that fails
		// (ENOSPC) publishes none and removes the files written, so the retry has their
		// space and the buffer it keeps holds no published document.
		std::exception_ptr failure;
		try {
			for (std::size_t k = 0; k < runs; ++k) {
				names[k] = index_detail::segment_name(ids[k]);
				files[k] = io_->create_exclusive(names[k]);
			}
			std::vector<std::thread> threads;
			for (std::size_t k = 1; k < runs; ++k) { threads.emplace_back(stream, k); }
			stream(0);
			for (auto& t : threads) { t.join(); }
			for (auto& e : errors) { if (e) { std::rethrow_exception(e); } }
		} catch (...) {
			failure = std::current_exception();
		}
		if (failure) {
			bool removed = false;
			for (std::size_t k = 0; k < runs; ++k) {
				if (!files[k]) { continue; }
				files[k].reset();
				try { remove_file(names[k]); removed = true; } catch (...) {}  // best effort: the failure is the news
			}
			if (removed) { try { sync_dir(); } catch (...) {} }
			try {
				std::rethrow_exception(failure);
			} catch (const std::system_error& e) {
				if (e.code().value() != ENOSPC) { throw; }
				throw space_error(e.code(), e.what());
			}
		}
		for (std::size_t k = 0; k < runs; ++k) { ledger_.track(names[k], sizes[k]); }
		for (std::size_t k = 0; k < runs; ++k) {
			files::segment_ref ref;
			ref.id = ids[k];
			ref.identity = identities[k];
			ref.size = sizes[k];
			ref.doc_count = docs.size() * (k + 1) / runs - docs.size() * k / runs;
			segs_.emplace_back(streamed_segment(std::move(files[k]), ref, &charges[k]));
			charges[k].settle();
		}
		index_ranges();
	}
}

}  // namespace strata
