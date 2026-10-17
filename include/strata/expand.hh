/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Multi-term expansion (docs/design/automata.md): an automaton intersected with a
// segment's term dictionary (D7), merged over a snapshot's segments under Xapian's
// expansion limits (D8), and the `or` combination of the expanded terms (D9).

#pragma once

#include "strata/automata.hh"
#include "strata/searcher.hh"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata {

// Consecutive entries under one dead prefix before the walk seeks past it (a preset: a
// seek re-decodes up to one block and one restart run).
inline constexpr std::size_t seek_after = termdict::max_block_entries;

struct intersect_stats {
	std::uint64_t visited = 0, matched = 0, seeks = 0;
};

// Calls f(term, info) for every term of seg the automaton accepts, in order, until f
// returns false. Each info is decoded as segment::find_term decodes it (the competitive
// pairs of a term below 128 postings are left to complete_impacts). `seek` is the dead run
// that triggers a seek.
template <class Segment, class A, class F>
void intersect(const Segment& seg, const A& a, F&& f, intersect_stats* stats = nullptr, std::size_t seek = seek_after) {
	const termdict::reader& dict = seg.dictionary();
	const auto hi = automata::successor(a.literal());
	std::optional<automata::matcher<A>> m;
	if constexpr (!A::byte_range) { m.emplace(a); }
	struct replayed {
		const char* value;
		format::term_run before;  // the run cursor before this value
	};
	std::vector<replayed> replay;
	std::string from(a.literal());
	intersect_stats local;
	intersect_stats& st = stats ? *stats : local;
	try {
		for (bool done = false; !done;) {
			// A seek: the run cursor up to the first entry at or past `from` (D7).
			format::term_run run;
			replay.clear();
			if (!from.empty()) {
				(void)dict.find_run(from, [&](std::string_view v) {
					replay.push_back({v.data(), run});
					(void)format::decode_term_info(v, run, false);
				});
			}
			bool first = true, reseek = false;
			std::size_t dead_run = 0;
			dict.scan(from, [&](std::string_view key, std::string_view value) {
				if (hi && key >= std::string_view(*hi)) {
					done = true;
					return false;
				}
				if (first) {
					first = false;
					for (const auto& r : replay) {
						if (r.value == value.data()) {
							run = r.before;
							break;
						}
					}
				}
				++st.visited;
				typename automata::matcher<A>::result r;
				if constexpr (A::byte_range) {
					r.match = true;
				} else {
					r = (*m)(key);
				}
				const auto info = format::decode_term_info(value, run, r.match);
				if (info.df > seg.doc_count()) { corrupt("document frequency"); }
				if (r.match) {
					++st.matched;
					dead_run = 0;
					if (!f(key, info)) {
						done = true;
						return false;
					}
					return true;
				}
				dead_run = r.dead ? (r.known ? dead_run + 1 : 1) : 0;
				if (dead_run >= seek) {
					auto next = automata::successor(key.substr(0, r.dead));
					if (!next) {
						done = true;
					} else {
						from = std::move(*next);
						reseek = true;
					}
					return false;
				}
				return true;
			});
			if (!reseek) { break; }
			++st.seeks;
		}
	} catch (const termdict::corrupt&) {
		corrupt("terms checksum");  // a dictionary block failed its region check
	}
}

// --- expansion over a snapshot (D8) -------------------------------------------------------------

enum class limit_type {
	error,          // more than `max` terms throws expansion_error (Xapian's default)
	first,          // the first `max` terms in term order
	most_frequent,  // the `max` terms of highest snapshot df, ties by term order
};

struct expansion_limit {
	std::uint64_t max = 0;  // 0: no limit
	limit_type type = limit_type::error;
};

// An error-limited expansion past its limit (Xapian's WildcardError).
struct expansion_error : std::runtime_error {
	std::uint64_t limit;
	explicit expansion_error(std::uint64_t n) : std::runtime_error("strata: expansion past " + std::to_string(n) + " terms"), limit(n) {}
};

