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

// The linearizability checker (simulator design D9), shared with Track C's C5: client
// histories of strata's versioned register, checked per component (P-compositionality, Horn
// and Kroening, FORTE 2015) by a search over linearizations (Wing and Gong, JPDC 1993) with
// Lowe's cache of (linearized set, model state). A component is the keys that batches and
// deletes by key range join, so an atomic batch is one step of the search over every key it
// writes. A component's history splits where no finite operation is pending; the checker
// carries the reachable configurations across the cut, recorded only where a segment's last
// finite operation is linearized. An operation without a response (unknown), and a write
// acknowledged before commit (ack local or none), may take effect after its invocation or
// never. Session and nearest reads are checked by the necessary conditions D9 states. Past
// its bounds the checker says inconclusive.

#pragma once

#include "clock.hh"
#include "random.hh"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kronuz::simulation::check {

// erase_range: strata's delete by query over the keys in [key, value).
enum class kind : std::uint8_t { put, erase, get, erase_range };
// unknown: the client saw no response. aborted: a write of a batch another write rejected.
enum class outcome : std::uint8_t { ok, conflict, missing, unknown, aborted };
// strong: a linearizable read, or a write acknowledged at quorum. all, local, none: the other
// write acknowledgements; session, nearest: the other reads (REPLICATION, "Consistency modes").
enum class consistency : std::uint8_t { strong, all, local, none, session, nearest };

// One client operation, as D9's history file records it. A put writes `value`; an ok get's
// `value` is what it read; an erase_range's `value` is the end of its key range. `expected` is
// a put's or erase's expected version (0: the key must be absent). `version` is an ok put's
// written version, an ok get's read version, a conflict's reported current version, an ok
// erase_range's count of keys deleted, and 0 otherwise. An erase of an absent key is accepted
// and deletes nothing, recorded as missing. Writes with one nonzero `batch` form one atomic
// transaction, in history order. Times share one clock; `returned` is ignored for unknown
// outcomes. `client` is the recording process.
struct operation {
	std::uint64_t key = 0;
	kind op = kind::get;
	time_ns invoked = 0, returned = 0;
	std::uint64_t value = 0;
	std::optional<std::uint64_t> expected;
	outcome result = outcome::ok;
	std::uint64_t version = 0;
	std::uint64_t client = 0;
	std::uint64_t batch = 0;
	consistency mode = consistency::strong;
	bool operator==(const operation&) const = default;
};

enum class verdict : std::uint8_t { linearizable, violation, inconclusive };
struct options {
	std::uint64_t max_nodes = 10'000'000;  // search nodes per segment and configuration
	std::uint64_t max_total_nodes = 1'000'000'000;  // search nodes over one component's history
	std::size_t max_configurations = 1'024;  // reachable configurations carried across a cut
	std::size_t max_component_keys = 64;  // keys one component may join
};
struct report {
	verdict result = verdict::linearizable;
	std::uint64_t key = 0;  // the smallest key of the first component that is not linearizable, or inconclusive
	std::string detail;
	std::uint64_t keys = 0, components = 0, segments = 0, nodes = 0;
};

// strata's register (phase-3.md, "Keys and versions"): absent at version 0, or present at
// version v >= 1. A put makes version v + 1 (1 on an absent key, so a re-add starts over);
// an erase makes the key absent.
struct state {
	std::uint64_t version = 0, value = 0;
	bool present = false;
	bool operator==(const state&) const = default;
	auto operator<=>(const state&) const = default;
};

