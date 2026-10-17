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
// all rejected, including re-adds at version 1, expected 0 after an erase, and conflict
// versions (SIM-R3); generated concurrent histories, linearizable by construction, accepted,
// and mutated into violations of four classes (stale read, lost write, reordered versions, an
// unknown write seen then unseen), each a violation by construction, all rejected; agreement
// with an independent brute force on random small histories; carried unknown writes that stay
// conclusive and a node budget per segment (SIM-R13); and inconclusive, never pass, past the
// bounds.
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

operation put(time_ns a, time_ns b, std::uint64_t value, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::put, a, b, value, {}, outcome::ok, version, 0}; }
operation put_unknown(time_ns a, std::uint64_t value, std::uint64_t key = 1) { return {key, kind::put, a, 0, value, {}, outcome::unknown, 0, 0}; }
operation put_if(time_ns a, time_ns b, std::uint64_t value, std::uint64_t expected, outcome r, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::put, a, b, value, expected, r, version, 0}; }
operation erase(time_ns a, time_ns b, outcome r, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::erase, a, b, 0, {}, r, version, 0}; }
operation erase_if(time_ns a, time_ns b, std::uint64_t expected, outcome r, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::erase, a, b, 0, expected, r, version, 0}; }
operation get(time_ns a, time_ns b, std::uint64_t value, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::get, a, b, value, {}, outcome::ok, version, 0}; }
operation get_missing(time_ns a, time_ns b, std::uint64_t key = 1) { return {key, kind::get, a, b, 0, {}, outcome::missing, 0, 0}; }

verdict run(const std::vector<operation>& h, const options& o = {}) { return check::check(h, o).result; }

// strata's register, written from phase-3.md's rules apart from checker.hh: the generators
// and the brute force share it.
struct model { bool present = false; std::uint64_t version = 0, value = 0; };
// Sets o's outcome as strata answers it at m, and applies it.
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
// An outcome hidden from the client: the fields D9's format keeps for an unknown.
void hide(operation& o) { o.result = outcome::unknown; o.returned = 0; o.version = 0; if (o.op == kind::get) { o.value = 0; } }

