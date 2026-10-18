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
// expansion limits (D8), and the `or` and constant-score combinations of the expanded
// terms (D9).

#pragma once

#include "strata/automata.hh"
#include "strata/budget.hh"
#include "strata/searcher.hh"

#include "task.hh"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata {

// Consecutive dead entries before the walk seeks to the matcher's target (a preset: a seek
// re-decodes up to one block and one restart run).
inline constexpr std::size_t default_seek_after = termdict::max_block_entries;
// Entries between cancellation and pause checks (a preset).
inline constexpr std::size_t default_check_every = 1024;

struct intersect_stats {
	std::uint64_t visited = 0, matched = 0, seeks = 0;
};

// A charge to the query's byte budget was refused (D12; Phase 4's D13a budget error, which
// moves to D13a's home when that lands).
struct query_budget_error : std::runtime_error {
	std::uint64_t bytes;
	explicit query_budget_error(std::uint64_t n) : std::runtime_error("strata: query budget exhausted (" + std::to_string(n) + " bytes refused)"), bytes(n) {}
};

// Charges to permit, throwing query_budget_error when refused; no permit, no charges.
inline automata::charge_fn charge_to(byte_budget::permit* permit) {
	if (!permit) { return {}; }
	return [permit](std::uint64_t n) {
		if (!permit->try_grow(n)) { throw query_budget_error(n); }
	};
}

// How a walk runs (D7): where it resumes, its cancellation and pauses, its budget.
struct walk_options {
	std::string_view from;                      // resume at this key (empty: the literal prefix)
	const task::cancel_token* cancel = nullptr;  // set: the walk throws task::cancelled
	std::function<bool()> pause;                 // true: the walk stops and returns where to resume
	byte_budget::permit* permit = nullptr;       // charged with the matcher's states
	intersect_stats* stats = nullptr;
	std::size_t seek_after = default_seek_after;
	std::size_t check_every = default_check_every;
};

// Calls f(term, info) for every term of the inverted reader the automaton accepts, in
// order, until f returns false. Each info is decoded as find_term decodes it (the
// competitive pairs of a term below 128 postings are left to complete_impacts). Returns
// the key to resume from when `pause` stopped the walk, else none.
template <class Inverted, class A, class F>
std::optional<std::string> intersect(const Inverted& inv, const A& a, F&& f, const walk_options& opt = {}) {
	const termdict::reader& dict = inv.dictionary();
	const auto hi = automata::successor(a.literal());
	std::optional<automata::matcher<A>> m;
	if constexpr (!A::byte_range) { m.emplace(a, automata::default_resume_depth, charge_to(opt.permit)); }
	struct replayed {
		const char* value;
		format::term_run before;  // the run cursor before this value
	};
	std::vector<replayed> replay;
	std::string from(std::max(opt.from, a.literal()));
	intersect_stats local;
	intersect_stats& st = opt.stats ? *opt.stats : local;
	const std::size_t every = std::max<std::size_t>(opt.check_every, 1);
	std::size_t until_check = every;
	std::optional<std::string> resume;
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
				if (--until_check == 0) {
					until_check = every;
					if (opt.cancel && opt.cancel->requested()) { throw task::cancelled{}; }
					if (opt.pause && opt.pause()) {
						resume.emplace(key);
						done = true;
						return false;
					}
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
				if (info.df > inv.doc_count()) { corrupt("document frequency"); }
				if (r.match) {
					++st.matched;
					dead_run = 0;
					if (!f(key, info)) {
						done = true;
						return false;
					}
					return true;
				}
				dead_run = r.died ? dead_run + 1 : 0;
				if constexpr (!A::byte_range) {
					if (dead_run >= opt.seek_after) {
						auto next = m->seek();
						if (!next) {
							done = true;
							return false;
						}
						if (next->size() != key.size() + 1 || !next->starts_with(key)) {  // more than the next key
							from = std::move(*next);
							reseek = true;
							return false;
						}
					}
				}
				return true;
			});
			if (!reseek) { break; }
			++st.seeks;
		}
	} catch (const termdict::corrupt&) {
		corrupt("terms checksum");  // a dictionary block failed its region check
	}
	return resume;
}