namespace detail {

// A write the client may not have seen take effect.
inline bool indefinite(const operation& o) {
	return o.result == outcome::unknown || (o.op != kind::get && (o.mode == consistency::local || o.mode == consistency::none));
}

// One step of the search: a single operation, or a batch's writes. A session read follows
// `after`, its client's last write; its interval and a nearest read's reach back (D9).
struct atom {
	std::vector<std::size_t> ops;  // indices into the component's operations, in batch order
	time_ns invoked = 0, returned = 0;
	bool finite = true;
	std::size_t after = SIZE_MAX;  // an atom index, or none
};

struct component {
	std::vector<std::uint64_t> keys;  // sorted
	std::vector<operation> ops;
	std::vector<atom> atoms;
	std::size_t index(std::uint64_t key) const { return std::size_t(std::lower_bound(keys.begin(), keys.end(), key) - keys.begin()); }
};

using states = std::vector<state>;  // by component key index

// Evaluates an atom's writes on `s` as strata applies a transaction: each write's expected
// version checked in order on an overlay, the first mismatch rejecting the whole atom with
// the current version. Fills each write's outcome and version; false when rejected (s kept).
inline bool evaluate(const component& c, const atom& a, states& s, std::vector<std::pair<outcome, std::uint64_t>>& out) {
	states overlay = s;
	out.clear();
	for (std::size_t j = 0; j < a.ops.size(); ++j) {
		const auto& o = c.ops[a.ops[j]];
		if (o.op == kind::erase_range) {
			std::uint64_t count = 0;
			for (auto k = c.index(o.key); k < c.keys.size() && c.keys[k] < o.value; ++k) {
				if (overlay[k].present) { overlay[k] = {}; ++count; }
			}
			out.push_back({outcome::ok, count});
			continue;
		}
		auto& st = overlay[c.index(o.key)];
		if (o.expected && *o.expected != st.version) {
			out.assign(a.ops.size(), {outcome::aborted, 0});
			out[j] = {outcome::conflict, st.version};
			return false;
		}
		if (o.op == kind::put) { st = {st.version + 1, o.value, true}; out.push_back({outcome::ok, st.version}); continue; }
		out.push_back({st.present ? outcome::ok : outcome::missing, 0});
		st = {};
	}
	s = std::move(overlay);
	return true;
}

// Applies an atom to `s` and compares the outcomes with the recorded ones; false when they
// differ.
inline bool apply(const component& c, const atom& a, states& s) {
	const auto& first = c.ops[a.ops.front()];
	if (first.op == kind::get) {
		const auto& st = s[c.index(first.key)];
		return st.present ? first.result == outcome::ok && first.value == st.value && first.version == st.version : first.result == outcome::missing;
	}
	states next = s;
	std::vector<std::pair<outcome, std::uint64_t>> out;
	const bool accepted = evaluate(c, a, next, out);
	if (!a.finite) {
		if (accepted) { s = std::move(next); }
		return true;
	}
	for (std::size_t j = 0; j < a.ops.size(); ++j) {
		const auto& o = c.ops[a.ops[j]];
		if (o.result != out[j].first || o.version != out[j].second) { return false; }
	}
	if (accepted) { s = std::move(next); }
	return true;
}

// A configuration at a cut: the component's state and the indefinite atoms not yet applied.
struct configuration {
	states s;
	std::vector<std::size_t> pending;  // atom indices, sorted
	auto operator<=>(const configuration&) const = default;
};

struct node_key {
	std::vector<std::uint64_t> done;
	states s;
	bool operator==(const node_key&) const = default;
};
struct node_hash {
	std::size_t operator()(const node_key& k) const noexcept {
		std::uint64_t h = 0;
		for (const auto& st : k.s) { h = mix(h ^ mix(st.version) ^ mix(st.value + 1) ^ (st.present ? 0x51ULL : 0)); }
		for (auto w : k.done) { h = mix(h ^ w); }
		return std::size_t(h);
	}
};

// The configurations reached where the last of `segment`'s finite atoms is linearized,
// starting from `start`; null, or the budget that ran out. A configuration that applies more
// carried atoms after that point is not recorded: the next segment can apply them first, so
// it has no future the recorded one lacks.
inline const char* search(const component& c, const std::vector<std::size_t>& segment, const configuration& start,
	const options& limits, std::set<configuration>& ends, std::uint64_t& total) {
	std::vector<std::size_t> members = start.pending;  // carried atoms first
	members.insert(members.end(), segment.begin(), segment.end());
	const auto m = members.size();
	const std::size_t words = (m + 63) / 64;
	auto finite = [&](std::size_t i) { return c.atoms[members[i]].finite; };
	std::size_t finite_count = 0;
	for (std::size_t i = 0; i < m; ++i) { finite_count += finite(i); }
	// A session read's write, by member position when it is a member.
	std::vector<std::size_t> after(m, SIZE_MAX);
	for (std::size_t i = 0; i < m; ++i) {
		const auto w = c.atoms[members[i]].after;
		if (w == SIZE_MAX) { continue; }
		for (std::size_t j = 0; j < m; ++j) { if (members[j] == w) { after[i] = j; } }
	}

	std::unordered_set<node_key, node_hash> seen;
	struct frame { node_key key; std::size_t finite_done; };
	std::vector<frame> stack{{{std::vector<std::uint64_t>(words), start.s}, 0}};
	seen.insert(stack.back().key);
	std::uint64_t nodes = 0;
	while (!stack.empty()) {
		if (++nodes > limits.max_nodes) { return "search budget exhausted in one segment"; }
		if (++total > limits.max_total_nodes) { return "search budget exhausted for the component"; }
		auto f = std::move(stack.back()); stack.pop_back();
		auto is_done = [&](std::size_t i) { return (f.key.done[i / 64] >> (i % 64)) & 1; };
		if (f.finite_done == finite_count) {
			configuration end{f.key.s, {}};
			for (std::size_t i = 0; i < m; ++i) { if (!is_done(i)) { end.pending.push_back(members[i]); } }
			std::sort(end.pending.begin(), end.pending.end());  // distinct indices: no ties
			ends.insert(std::move(end));
			continue;
		}
		// A candidate follows no undone atom in real time: its invocation is not after any
		// undone finite atom's response. Equal times count as concurrent.
		time_ns horizon = never;
		for (std::size_t i = 0; i < m; ++i) { if (!is_done(i) && finite(i)) { horizon = std::min(horizon, c.atoms[members[i]].returned); } }
		for (std::size_t i = 0; i < m; ++i) {
			const auto& a = c.atoms[members[i]];
			if (is_done(i) || a.invoked > horizon || (after[i] != SIZE_MAX && !is_done(after[i]))) { continue; }
			states next = f.key.s;
			if (!apply(c, a, next)) { continue; }
			node_key child{f.key.done, std::move(next)};
			child.done[i / 64] |= std::uint64_t(1) << (i % 64);
			if (seen.insert(child).second) { stack.push_back({std::move(child), f.finite_done + finite(i)}); }
		}
	}
	return nullptr;
}

// Drops what cannot matter: a configuration whose pending set is a subset of another's at
// the same state has no future the larger one lacks, since a pending atom may also never take
// effect. In a component without erases versions never decrease, so a pending conditional
// write whose expected version has passed can only conflict, a no-op; after an erase a
// version recurs, and the prune would drop writes that can still succeed.
inline std::set<configuration> normalize(const component& c, std::set<configuration> in, bool monotone) {
	std::map<states, std::vector<std::vector<std::size_t>>> by_state;
	for (auto x : in) {
		if (monotone) {
			std::erase_if(x.pending, [&](std::size_t i) {
				const auto& a = c.atoms[i];
				if (a.ops.size() != 1) { return false; }
				const auto& o = c.ops[a.ops.front()];
				return o.expected && *o.expected < x.s[c.index(o.key)].version;
			});
		}
		by_state[x.s].push_back(std::move(x.pending));
	}
	std::set<configuration> out;
	for (auto& [s, sets] : by_state) {
		// Larger sets first; ties in a total order (D2).
		std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) { return a.size() != b.size() ? a.size() > b.size() : a < b; });
		std::vector<std::vector<std::size_t>> kept;
		for (auto& p : sets) {
			const bool dominated = std::any_of(kept.begin(), kept.end(), [&](const auto& k) { return std::includes(k.begin(), k.end(), p.begin(), p.end()); });
			if (!dominated) { kept.push_back(std::move(p)); }
		}
		for (auto& p : kept) { out.insert({s, std::move(p)}); }
	}
	return out;
}

