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

// The linearizability checker (simulator design D9, gate row T3): a hand-built corpus of
// linearizable histories, all accepted, and of non-linearizable ones (stale reads, lost
// writes, reordered versions, an unknown write seen then unseen, wrong conflicts), all
// rejected; generated concurrent histories, linearizable by construction, accepted, and each
// mutated into a guaranteed violation (a read of a version older than one a preceding write
// produced), rejected; inconclusive, never pass, past the search and configuration bounds.
#include "simulation/checker.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;
using namespace kronuz::simulation::check;

namespace {

operation put(time_ns a, time_ns b, std::uint64_t value, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::put, a, b, value, {}, outcome::ok, version}; }
operation put_unknown(time_ns a, std::uint64_t value, std::uint64_t key = 1) { return {key, kind::put, a, 0, value, {}, outcome::unknown, 0}; }
operation put_if(time_ns a, time_ns b, std::uint64_t value, std::uint64_t expected, outcome r, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::put, a, b, value, expected, r, version}; }
operation erase(time_ns a, time_ns b, outcome r, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::erase, a, b, 0, {}, r, version}; }
operation get(time_ns a, time_ns b, std::uint64_t value, std::uint64_t version, std::uint64_t key = 1) { return {key, kind::get, a, b, value, {}, outcome::ok, version}; }
operation get_missing(time_ns a, time_ns b, std::uint64_t key = 1) { return {key, kind::get, a, b, 0, {}, outcome::missing, 0}; }

verdict run(const std::vector<operation>& h, const options& o = {}) { return check::check(h, o).result; }