// Whether a term has a live document: its postings up to the first one not deleted.
template <class Inverted>
bool has_live(const Inverted& inv, const format::term_info& info, const std::vector<std::uint64_t>& deleted) {
	postings_iterator it(inv, info);
	for (ordinal_t d = it.next(); d != no_more_docs; d = it.next()) {
		if (d / 64 >= deleted.size() || !((deleted[d / 64] >> (d % 64)) & 1)) { return true; }
	}
	return false;
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

// The terms an automaton selects in a snapshot, in term order, each with a live document
// somewhere, with their snapshot document frequencies (deleted documents included) and the
// infos of every segment holding them.
struct expansion {
	struct entry {
		std::size_t term;  // index into terms
		format::term_info info;
		bool live = true;  // the segment holds a live document with the term
	};
	std::vector<std::string> terms;
	std::vector<std::uint64_t> df;
	std::vector<std::vector<entry>> segments;  // by snapshot segment, in term order

	bool empty() const noexcept { return terms.empty(); }
};

// The inverted reader of a snapshot segment: main's segment itself.
struct segment_inverted {
	const segment& operator()(const segment_snapshot& s) const noexcept { return *s.seg; }
};

// Segments are walked in turn into one map of distinct terms, each copied once; under
// `first` the n-th eligible term is a cutoff every later walk stops past (D8).
template <class A, class Get = segment_inverted>
expansion expand(std::span<const segment_snapshot> segs, const A& a, expansion_limit limit = {}, const walk_options& opt = {}, Get get = {}) {
	const std::uint64_t max = limit.max;
	struct held_info {
		std::size_t seg;
		format::term_info info;
		bool live;
	};
	struct held {
		std::uint64_t df = 0;
		bool live = false;
		std::vector<held_info> infos;
	};
	std::map<std::string, held, std::less<>> terms;
	std::uint64_t eligible = 0;  // terms with a live document
	bool full = false;           // `first` holds max eligible terms; the last is the cutoff
	const automata::charge_fn charge = charge_to(opt.permit);
	walk_options walk = opt;
	walk.from = {};
	walk.pause = nullptr;
	for (std::size_t s = 0; s < segs.size(); ++s) {
		const auto& inv = get(segs[s]);
		const bool deletes = segs[s].any_deleted();
		intersect(inv, a, [&](std::string_view term, const format::term_info& info) {
			if (full && term > std::string_view(terms.rbegin()->first)) { return false; }
			const bool live = !deletes || has_live(inv, info, *segs[s].deleted);
			auto it = terms.find(term);
			if (it == terms.end()) {
				if (charge) { charge(term.size() + sizeof(held) + 64); }  // the term, its map node
				it = terms.emplace(std::string(term), held{}).first;
			}
			if (charge) { charge(sizeof(held_info)); }
			held& h = it->second;
			h.df += info.df;
			h.infos.push_back({s, info, live});
			if (live && !h.live) {
				h.live = true;
				++eligible;
				if (max && eligible > max) {
					if (limit.type == limit_type::error) { throw expansion_error(max); }
					if (limit.type == limit_type::first) {
						// Drop the (max + 1)-th eligible term and every term past the new cutoff.
						while (eligible > max) {
							if (terms.rbegin()->second.live) { --eligible; }
							terms.erase(std::prev(terms.end()));
						}
						while (!terms.rbegin()->second.live) { terms.erase(std::prev(terms.end())); }
					}
				}
				if (max && limit.type == limit_type::first && eligible == max) { full = true; }
			}
			return true;
		}, walk);
	}
	// The selection, in term order.
	std::vector<const std::pair<const std::string, held>*> chosen;
	for (const auto& t : terms) {
		if (t.second.live) { chosen.push_back(&t); }
	}
	if (max && chosen.size() > max && limit.type == limit_type::most_frequent) {
		std::stable_sort(chosen.begin(), chosen.end(), [](const auto* x, const auto* y) { return x->second.df > y->second.df; });
		chosen.resize(max);
		std::sort(chosen.begin(), chosen.end(), [](const auto* x, const auto* y) { return x->first < y->first; });
	}
	expansion out;
	out.segments.resize(segs.size());
	for (const auto* t : chosen) {
		const std::size_t i = out.terms.size();
		out.terms.push_back(t->first);
		out.df.push_back(t->second.df);
		for (const auto& h : t->second.infos) { out.segments[h.seg].push_back({i, h.info, h.live}); }
	}
	return out;
}

// --- combinations (D9) --------------------------------------------------------------------------

// Segment s's scorer summing each expanded term's own score, weighted by its snapshot df;
// null when the segment holds none of the terms live.
template <class Get = segment_inverted>
scorer_ptr expansion_or(const expansion& e, std::span<const segment_snapshot> segs, std::size_t s, scheme kind = scheme::bm25, Get get = {}) {
	const statistics st = snapshot_statistics(segs);
	const auto& seg = get(segs[s]);
	std::vector<scorer_ptr> parts;
	for (const auto& en : e.segments[s]) {
		if (!en.live) { continue; }
		format::term_info info = en.info;
		seg.complete_impacts(info);
		const std::uint64_t df = e.df[en.term];
		parts.push_back(std::make_unique<term_scorer>(seg, info, kind == scheme::bm25_xapian ? weight::bm25_xapian(st, df) : weight::bm25(st, df)));
	}
	if (parts.empty()) { return nullptr; }
	if (parts.size() == 1) { return std::move(parts[0]); }
	return std::make_unique<disjunction_scorer>(std::move(parts));
}

// Every match of the scorer it wraps scores `value` (constant score, D9); it ends once
// nothing above `value` is wanted.
class constant_scorer final : public scorer {
public:
	constant_scorer(scorer_ptr inner, double value) : inner_(std::move(inner)), value_(value), doc_(inner_->doc()) {}
	ordinal_t doc() const noexcept override { return doc_; }
	ordinal_t next() override { return doc_ = done_ ? no_more_docs : inner_->next(); }
	ordinal_t advance(ordinal_t t) override { return doc_ = done_ ? no_more_docs : inner_->advance(t); }
	bool two_phase() const noexcept override { return inner_->two_phase(); }
	ordinal_t approx_next() override { return doc_ = done_ ? no_more_docs : inner_->approx_next(); }
	ordinal_t approx_advance(ordinal_t t) override { return doc_ = done_ ? no_more_docs : inner_->approx_advance(t); }
	bool matches() override { return inner_->matches(); }
	std::uint64_t cost() const noexcept override { return inner_->cost(); }
	double score() override { return value_; }
	double max_score(ordinal_t, ordinal_t) override { return value_; }
	ordinal_t block_end(ordinal_t) override { return no_more_docs; }
	double max_score_total() const noexcept override { return value_; }
	void set_min_competitive_score(double s) override { done_ = done_ || s >= value_; }
	std::optional<std::uint64_t> exact_count() override { return inner_->exact_count(); }
	bool matches_all() const noexcept override { return inner_->matches_all(); }
	std::unique_ptr<scorer> fresh() const override {
		auto f = inner_->fresh();
		return f ? std::make_unique<constant_scorer>(std::move(f), value_) : nullptr;
	}
	void fill(ordinal_t base, ordinal_t end, std::uint64_t* bits) override {
		if (done_) {
			doc_ = no_more_docs;
			return;
		}
		inner_->fill(base, end, bits);
		doc_ = inner_->doc();
	}

private:
	scorer_ptr inner_;
	double value_;
	ordinal_t doc_;
	bool done_ = false;
};

// Segment s's union of the expanded live terms, every match scoring `value`, without
// completing impacts; null when the segment holds none of the terms live.
template <class Get = segment_inverted>
scorer_ptr expansion_constant(const expansion& e, std::span<const segment_snapshot> segs, std::size_t s, double value = 1.0, Get get = {}) {
	const auto& seg = get(segs[s]);
	std::vector<scorer_ptr> parts;
	for (const auto& en : e.segments[s]) {
		if (en.live) { parts.push_back(std::make_unique<term_scorer>(seg, en.info)); }
	}
	if (parts.empty()) { return nullptr; }
	scorer_ptr inner;
	if (parts.size() == 1) {
		inner = std::move(parts[0]);
	} else {
		inner = std::make_unique<disjunction_scorer>(std::move(parts));
	}
	return std::make_unique<constant_scorer>(std::move(inner), value);
}

}  // namespace strata