// Builds the component's atoms from its operations (in history order).
inline const char* build_atoms(component& c) {
	std::map<std::uint64_t, std::size_t> batches;  // batch id -> atom
	std::vector<std::size_t> atom_of(c.ops.size());
	for (std::size_t i = 0; i < c.ops.size(); ++i) {
		const auto& o = c.ops[i];
		if (o.op == kind::get && o.result == outcome::unknown) { atom_of[i] = SIZE_MAX; continue; }  // constrains nothing
		if (o.result != outcome::unknown && o.returned < o.invoked) { return "an operation returned before it was invoked"; }
		if (o.batch != 0 && o.op != kind::get) {
			auto [found, fresh] = batches.try_emplace(o.batch, c.atoms.size());
			if (fresh) { c.atoms.push_back({{}, o.invoked, o.returned, true, SIZE_MAX}); }
			auto& a = c.atoms[found->second];
			a.ops.push_back(i);
			a.invoked = std::min(a.invoked, o.invoked); a.returned = std::max(a.returned, o.returned);
			a.finite = a.finite && !indefinite(o);
			atom_of[i] = found->second;
			continue;
		}
		atom_of[i] = c.atoms.size();
		c.atoms.push_back({{i}, o.invoked, o.returned, !indefinite(o), SIZE_MAX});
	}
	// Reads below linearizable: a nearest read sees some state before its response; a session
	// read sees one at or after its client's last write before it and before its response.
	for (std::size_t i = 0; i < c.ops.size(); ++i) {
		const auto& o = c.ops[i];
		if (o.op != kind::get || atom_of[i] == SIZE_MAX) { continue; }
		auto& a = c.atoms[atom_of[i]];
		if (o.mode == consistency::nearest) { a.invoked = 0; continue; }
		if (o.mode != consistency::session) { continue; }
		a.invoked = 0;
		std::size_t last = SIZE_MAX;
		for (std::size_t j = 0; j < c.ops.size(); ++j) {
			const auto& w = c.ops[j];
			if (w.op == kind::get || w.client != o.client || atom_of[j] == SIZE_MAX || !c.atoms[atom_of[j]].finite || w.returned > o.invoked) { continue; }
			if (last == SIZE_MAX || w.returned >= c.ops[last].returned) { last = j; }
		}
		if (last != SIZE_MAX) { a.after = atom_of[last]; a.invoked = c.atoms[a.after].invoked; }
	}
	return nullptr;
}

