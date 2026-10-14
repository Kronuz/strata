// The tiered merge policy against the behavior Lucene 10.4.0's TestTieredMergePolicy and
// BaseMergePolicyTestCase check (re-implemented here, not copied): the segment-count
// budget after every flush and merge in simulated append-only and update workloads, the
// deleted fraction, logarithmic write amplification, merges of equal segments, maximum-size
// segments left alone, floor-sized merges, singleton merges reclaiming deletes, forced
// merges to an exact count within the size limit, and forced deletes merges.
#include "strata/merge_policy.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::merge_policy;
using candidate = merge_policy::candidate;
using merges = std::vector<merge_policy::merge>;

static constexpr double MB = 1024.0 * 1024.0;

struct sim {
	std::vector<candidate> segs;
	std::uint64_t next_id = 1;
	double flushed = 0, merged = 0;

	void flush(std::uint64_t docs, double bytes) {
		segs.push_back({next_id++, std::uint64_t(bytes), docs, 0});
		flushed += bytes;
	}
	void add(std::uint64_t docs, std::uint64_t deleted, double bytes) { segs.push_back({next_id++, std::uint64_t(bytes), docs, deleted}); }
	// Replaces the merge's segments by one holding their live documents.
	void apply(const merge_policy::merge& m) {
		candidate out{next_id++, 0, 0, 0};
		double bytes = 0;
		for (auto id : m) {
			auto it = std::find_if(segs.begin(), segs.end(), [&](const candidate& c) { return c.id == id; });
			CHECK(it != segs.end());
			if (it == segs.end()) { continue; }
			bytes += it->live_bytes();
			out.docs += it->docs - it->deleted;
			segs.erase(it);
		}
		out.bytes = std::uint64_t(bytes);
		merged += bytes;
		if (out.docs) { segs.push_back(out); }
	}
	// Deletes n live documents spread at random over the segments.
	void delete_docs(std::uint64_t n, std::mt19937_64& r) {
		std::uint64_t live = 0;
		for (const auto& s : segs) { live += s.docs - s.deleted; }
		n = std::min(n, live);
		for (; n > 0; --n) {
			std::uint64_t pick = r() % live;
			for (auto& s : segs) {
				if (pick < s.docs - s.deleted) {
					++s.deleted;
					break;
				}
				pick -= s.docs - s.deleted;
			}
			--live;
		}
	}
	std::size_t merge_all(const merge_policy& p) {
		std::size_t rounds = 0;
		for (auto ms = p.find_merges(segs); !ms.empty(); ms = p.find_merges(segs)) {
			check_disjoint(p, ms, p.max_merge_at_once);
			for (const auto& m : ms) {
				check_balanced(m);
				apply(m);
			}
			CHECK(++rounds < 10000);
			if (rounds >= 10000) { break; }
		}
		return rounds;
	}
	// A merge of two or more: its largest segment at most two thirds of it.
	void check_balanced(const merge_policy::merge& m) {
		if (m.size() < 2) { return; }
		double largest = 0, bytes = 0;
		for (auto id : m) {
			const double b = std::find_if(segs.begin(), segs.end(), [&](const candidate& c) { return c.id == id; })->live_bytes();
			largest = std::max(largest, b);
			bytes += b;
		}
		CHECK(1.5 * largest <= bytes);
	}
	void check_disjoint(const merge_policy&, const merges& ms, std::size_t at_once) {
		std::set<std::uint64_t> seen;
		for (const auto& m : ms) {
			CHECK(!m.empty() && m.size() <= at_once);
			for (auto id : m) { CHECK(seen.insert(id).second); }
		}
	}
};

// The budget Lucene's assertSegmentInfos checks: within the deleted fraction, and within
// the tiers' segment count unless no two neighbors by size form a balanced merge.
static bool within_budget(const merge_policy& p, const std::vector<candidate>& segs) {
	if (segs.empty()) { return true; }
	double total = 0, docs = 0, deleted = 0, smallest = 0;
	std::vector<double> sizes;
	std::size_t too_big = 0;
	for (const auto& s : segs) {
		docs += double(s.docs);
		deleted += double(s.deleted);
		sizes.push_back(s.live_bytes());
		total += s.live_bytes();
		if (s.live_bytes() >= double(p.max_merged_bytes) / 2) { ++too_big; }
	}
	std::sort(sizes.begin(), sizes.end());
	smallest = sizes.front();
	if (100.0 * deleted / docs > p.deletes_pct_allowed) { return false; }
	const double half = double(p.max_merged_bytes) / 2;
	double level = std::max(smallest, double(p.floor_bytes)), left = total, allowed = 0;
	for (;;) {
		const double here = left / level;
		if (here <= p.segments_per_tier || level >= half) {
			allowed += std::ceil(here);
			break;
		}
		allowed += p.segments_per_tier;
		left -= p.segments_per_tier * level;
		level = std::min(level * double(p.merge_factor()), half);
	}
	allowed = std::max(allowed, double(too_big) + p.segments_per_tier);
	bool legal = false;
	for (std::size_t i = 0; i + 1 < sizes.size() && !legal; ++i) {
		legal = sizes[i] + sizes[i + 1] <= double(p.max_merged_bytes) && sizes[i + 1] * 1.5 <= sizes[i] + sizes[i + 1];
	}
	if (double(segs.size()) > allowed && legal) {
		std::fprintf(stderr, "  %zu segments, %.1f allowed, a balanced merge left\n", segs.size(), allowed);
		return false;
	}
	return true;
}

