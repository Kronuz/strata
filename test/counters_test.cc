// STRATA_COUNTERS: exact counts for known traversals of a small segment.
#include "strata/search.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK_EQ(got, want)                                                                                   \
	do {                                                                                                      \
		const auto g_ = (got);                                                                                \
		const auto w_ = (want);                                                                               \
		if (g_ != w_) {                                                                                       \
			std::fprintf(stderr, "%s:%d: %s = %llu, want %llu\n", __FILE__, __LINE__, #got, (unsigned long long)g_, (unsigned long long)w_); \
			++failures;                                                                                       \
		}                                                                                                     \
	} while (0)

namespace c = strata::counters;

int main() {
	// "s" in every 4th document: 40 FOR blocks (two groups) and a tail of 7. Document
	// 4 * (128 * 35 + 10), in block 35 of group 1, has wdf 5; the rest wdf 1, all length 4.
	constexpr std::uint32_t ndocs = 4 * (128 * 40 + 7);
	constexpr std::uint32_t hot = 4 * (128 * 35 + 10);
	strata::segment_writer w;
	for (std::uint32_t d = 0; d < ndocs; ++d) {
		std::vector<strata::posting_input> in;
		if (d % 32 < 28) { in.push_back({"a", 1, {}}); }
		if (d % 4 == 0) { in.push_back({"s", d == hot ? 5u : 1u, {}}); }
		w.add(d + 1, in, 4);
	}
	const std::string file = w.flush({});
	strata::segment seg(file);
	const auto info = *seg.term("s");
	const auto weight = strata::weight::bm25(seg, info.df);

	{  // Every posting by next(): each block decoded once, no advances.
		strata::term_scorer s(seg, info, weight);
		c::reset();
		std::uint64_t n = 0;
		for (auto d = s.next(); d != strata::no_more_docs; d = s.next()) { ++n; }
		CHECK_EQ(n, 128u * 40 + 7);
		CHECK_EQ(c::current().blocks_for.get(), 40u);
		CHECK_EQ(c::current().blocks_tail.get(), 1u);
		CHECK_EQ(c::current().blocks_bitset.get(), 0u);
		CHECK_EQ(c::current().groups_decoded.get(), 2u);
		CHECK_EQ(c::current().advances.get(), 0u);
	}
	{  // Advances: into block 0 (group 0), inside it, into block 1, into block 32 (group 1).
		strata::term_scorer s(seg, info, weight);
		c::reset();
		s.advance(0);
		s.advance(4);
		s.advance(600);
		s.advance(4 * 128 * 32);
		CHECK_EQ(c::current().advances.get(), 4u);
		CHECK_EQ(c::current().advances_in_block.get(), 1u);
		CHECK_EQ(c::current().advances_new_block.get(), 3u);
		CHECK_EQ(c::current().advances_new_group.get(), 2u);
		CHECK_EQ(c::current().blocks_for.get(), 3u);
		CHECK_EQ(c::current().groups_decoded.get(), 2u);
	}
	{  // Top-1 under a floor only the hot document beats: group 0 and every block of
	   // group 1 but block 35 skip by bound; block 35 and the tail (term bound) score.
		strata::term_scorer s(seg, info, weight);
		const double floor = (s.score_of(hot / 4 * 4 + 4, 1) + s.score_of(hot, 5)) / 2;
		c::reset();
		const auto hits = strata::top_k(seg, s, 1, nullptr, floor);
		CHECK_EQ(hits.size(), 1u);
		CHECK_EQ(hits.empty() ? 0u : hits[0].ord, hot);
		const auto& k = c::current();
		CHECK_EQ(k.skipped_groups.get(), 1u);
		CHECK_EQ(k.skipped_blocks.get(), 7u);
		CHECK_EQ(k.skipped_terms.get(), 0u);
		CHECK_EQ(k.blocks_for.get(), 9u);  // 0, then 32 to 39: a skip decodes the next block
		CHECK_EQ(k.blocks_tail.get(), 1u);
		CHECK_EQ(k.groups_decoded.get(), 2u);
		CHECK_EQ(k.bound_evals.get(), 10u);  // two group bounds, eight block bounds
		CHECK_EQ(k.pairs_evaluated.get(), 10u);
		CHECK_EQ(k.advances.get(), 8u);
		CHECK_EQ(k.advances_new_block.get(), 8u);
		CHECK_EQ(k.advances_new_group.get(), 2u);
		CHECK_EQ(k.postings_scored.get(), 128u + 7);
		CHECK_EQ(k.candidates.get(), 128u + 7);
		CHECK_EQ(k.collected.get(), 1u);
		CHECK_EQ(k.theta == floor, true);
	}
	{  // "a" holds 28 of every 32 documents, 17,948 postings: dense blocks are bitsets.
		strata::term_scorer s(seg, *seg.term("a"), strata::weight::bm25(seg, 17948));
		c::reset();
		for (auto d = s.next(); d != strata::no_more_docs; d = s.next()) {}
		CHECK_EQ(c::current().blocks_bitset.get(), 140u);
		CHECK_EQ(c::current().blocks_for.get(), 0u);
		CHECK_EQ(c::current().blocks_tail.get(), 1u);
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