// Concurrent sequential clients over a few keys: each operation takes effect at a point in
// its interval, in that order; some responses are lost (unknown), half of those never took
// effect, and the client continues under a new id. Values are unique. Linearizable by
// construction.
std::vector<operation> generate(std::uint64_t seed, std::size_t clients, std::size_t keys, std::size_t rounds) {
	stream s(seed);
	struct pending { operation o; time_ns point; bool applies, unknown; };
	std::vector<pending> all;
	std::uint64_t next_value = 1, next_client = 1;
	std::vector<std::uint64_t> client(clients);
	for (auto& c : client) { c = next_client++; }
	for (std::size_t r = 0; r < rounds; ++r) {
		const time_ns base = r * 1'000'000;  // rounds leave quiet gaps, so histories have cuts
		for (std::size_t c = 0; c < clients; ++c) {
			time_ns t = base + s.below(1'000);
			for (int k = 0, n = int(1 + s.below(3)); k < n; ++k) {
				operation o; o.key = 1 + s.below(keys); o.client = client[c];
				const auto pick = s.below(10);
				o.op = pick < 4 ? kind::put : pick < 5 ? kind::erase : kind::get;
				if (o.op == kind::put) { o.value = next_value++; }
				if (o.op != kind::get && s.chance_ppm(250'000)) { o.expected = s.below(4); }
				o.invoked = t;
				const time_ns point = t + s.below(50'000);
				o.returned = point + s.below(50'000);
				pending p{o, point, true, s.chance_ppm(50'000)};
				if (p.unknown) { p.applies = s.chance_ppm(500'000); client[c] = next_client++; }
				t = o.returned + 1 + s.below(10'000);
				all.push_back(p);
			}
		}
	}
	std::stable_sort(all.begin(), all.end(), [](const pending& a, const pending& b) { return a.point < b.point; });
	std::map<std::uint64_t, model> models;
	std::vector<operation> history;
	for (auto& p : all) {
		if (p.applies) { execute(p.o, models[p.o.key]); }
		if (p.unknown) { hide(p.o); }
		history.push_back(p.o);
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
				return e.op == kind::erase && e.key == g.key && e.invoked <= g.returned && !(answered(e) && e.returned < p.invoked);
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

// The brute force: every order of the operations that respects real time, each unknown
// applied at some point or never, run on the model.
bool matches(const operation& o, model& m) {
	operation e = o;
	execute(e, m);
	if (o.result == outcome::unknown) { return true; }
	return e.result == o.result && e.version == o.version && (o.op != kind::get || o.result != outcome::ok || e.value == o.value);
}
bool brute(const std::vector<operation>& ops, std::vector<char>& used, const model& m, std::size_t left) {
	if (left == 0) { return true; }
	for (std::size_t i = 0; i < ops.size(); ++i) {
		if (used[i]) { continue; }
		bool ready = true;
		for (std::size_t j = 0; j < ops.size() && ready; ++j) { ready = used[j] || !answered(ops[j]) || !(ops[j].returned < ops[i].invoked); }
		if (!ready) { continue; }
		model next = m;
		if (!matches(ops[i], next)) { continue; }
		used[i] = 1;
		if (brute(ops, used, next, left - answered(ops[i]))) { return true; }
		used[i] = 0;
	}
	return false;
}
bool brute(const std::vector<operation>& ops) {
	std::vector<char> used(ops.size());
	return brute(ops, used, {}, std::size_t(std::count_if(ops.begin(), ops.end(), answered)));
}
// Keeps a record within D9's format: a version only where an outcome reports one, a value
// only for puts and ok gets.
void conform(operation& o) {
	if (!(o.result == outcome::conflict || (o.result == outcome::ok && o.op != kind::erase))) { o.version = 0; }
	if (!(o.op == kind::put || (o.op == kind::get && o.result == outcome::ok))) { o.value = 0; }
	if (o.op == kind::get) { o.expected.reset(); if (o.result == outcome::conflict) { o.result = outcome::missing; } }
	if (o.op == kind::put && o.result == outcome::missing) { o.result = outcome::conflict; }
	if (o.result == outcome::unknown) { o.returned = 0; }
}
// Up to 7 operations on one key over a short span, so they overlap and cut; values repeat.
// Half are linearizable by construction, half have arbitrary outcomes, and half of each get
// one field changed.
std::vector<operation> small_history(stream& s) {
	std::vector<operation> ops(1 + s.below(7));
	for (auto& o : ops) {
		o.key = 1; o.invoked = s.below(60); o.returned = o.invoked + s.below(30);
		const auto pick = s.below(10);
		o.op = pick < 4 ? kind::put : pick < 6 ? kind::erase : kind::get;
		if (o.op == kind::put) { o.value = 1 + s.below(3); }
		if (o.op != kind::get && s.below(3) == 0) { o.expected = s.below(3); }
	}
	if (s.below(2) == 0) {
		struct at { std::size_t i; time_ns point; };
		std::vector<at> order;
		for (std::size_t i = 0; i < ops.size(); ++i) { order.push_back({i, ops[i].invoked + s.below(ops[i].returned - ops[i].invoked + 1)}); }
		std::stable_sort(order.begin(), order.end(), [](const at& a, const at& b) { return a.point < b.point; });
		model m;
		for (const auto& a : order) {
			const auto i = a.i;
			const bool unknown = s.below(6) == 0, applies = !unknown || s.below(2) == 0;
			if (applies) { execute(ops[i], m); }
			if (unknown) { hide(ops[i]); }
		}
	} else {
		for (auto& o : ops) {
			o.result = outcome(s.below(4));
			o.version = s.below(4);
			if (o.op == kind::get) { o.value = 1 + s.below(3); }
		}
	}
	if (s.below(2) == 0) {
		auto& o = ops[s.below(ops.size())];
		switch (s.below(4)) {
		case 0: o.result = outcome(s.below(4)); break;
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
	// The node budget is per segment and configuration, with a separate total for the key.
	{
		std::vector<operation> h;
		for (std::uint64_t i = 0; i < 200; ++i) { h.push_back(put(i * 100, i * 100 + 10, i + 1, i + 1)); }
		options per_segment; per_segment.max_nodes = 10;
		const auto r = check::check(h, per_segment);
		CHECK(r.result == verdict::linearizable && r.segments == 200 && r.nodes > 10);
		options total; total.max_total_nodes = 50;
		const auto t = check::check(h, total);
		CHECK(t.result == verdict::inconclusive && t.detail == "search budget exhausted for the key");
	}

	// Generated: linearizable by construction, then mutated into violations of each class.
	struct tally { const char* name; bool (*mutate)(std::vector<operation>&); std::uint64_t mutated = 0, rejected = 0; };
	tally classes[] = {{"stale read", stale_read, 0, 0}, {"lost write", lost_write, 0, 0}, {"reordered versions", reordered, 0, 0}, {"unknown seen then unseen", seen_then_unseen, 0, 0}};
	std::uint64_t accepted = 0, unknowns = 0, segments = 0, erases = 0;
	for (std::uint64_t seed = 1; seed <= 300; ++seed) {
		const auto h = generate(seed, 2 + seed % 5, 1 + seed % 3, 20);
		for (const auto& o : h) { unknowns += o.result == outcome::unknown; erases += o.op == kind::erase && o.result == outcome::ok; }
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
	CHECK(accepted == 300 && unknowns > 100 && segments > 300 && erases > 100);
	std::printf("generated: %llu accepted, %llu unknown operations, %llu ok erases, %llu segments\n",
		(unsigned long long)accepted, (unsigned long long)unknowns, (unsigned long long)erases, (unsigned long long)segments);
	for (const auto& c : classes) {
		CHECK(c.mutated >= 20 && c.rejected == c.mutated);
		std::printf("  %s: %llu mutated, %llu rejected\n", c.name, (unsigned long long)c.mutated, (unsigned long long)c.rejected);
	}

	// Brute force: the checker agrees with every order the model admits, on random small
	// histories with unknown outcomes, cuts, erases, and repeated values.
	{
		stream s(20261017);
		std::uint64_t yes = 0, no = 0, mismatches = 0, inconclusive = 0, cut = 0;
		for (int i = 0; i < 50'000; ++i) {
			const auto h = small_history(s);
			const bool expected = brute(h);
			const auto r = check::check(h);
			inconclusive += r.result == verdict::inconclusive;
			cut += r.segments > 1;
			if ((r.result == verdict::linearizable) != expected || r.result == verdict::inconclusive) {
				if (++mismatches <= 3) { std::fprintf(stderr, "brute force %s, checker %d on history %d\n", expected ? "linearizable" : "violation", int(r.result), i); }
			}
			(expected ? yes : no) += 1;
		}
		CHECK(mismatches == 0 && inconclusive == 0 && yes > 5'000 && no > 5'000 && cut > 5'000);
		std::printf("brute force: %llu linearizable, %llu violations, %llu with a cut, %llu mismatches\n",
			(unsigned long long)yes, (unsigned long long)no, (unsigned long long)cut, (unsigned long long)mismatches);
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
	}

	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
