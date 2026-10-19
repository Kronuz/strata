// STRATA_COUNTERS: exact counts for known traversals of a small segment.
#include "engine_query.hh"
#include "strata/search.hh"
#include "strata/searcher.hh"
#include "strata/writer.hh"

#include <cstdio>
#include <cstdlib>
#include <memory>

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

// n documents from first_docid: "a" in two of every three, "b" in two of every three
// (a third overlapping), wdf varying, all length 8.
static std::string two_terms(std::uint32_t n, std::uint64_t first_docid) {
	strata::segment_writer w;
	for (std::uint32_t d = 0; d < n; ++d) {
		std::vector<strata::posting_input> in;
		if (d % 3 != 2) { in.push_back({"a", 1 + d % 5, {}}); }
		if (d % 3 != 0) { in.push_back({"b", 1 + d % 4, {}}); }
		w.add(first_docid + d, in, 8);
	}
	return w.flush({});
}

int main() {
	// "s" in every 4th document: 40 FOR blocks (two groups) and a tail of 7. Document
	// 4 * (128 * 35 + 10), in block 35 of group 1, has wdf 5; the rest wdf 1, all length 4.
	// "x" in the first 10 documents, wdf 1 to 10. "h" in every 2nd, wdf 9 in document 2.
	constexpr std::uint32_t ndocs = 4 * (128 * 40 + 7);
	constexpr std::uint32_t hot = 4 * (128 * 35 + 10);
	strata::segment_writer w;
	for (std::uint32_t d = 0; d < ndocs; ++d) {
		std::vector<strata::posting_input> in;
		if (d % 32 < 28) { in.push_back({"a", 1, {}}); }
		if (d % 4 == 0) { in.push_back({"s", d == hot ? 5u : 1u, {}}); }
		if (d < 10) { in.push_back({"x", d + 1, {}}); }
		if (d % 2 == 0) { in.push_back({"h", d == 2 ? 9u : 1u, {}}); }
		w.add(d + 1, in, 4);
	}
	const std::string file = w.flush({});
	strata::segment seg(file);
	const auto& inv = seg.get<strata::inverted_plugin>();
	const auto info = *inv.term("s");
	const auto weight = strata::weight::bm25(inv, info.df);

	{  // Every posting by next(): each block decoded once, no advances.
		strata::term_scorer s(inv, info, weight);
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
		strata::term_scorer s(inv, info, weight);
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
		strata::term_scorer s(inv, info, weight);
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
		CHECK_EQ(k.theta == hits[0].score, true);  // the k-th score, above the floor
	}
	{  // "a" holds 28 of every 32 documents, 17,948 postings: dense blocks are bitsets.
		strata::term_scorer s(inv, *inv.term("a"), strata::weight::bm25(inv, 17948));
		c::reset();
		for (auto d = s.next(); d != strata::no_more_docs; d = s.next()) {}
		CHECK_EQ(c::current().blocks_bitset.get(), 140u);
		CHECK_EQ(c::current().blocks_for.get(), 0u);
		CHECK_EQ(c::current().blocks_tail.get(), 1u);
	}
	{  // Exactly k hits: θ is the k-th score on the collector's path and on the heap's.
		const auto xi = *inv.term("x");
		strata::term_scorer s(inv, xi);
		c::reset();
		const auto hits = strata::top_k(seg, s, 10);  // term_scorer's bulk collection
		CHECK_EQ(hits.size(), 10u);
		CHECK_EQ(c::current().theta > 0 && c::current().theta == hits.back().score, true);
		const std::vector<std::uint64_t> none((ndocs + 63) / 64, 0);
		strata::live_scorer l(std::make_unique<strata::term_scorer>(inv, xi), none);
		c::reset();
		const auto heap = strata::top_k(seg, l, 10);  // no bulk path: the heap
		CHECK_EQ(heap.size(), 10u);
		CHECK_EQ(c::current().theta == hits.back().score, true);
	}
	{  // CR2: a counted conjunction's matches are candidates or rejected, on one segment and
	   // across two (search()'s generic loop).
		const std::string one = two_terms(40000, 1), lo = two_terms(20000, 1), hi = two_terms(20000, 20001);
		strata::segment s1(one), h1(lo), h2(hi);
		const auto q = engine::parse("+a +b");
		for (const std::vector<const strata::segment*>& segs : {std::vector<const strata::segment*>{&s1}, {&h1, &h2}}) {
			std::vector<strata::segment_snapshot> snaps;
			for (const auto* p : segs) { snaps.push_back({p, nullptr}); }
			std::uint64_t n = 0;
			c::reset();
			const auto hits = engine::search_snapshot(snaps, q, 10, strata::scheme::bm25, &n);
			const auto& k = c::current();
			CHECK_EQ(hits.size(), 10u);
			CHECK_EQ(n, segs.size() == 1 ? 13333u : 13334u);  // ordinals 1 mod 3, per segment
			CHECK_EQ(k.candidates.get() + k.rejected_by_bound.get(), n);
			CHECK_EQ(k.rejected_by_bound.get() > 0, true);
		}
	}
	{  // CR2: count()'s windows; bitset blocks OR'd or decoded, never both. A live view
	   // without deletes counts by windows, not by the term's df.
		const std::vector<std::uint64_t> none((ndocs + 63) / 64, 0);
		strata::live_scorer l(std::make_unique<strata::term_scorer>(inv, *inv.term("a")), none);
		c::reset();
		CHECK_EQ(strata::count(l), 17948u);
		const auto& k = c::current();
		CHECK_EQ(k.count_windows.get(), (ndocs + 4095) / 4096);
		CHECK_EQ(k.blocks_ored.get() > 0, true);
		CHECK_EQ(k.blocks_ored.get() + k.blocks_bitset.get(), 140u);
		CHECK_EQ(k.blocks_tail.get(), 1u);
	}
	{  // CR2: an exclusion's counting windows count each included document once, whether it
	   // fills both sides ("s -a") or steps the excluded side per document ("x -s"). A live
	   // view keeps term-minus-term from its exact count.
		const std::vector<std::uint64_t> none((ndocs + 63) / 64, 0);
		for (const char* text : {"s -a", "x -s"}) {
			strata::live_scorer q(engine::build(seg, engine::parse(text)), none);
			c::reset();
			const std::uint64_t n = strata::count(q);
			const auto& k = c::current();
			CHECK_EQ(n, text[0] == 's' ? ndocs / 32 : 7u);  // ordinals 28 mod 32; 0 to 9 but 0, 4, 8
			CHECK_EQ(k.included.get(), n);
			CHECK_EQ(k.excluded_advances.get() > 0, true);
			CHECK_EQ(k.count_windows.get() > 0, true);
		}
	}
	{  // CR2: a short term's competitive pairs walk its tail once, apart from its iterators'.
		c::reset();
		const auto xi = *inv.term("x");
		CHECK_EQ(c::current().bound_tails.get(), 1u);
		CHECK_EQ(c::current().blocks_tail.get(), 0u);
		strata::term_scorer s(inv, xi);
		(void)strata::top_k(seg, s, 10);
		CHECK_EQ(c::current().bound_tails.get(), 1u);
		CHECK_EQ(c::current().blocks_tail.get(), 1u);
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
