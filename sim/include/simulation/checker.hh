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
// histories of a versioned register per key, checked per key (P-compositionality, Horn and
// Kroening, FORTE 2015) by a search over linearizations (Wing and Gong, JPDC 1993) with
// Lowe's cache of (linearized set, model state). A key's history splits where no finite
// operation is pending; the checker carries every reachable configuration across the cut.
// An operation without a response (unknown) may take effect after its invocation or never,
// so it carries forward until it is linearized. Past its bounds it says inconclusive.

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
#include <vector>

namespace kronuz::simulation::check {

enum class kind : std::uint8_t { put, erase, get };
// unknown: the client saw no response; the operation may or may not have taken effect.
enum class outcome : std::uint8_t { ok, conflict, missing, unknown };

// One client operation. A put writes `value`; a get's `value` is what it read. `expected` is a
// put's or erase's expected version (0: absent and never written). `version` is the version a
// successful put or erase produced, or the version a get read. Times are simulated ns;
// `returned` is ignored for unknown outcomes.
struct operation {
	std::uint64_t key = 0;
	kind op = kind::get;
	time_ns invoked = 0, returned = 0;
	std::uint64_t value = 0;
	std::optional<std::uint64_t> expected;
	outcome result = outcome::ok;
	std::uint64_t version = 0;
};

enum class verdict : std::uint8_t { linearizable, violation, inconclusive };
struct options {
	std::uint64_t max_nodes = 10'000'000;  // search nodes per segment and configuration
	std::size_t max_configurations = 1'024;  // reachable configurations carried across a cut
};
struct report {
	verdict result = verdict::linearizable;
	std::uint64_t key = 0;  // the first key that is not linearizable, or inconclusive
	std::string detail;
	std::uint64_t keys = 0, segments = 0, nodes = 0;
};

// The register: each successful put or erase advances the version by one.
struct state {
	std::uint64_t version = 0, value = 0;
	bool present = false;
	bool operator==(const state&) const = default;
	auto operator<=>(const state&) const = default;
};

namespace detail {

// Applies `o` to `s` and compares the outcome with the recorded one; false when they differ.
inline bool apply(const operation& o, state& s) {
	const bool unknown = o.result == outcome::unknown;
	switch (o.op) {
	case kind::get:
		if (unknown) { return true; }
		return s.present ? o.result == outcome::ok && o.value == s.value && o.version == s.version : o.result == outcome::missing;
	case kind::put:
		if (o.expected && *o.expected != s.version) { return unknown || o.result == outcome::conflict; }
		if (!unknown && (o.result != outcome::ok || o.version != s.version + 1)) { return false; }
		s = {s.version + 1, o.value, true};
		return true;
	case kind::erase:
		if (o.expected && *o.expected != s.version) { return unknown || o.result == outcome::conflict; }
		if (!s.present) { return unknown || o.result == outcome::missing; }
		if (!unknown && (o.result != outcome::ok || o.version != s.version + 1)) { return false; }
		s = {s.version + 1, 0, false};
		return true;
	}
	return false;
}

// A configuration at a cut: the register's state and the unknown operations not yet applied.
struct configuration {
	state s;
	std::vector<std::size_t> pending;  // indices into the key's operations, sorted
	auto operator<=>(const configuration&) const = default;
};

struct node_key {
	std::vector<std::uint64_t> done;
	state s;
	bool operator==(const node_key&) const = default;
};
struct node_hash {
	std::size_t operator()(const node_key& k) const noexcept {
		std::uint64_t h = mix(k.s.version) ^ mix(k.s.value + 1) ^ (k.s.present ? 0x51ULL : 0);
		for (auto w : k.done) { h = mix(h ^ w); }
		return std::size_t(h);
	}
};

// Every configuration reachable after linearizing all of `segment`'s finite operations,
// starting from `start`. False when the node budget runs out.
inline bool search(const std::vector<operation>& ops, const std::vector<std::size_t>& segment, const configuration& start,
	const options& limits, std::set<configuration>& ends, std::uint64_t& nodes) {
	std::vector<std::size_t> members = start.pending;  // carried unknown operations first
	members.insert(members.end(), segment.begin(), segment.end());
	const auto m = members.size();
	const std::size_t words = (m + 63) / 64;
	auto finite = [&](std::size_t i) { return ops[members[i]].result != outcome::unknown; };
	std::size_t finite_count = 0;
	for (std::size_t i = 0; i < m; ++i) { finite_count += finite(i); }

	std::unordered_set<node_key, node_hash> seen;
	struct frame { node_key key; std::size_t finite_done; };
	std::vector<frame> stack{{{std::vector<std::uint64_t>(words), start.s}, 0}};
	seen.insert(stack.back().key);
	while (!stack.empty()) {
		if (++nodes > limits.max_nodes) { return false; }
		auto f = std::move(stack.back()); stack.pop_back();
		auto is_done = [&](std::size_t i) { return (f.key.done[i / 64] >> (i % 64)) & 1; };
		if (f.finite_done == finite_count) {
			configuration end{f.key.s, {}};
			for (std::size_t i = 0; i < m; ++i) { if (!is_done(i)) { end.pending.push_back(members[i]); } }
			std::sort(end.pending.begin(), end.pending.end());
			ends.insert(std::move(end));
		}
		// A candidate follows no undone operation in real time: its invocation is not after
		// any undone finite operation's response. Equal times count as concurrent.
		time_ns horizon = never;
		for (std::size_t i = 0; i < m; ++i) { if (!is_done(i) && finite(i)) { horizon = std::min(horizon, ops[members[i]].returned); } }
		for (std::size_t i = 0; i < m; ++i) {
			const auto& o = ops[members[i]];
			if (is_done(i) || o.invoked > horizon) { continue; }
			state next = f.key.s;
			if (!apply(o, next)) { continue; }
			node_key child{f.key.done, next};
			child.done[i / 64] |= std::uint64_t(1) << (i % 64);
			if (seen.insert(child).second) { stack.push_back({std::move(child), f.finite_done + finite(i)}); }
		}
	}
	return true;
}

// Drops what cannot matter: a pending conditional operation whose expected version has
// passed can only conflict, a no-op; and a configuration whose pending set is a subset of
// another's at the same state has no future the larger one lacks, since a pending operation
// may also never take effect.
inline std::set<configuration> normalize(const std::vector<operation>& ops, std::set<configuration> in) {
	std::map<state, std::vector<std::vector<std::size_t>>> by_state;
	for (auto c : in) {
		std::erase_if(c.pending, [&](std::size_t i) { return ops[i].expected && *ops[i].expected < c.s.version; });
		by_state[c.s].push_back(std::move(c.pending));
	}
	std::set<configuration> out;
	for (auto& [s, sets] : by_state) {
		std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
		std::vector<std::vector<std::size_t>> kept;
		for (auto& p : sets) {
			const bool dominated = std::any_of(kept.begin(), kept.end(), [&](const auto& k) { return std::includes(k.begin(), k.end(), p.begin(), p.end()); });
			if (!dominated) { kept.push_back(std::move(p)); }
		}
		for (auto& p : kept) { out.insert({s, std::move(p)}); }
	}
	return out;
}

}  // namespace detail

// Checks one key's operations, in any order.
inline report check_key(std::uint64_t key, std::vector<operation> ops, const options& limits = {}) {
	report r; r.key = key; r.keys = 1;
	// A get without a response constrains nothing.
	std::erase_if(ops, [](const operation& o) { return o.op == kind::get && o.result == outcome::unknown; });
	std::stable_sort(ops.begin(), ops.end(), [](const operation& a, const operation& b) { return a.invoked < b.invoked; });
	for (const auto& o : ops) {
		if (o.result != outcome::unknown && o.returned < o.invoked) { r.result = verdict::violation; r.detail = "an operation returned before it was invoked"; return r; }
	}
	// Segments: a cut before operation i when every earlier finite operation returned before it.
	std::vector<std::vector<std::size_t>> segments(1);
	time_ns latest = 0; bool any = false;
	for (std::size_t i = 0; i < ops.size(); ++i) {
		if (any && latest < ops[i].invoked && !segments.back().empty()) { segments.emplace_back(); }
		segments.back().push_back(i);
		if (ops[i].result != outcome::unknown) { latest = any ? std::max(latest, ops[i].returned) : ops[i].returned; any = true; }
	}
	std::set<detail::configuration> configurations{{}};
	for (const auto& segment : segments) {
		++r.segments;
		std::set<detail::configuration> next;
		for (const auto& c : configurations) {
			if (!detail::search(ops, segment, c, limits, next, r.nodes)) {
				r.result = verdict::inconclusive; r.detail = "search budget exhausted"; return r;
			}
		}
		if (next.empty()) { r.result = verdict::violation; r.detail = "no linearization through segment " + std::to_string(r.segments); return r; }
		next = detail::normalize(ops, std::move(next));
		if (next.size() > limits.max_configurations) { r.result = verdict::inconclusive; r.detail = "too many configurations at a cut"; return r; }
		configurations = std::move(next);
	}
	return r;
}

// Checks a whole history: linearizable if and only if every key's operations are.
inline report check(const std::vector<operation>& history, const options& limits = {}) {
	std::map<std::uint64_t, std::vector<operation>> by_key;
	for (const auto& o : history) { by_key[o.key].push_back(o); }
	report total;
	for (auto& [key, ops] : by_key) {
		auto r = check_key(key, std::move(ops), limits);
		total.keys += 1; total.segments += r.segments; total.nodes += r.nodes;
		if (r.result == verdict::violation) { r.keys = total.keys; r.segments = total.segments; r.nodes = total.nodes; return r; }
		if (r.result == verdict::inconclusive && total.result == verdict::linearizable) { total.result = verdict::inconclusive; total.key = key; total.detail = r.detail; }
	}
	return total;
}

}  // namespace kronuz::simulation::check
