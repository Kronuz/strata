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

// The linearizability checker (simulator design D9, gate row T3), over strata's register: a
// hand-built corpus of linearizable histories, all accepted, and of non-linearizable ones,
// all rejected: re-adds at version 1, expected 0 after an erase, and conflict versions
// (SIM-R3); torn, rejected, and unknown batches, deletes by key range, session and nearest
// reads, and writes acknowledged before commit (SYS-10). Generated concurrent histories with
// batches and range deletes, linearizable by construction, accepted, and mutated into
// violations of four classes (stale read, lost write, reordered versions, an unknown write
// seen then unseen), each a violation by construction, all rejected. Agreement with an
// independent brute force on random small histories over two keys. Carried unknown writes
// that stay conclusive and a node budget per segment (SIM-R13), and inconclusive, never pass,
// past the bounds.
#include "simulation/checker.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;
using namespace kronuz::simulation::check;

namespace {

using C = consistency;
operation make(std::uint64_t key, kind op, time_ns a, time_ns b, std::uint64_t value, std::optional<std::uint64_t> expected, outcome r, std::uint64_t version) {
	return {key, op, a, b, value, expected, r, version, 0, 0, C::strong};
}
operation put(time_ns a, time_ns b, std::uint64_t value, std::uint64_t version, std::uint64_t key = 1) { return make(key, kind::put, a, b, value, {}, outcome::ok, version); }
operation put_unknown(time_ns a, std::uint64_t value, std::uint64_t key = 1) { return make(key, kind::put, a, 0, value, {}, outcome::unknown, 0); }
operation put_if(time_ns a, time_ns b, std::uint64_t value, std::uint64_t expected, outcome r, std::uint64_t version, std::uint64_t key = 1) { return make(key, kind::put, a, b, value, expected, r, version); }
operation erase(time_ns a, time_ns b, outcome r, std::uint64_t version, std::uint64_t key = 1) { return make(key, kind::erase, a, b, 0, {}, r, version); }
operation erase_if(time_ns a, time_ns b, std::uint64_t expected, outcome r, std::uint64_t version, std::uint64_t key = 1) { return make(key, kind::erase, a, b, 0, expected, r, version); }
operation erase_range(time_ns a, time_ns b, std::uint64_t lo, std::uint64_t hi, outcome r, std::uint64_t count) { return make(lo, kind::erase_range, a, b, hi, {}, r, count); }
operation get(time_ns a, time_ns b, std::uint64_t value, std::uint64_t version, std::uint64_t key = 1) { return make(key, kind::get, a, b, value, {}, outcome::ok, version); }
operation get_missing(time_ns a, time_ns b, std::uint64_t key = 1) { return make(key, kind::get, a, b, 0, {}, outcome::missing, 0); }
operation in_batch(operation o, std::uint64_t batch) { o.batch = batch; return o; }
operation by(operation o, std::uint64_t client, C mode = C::strong) { o.client = client; o.mode = mode; return o; }

verdict run(const std::vector<operation>& h, const options& o = {}) { return check::check(h, o).result; }

// strata's register, written from phase-3.md's rules apart from checker.hh: the generators
// and the brute force share it.
struct model { bool present = false; std::uint64_t version = 0, value = 0; };
using models = std::map<std::uint64_t, model>;
// Sets a single write's or read's outcome as strata answers it at m, and applies it.
void execute(operation& o, model& m) {
	if (o.op == kind::get) {
		if (m.present) { o.result = outcome::ok; o.value = m.value; o.version = m.version; } else { o.result = outcome::missing; o.value = 0; o.version = 0; }
		return;
	}
	const std::uint64_t current = m.present ? m.version : 0;
	if (o.expected && (*o.expected == 0 ? m.present : (!m.present || *o.expected != m.version))) { o.result = outcome::conflict; o.version = current; return; }
	if (o.op == kind::put) { m = {true, m.present ? m.version + 1 : 1, o.value}; o.result = outcome::ok; o.version = m.version; return; }
	o.result = m.present ? outcome::ok : outcome::missing;
	o.version = 0;
	m = {};
}
// Runs an atom (one operation, or a batch's writes in order) as strata applies a transaction:
// the first conflict rejects it whole and the rest abort. Sets outcomes; true when it took
// effect.
bool execute_atom(std::vector<operation>& h, const std::vector<std::size_t>& members, models& state) {
	auto overlay = state;
	for (std::size_t j = 0; j < members.size(); ++j) {
		auto& o = h[members[j]];
		if (o.op == kind::erase_range) {
			std::uint64_t count = 0;
			for (auto& [k, m] : overlay) { if (k >= o.key && k < o.value && m.present) { m = {}; ++count; } }
			o.result = outcome::ok; o.version = count;
			continue;
		}
		execute(o, overlay[o.key]);
		if (o.result == outcome::conflict) {
			for (std::size_t x = 0; x < members.size(); ++x) { if (x != j) { h[members[x]].result = outcome::aborted; h[members[x]].version = 0; } }
			return false;
		}
	}
	state = std::move(overlay);
	return true;
}
// An outcome hidden from the client: the fields D9's format keeps for an unknown.
void hide(operation& o) { o.result = outcome::unknown; o.returned = 0; o.version = 0; if (o.op == kind::get) { o.value = 0; } }

// Concurrent sequential clients over a few keys: each operation, batch of two writes, or
// delete of every key takes effect at a point in its interval, in that order; some responses
// are lost (unknown), half of those never took effect, and the client continues under a new
// id. Values are unique. Linearizable by construction.
std::vector<operation> generate(std::uint64_t seed, std::size_t clients, std::size_t keys, std::size_t rounds) {
	stream s(seed);
	struct pending { std::vector<operation> ops; time_ns point; bool applies, unknown; };
	std::vector<pending> all;
	std::uint64_t next_value = 1, next_client = 1, next_batch = 1;
	std::vector<std::uint64_t> client(clients);
	for (auto& c : client) { c = next_client++; }
	auto write = [&](operation& o) {
		o.key = 1 + s.below(keys);
		o.op = s.below(5) < 4 ? kind::put : kind::erase;
		if (o.op == kind::put) { o.value = next_value++; }
		if (s.chance_ppm(250'000)) { o.expected = s.below(4); }
	};
	for (std::size_t r = 0; r < rounds; ++r) {
		const time_ns base = r * 1'000'000;  // rounds leave quiet gaps, so histories have cuts
		for (std::size_t c = 0; c < clients; ++c) {
			time_ns t = base + s.below(1'000);
			for (int k = 0, n = int(1 + s.below(3)); k < n; ++k) {
				operation o; o.client = client[c]; o.invoked = t;
				const time_ns point = t + s.below(50'000);
				o.returned = point + s.below(50'000);
				pending p{{}, point, true, s.chance_ppm(50'000)};
				const auto pick = s.below(100);
				if (pick < 8) {  // a batch of two writes
					o.batch = next_batch++;
					for (int w = 0; w < 2; ++w) { operation x = o; write(x); p.ops.push_back(x); }
				} else if (pick < 10) {  // delete by query over every key
					o.op = kind::erase_range; o.key = 1; o.value = keys + 1;
					p.ops.push_back(o);
				} else if (pick < 55) {
					write(o); p.ops.push_back(o);
				} else {
					o.op = kind::get; o.key = 1 + s.below(keys); p.ops.push_back(o);
				}
				if (p.unknown) { p.applies = s.chance_ppm(500'000); client[c] = next_client++; }
				t = o.returned + 1 + s.below(10'000);
				all.push_back(std::move(p));
			}
		}
	}
	std::stable_sort(all.begin(), all.end(), [](const pending& a, const pending& b) { return a.point < b.point; });
	models state;
	std::vector<operation> history;
	for (auto& p : all) {
		std::vector<std::size_t> members(p.ops.size());
		for (std::size_t i = 0; i < members.size(); ++i) { members[i] = i; }
		if (p.applies) { execute_atom(p.ops, members, state); }
		for (auto& o : p.ops) {
			if (p.unknown) { hide(o); }
			history.push_back(o);
		}
	}
	return history;
}

// Mutations that make a generated history a violation by construction, since values are
// unique per write. Each returns false when the history has no candidate.
const operation* writer(const std::vector<operation>& h, const operation& g) {
	for (const auto& p : h) { if (p.op == kind::put && p.key == g.key && p.value == g.value && p.result == outcome::ok) { return &p; } }
	return nullptr;
}
bool answered(const operation& o) { return o.result != outcome::unknown; }
// A stale read: a get after P2 finished reads P1's value, where P1 finished before P2 began.
bool stale_read(std::vector<operation>& h) {
	for (auto& g : h) {
		if (g.op != kind::get || g.result != outcome::ok) { continue; }
		for (const auto& p2 : h) {
			if (p2.op != kind::put || p2.key != g.key || p2.result != outcome::ok || !(p2.returned < g.invoked)) { continue; }
			for (const auto& p1 : h) {
				if (p1.op != kind::put || p1.key != g.key || p1.result != outcome::ok || !(p1.returned < p2.invoked) || p1.value == g.value) { continue; }
				g.value = p1.value; g.version = p1.version;
				return true;
			}
		}
	}
	return false;
}
// A lost write: a get after an ok put reads missing, where no erase could fall between them.
bool lost_write(std::vector<operation>& h) {
	for (auto& g : h) {
		if (g.op != kind::get || g.result != outcome::ok) { continue; }
		for (const auto& p : h) {
			if (p.op != kind::put || p.key != g.key || p.result != outcome::ok || !(p.returned < g.invoked)) { continue; }
			const bool erasable = std::any_of(h.begin(), h.end(), [&](const operation& e) {
				const bool erases = (e.op == kind::erase && e.key == g.key) || (e.op == kind::erase_range && e.key <= g.key && g.key < e.value);
				return erases && e.invoked <= g.returned && !(answered(e) && e.returned < p.invoked);
			});
			if (erasable) { continue; }
			g.result = outcome::missing; g.value = 0; g.version = 0;
			return true;
		}
	}
	return false;
}
// Reordered versions: two gets in real-time order swap what they read, where the first value's
// write finished before the second's began.
bool reordered(std::vector<operation>& h) {
	for (auto& g1 : h) {
		if (g1.op != kind::get || g1.result != outcome::ok) { continue; }
		for (auto& g2 : h) {
			if (g2.op != kind::get || g2.key != g1.key || g2.result != outcome::ok || !(g1.returned < g2.invoked) || g1.value == g2.value) { continue; }
			const auto* p1 = writer(h, g1); const auto* p2 = writer(h, g2);
			if (!p1 || !p2 || !(p1->returned < p2->invoked)) { continue; }
			std::swap(g1.value, g2.value); std::swap(g1.version, g2.version);
			return true;
		}
	}
	return false;
}
// An unknown write seen, then unseen: a get reads an unknown put's value, and a later get reads
// a value whose write finished before the first get began.
bool seen_then_unseen(std::vector<operation>& h) {
	for (const auto& u : h) {
		if (u.op != kind::put || u.result != outcome::unknown) { continue; }
		for (auto& g1 : h) {
			if (g1.op != kind::get || g1.key != u.key || g1.result != outcome::ok || !(u.invoked < g1.invoked) || g1.value == u.value) { continue; }
			for (const auto& g2 : h) {
				if (g2.op != kind::get || g2.key != u.key || g2.result != outcome::ok || !(g1.returned < g2.invoked)) { continue; }
				const auto* p = writer(h, g2);
				if (!p || !(p->returned < g1.invoked)) { continue; }
				g1.value = u.value; g1.version = 1;
				return true;
			}
		}
	}
	return false;
}

// The brute force: every order of the atoms that respects D9's ordering rules, each
// indefinite atom applied at some point or never, run on the model.
struct brute_atom { std::vector<std::size_t> ops; time_ns invoked, returned; bool finite, nearest; std::size_t after; };
bool indefinite_write(const operation& o) { return o.op != kind::get && (o.mode == C::local || o.mode == C::none); }
std::vector<brute_atom> brute_atoms(const std::vector<operation>& h) {
	std::vector<brute_atom> atoms;
	std::map<std::uint64_t, std::size_t> batches;
	for (std::size_t i = 0; i < h.size(); ++i) {
		const auto& o = h[i];
		if (o.op == kind::get && o.result == outcome::unknown) { continue; }
		const bool definite = answered(o) && !indefinite_write(o);
		if (o.batch != 0 && o.op != kind::get) {
			const auto [found, fresh] = batches.try_emplace(o.batch, atoms.size());
			if (fresh) { atoms.push_back({{}, o.invoked, o.returned, true, false, SIZE_MAX}); }
			atoms[found->second].ops.push_back(i);
			atoms[found->second].finite = atoms[found->second].finite && definite;
			continue;
		}
		atoms.push_back({{i}, o.invoked, o.returned, definite, o.op == kind::get && o.mode == C::nearest, SIZE_MAX});
	}
	// Keys joined by batches and range deletes: a session read follows its client's last
	// definite write in its own component.
	std::map<std::uint64_t, std::uint64_t> root;
	for (const auto& o : h) { root[o.key] = o.key; }
	auto find = [&](std::uint64_t k) { while (root[k] != k) { k = root[k]; } return k; };
	auto join = [&](std::uint64_t a, std::uint64_t b) { a = find(a); b = find(b); root[std::max(a, b)] = std::min(a, b); };
	for (const auto& a : atoms) { for (auto i : a.ops) { join(h[a.ops.front()].key, h[i].key); } }
	for (const auto& o : h) { if (o.op == kind::erase_range) { for (const auto& p : h) { if (p.key >= o.key && p.key < o.value) { join(o.key, p.key); } } } }
	for (auto& r : atoms) {
		const auto& o = h[r.ops.front()];
		if (o.op != kind::get || o.mode != C::session) { continue; }
		for (std::size_t j = 0; j < atoms.size(); ++j) {
			const auto& w = h[atoms[j].ops.front()];
			if (w.op == kind::get || w.client != o.client || !atoms[j].finite || w.returned > o.invoked || find(w.key) != find(o.key)) { continue; }
			if (r.after == SIZE_MAX || w.returned >= h[atoms[r.after].ops.front()].returned) { r.after = j; }
		}
	}
	return atoms;
}
bool must_precede(const std::vector<brute_atom>& atoms, std::size_t j, std::size_t i, const std::vector<operation>& h) {
	const auto& r = atoms[i];
	const auto& o = h[r.ops.front()];
	if (o.op == kind::get && o.mode == C::nearest) { return false; }
	if (o.op == kind::get && o.mode == C::session) { return j == r.after; }
	return atoms[j].finite && atoms[j].returned < r.invoked;
}
bool atom_matches(const std::vector<operation>& h, const brute_atom& a, models& state) {
	std::vector<operation> scratch = h;
	auto next = state;
	const bool effect = execute_atom(scratch, a.ops, next);
	if (a.finite) {
		for (auto i : a.ops) {
			const auto& want = scratch[i]; const auto& got = h[i];
			if (want.result != got.result || want.version != got.version || (got.op == kind::get && got.result == outcome::ok && want.value != got.value)) { return false; }
		}
	}
	if (effect) { state = std::move(next); }
	return true;
}
bool brute(const std::vector<operation>& h, const std::vector<brute_atom>& atoms, std::vector<char>& used, const models& state, std::size_t left) {
	if (left == 0) { return true; }
	for (std::size_t i = 0; i < atoms.size(); ++i) {
		if (used[i]) { continue; }
		bool ready = true;
		for (std::size_t j = 0; j < atoms.size() && ready; ++j) { ready = used[j] || j == i || !must_precede(atoms, j, i, h); }
		if (!ready) { continue; }
		models next = state;
		if (!atom_matches(h, atoms[i], next)) { continue; }
		used[i] = 1;
		if (brute(h, atoms, used, next, left - atoms[i].finite)) { return true; }
		used[i] = 0;
	}
	return false;
}
bool brute(const std::vector<operation>& h) {
	const auto atoms = brute_atoms(h);
	std::vector<char> used(atoms.size());
	return brute(h, atoms, used, {}, std::size_t(std::count_if(atoms.begin(), atoms.end(), [](const brute_atom& a) { return a.finite; })));
}
// Keeps a record within D9's format: a version only where an outcome reports one, a value
// only for puts, ranges, and ok gets, aborts only in batches.
void conform(operation& o) {
	if (o.result == outcome::aborted && o.batch == 0) { o.result = outcome::conflict; }
	if (o.op == kind::get) { o.expected.reset(); if (o.result == outcome::conflict || o.result == outcome::aborted) { o.result = outcome::missing; } }
	if (o.op == kind::put && o.result == outcome::missing) { o.result = outcome::conflict; }
	if (o.op == kind::erase_range) { o.expected.reset(); if (o.result == outcome::conflict || o.result == outcome::missing) { o.result = outcome::ok; } }
	if (!(o.result == outcome::conflict || (o.result == outcome::ok && o.op != kind::erase))) { o.version = 0; }
	if (!(o.op == kind::put || o.op == kind::erase_range || (o.op == kind::get && o.result == outcome::ok))) { o.value = 0; }
	if (o.result == outcome::unknown) { o.returned = 0; }
}
// Up to 6 atoms over keys 1 and 2 and a short span, so they overlap and cut; values repeat.
// Batches of two writes, range deletes, session and nearest reads, writes acknowledged at
// every level. Half are linearizable by construction (a write acknowledged before commit may
// be lost), half have arbitrary outcomes, and half of each get one field changed.
std::vector<operation> small_history(stream& s) {
	std::vector<operation> ops;
	std::vector<std::vector<std::size_t>> atoms;
	const auto n = 1 + s.below(6);
	for (std::uint64_t a = 0; a < n; ++a) {
		operation o; o.client = 1 + s.below(3); o.invoked = s.below(60); o.returned = o.invoked + s.below(30);
		const auto pick = s.below(20);
		const auto ack = s.below(10);
		const C write_mode = ack < 7 ? C::strong : ack == 7 ? C::all : ack == 8 ? C::local : C::none;
		auto write = [&](operation x) {
			x.key = 1 + s.below(2);
			x.op = s.below(3) < 2 ? kind::put : kind::erase;
			if (x.op == kind::put) { x.value = 1 + s.below(3); }
			if (s.below(3) == 0) { x.expected = s.below(3); }
			x.mode = write_mode;
			return x;
		};
		std::vector<std::size_t> members;
		if (pick < 3) {
			o.batch = a + 1;
			for (int w = 0; w < 2; ++w) { members.push_back(ops.size()); ops.push_back(write(o)); }
		} else if (pick < 4) {
			o.op = kind::erase_range; o.key = 1; o.value = 3; o.mode = write_mode;
			members.push_back(ops.size()); ops.push_back(o);
		} else if (pick < 11) {
			members.push_back(ops.size()); ops.push_back(write(o));
		} else {
			o.op = kind::get; o.key = 1 + s.below(2);
			const auto m = s.below(5);
			o.mode = m < 3 ? C::strong : m == 3 ? C::session : C::nearest;
			members.push_back(ops.size()); ops.push_back(o);
		}
		atoms.push_back(std::move(members));
	}
	if (s.below(2) == 0) {
		struct at { std::size_t atom; time_ns point; };
		std::vector<at> order;
		for (std::size_t a = 0; a < atoms.size(); ++a) {
			const auto& o = ops[atoms[a].front()];
			order.push_back({a, o.invoked + s.below(o.returned - o.invoked + 1)});
		}
		std::stable_sort(order.begin(), order.end(), [](const at& x, const at& y) { return x.point < y.point; });
		models state;
		for (const auto& x : order) {
			const auto& members = atoms[x.atom];
			const bool unknown = s.below(6) == 0;
			const bool lost = unknown || indefinite_write(ops[members.front()]);
			if (!lost || s.below(2) == 0) { execute_atom(ops, members, state); }
			else { models scratch = state; execute_atom(ops, members, scratch); }  // the outcome the client was told
			if (unknown) { for (auto i : members) { hide(ops[i]); } }
		}
	} else {
		for (auto& o : ops) {
			o.result = outcome(s.below(5));
			o.version = s.below(4);
			if (o.op == kind::get) { o.value = 1 + s.below(3); }
		}
	}
	if (s.below(2) == 0) {
		auto& o = ops[s.below(ops.size())];
		switch (s.below(4)) {
		case 0: o.result = outcome(s.below(5)); break;
		case 1: o.version = o.version + 1; break;
		case 2: o.version = o.version ? o.version - 1 : 0; break;
		default: o.value = 1 + s.below(3); break;
		}
	}
	for (auto& o : ops) { conform(o); }
	return ops;
}

}  // namespace

int main() {
	// Linearizable, by hand.
	CHECK(run({}) == verdict::linearizable);
	CHECK(run({put(0, 10, 7, 1), get(20, 30, 7, 1), erase(40, 50, outcome::ok, 0), get_missing(60, 70), erase(80, 90, outcome::missing, 0)}) == verdict::linearizable);
	CHECK(run({put(0, 100, 7, 1), get_missing(10, 20)}) == verdict::linearizable);  // a read inside the write sees the old state
	CHECK(run({put(0, 100, 7, 1), get(10, 20, 7, 1)}) == verdict::linearizable);  // or the new one
	CHECK(run({put(0, 100, 1, 1), put(0, 100, 2, 2), get(200, 210, 2, 2)}) == verdict::linearizable);
	CHECK(run({put(0, 100, 1, 2), put(0, 100, 2, 1), get(200, 210, 1, 2)}) == verdict::linearizable);
	CHECK(run({put(0, 10, 1, 1), put(10, 20, 2, 2)}) == verdict::linearizable);  // touching intervals are concurrent
	CHECK(run({put(0, 10, 1, 1), put_if(20, 30, 5, 0, outcome::conflict, 1), put_if(40, 50, 6, 1, outcome::ok, 2), get(60, 70, 6, 2)}) == verdict::linearizable);
	CHECK(run({put_unknown(0, 9), get(100, 110, 9, 1), get(120, 130, 9, 1)}) == verdict::linearizable);  // unknown, then seen
	CHECK(run({put_unknown(0, 9), get_missing(100, 110), get_missing(120, 130)}) == verdict::linearizable);  // or never
	CHECK(run({put_unknown(0, 9), get_missing(100, 110), get(120, 130, 9, 1)}) == verdict::linearizable);  // or late
	CHECK(run({put(0, 10, 1, 1, 1), put(0, 10, 1, 1, 2), get(20, 30, 1, 1, 1), get(20, 30, 1, 1, 2)}) == verdict::linearizable);  // keys apart
	// An unknown write carried across several cuts, seen in a later segment.
	CHECK(run({put_unknown(0, 9), get_missing(10, 20), get_missing(30, 40), put(50, 60, 3, 1), get(70, 80, 3, 1), get(90, 100, 9, 2)}) == verdict::linearizable);
	// An unknown conditional write still possible at its expected version, carried across a cut.
	CHECK(run({put(0, 10, 1, 1), put_if(20, 0, 9, 1, outcome::unknown, 0), get(50, 60, 1, 1), get(100, 110, 9, 2)}) == verdict::linearizable);

	// strata's versions (SIM-R3): a re-add starts over at version 1; expected 0 means absent,
	// also after an erase; a conflict reports the current version; an erase of an absent key
	// deletes nothing.
	CHECK(run({put(0, 10, 1, 1), erase(20, 30, outcome::ok, 0), put(40, 50, 2, 1), get(60, 70, 2, 1)}) == verdict::linearizable);
	CHECK(run({put(0, 10, 1, 1), erase(20, 30, outcome::ok, 0), put_if(40, 50, 2, 0, outcome::ok, 1), get(60, 70, 2, 1)}) == verdict::linearizable);
	CHECK(run({put(0, 10, 1, 1), put_if(20, 30, 5, 3, outcome::conflict, 1)}) == verdict::linearizable);
	CHECK(run({put_if(0, 10, 5, 2, outcome::conflict, 0)}) == verdict::linearizable);  // absent: current version 0
	CHECK(run({put(0, 10, 1, 1), erase_if(20, 30, 1, outcome::ok, 0), erase_if(40, 50, 1, outcome::conflict, 0), get_missing(60, 70)}) == verdict::linearizable);
	CHECK(run({erase_if(0, 10, 0, outcome::missing, 0)}) == verdict::linearizable);
	// A pending conditional write whose expected version passed, then recurs after an erase and
	// a re-add: it can still succeed, so it may not be pruned at the cut.
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), put_if(40, 0, 9, 1, outcome::unknown, 0), get(50, 60, 2, 2),
		erase(70, 80, outcome::ok, 0), put(90, 100, 3, 1), get(110, 120, 9, 2)}) == verdict::linearizable);

	// Not linearizable, by hand.
	CHECK(run({put(0, 10, 7, 1), get_missing(20, 30)}) == verdict::violation);  // lost write
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), get(40, 50, 1, 1)}) == verdict::violation);  // stale read
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), get(40, 50, 2, 2), get(60, 70, 1, 1)}) == verdict::violation);  // versions reordered
	CHECK(run({put(0, 10, 1, 2)}) == verdict::violation);  // a version that never existed
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 1, 2), get(40, 50, 1, 1)}) == verdict::violation);  // stale by version alone
	CHECK(run({put_unknown(0, 9), get(100, 110, 9, 1), get_missing(120, 130)}) == verdict::violation);  // unknown write seen, then unseen
	CHECK(run({put(0, 10, 1, 1), put_if(20, 30, 5, 1, outcome::conflict, 1)}) == verdict::violation);  // a conflict at the expected version
	CHECK(run({put_if(0, 10, 5, 3, outcome::ok, 1)}) == verdict::violation);  // a success at the wrong expected version
	CHECK(run({erase(0, 10, outcome::ok, 0)}) == verdict::violation);  // erasing an absent key
	CHECK(run({get(10, 5, 0, 0)}) == verdict::violation);  // returned before invoked
	CHECK(run({put(0, 100, 7, 1), get(10, 20, 0, 0)}) == verdict::violation);  // a value nobody wrote
	CHECK(run({put(0, 10, 1, 1), put_if(20, 30, 5, 3, outcome::conflict, 2)}) == verdict::violation);  // a wrong conflict version
	CHECK(run({put(0, 10, 1, 1), erase(20, 30, outcome::ok, 0), put(40, 50, 2, 2)}) == verdict::violation);  // a re-add continuing the old version
	CHECK(run({put(0, 10, 1, 1), erase(20, 30, outcome::ok, 2)}) == verdict::violation);  // an erase reporting a version
	CHECK(run({put(0, 10, 1, 1), erase(20, 30, outcome::ok, 0), put_if(40, 50, 2, 0, outcome::conflict, 0)}) == verdict::violation);  // expected 0 refused on an absent key
	{
		auto r = check::check({put(0, 10, 1, 1, 1), put(0, 10, 1, 1, 2), get_missing(20, 30, 2)});
		CHECK(r.result == verdict::violation && r.key == 2);
	}

	// Batches (SYS-10): one atomic step over every key they write. A torn batch, visible on one
	// key and not the other, is a violation that a per-key check accepts.
	{
		const auto b1 = in_batch(put(0, 10, 10, 1, 1), 1), b2 = in_batch(put(0, 10, 20, 1, 2), 1);
		CHECK(run({b1, b2, get(20, 30, 10, 1, 1), get_missing(40, 50, 2)}) == verdict::violation);
		CHECK(run({b1, b2, get(20, 30, 10, 1, 1), get(40, 50, 20, 1, 2)}) == verdict::linearizable);
		const auto c1 = in_batch(put(0, 100, 10, 1, 1), 2), c2 = in_batch(put(0, 100, 20, 1, 2), 2);
		CHECK(run({c1, c2, get(10, 20, 10, 1, 1), get_missing(30, 40, 2)}) == verdict::violation);  // torn while in flight
		CHECK(run({c1, c2, get_missing(10, 20, 1), get(30, 40, 20, 1, 2)}) == verdict::linearizable);
		const auto r = check::check({b1, b2, get(20, 30, 10, 1, 1), get(40, 50, 20, 1, 2)});
		CHECK(r.components == 1 && r.keys == 2);
		// Unknown: all of it or none of it.
		auto u1 = in_batch(put_unknown(0, 10, 1), 3), u2 = in_batch(put_unknown(0, 20, 2), 3);
		CHECK(run({u1, u2, get(50, 60, 10, 1, 1), get_missing(70, 80, 2)}) == verdict::violation);
		CHECK(run({u1, u2, get(50, 60, 10, 1, 1), get(70, 80, 20, 1, 2)}) == verdict::linearizable);
		CHECK(run({u1, u2, get_missing(50, 60, 1), get_missing(70, 80, 2)}) == verdict::linearizable);
		// Rejected: the first conflicting write reports the current version, the others abort,
		// and nothing of it is visible.
		const auto ok2 = in_batch(put_if(20, 30, 5, 0, outcome::aborted, 0, 2), 4), no1 = in_batch(put_if(20, 30, 6, 0, outcome::conflict, 1, 1), 4);
		CHECK(run({put(0, 10, 1, 1, 1), ok2, no1, get_missing(40, 50, 2)}) == verdict::linearizable);
		CHECK(run({put(0, 10, 1, 1, 1), ok2, no1, get(40, 50, 5, 1, 2)}) == verdict::violation);
		CHECK(run({put(0, 10, 1, 1, 1), in_batch(put_if(20, 30, 5, 0, outcome::ok, 1, 2), 4), no1}) == verdict::violation);
		const auto first = in_batch(put_if(20, 30, 5, 5, outcome::conflict, 1, 1), 5), second = in_batch(put_if(20, 30, 6, 7, outcome::aborted, 0, 2), 5);
		CHECK(run({put(0, 10, 1, 1, 1), first, second}) == verdict::linearizable);
		const auto first2 = in_batch(put_if(20, 30, 5, 5, outcome::aborted, 0, 1), 5), second2 = in_batch(put_if(20, 30, 6, 7, outcome::conflict, 0, 2), 5);
		CHECK(run({put(0, 10, 1, 1, 1), first2, second2}) == verdict::violation);  // strata reports the first
		// A batch's writes apply in order: a put then an erase of one key leaves it absent.
		CHECK(run({in_batch(put(0, 10, 1, 1), 6), in_batch(erase(0, 10, outcome::ok, 0), 6), get_missing(20, 30)}) == verdict::linearizable);
	}
	// Delete by key range: every present key in [lo, hi), atomically, with its count.
	{
		CHECK(run({put(0, 10, 1, 1, 1), put(0, 10, 2, 1, 2), erase_range(20, 30, 1, 3, outcome::ok, 2), get_missing(40, 50, 1), get_missing(40, 50, 2)}) == verdict::linearizable);
		CHECK(run({put(0, 10, 1, 1, 1), put(0, 10, 2, 1, 2), erase_range(20, 30, 1, 3, outcome::ok, 1)}) == verdict::violation);
		CHECK(run({put(0, 10, 1, 1, 1), put(0, 10, 2, 1, 2), erase_range(20, 30, 1, 3, outcome::ok, 2), get(40, 50, 2, 1, 2)}) == verdict::violation);
		CHECK(run({put(0, 10, 1, 1, 1), put(0, 10, 2, 1, 5), erase_range(20, 30, 1, 3, outcome::ok, 1), get(40, 50, 2, 1, 5)}) == verdict::linearizable);  // 5 is outside
		CHECK(run({put(0, 10, 1, 1, 1), put(15, 35, 2, 1, 2), erase_range(20, 30, 1, 3, outcome::ok, 1), get(40, 50, 2, 1, 2)}) == verdict::linearizable);  // the put after the delete
		CHECK(run({put(0, 10, 1, 1, 1), put(15, 35, 2, 1, 2), erase_range(20, 30, 1, 3, outcome::ok, 2), get(40, 50, 2, 1, 2)}) == verdict::violation);
	}
	// Reads below linearizable, by their necessary conditions: a nearest read sees some state
	// before its response; a session read sees one after its client's last write.
	{
		CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), by(get(40, 50, 1, 1), 9, C::nearest)}) == verdict::linearizable);
		CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), by(get(40, 50, 1, 1), 9)}) == verdict::violation);
		CHECK(run({put(0, 10, 1, 1), put(100, 110, 3, 2), by(get(40, 50, 3, 2), 9, C::nearest)}) == verdict::violation);  // from the future
		CHECK(run({put(0, 10, 1, 1), by(get(40, 50, 9, 1), 9, C::nearest)}) == verdict::violation);  // never written
		CHECK(run({put(0, 10, 1, 1), by(get_missing(40, 50), 9, C::nearest)}) == verdict::linearizable);  // before the put
		CHECK(run({by(put(0, 10, 1, 1), 7), by(put(20, 30, 2, 2), 8), by(get(40, 50, 1, 1), 7, C::session)}) == verdict::linearizable);
		CHECK(run({by(put(0, 10, 1, 1), 7), by(put(20, 30, 2, 2), 7), by(get(40, 50, 1, 1), 7, C::session)}) == verdict::violation);  // its own write unseen
		CHECK(run({by(put(0, 10, 1, 1), 7), by(get_missing(40, 50), 8, C::session)}) == verdict::linearizable);  // no write of its own
		// A write acknowledged before commit (ack local or none) may be lost.
		CHECK(run({by(put(0, 10, 1, 1), 7, C::local), get_missing(20, 30)}) == verdict::linearizable);
		CHECK(run({by(put(0, 10, 1, 1), 7, C::none), get(20, 30, 1, 1)}) == verdict::linearizable);
		CHECK(run({by(put(0, 10, 1, 1), 7, C::all), get_missing(20, 30)}) == verdict::violation);
	}

	// Carried unknown writes (SIM-R13): ten unknown puts of distinct values, a read that sees none
	// of them, then more segments. Recording ends only where the read is linearized keeps one
	// configuration; recording every later node kept 1 + 10 · 2^9 = 5,121, past the bound.
	{
		std::vector<operation> h;
		for (std::uint64_t i = 0; i < 10; ++i) { h.push_back(put_unknown(0, 100 + i)); }
		h.push_back(get_missing(100, 110));
		h.push_back(put(200, 210, 77, 1));
		h.push_back(get(300, 310, 77, 1));
		const auto r = check::check(h);
		CHECK(r.result == verdict::linearizable && r.segments == 3);
		if (r.result != verdict::linearizable) { std::fprintf(stderr, "carried unknowns: %s\n", r.detail.c_str()); }
	}
	// The node budget is per segment and configuration, with a separate total.
	{
		std::vector<operation> h;
		for (std::uint64_t i = 0; i < 200; ++i) { h.push_back(put(i * 100, i * 100 + 10, i + 1, i + 1)); }
		options per_segment; per_segment.max_nodes = 10;
		const auto r = check::check(h, per_segment);
		CHECK(r.result == verdict::linearizable && r.segments == 200 && r.nodes > 10);
		options total; total.max_total_nodes = 50;
		const auto t = check::check(h, total);
		CHECK(t.result == verdict::inconclusive && t.detail == "search budget exhausted for the component");
	}

	// Generated: linearizable by construction, then mutated into violations of each class.
	struct tally { const char* name; bool (*mutate)(std::vector<operation>&); std::uint64_t mutated, rejected; };
	tally classes[] = {{"stale read", stale_read, 0, 0}, {"lost write", lost_write, 0, 0}, {"reordered versions", reordered, 0, 0}, {"unknown seen then unseen", seen_then_unseen, 0, 0}};
	std::uint64_t accepted = 0, unknowns = 0, segments = 0, erases = 0, batched = 0, ranges = 0;
	for (std::uint64_t seed = 1; seed <= 300; ++seed) {
		const auto h = generate(seed, 2 + seed % 5, 1 + seed % 3, 20);
		for (const auto& o : h) {
			unknowns += o.result == outcome::unknown; erases += o.op == kind::erase && o.result == outcome::ok;
			batched += o.batch != 0; ranges += o.op == kind::erase_range;
		}
		const auto r = check::check(h);
		CHECK(r.result == verdict::linearizable);
		if (r.result != verdict::linearizable) { std::fprintf(stderr, "seed %llu key %llu: %s\n", (unsigned long long)seed, (unsigned long long)r.key, r.detail.c_str()); }
		accepted += r.result == verdict::linearizable; segments += r.segments;
		for (auto& c : classes) {
			auto bad = h;
			if (!c.mutate(bad)) { continue; }
			++c.mutated;
			const auto v = run(bad);
			CHECK(v == verdict::violation);
			c.rejected += v == verdict::violation;
		}
	}
	CHECK(accepted == 300 && unknowns > 100 && segments > 300 && erases > 100 && batched > 100 && ranges > 50);
	std::printf("generated: %llu accepted, %llu unknown operations, %llu ok erases, %llu batched writes, %llu range deletes, %llu segments\n",
		(unsigned long long)accepted, (unsigned long long)unknowns, (unsigned long long)erases, (unsigned long long)batched, (unsigned long long)ranges, (unsigned long long)segments);
	for (const auto& c : classes) {
		CHECK(c.mutated >= 20 && c.rejected == c.mutated);
		std::printf("  %s: %llu mutated, %llu rejected\n", c.name, (unsigned long long)c.mutated, (unsigned long long)c.rejected);
	}

	// Brute force: the checker agrees with every order the model admits, on random small
	// histories over two keys with batches, range deletes, unknown outcomes, every read and
	// acknowledgement mode, cuts, erases, and repeated values.
	{
		stream s(20261017);
		std::uint64_t yes = 0, no = 0, mismatches = 0, inconclusive = 0, cut = 0, joined = 0;
		for (int i = 0; i < 50'000; ++i) {
			const auto h = small_history(s);
			const bool expected = brute(h);
			const auto r = check::check(h);
			inconclusive += r.result == verdict::inconclusive;
			cut += r.segments > r.components;
			joined += r.components == 1 && r.keys == 2;
			if ((r.result == verdict::linearizable) != expected || r.result == verdict::inconclusive) {
				if (++mismatches <= 3) { std::fprintf(stderr, "brute force %s, checker %d (%s) on history %d\n", expected ? "linearizable" : "violation", int(r.result), r.detail.c_str(), i); }
			}
			(expected ? yes : no) += 1;
		}
		CHECK(mismatches == 0 && inconclusive == 0 && yes > 5'000 && no > 5'000 && cut > 5'000 && joined > 5'000);
		std::printf("brute force: %llu linearizable, %llu violations, %llu with a cut, %llu over joined keys, %llu mismatches\n",
			(unsigned long long)yes, (unsigned long long)no, (unsigned long long)cut, (unsigned long long)joined, (unsigned long long)mismatches);
	}

	// Bounds: inconclusive, never linearizable.
	{
		std::vector<operation> wide;
		for (std::uint64_t i = 0; i < 12; ++i) { wide.push_back(put(0, 1'000, i, i + 1)); }
		options tight; tight.max_nodes = 5;
		CHECK(run(wide, tight) == verdict::inconclusive);
		CHECK(run(wide) == verdict::linearizable);
		// Twelve unknown puts, then a read of the fourth's value at version 5: any four others
		// came first, so C(11, 4) = 330 configurations of equal size survive the cut.
		std::vector<operation> many;
		for (std::uint64_t i = 0; i < 12; ++i) { many.push_back(put_unknown(0, i)); }
		many.push_back(get(100, 110, 3, 5));
		options few; few.max_configurations = 4;
		CHECK(run(many, few) == verdict::inconclusive);
		CHECK(run(many) == verdict::linearizable);
		// A range delete joins every key it covers; past the component bound, inconclusive.
		std::vector<operation> joined;
		for (std::uint64_t k = 1; k <= 10; ++k) { joined.push_back(put(0, 10, k, 1, k)); }
		joined.push_back(erase_range(20, 30, 1, 11, outcome::ok, 10));
		options narrow; narrow.max_component_keys = 4;
		CHECK(run(joined, narrow) == verdict::inconclusive);
		CHECK(run(joined) == verdict::linearizable);
	}

	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
