// The tiered merge policy (Phase 3 design, "Merges"), with the defaults of Lucene 10.4.0's
// TieredMergePolicy as that release reports them (8 segments per tier, 10 per merge, a
// 16 MiB floor, 5 GiB merged segments, 20% deleted documents allowed). It decides, from
// segment sizes and deleted fractions alone, which segments to merge next; the index runs
// the merges.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace strata {

struct merge_policy {
	double segments_per_tier = 8;
	std::size_t max_merge_at_once = 10;
	std::uint64_t floor_bytes = 16ull << 20;
	std::uint64_t max_merged_bytes = 5ull << 30;
	double deletes_pct_allowed = 20;

	struct candidate {
		std::uint64_t id = 0, bytes = 0, docs = 0, deleted = 0;
		double live_bytes() const noexcept { return docs ? double(bytes) * double(docs - deleted) / double(docs) : 0.0; }
	};

	// The next merge (segment ids), or none when the index is within its budget of
	// segments and its deleted fraction.
	std::vector<std::uint64_t> find_merge(std::vector<candidate> segs) const {
		std::sort(segs.begin(), segs.end(), [](const candidate& a, const candidate& b) { return a.live_bytes() > b.live_bytes(); });
		// Segments already near the maximum size stay out unless mostly deleted.
		std::vector<candidate> eligible;
		double total = 0, total_deleted = 0, total_docs = 0;
		for (const auto& s : segs) {
			total_docs += double(s.docs);
			total_deleted += double(s.deleted);
			const double deleted_pct = s.docs ? 100.0 * double(s.deleted) / double(s.docs) : 0;
			if (s.live_bytes() > double(max_merged_bytes) / 2 && deleted_pct < deletes_pct_allowed) { continue; }
			eligible.push_back(s);
			total += std::max(s.live_bytes(), double(floor_bytes));
		}
		const double deleted_pct = total_docs ? 100.0 * total_deleted / total_docs : 0;
		if (eligible.size() < 2) { return {}; }
		// The number of segments the tiers allow: segments_per_tier at each tier size, tiers
		// growing by max_merge_at_once from the floor.
		double allowed = 0, level = double(floor_bytes), left = total;
		for (;;) {
			const double here = left / level;
			if (here < segments_per_tier) {
				allowed += std::ceil(here);
				break;
			}
			allowed += segments_per_tier;
			left -= segments_per_tier * level;
			level *= double(max_merge_at_once);
		}
		if (double(eligible.size()) <= std::max(allowed, segments_per_tier) && deleted_pct <= deletes_pct_allowed) { return {}; }
		// The cheapest window of up to max_merge_at_once consecutive segments (by size):
		// balanced (skew), small (size^0.05), and reclaiming deletes (live ratio squared).
		std::vector<std::uint64_t> best;
		double best_score = 0;
		for (std::size_t start = 0; start + 1 < eligible.size(); ++start) {
			std::vector<std::uint64_t> ids;
			double bytes = 0, largest = 0, live = 0, all = 0;
			for (std::size_t i = start; i < eligible.size() && ids.size() < max_merge_at_once; ++i) {
				const auto& s = eligible[i];
				if (bytes + s.live_bytes() > double(max_merged_bytes)) { continue; }
				ids.push_back(s.id);
				bytes += s.live_bytes();
				largest = std::max(largest, std::max(s.live_bytes(), double(floor_bytes)));
				live += s.live_bytes();
				all += double(s.bytes);
			}
			if (ids.size() < 2) { continue; }
			const double floored = std::max(bytes, double(floor_bytes) * double(ids.size()));
			const double skew = largest / floored;
			const double live_ratio = all > 0 ? live / all : 1;
			const double score = skew * std::pow(std::max(bytes, 1.0), 0.05) * live_ratio * live_ratio;
			if (best.empty() || score < best_score) {
				best = std::move(ids);
				best_score = score;
			}
		}
		return best;
	}
};

}  // namespace strata