// The terms an automaton selects in a snapshot, in term order, with their snapshot
// document frequencies (deleted documents included) and each segment's infos.
struct expansion {
	struct entry {
		std::size_t term;  // index into terms
		format::term_info info;
	};
	std::vector<std::string> terms;
	std::vector<std::uint64_t> df;
	std::vector<std::vector<entry>> segments;  // by snapshot segment, in term order

	bool empty() const noexcept { return terms.empty(); }
};

template <class A>
expansion expand(std::span<const segment_snapshot> segs, const A& a, expansion_limit limit = {}) {
	const std::uint64_t max = limit.max;
	// A segment walk stops once its own matches decide the limit.
	const std::uint64_t stop = max == 0 || limit.type == limit_type::most_frequent ? 0 : limit.type == limit_type::error ? max + 1 : max;
	std::vector<std::vector<std::pair<std::string, format::term_info>>> found(segs.size());
	for (std::size_t s = 0; s < segs.size(); ++s) {
		intersect(*segs[s].seg, a, [&](std::string_view term, const format::term_info& info) {
			found[s].emplace_back(std::string(term), info);
			return stop == 0 || found[s].size() < stop;
		});
		if (limit.type == limit_type::error && max && found[s].size() > max) { throw expansion_error(max); }
	}
	// Merge: every (term, segment, index), grouped by term.
	struct ref {
		std::string_view term;
		std::size_t seg, at;
	};
	std::vector<ref> all;
	for (std::size_t s = 0; s < found.size(); ++s) {
		for (std::size_t i = 0; i < found[s].size(); ++i) { all.push_back({found[s][i].first, s, i}); }
	}
	std::sort(all.begin(), all.end(), [](const ref& x, const ref& y) { return x.term != y.term ? x.term < y.term : x.seg < y.seg; });
	struct group {
		std::size_t begin, end;
		std::uint64_t df;
	};
	std::vector<group> groups;
	for (std::size_t i = 0; i < all.size();) {
		group g{i, i, 0};
		for (; g.end < all.size() && all[g.end].term == all[i].term; ++g.end) { g.df += found[all[g.end].seg][all[g.end].at].second.df; }
		groups.push_back(g);
		i = g.end;
	}
	if (max && groups.size() > max) {
		switch (limit.type) {
		case limit_type::error: throw expansion_error(max);
		case limit_type::first: groups.resize(max); break;
		case limit_type::most_frequent:
			std::stable_sort(groups.begin(), groups.end(), [](const group& x, const group& y) { return x.df > y.df; });
			groups.resize(max);
			std::sort(groups.begin(), groups.end(), [](const group& x, const group& y) { return x.begin < y.begin; });
			break;
		}
	}
	expansion out;
	out.segments.resize(segs.size());
	for (const auto& g : groups) {
		const std::size_t t = out.terms.size();
		out.terms.emplace_back(all[g.begin].term);
		out.df.push_back(g.df);
		for (std::size_t i = g.begin; i < g.end; ++i) { out.segments[all[i].seg].push_back({t, found[all[i].seg][all[i].at].second}); }
	}
	return out;
}

// --- the `or` combination (D9) ------------------------------------------------------------------

// Segment s's scorer summing each expanded term's own score, weighted by its snapshot df;
// null when the segment holds none of the terms.
inline scorer_ptr expansion_or(const expansion& e, std::span<const segment_snapshot> segs, std::size_t s, scheme kind = scheme::bm25) {
	const statistics st = snapshot_statistics(segs);
	const segment& seg = *segs[s].seg;
	std::vector<scorer_ptr> parts;
	for (const auto& en : e.segments[s]) {
		format::term_info info = en.info;
		seg.complete_impacts(info);
		const std::uint64_t df = e.df[en.term];
		parts.push_back(std::make_unique<term_scorer>(seg, info, kind == scheme::bm25_xapian ? weight::bm25_xapian(st, df) : weight::bm25(st, df)));
	}
	if (parts.empty()) { return nullptr; }
	if (parts.size() == 1) { return std::move(parts[0]); }
	return std::make_unique<disjunction_scorer>(std::move(parts));
}

}  // namespace strata
