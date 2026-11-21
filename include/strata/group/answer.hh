// What a write's proposer hears (cluster-release D3, D4, Q10). Q10 is open with the owner; this
// file holds the lead's default (b), and is the one place a different ruling changes:
//   - `ack::quorum` completes when the proposer applies its placement, carrying apply's verdict;
//   - a refusal against expectations the leader chose is submitted again from the original
//     request, at most `max_placements` placements in all, then answered as unavailable (a
//     retryable 503 with Retry-After), so a merge is never answered 409.
#pragma once

#include "strata/core/index.hh"

#include <consensus/driver.h>

#include <optional>

namespace strata::group {

enum class ack : std::uint8_t {
	none,    // at ProposalPlaced: durable here, the verdict unknown
	quorum,  // at the proposer's apply of its placement, with the verdict
};

struct answer {
	kronuz::consensus::Placement placement;
	std::optional<verdict> applied;  // `quorum` only
};

inline constexpr unsigned max_placements = 3;

// The write path's next step after a definite verdict (Q10 (b)).
enum class next_step : std::uint8_t {
	reply,        // the verdict is the client's answer: 2xx, 409 on its own condition, 400 on invalid
	resubmit,     // prepare again from the original request; a deposed proposer forwards once
	unavailable,  // the bound is spent: a retryable 503 with Retry-After
};

// `leader_chose` is whether the refused expectations were the leader's (a merge's), not the
// client's own condition; `placements` counts this request's placements so far, forwards included.
inline next_step after(const verdict& v, bool leader_chose, unsigned placements) {
	switch (v.result) {
	case verdict::version_conflict:
	case verdict::schema_conflict:
	case verdict::fields_conflict:
		if (!leader_chose) { return next_step::reply; }
		return placements < max_placements ? next_step::resubmit : next_step::unavailable;
	default:
		return next_step::reply;
	}
}

}  // namespace strata::group
