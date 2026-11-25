// basic_index's shared readers: the views it publishes, retires and frees (shared-reader)
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
auto basic_index<Layout>::reader() -> reader_handle {
	const writer_scope leased{*this};
	live();
	if (!pub_) {
		pub_ = std::make_unique<publication>();
		std::unique_ptr<reader_registry> own;
		if (!opts_.readers) { own = std::make_unique<reader_registry>(opts_.reader_records); }
		pub_->rs = std::make_shared<reader_state>(opts_.readers ? opts_.readers : own.get(), std::move(own));
		if (logged_) {
			logged_->attach_readers(std::shared_ptr<std::atomic<std::uint64_t>>(pub_->rs, &pub_->rs->visible));
		} else {
			pub_->rs->visible.store(visible_, std::memory_order_release);
		}
	}
	if (!pub_->current) { refresh(); }
	return reader_handle(pub_->rs);
}

template <class Layout>
void basic_index<Layout>::publish_through(files::position p) {
	const writer_scope leased{*this};
	live();
	if (!pub_) { throw std::logic_error("strata: publish_through() needs a reader()"); }
	if (pub_->current && pub_->current->position_ >= p) { return; }
	refresh();
}

template <class Layout>
auto basic_index<Layout>::views_held() const -> views_report {
	views_report r;
	if (!pub_) { return r; }
	std::set<std::uint64_t> mine, theirs;  // by segment id: a twin and the record it replaced are one file
	for (const auto& s : segs_) { mine.insert(s->ref.id); }
	for_each_view([&](const view& v) {
		++r.views;
		for (const auto& s : v.keep_) {
			if (!mine.count(s->ref.id) && theirs.insert(s->ref.id).second) { r.bytes += s->ref.size; }
		}
	});
	return r;
}

template <class Layout>
void basic_index<Layout>::changed() noexcept {
	if (pub_) { ++pub_->changes; }
}

template <class Layout>
void basic_index<Layout>::publish_point() {
	if (!pub_ || fenced_ || buffered_records() != 0) { return; }
	if (pub_->current && pub_->changes == pub_->published && !retire_tables()) { return; }
	publish_view(build_view());
	free_views();
}

template <class Layout>
auto basic_index<Layout>::build_view() -> std::unique_ptr<const view> {
	auto v = std::make_unique<view>();
	v->keep_.reserve(segs_.size());
	v->bits_.reserve(segs_.size());  // segment_views point into it
	v->views_.reserve(segs_.size());
	for (const auto& s : segs_) {
		v->keep_.push_back(s);
		v->bits_.push_back(s->dead ? s->deleted.share() : deleted_bits{});
		v->views_.push_back({s->ref.id, s.get(), s->dead ? &v->bits_.back() : nullptr, s->dead});
	}
	const view* last = pub_->current.get();
	v->visible_ = logged_visible();
	v->generation_ = pub_->generation + 1;
	if (last && ahead_of_log()) {
		// A logged index's index-level state may include transactions past V: the last view's stays.
		v->meta_ = last->meta_;
		v->fields_ = last->fields_;
		v->fields_epoch_ = last->fields_epoch_;
		v->last_docid_ = last->last_docid_;
		v->schema_version_ = last->schema_version_;
		v->configured_ = last->configured_;
		v->position_ = last->position_;
	} else {
		v->meta_ = last && *last->meta_ == meta_ ? last->meta_ : std::make_shared<const std::map<std::string, std::string>>(meta_);
		if (last && last->fields_epoch_ == fields_epoch_) {
			v->fields_ = last->fields_;
		} else {
			v->fields_ = std::make_shared<const field_table>(fields_);
		}
		v->fields_epoch_ = fields_epoch_;
		v->last_docid_ = cur_.last_docid;
		v->schema_version_ = cur_.schema_version;
		v->configured_ = cur_.configured;
		v->position_ = cur_.applied;
	}
	if (live_reads()) {
		// An empty active buffer now, so the next write needs no view of its own (D5).
		if (!wb_) { wb_ = std::make_shared<buffer>(visible_, opts_.lane_chunks_cap); }
		v->active_ = wb_;
		v->frozen_.assign(frozen_.begin(), frozen_.end());
	}
	return v;
}

