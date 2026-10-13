// The tiered merge policy (Phase 3 design, "Merges"), with the defaults of Lucene 10.4.0's
// TieredMergePolicy as that release reports them (8 segments per tier, 10 per merge, a
// 16 MiB floor, 5 GiB merged segments, 20% deleted documents allowed, 10% for
// force_merge_deletes). It decides, from segment sizes and deleted counts alone, which
// segments to merge next; the index runs the merges. The behavior it promises is the one
// Lucene's TestTieredMergePolicy checks, re-implemented in merge_test.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace strata {

struct merge_policy {
	double segments_per_tier = 8;
	std::size_t max_merge_at_once = 10;
	std::size_t max_forced_merge_at_once = 30;
	std::uint64_t floor_bytes = 16ull << 20;
	std::uint64_t max_merged_bytes = 5ull << 30;
	double deletes_pct_allowed = 20;
	double force_merge_deletes_pct_allowed = 10;

	struct candidate {
		std::uint64_t id = 0, bytes = 0, docs = 0, deleted = 0;
		double live_bytes() const noexcept { return docs ? double(bytes) * double(docs - deleted) / double(docs) : 0.0; }
		double deleted_bytes() const noexcept { return double(bytes) - live_bytes(); }
		double deleted_pct() const noexcept { return docs ? 100.0 * double(deleted) / double(docs) : 0.0; }
	};
	using merge = std::vector<std::uint64_t>;  // segment ids

	std::size_t merge_factor() const noexcept { return std::min(std::size_t(segments_per_tier), max_merge_at_once); }

	// The segment count the tiers allow for `bytes` of live data whose smallest segment
	// holds `smallest`: segments_per_tier per tier, tiers growing by the merge factor from
	// the floor up to half the maximum merged size, and never fewer than one full tier.
	double allowed_segments(double bytes, double smallest) const {
		const double half = double(max_merged_bytes) / 2;
		double level = std::max(smallest, double(floor_bytes)), allowed = 0;
		for (;;) {
			const double here = bytes / level;
			if (here <= segments_per_tier || level >= half) {
				allowed += std::ceil(here);
				break;
			}
			allowed += segments_per_tier;
			bytes -= segments_per_tier * level;
			level = std::min(level * double(merge_factor()), half);
		}
		return std::max(allowed, segments_per_tier);
	}

	// The merges to run now (disjoint), none when the segments are within the tiers'
	// budget and the deleted fraction. A merge's largest segment is at most two thirds of
	// it, so every merged byte lands in a segment at least 1.5 times larger and write
	// amplification stays logarithmic; merges only reclaiming deletes may be singletons.
	std::vector<merge> find_merges(std::vector<candidate> segs) const {
		std::sort(segs.begin(), segs.end(), [](const candidate& a, const candidate& b) { return a.live_bytes() > b.live_bytes(); });
		// Segments at half the maximum size or more stay out of the tiers.
		std::vector<const candidate*> eligible;
		double bytes = 0, smallest = 0, docs = 0, deleted = 0;
		for (const auto& s : segs) {
			docs += double(s.docs);
			deleted += double(s.deleted);
			if (s.live_bytes() >= double(max_merged_bytes) / 2) { continue; }
			eligible.push_back(&s);
			bytes += s.live_bytes();
			smallest = eligible.size() == 1 ? s.live_bytes() : std::min(smallest, s.live_bytes());
		}
		std::vector<merge> merges;
		std::vector<bool> used(segs.size(), false);
		auto index_of = [&](const candidate* c) { return std::size_t(c - segs.data()); };
		// Tier merges while the eligible segments outnumber the budget.
		double count = double(eligible.size());
		const double allowed = allowed_segments(bytes, smallest);
		while (count > allowed) {
			const auto best = best_window(eligible, used, index_of, false);
			if (best.empty()) { break; }
			take(best, used, index_of, merges);
			count -= double(best.size() - 1);
		}
		// Reclaiming merges while the index's deleted fraction is over the limit, from every
		// segment not yet merged (the too-large ones included, as singletons).
		for (const auto& m : merges) {
			for (auto id : m) {
				const auto& s = *std::find_if(segs.begin(), segs.end(), [&](const candidate& c) { return c.id == id; });
				deleted -= double(s.deleted);
				docs -= double(s.deleted);
			}
		}
		std::vector<const candidate*> all;
		for (const auto& s : segs) { all.push_back(&s); }
		while (docs > 0 && 100.0 * deleted / docs > deletes_pct_allowed) {
			const auto best = best_window(all, used, index_of, true);
			if (best.empty()) { break; }
			for (const auto* s : best) {
				deleted -= double(s->deleted);
				docs -= double(s->deleted);
			}
			take(best, used, index_of, merges);
		}
		return merges;
	}