inline report check_component(component c, const options& limits) {
	report r; r.key = c.keys.front(); r.keys = c.keys.size(); r.components = 1;
	if (c.keys.size() > limits.max_component_keys) { r.result = verdict::inconclusive; r.detail = "too many keys joined in one component"; return r; }
	if (const char* error = build_atoms(c)) { r.result = verdict::violation; r.detail = error; return r; }
	std::vector<std::size_t> order(c.atoms.size());
	for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
	// By invocation; a session read reaches back to its write's invocation and sorts after it,
	// so no cut falls between them.
	std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
		const auto& x = c.atoms[a]; const auto& y = c.atoms[b];
		return x.invoked != y.invoked ? x.invoked < y.invoked : (x.after == SIZE_MAX && y.after != SIZE_MAX);
	});
	// Segments: a cut before an atom when every earlier finite atom returned before it.
	std::vector<std::vector<std::size_t>> segments(1);
	time_ns latest = 0; bool any = false;
	for (const auto i : order) {
		const auto& a = c.atoms[i];
		if (any && latest < a.invoked && !segments.back().empty()) { segments.emplace_back(); }
		segments.back().push_back(i);
		if (a.finite) { latest = any ? std::max(latest, a.returned) : a.returned; any = true; }
	}
	const bool monotone = std::none_of(c.ops.begin(), c.ops.end(), [](const operation& o) { return o.op == kind::erase || o.op == kind::erase_range; });
	std::set<configuration> configurations;
	configurations.insert(configuration{states(c.keys.size()), {}});
	for (const auto& segment : segments) {
		++r.segments;
		std::set<configuration> next;
		for (const auto& x : configurations) {
			if (const char* exhausted = search(c, segment, x, limits, next, r.nodes)) {
				r.result = verdict::inconclusive; r.detail = exhausted; return r;
			}
		}
		if (next.empty()) { r.result = verdict::violation; r.detail = "no linearization through segment " + std::to_string(r.segments); return r; }
		next = normalize(c, std::move(next), monotone);
		if (next.size() > limits.max_configurations) { r.result = verdict::inconclusive; r.detail = "too many configurations at a cut"; return r; }
		configurations = std::move(next);
	}
	return r;
}

}  // namespace detail

// Checks a whole history: linearizable if and only if every component's operations are.
// Batches join the keys they write, and an erase_range joins the keys in its range.
inline report check(const std::vector<operation>& history, const options& limits = {}) {
	std::map<std::uint64_t, std::size_t> index;  // key -> union-find slot
	for (const auto& o : history) { index.try_emplace(o.key, 0); }
	std::size_t n = 0;
	for (auto& [key, slot] : index) { slot = n++; }
	std::vector<std::size_t> parent(n);
	for (std::size_t i = 0; i < n; ++i) { parent[i] = i; }
	auto find = [&](std::size_t x) { while (parent[x] != x) { x = parent[x] = parent[parent[x]]; } return x; };
	auto join = [&](std::size_t a, std::size_t b) { a = find(a); b = find(b); if (a != b) { parent[std::max(a, b)] = std::min(a, b); } };
	std::map<std::uint64_t, std::size_t> batch_key;
	for (const auto& o : history) {
		const auto slot = index.at(o.key);
		if (o.batch != 0 && o.op != kind::get) {
			auto [found, fresh] = batch_key.try_emplace(o.batch, slot);
			if (!fresh) { join(found->second, slot); }
		}
		if (o.op == kind::erase_range) {
			for (auto k = index.lower_bound(o.key); k != index.end() && k->first < o.value; ++k) { join(slot, k->second); }
		}
	}
	std::map<std::size_t, detail::component> components;  // by root slot: ordered by smallest key
	for (const auto& [key, slot] : index) { components[find(slot)].keys.push_back(key); }
	for (const auto& o : history) { components[find(index.at(o.key))].ops.push_back(o); }
	report total;
	for (auto& [root, c] : components) {
		auto r = detail::check_component(std::move(c), limits);
		total.keys += r.keys; total.components += 1; total.segments += r.segments; total.nodes += r.nodes;
		if (r.result == verdict::violation) { r.keys = total.keys; r.components = total.components; r.segments = total.segments; r.nodes = total.nodes; return r; }
		if (r.result == verdict::inconclusive && total.result == verdict::linearizable) { total.result = verdict::inconclusive; total.key = r.key; total.detail = r.detail; }
	}
	return total;
}

}  // namespace kronuz::simulation::check