static std::size_t post_count(std::size_t n, const merges& ms) {
	for (const auto& m : ms) { n -= m.size() - 1; }
	return n;
}

int main() {
	// Maximum-size segments stay out of the tiers: 30 of them plus 8 flushes need nothing;
	// 13 flushes need one merge of 8.
	{
		merge_policy p;
		p.max_merged_bytes = 1024 * std::uint64_t(MB);
		sim s;
		for (int i = 0; i < 30; ++i) { s.add(1000, 0, 1024 * MB); }
		for (int i = 0; i < 8; ++i) { s.add(1000, 0, 102 * MB); }
		CHECK(p.find_merges(s.segs).empty());
		for (int i = 0; i < 5; ++i) { s.add(1000, 0, 102 * MB); }
		const auto ms = p.find_merges(s.segs);
		CHECK(ms.size() == 1 && ms[0].size() == 8);
	}
	// A lone segment merges with itself only to reclaim deletes past the allowed fraction.
	{
		merge_policy p;
		for (std::uint64_t pct : {0, 15, 20}) { CHECK(p.find_merges({{1, 1024 * std::uint64_t(MB), 1000000, pct * 10000}}).empty()); }
		const auto ms = p.find_merges({{1, 1024 * std::uint64_t(MB), 1000000, 210000}});
		CHECK(ms.size() == 1 && ms[0] == merge_policy::merge{1});
	}
	// A segment past the size cap (from a forced merge) is rewritten alone once its deletes
	// pass the allowed fraction, not left until most of it is deleted (review finding 22).
	{
		merge_policy p;
		const std::uint64_t gib = 1ull << 30;
		CHECK(p.find_merges({{1, 20 * gib, 1000000, 150000}}).empty());  // 15%: within the limit
		const auto ms = p.find_merges({{1, 20 * gib, 1000000, 250000}, {2, gib, 50000, 0}});  // 23.8% of the index
		CHECK(ms.size() == 1 && ms[0] == merge_policy::merge{1});
	}
	// Below the floor, merges take segments until they reach it, up to max_merge_at_once.
	{
		merge_policy p;
		p.max_merge_at_once = 30;
		std::vector<candidate> segs;
		for (std::uint64_t i = 1; i <= 40; ++i) { segs.push_back({i, std::uint64_t(MB), 1000000, 0}); }
		for (auto [floor_mb, sizes] : std::vector<std::pair<double, std::vector<std::size_t>>>{{0.1, {8, 8, 8, 8}}, {12, {12, 12, 12}}, {60, {30, 10}}}) {
			p.floor_bytes = std::uint64_t(floor_mb * MB);
			const auto ms = p.find_merges(segs);
			std::vector<std::size_t> got;
			for (const auto& m : ms) { got.push_back(m.size()); }
			CHECK(got == sizes);
		}
	}
	// Equal flushes merge into equal segments: 100, 800, or 6,400 documents, never between.
	{
		merge_policy p;
		p.floor_bytes = 10;
		sim s;
		for (int i = 0; i < 150; ++i) {
			s.flush(100, 100 * 128.0);
			s.merge_all(p);
		}
		for (const auto& c : s.segs) { CHECK(c.docs == 100 || c.docs == 800 || c.docs == 6400); }
	}
	// Append-only: within budget after every flush, at any maximum merged size.
	{
		std::mt19937_64 r(11);
		for (int round = 0; round < 3; ++round) {
			merge_policy p;
			p.max_merged_bytes = (1024 + r() % (9 * 1024)) * std::uint64_t(MB);
			sim s;
			bool ok = true;
			for (std::uint64_t docs = 0; docs < 20000000 && ok;) {
				const std::uint64_t n = 1 + r() % 10000;
				docs += n;
				s.flush(n, double(n) * 5 * 1024);
				s.merge_all(p);
				ok = within_budget(p, s.segs);
			}
			CHECK(ok);
			std::printf("  append-only, max %llu MB: %zu segments, write amplification %.2f\n", (unsigned long long)(p.max_merged_bytes >> 20), s.segs.size(), (s.flushed + s.merged) / s.flushed);
		}
	}
	// Updates spread over the index: within budget, deleted fraction included.
	{
		std::mt19937_64 r(12);
		merge_policy p;
		p.max_merged_bytes = (1024 + r() % (9 * 1024)) * std::uint64_t(MB);
		sim s;
		const std::uint64_t total = 1000000, per_flush = 2500;
		bool ok = true;
		for (std::uint64_t docs = 0; docs < total && ok;) {
			const std::uint64_t n = r() % 8 ? per_flush / 2 + r() % (per_flush / 2 + 1) : 1 + r() % per_flush;
			const auto updates = std::uint64_t(double(n) * 0.9 * double(docs) / double(total));
			docs += n - updates;
			s.delete_docs(updates, r);
			s.flush(n, double(n) * 5 * 1024);
			s.merge_all(p);
			ok = within_budget(p, s.segs);
		}
		CHECK(ok);
		std::printf("  updates: %zu segments, write amplification %.2f\n", s.segs.size(), (s.flushed + s.merged) / s.flushed);
	}
	// Tiny flushes under a large floor: write amplification below log(flushes) / log(1.5).
	{
		std::mt19937_64 r(13);
		merge_policy p;
		sim s;
		std::size_t flushes = 0;
		bool ok = true;
		for (std::uint64_t docs = 0; docs < 10000 && ok; ++flushes) {
			const std::uint64_t n = 1 + r() % 3;
			docs += n;
			s.flush(n, double(n) * 10);
			s.merge_all(p);
			ok = within_budget(p, s.segs);
		}
		CHECK(ok);
		const double amplification = (s.flushed + s.merged) / s.flushed;
		CHECK(amplification < std::log(double(flushes)) / std::log(1.5));
		std::printf("  tiny flushes: %zu flushes, write amplification %.2f\n", flushes, amplification);
	}
	// Forced merges reach the requested count exactly when sizes allow, within 1.5 times
	// max(max merged size, total / count); repeated, they reach it in any case.
	{
		merge_policy p;
		p.max_merged_bytes = 10 * std::uint64_t(MB);
		for (std::size_t k = 1; k <= 12; ++k) {
			for (auto [n, mb] : std::vector<std::pair<std::size_t, double>>{{30, 1.0}, {137, 0.1}}) {
				sim s;
				for (std::size_t i = 0; i < n; ++i) { s.add(1000, 0, mb * MB); }
				const double limit = 1.5 * std::max(double(p.max_merged_bytes), double(n) * mb * MB / double(k));
				auto ms = p.find_forced_merges(s.segs, k);
				if (n == 30) { CHECK(post_count(n, ms) == k); }
				CHECK(post_count(n, ms) >= k);
				for (int rounds = 0; !ms.empty(); ms = p.find_forced_merges(s.segs, k)) {
					s.check_disjoint(p, ms, p.max_forced_merge_at_once);
					for (const auto& m : ms) {
						double bytes = 0;
						for (auto id : m) { bytes += double(std::find_if(s.segs.begin(), s.segs.end(), [&](const candidate& c) { return c.id == id; })->bytes); }
						CHECK(bytes <= limit);
						s.apply(m);
					}
					CHECK(++rounds < 100);
					if (rounds >= 100) { break; }
				}
				CHECK(s.segs.size() <= k);
			}
		}
		// A lone segment with deletes is rewritten; one without is left alone.
		CHECK(p.find_forced_merges({{1, 4096, 100, 0}}, 1).empty());
		CHECK(p.find_forced_merges({{1, 4096, 100, 1}}, 1) == merges{{1}});
	}
	// Forced deletes merges: segments over the threshold only, grouped within the size cap.
	{
		merge_policy p;
		std::vector<candidate> segs;
		for (std::uint64_t i = 1; i <= 20; ++i) { segs.push_back({i, 4000, 4, 1}); }
		p.force_merge_deletes_pct_allowed = 30;
		CHECK(p.find_forced_deletes_merges(segs).empty());
		p.force_merge_deletes_pct_allowed = 10;
		std::size_t merged = 0;
		for (const auto& m : p.find_forced_deletes_merges(segs)) { merged += m.size(); }
		CHECK(merged == 20);
		p.max_merged_bytes = 4000;
		const auto ms = p.find_forced_deletes_merges(segs);
		CHECK(ms.size() == 20);
		segs[3].deleted = 0;
		p.force_merge_deletes_pct_allowed = 0;
		CHECK(p.find_forced_deletes_merges(segs).size() == 19);
	}
	std::printf("merge_policy: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
