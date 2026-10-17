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

// The engine (simulator design D2): events run in (time, scheduling order); cancellation
// survives slot reuse; run_until advances time; and a randomized workload of actors that
// reschedule and cancel each other gives the same trace hash and checkpoints on every run of
// a seed. The pinned hash is checked on every CI platform (gate row T1).
#include "simulation/engine.hh"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;

namespace {

struct result { std::uint64_t hash, executed; time_ns end; std::vector<std::uint64_t> checkpoints; };

// 100 actors; each wakes after a delay from its own stream, folds its state, sometimes arms a
// timeout on another actor and sometimes cancels one it armed.
result workload(std::uint64_t seed, std::uint64_t steps) {
	engine e(seed);
	const quantile_table delay({{0, 1'000}, {500'000, 100'000}, {990'000, 2'000'000}, {1'000'000, 50'000'000}});
	struct actor { stream random; std::uint64_t state = 0; std::vector<engine::id> armed; };
	std::vector<actor> actors;
	for (std::uint64_t i = 0; i < 100; ++i) { actors.push_back({e.random().child("actor").child(i), 0, {}}); }
	std::function<void(std::size_t)> wake = [&](std::size_t i) {
		auto& a = actors[i];
		a.state = mix(a.state ^ e.now());
		e.trace(a.state);
		if (a.random.chance_ppm(200'000)) {
			const auto target = a.random.below(actors.size());
			a.armed.push_back(e.after(delay.draw(a.random), [&, target] { actors[target].state ^= 1; e.trace(target); }, 2, target));
		}
		if (!a.armed.empty() && a.random.chance_ppm(100'000)) {
			const auto k = a.random.below(a.armed.size());
			e.cancel(a.armed[k]); a.armed.erase(a.armed.begin() + std::ptrdiff_t(k));
		}
		e.after(delay.draw(a.random), [&, i] { wake(i); }, 1, i);
	};
	for (std::size_t i = 0; i < actors.size(); ++i) { e.at(0, [&, i] { wake(i); }, 1, i); }
	e.run_steps(steps);
	return {e.trace_hash(), e.executed(), e.now(), e.checkpoints()};
}

}  // namespace

int main() {
	// Order: by time, then by scheduling order; events scheduled while running take their turn.
	{
		engine e(1); std::string order;
		e.at(20, [&] { order += 'c'; });
		e.at(10, [&] { order += 'a'; e.at(10, [&] { order += 'b'; }); e.after(5, [&] { order += 'x'; }); });
		e.at(10, [&] { order += 'B'; });
		e.at(20, [&] { order += 'd'; });
		CHECK(e.pending() == 4 && e.next_time() == 10);
		e.run_until(1'000);
		CHECK(order == "aBbxcd" && e.now() == 1'000 && e.pending() == 0 && e.executed() == 6);
		bool threw = false; try { e.at(999, [] {}); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
		CHECK(!e.step() && e.next_time() == never);
	}
	// Cancellation: only live events, by generation, also after the slot is reused.
	{
		engine e(1); int ran = 0;
		auto a = e.at(5, [&] { ran += 1; });
		auto b = e.at(6, [&] { ran += 10; });
		CHECK(e.cancel(a) && !e.cancel(a) && e.pending() == 1);
		auto c = e.at(7, [&] { ran += 100; });  // reuses a's slot
		CHECK(c.slot == a.slot && !(c == a) && !e.cancel(a));
		e.run_until(6);
		CHECK(ran == 10 && !e.cancel(b) && e.next_time() == 7);
		e.run_until(100);
		CHECK(ran == 110 && !e.cancel(c) && !e.cancel({999, 0}));
	}
	// run_until stops at its bound; run_steps counts.
	{
		engine e(1);
		for (time_ns t = 1; t <= 10; ++t) { e.at(t * 100, [] {}); }
		CHECK(e.run_until(450) == 4 && e.now() == 450 && e.run_steps(3) == 3 && e.now() == 700 && e.run_steps(10) == 3);
		e.at(5'000, [] {}); e.at(9'000, [] {});
		CHECK(e.drain() == 2 && e.now() == 9'000 && e.pending() == 0);
	}
	// Determinism: two runs of a seed agree in hash, count, end time, and every checkpoint.
	{
		const auto x = workload(1, 300'000), y = workload(1, 300'000), z = workload(2, 300'000);
		CHECK(x.hash == y.hash && x.executed == y.executed && x.end == y.end && x.checkpoints == y.checkpoints);
		CHECK(x.executed == 300'000 && x.checkpoints.size() == 300'000 / engine::checkpoint_interval);
		CHECK(x.hash != z.hash);
		// Pinned for seed 1: a change here means the engine, the streams, or the workload drew differently.
		constexpr std::uint64_t pinned = 0xa17f79e528ea2e4eULL;
		if (x.hash != pinned) { std::fprintf(stderr, "seed 1 trace hash 0x%016llx, end %llu\n", (unsigned long long)x.hash, (unsigned long long)x.end); }
		CHECK(x.hash == pinned);
	}
	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
