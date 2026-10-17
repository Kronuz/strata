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

// Deterministic randomness (simulator design D2): Lemire's bounded draws are exactly
// uniform (checked over every 16-bit input), splitmix64 matches its reference output,
// child streams do not depend on their parent's draws, quantile tables interpolate, and
// node clocks are monotone with a minimal inverse.
#include "simulation/clock.hh"
#include "simulation/random.hh"

#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;

int main() {
	// splitmix64's reference: the first output from state 0.
	std::uint64_t state = 0;
	CHECK(splitmix64(state) == 0xe220a8397b1dcdafULL);

	// Every accepted 16-bit draw: each value in [0, n) appears exactly floor(2^16 / n) times.
	for (std::uint32_t n = 1; n <= 257; ++n) {
		std::vector<std::uint32_t> counts(n);
		for (std::uint32_t x = 0; x < 65536; ++x) {
			std::uint16_t out;
			if (bounded_draw<std::uint16_t, std::uint32_t>(std::uint16_t(x), std::uint16_t(n), out)) { CHECK(out < n); ++counts[out]; }
		}
		for (auto c : counts) { CHECK(c == 65536 / n); }
	}

	// Children: keyed by label, independent of the parent's position and of siblings.
	stream root(42);
	auto a = root.child("node/1/disk"), b = root.child("node/2/disk");
	const auto a0 = a.next(), b0 = b.next();
	for (int i = 0; i < 1000; ++i) { root.next(); }
	CHECK(root.child("node/1/disk").next() == a0 && root.child("node/2/disk").next() == b0 && a0 != b0);
	CHECK(root.child(1).next() != root.child(2).next() && root.child(1).next() == stream(42).child(1).next());
	CHECK(stream(1).next() != stream(2).next());

	// below(10) over 10^6 draws: chi-square under 27.88 (df 9, p = 0.001).
	{
		stream s(7); std::vector<double> counts(10);
		for (int i = 0; i < 1'000'000; ++i) { auto v = s.below(10); CHECK(v < 10); ++counts[v]; }
		double chi = 0; for (auto c : counts) { chi += (c - 100'000) * (c - 100'000) / 100'000.0; }
		CHECK(chi < 27.88);
		bool threw = false; try { s.below(0); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
		CHECK(s.between(5, 5) == 5);
		for (int i = 0; i < 1000; ++i) { auto v = s.between(3, 9); CHECK(v >= 3 && v <= 9); }
		bool never_true = true, always_true = true;
		for (int i = 0; i < 1000; ++i) { never_true &= !s.chance_ppm(0); always_true &= s.chance_ppm(1'000'000); }
		CHECK(never_true && always_true);
	}

	// Quantile tables: ends, interpolation, steps, validation.
	{
		quantile_table t({{0, 100}, {500'000, 200}, {990'000, 1'000}, {990'000, 5'000}, {1'000'000, 10'000}});
		CHECK(t.at(0) == 100 && t.at(250'000) == 150 && t.at(500'000) == 200 && t.at(1'000'000) == 10'000);
		CHECK(t.at(990'000) == 1'000 && t.at(995'000) == 7'500);
		stream s(9); std::uint64_t last = 0;
		for (std::uint32_t q = 0; q <= 1'000'000; q += 997) { auto v = t.at(q); CHECK(v >= last); last = v; }
		for (int i = 0; i < 1000; ++i) { auto v = t.draw(s); CHECK(v >= 100 && v <= 10'000); }
		int threw = 0;
		try { quantile_table({{0, 1}}); } catch (const std::invalid_argument&) { ++threw; }
		try { quantile_table({{0, 5}, {1'000'000, 4}}); } catch (const std::invalid_argument&) { ++threw; }
		try { quantile_table({{1, 5}, {1'000'000, 6}}); } catch (const std::invalid_argument&) { ++threw; }
		CHECK(threw == 3);
	}

	// Clocks: monotone at T2's extreme rates, and global_at is the earliest time reaching a target.
	for (std::uint64_t rate : {500'000ULL, 999'800ULL, 1'000'000ULL, 1'000'200ULL, 2'000'000ULL}) {
		node_clock c(12'345, rate); stream s(rate);
		time_ns last = 0;
		for (time_ns t = 0; t < 10'000'000; t += 1 + s.below(1'000)) { auto l = c.local(t); CHECK(l >= last); last = l; }
		for (int i = 0; i < 10'000; ++i) {
			const time_ns target = 12'345 + s.below(1ULL << 40), g = c.global_at(target);
			CHECK(c.local(g) >= target && (g == 0 || c.local(g - 1) < target));
		}
		CHECK(c.global_at(0) == 0 && (rate < 1'000'000 || c.local(never) == never));
	}

	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
