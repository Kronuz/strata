// Per-query work counters for attribution (the query-gap design's E1). Built only with
// STRATA_COUNTERS defined; otherwise every STRATA_COUNT expands to nothing. Counts are
// the calling thread's: reset() before a query, read current() after it. Define it in
// every translation unit of a program or none: it changes inline functions' bodies.
#pragma once

#ifdef STRATA_COUNTERS
#include "metrics.hh"

#include <cstddef>
#include <cstdint>

namespace strata::counters {

using kronuz::metrics::Counter;

// X(name): every counter, in print order.
#define STRATA_COUNTER_FIELDS(X)                                                                  \
	X(blocks_for)            /* FOR blocks decoded */                                             \
	X(blocks_bitset)         /* bitset blocks decoded */                                          \
	X(blocks_tail)           /* tails decoded by iterators */                                     \
	X(bound_tails)           /* tails walked for a short term's competitive pairs */              \
	X(blocks_ored)           /* bitset blocks OR'd into a window without decoding */             \
	X(groups_decoded)        /* level-0 groups decoded */                                         \
	X(advances)              /* advance() calls that move */                                      \
	X(advances_in_block)     /* ... landing in the decoded block */                               \
	X(advances_new_block)    /* ... needing another block */                                      \
	X(advances_new_group)    /* ... whose target lies in another group */                         \
	X(max_score_block)       /* max_score resolved by a block's pairs */                          \
	X(max_score_group)       /* ... by a group's pairs */                                         \
	X(max_score_term)        /* ... by the term's pairs */                                        \
	X(bound_evals)           /* bounds computed from pairs (memoized ones not counted) */         \
	X(pairs_evaluated)       /* pairs those bounds read */                                        \
	X(skipped_blocks)        /* blocks skipped by bound */                                        \
	X(skipped_groups)        /* groups skipped by bound */                                        \
	X(skipped_terms)         /* term lists ended by their whole-term bound */                     \
	X(windows)               /* windows opened (lead blocks, 4,096-ordinal union windows) */      \
	X(windows_skipped)       /* ... skipped whole by their summed bounds */                       \
	X(count_windows)         /* count()'s 4,096-ordinal windows filled */                         \
	X(postings_scored)       /* term scores computed */                                           \
	X(candidates)            /* documents scored and offered to the collector */                  \
	X(rejected_by_bound)     /* documents dropped by a bound before full scoring */               \
	X(collected)             /* candidates the collector kept (at the time) */                    \
	X(excluded_advances)     /* the excluded side's advances (H12) */                             \
	X(included)              /* documents the exclusion let through (H12) */

struct query_counters {
#define STRATA_COUNTER_MEMBER(name) Counter name;
	STRATA_COUNTER_FIELDS(STRATA_COUNTER_MEMBER)
#undef STRATA_COUNTER_MEMBER
	double theta = 0;  // the largest k-th score of the query's collections at their end (0: < k hits)

	// Calls f(name, value) for every counter, in order.
	template <class F>
	void each(F&& f) const {
#define STRATA_COUNTER_VISIT(name) f(#name, name.get());
		STRATA_COUNTER_FIELDS(STRATA_COUNTER_VISIT)
#undef STRATA_COUNTER_VISIT
	}
};

inline query_counters& current() noexcept {
	static thread_local query_counters c;
	return c;
}
inline void reset() noexcept { current() = query_counters{}; }
inline void observe_theta(double t) noexcept {
	if (t > current().theta) { current().theta = t; }
}

// E3's bound source (query-gap): stored skip data, or exact partition maxima under the
// query's weight, fixed (part postings) or variable (averaging part); counting builds only.
struct bound_source {
	bool exact = false, variable = false;
	std::uint32_t part = 128;
};
inline bound_source& bounds() noexcept {
	static thread_local bound_source b;
	return b;
}

}  // namespace strata::counters

#define STRATA_COUNT(name, n) (::strata::counters::current().name.add(n))
#define STRATA_COUNTING(...) __VA_ARGS__
#else
#define STRATA_COUNT(name, n) ((void)0)
#define STRATA_COUNTING(...)
#endif