template <class Layout>
void basic_index<Layout>::publish_cover() {
	maintain_runs();
	const bool stamped = retire_tables();
	if (!pub_ || !pub_->current || fenced_ || !live_reads()) { return; }
	const view& c = *pub_->current;
	bool same = stamped ? false : c.active_ == wb_ && c.frozen_.size() == frozen_.size();
	for (std::size_t i = 0; same && i < frozen_.size(); ++i) { same = c.frozen_[i] == frozen_[i]; }
	if (!same) { publish_view(build_view()); }
	// The registry scan, every 8 groups or past 32 retired objects (D10).
	if (++pub_->groups >= 8 || pub_->retired.size() + pub_->tables.size() > 32) { free_views(); }
}

template <class Layout>
bool basic_index<Layout>::retire_tables() {
	std::vector<std::shared_ptr<const void>> old;
	const auto take = [&](buffer& b) {
		b.keys.take_retired(old);
		b.docids.take_retired(old);
		for (auto& r : b.retired_runs) { old.push_back(std::move(r)); }
		b.retired_runs.clear();
	};
	if (wb_) { take(*wb_); }
	for (auto& b : frozen_) { take(*b); }
	if (old.empty() || !pub_ || !live_reads()) { return false; }
	for (auto& t : old) { pub_->tables.push_back({pub_->generation, std::move(t)}); }
	return true;
}

template <class Layout>
void basic_index<Layout>::free_tables() {
	if (!pub_ || pub_->tables.empty()) { return; }
	std::uint64_t oldest = pub_->current ? pub_->current->generation_ : ~std::uint64_t(0);
	pub_->retired.for_each([&](const view& v) { oldest = std::min(oldest, v.generation_); });
	std::erase_if(pub_->tables, [&](const auto& t) { return t.first < oldest; });
}

template <class Layout>
void basic_index<Layout>::publish_view(std::unique_ptr<const view> next) {
	reader_state& rs = *pub_->rs;
	pub_->retired.reserve();
	const std::uint64_t g = next->generation_;
	reader_registry::publish(rs.current, next.get());
	if (rs.probe) { rs.probe(0); }
	rs.seq.store(2 * g - 1, std::memory_order_release);
	rs.at_index.store(next->position_.index, std::memory_order_release);
	if (rs.probe) { rs.probe(1); }
	rs.at_applied.store(next->position_.applied, std::memory_order_release);
	rs.seq.store(2 * g, std::memory_order_release);
	if (rs.probe) { rs.probe(2); }
	if (pub_->current) { pub_->retired.retire(std::move(pub_->current)); }
	pub_->current = std::move(next);
	pub_->published = pub_->changes;
	pub_->generation = g;
}

template <class Layout>
void basic_index<Layout>::retire_current() {
	pub_->retired.reserve();
	reader_registry::publish(pub_->rs->current, static_cast<const view*>(nullptr));
	if (pub_->current) { pub_->retired.retire(std::move(pub_->current)); }
}

template <class Layout>
bool basic_index<Layout>::free_views() {
	if (!pub_) { return false; }
	pub_->groups = 0;
	if (pub_->retired.empty()) {
		free_tables();
		return false;
	}
	const std::size_t before = pub_->retired.size();
	const std::size_t left = pub_->retired.reclaim(*pub_->rs->registry, exec_->now());
	assert(left <= pub_->rs->registry->capacity());
	free_tables();
	return left < before;
}

template <class Layout>
template <class F>
void basic_index<Layout>::for_each_view(F&& f) const {
	if (!pub_) { return; }
	if (pub_->current) { f(*pub_->current); }
	pub_->retired.for_each(f);
}

}  // namespace strata