	// Merges toward at most max_segments segments, with as few merges as reach it: the
	// smallest segments first, each merge within max(max_merged_bytes, live bytes /
	// max_segments) plus a quarter where possible, past it when nothing else reduces the
	// count. Segments with deletes not otherwise merged are rewritten alone. Repeated until
	// it returns none, the index ends with at most max_segments segments and no deletes.
	std::vector<merge> find_forced_merges(std::vector<candidate> segs, std::size_t max_segments) const {
		max_segments = std::max<std::size_t>(max_segments, 1);
		std::sort(segs.begin(), segs.end(), [](const candidate& a, const candidate& b) { return a.live_bytes() < b.live_bytes(); });
		double total = 0;
		for (const auto& s : segs) { total += s.live_bytes(); }
		const double cap = 1.25 * std::max(double(max_merged_bytes), total / double(max_segments));
		std::size_t need = segs.size() > max_segments ? segs.size() - max_segments : 0;
		std::vector<merge> merges;
		std::vector<bool> used(segs.size(), false);
		for (std::size_t i = 0; i < segs.size() && need > 0; ++i) {
			if (used[i]) { continue; }
			merge m{segs[i].id};
			double size = segs[i].live_bytes();
			used[i] = true;
			for (std::size_t j = i + 1; j < segs.size() && m.size() - 1 < need && m.size() < max_forced_merge_at_once; ++j) {
				if (used[j] || size + segs[j].live_bytes() > cap) { continue; }
				m.push_back(segs[j].id);
				size += segs[j].live_bytes();
				used[j] = true;
			}
			if (m.size() == 1) {
				// Nothing fits under the cap: the next smallest joins anyway.
				for (std::size_t j = i + 1; j < segs.size(); ++j) {
					if (used[j]) { continue; }
					m.push_back(segs[j].id);
					used[j] = true;
					break;
				}
				if (m.size() == 1) {
					used[i] = false;
					break;
				}
			}
			need -= m.size() - 1;
			merges.push_back(std::move(m));
		}
		for (std::size_t i = 0; i < segs.size(); ++i) {
			if (!used[i] && segs[i].deleted) { merges.push_back({segs[i].id}); }
		}
		return merges;
	}

	// Merges for force_merge_deletes: every segment over force_merge_deletes_pct_allowed
	// deleted, grouped by size into merges within max_merged_bytes.
	std::vector<merge> find_forced_deletes_merges(std::vector<candidate> segs) const {
		std::sort(segs.begin(), segs.end(), [](const candidate& a, const candidate& b) { return a.live_bytes() > b.live_bytes(); });
		std::vector<merge> merges;
		merge m;
		double size = 0;
		for (const auto& s : segs) {
			if (s.deleted_pct() <= force_merge_deletes_pct_allowed) { continue; }
			if (!m.empty() && (size + s.live_bytes() > double(max_merged_bytes) || m.size() == max_forced_merge_at_once)) {
				merges.push_back(std::move(m));
				m.clear();
				size = 0;
			}
			m.push_back(s.id);
			size += s.live_bytes();
		}
		if (!m.empty()) { merges.push_back(std::move(m)); }
		return merges;
	}

private:
	// The best merge among unused segments (sorted by live bytes, largest first). A window
	// starts at each segment and takes the next ones that fit under max_merged_bytes until
	// it holds the merge factor and reaches the floor, or max_merge_at_once. Tier merges
	// are balanced windows of two or more, scored by skew (floored), size^0.05, and the
	// live ratio squared (lower first). Reclaiming merges hold deletes, may be singletons,
	// and are scored by live bytes written per deleted byte reclaimed.
	template <class IndexOf>
	std::vector<const candidate*> best_window(const std::vector<const candidate*>& pool, const std::vector<bool>& used, IndexOf index_of, bool reclaim) const {
		std::vector<const candidate*> best;
		double best_score = 0;
		const double floor = double(floor_bytes);
		for (std::size_t start = 0; start < pool.size(); ++start) {
			if (used[index_of(pool[start])]) { continue; }
			std::vector<const candidate*> w;
			double bytes = 0, all = 0;
			for (std::size_t i = start; i < pool.size() && w.size() < max_merge_at_once; ++i) {
				const auto* s = pool[i];
				if (used[index_of(s)] || bytes + s->live_bytes() > double(max_merged_bytes)) { continue; }
				w.push_back(s);
				bytes += s->live_bytes();
				all += double(s->bytes);
				if (w.size() >= merge_factor() && bytes >= floor) { break; }
			}
			const double largest = w.empty() ? 0 : w.front()->live_bytes();
			double score;
			if (reclaim) {
				// Every prefix of the window is a candidate: a singleton or a balanced group.
				double b = 0, dead = 0;
				for (std::size_t k = 0; k < w.size(); ++k) {
					b += w[k]->live_bytes();
					dead += w[k]->deleted_bytes();
					if (dead <= 0 || (k > 0 && 1.5 * largest > b)) { continue; }
					score = b / dead;
					if (best.empty() || score < best_score) {
						best.assign(w.begin(), w.begin() + std::ptrdiff_t(k + 1));
						best_score = score;
					}
				}
				continue;
			}
			if (w.size() < 2 || 1.5 * largest > bytes) { continue; }
			const double skew = std::max(largest, floor) / std::max(bytes, floor * double(w.size()));
			const double live_ratio = all > 0 ? bytes / all : 1;
			score = skew * std::pow(std::max(bytes, 1.0), 0.05) * live_ratio * live_ratio;
			if (best.empty() || score < best_score) {
				best = std::move(w);
				best_score = score;
			}
		}
		return best;
	}

	template <class IndexOf>
	static void take(const std::vector<const candidate*>& w, std::vector<bool>& used, IndexOf index_of, std::vector<merge>& merges) {
		merge m;
		for (const auto* s : w) {
			used[index_of(s)] = true;
			m.push_back(s->id);
		}
		merges.push_back(std::move(m));
	}
};

}  // namespace strata