// Concurrent clients over a few keys: each operation takes effect at a point inside its
// interval, in that order, on a model register; some responses are dropped (unknown), and
// half of those never took effect. Linearizable by construction.
struct generated { std::vector<operation> history; std::map<std::pair<std::uint64_t, std::uint64_t>, state> versions; };
generated generate(std::uint64_t seed, std::size_t clients, std::size_t keys, std::size_t rounds) {
	stream s(seed);
	struct pending { operation o; time_ns point; bool applies; };
	std::vector<pending> all;
	for (std::size_t r = 0; r < rounds; ++r) {
		const time_ns base = r * 1'000'000;  // rounds leave quiet gaps, so histories have cuts
		for (std::size_t c = 0; c < clients; ++c) {
			time_ns t = base + s.below(1'000);
			for (int k = 0, n = int(1 + s.below(3)); k < n; ++k) {
				operation o; o.key = 1 + s.below(keys);
				const auto pick = s.below(10);
				o.op = pick < 4 ? kind::put : pick < 5 ? kind::erase : kind::get;
				o.value = s.next() % 1000;
				if (o.op != kind::get && s.chance_ppm(250'000)) { o.expected = s.below(4); }
				o.invoked = t;
				const time_ns point = t + s.below(50'000);
				o.returned = point + s.below(50'000);
				bool applies = true;
				if (s.chance_ppm(50'000)) { o.result = outcome::unknown; applies = s.chance_ppm(500'000); }
				all.push_back({o, point, applies});
				t = o.returned + 1 + s.below(10'000);
			}
		}
	}
	std::stable_sort(all.begin(), all.end(), [](const pending& a, const pending& b) { return a.point < b.point; });
	generated g;
	std::map<std::uint64_t, state> model;
	for (auto& p : all) {
		auto& st = model[p.o.key];
		const bool unknown = p.o.result == outcome::unknown;
		if (p.applies) {
			auto& o = p.o;
			if (o.op == kind::get) { if (st.present) { o.result = outcome::ok; o.value = st.value; o.version = st.version; } else { o.result = outcome::missing; } }
			else if (o.expected && *o.expected != st.version) { o.result = outcome::conflict; }
			else if (o.op == kind::erase && !st.present) { o.result = outcome::missing; }
			else { st = o.op == kind::put ? state{st.version + 1, o.value, true} : state{st.version + 1, 0, false}; o.result = outcome::ok; o.version = st.version; g.versions[{o.key, st.version}] = st; }
			if (unknown) { o.result = outcome::unknown; }
		}
		g.history.push_back(p.o);
	}
	return g;
}

}  // namespace

int main() {
	// Linearizable, by hand.
	CHECK(run({}) == verdict::linearizable);
	CHECK(run({put(0, 10, 7, 1), get(20, 30, 7, 1), erase(40, 50, outcome::ok, 2), get_missing(60, 70), erase(80, 90, outcome::missing, 0)}) == verdict::linearizable);
	CHECK(run({put(0, 100, 7, 1), get_missing(10, 20)}) == verdict::linearizable);  // a read inside the write sees the old state
	CHECK(run({put(0, 100, 7, 1), get(10, 20, 7, 1)}) == verdict::linearizable);  // or the new one
	CHECK(run({put(0, 100, 1, 1), put(0, 100, 2, 2), get(200, 210, 2, 2)}) == verdict::linearizable);
	CHECK(run({put(0, 100, 1, 2), put(0, 100, 2, 1), get(200, 210, 1, 2)}) == verdict::linearizable);
	CHECK(run({put(0, 10, 1, 1), put(10, 20, 2, 2)}) == verdict::linearizable);  // touching intervals are concurrent
	CHECK(run({put(0, 10, 1, 1), put_if(20, 30, 5, 0, outcome::conflict, 0), put_if(40, 50, 6, 1, outcome::ok, 2), get(60, 70, 6, 2)}) == verdict::linearizable);
	CHECK(run({put_unknown(0, 9), get(100, 110, 9, 1), get(120, 130, 9, 1)}) == verdict::linearizable);  // unknown, then seen
	CHECK(run({put_unknown(0, 9), get_missing(100, 110), get_missing(120, 130)}) == verdict::linearizable);  // or never
	CHECK(run({put_unknown(0, 9), get_missing(100, 110), get(120, 130, 9, 1)}) == verdict::linearizable);  // or late
	CHECK(run({put(0, 10, 1, 1, 1), put(0, 10, 1, 1, 2), get(20, 30, 1, 1, 1), get(20, 30, 1, 1, 2)}) == verdict::linearizable);  // keys apart
	// An unknown write carried across several cuts, seen in a later segment.
	CHECK(run({put_unknown(0, 9), get_missing(10, 20), get_missing(30, 40), put(50, 60, 3, 1), get(70, 80, 3, 1), get(90, 100, 9, 2)}) == verdict::linearizable);

	// An unknown conditional write still possible at its expected version, carried across a cut.
	CHECK(run({put(0, 10, 1, 1), put_if(20, 0, 9, 1, outcome::unknown, 0), get(50, 60, 1, 1), get(100, 110, 9, 2)}) == verdict::linearizable);

	// Not linearizable, by hand.
	CHECK(run({put(0, 10, 7, 1), get_missing(20, 30)}) == verdict::violation);  // lost write
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), get(40, 50, 1, 1)}) == verdict::violation);  // stale read
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 2, 2), get(40, 50, 2, 2), get(60, 70, 1, 1)}) == verdict::violation);  // versions reordered
	CHECK(run({put(0, 10, 1, 2)}) == verdict::violation);  // a version that never existed
	CHECK(run({put(0, 10, 1, 1), put(20, 30, 1, 2), get(40, 50, 1, 1)}) == verdict::violation);  // stale by version alone
	CHECK(run({put_unknown(0, 9), get(100, 110, 9, 1), get_missing(120, 130)}) == verdict::violation);  // unknown write seen, then unseen
	CHECK(run({put(0, 10, 1, 1), put_if(20, 30, 5, 1, outcome::conflict, 0)}) == verdict::violation);  // a conflict at the expected version
	CHECK(run({put_if(0, 10, 5, 3, outcome::ok, 1)}) == verdict::violation);  // a success at the wrong expected version
	CHECK(run({erase(0, 10, outcome::ok, 1)}) == verdict::violation);  // erasing an absent key
	CHECK(run({get(10, 5, 0, 0)}) == verdict::violation);  // returned before invoked
	CHECK(run({put(0, 100, 7, 1), get(10, 20, 0, 0)}) == verdict::violation);  // a value nobody wrote
	{
		auto r = check::check({put(0, 10, 1, 1, 1), put(0, 10, 1, 1, 2), get_missing(20, 30, 2)});
		CHECK(r.result == verdict::violation && r.key == 2);
	}

	// Generated: linearizable by construction, then each mutated into a violation.
	std::uint64_t accepted = 0, mutated = 0, rejected = 0, unknowns = 0, segments = 0;
	for (std::uint64_t seed = 1; seed <= 300; ++seed) {
		auto g = generate(seed, 2 + seed % 5, 1 + seed % 3, 20);
		for (const auto& o : g.history) { unknowns += o.result == outcome::unknown; }
		const auto r = check::check(g.history);
		CHECK(r.result == verdict::linearizable);
		if (r.result != verdict::linearizable) { std::fprintf(stderr, "seed %llu key %llu: %s\n", (unsigned long long)seed, (unsigned long long)r.key, r.detail.c_str()); }
		accepted += r.result == verdict::linearizable; segments += r.segments;
		// A get that read version v after a write that produced w > v + 1 finished: make it
		// read version w − 1 instead, which some write produced and a present state had.
		for (auto& gt : g.history) {
			if (gt.op != kind::get || gt.result != outcome::ok) { continue; }
			const operation* before = nullptr;
			for (const auto& p : g.history) {
				if (p.key == gt.key && p.op != kind::get && p.result == outcome::ok && p.returned < gt.invoked && p.version >= 2 && (!before || p.version > before->version)) { before = &p; }
			}
			if (!before) { continue; }
			auto found = g.versions.find({gt.key, before->version - 1});
			if (found == g.versions.end() || !found->second.present) { continue; }
			auto bad = g.history; auto& b = bad[std::size_t(&gt - g.history.data())];
			b.version = found->second.version; b.value = found->second.value;
			++mutated;
			const auto v = run(bad);
			CHECK(v == verdict::violation);
			rejected += v == verdict::violation;
			break;
		}
	}
	CHECK(accepted == 300 && mutated >= 200 && rejected == mutated && unknowns > 100 && segments > 300);
	std::printf("generated: %llu accepted, %llu mutated, %llu rejected, %llu unknown operations, %llu segments\n",
		(unsigned long long)accepted, (unsigned long long)mutated, (unsigned long long)rejected, (unsigned long long)unknowns, (unsigned long long)segments);

	// Bounds: inconclusive, never linearizable.
	{
		std::vector<operation> wide;
		for (std::uint64_t i = 0; i < 12; ++i) { wide.push_back(put(0, 1'000, i, i + 1)); }
		options tight; tight.max_nodes = 5;
		CHECK(run(wide, tight) == verdict::inconclusive);
		CHECK(run(wide) == verdict::linearizable);
		std::vector<operation> many;
		for (std::uint64_t i = 0; i < 12; ++i) { many.push_back(put_unknown(0, i)); }
		many.push_back(get_missing(100, 110));
		options few; few.max_configurations = 4;
		CHECK(run(many, few) == verdict::inconclusive);
	}

	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
