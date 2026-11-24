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

// SIM-R17: a canceled event's heap entry outlives its slot's reuse. After 2^32 reuses of one
// slot a 32-bit generation would wrap and the stale entry would fire the slot's new event at
// the stale time; the 64-bit sequence never repeats. Slow (2^32 events), so it builds only
// with SIMULATION_SLOW_TESTS and runs in Release.
#include "simulation/engine.hh"

#include <cstdio>

using namespace kronuz::simulation;

int main() {
	engine e(1);
	const auto stale = e.at(1'000, [] {}, 1, 0);
	e.cancel(stale);  // its entry stays in the heap at 1,000; its slot is free
	for (std::uint64_t i = 0; i < (std::uint64_t(1) << 32) - 1; ++i) {
		e.at(0, [] {}, 2, 0);  // the only free slot: the canceled event's
		e.step();
	}
	time_ns ran_at = never;
	const auto late = e.at(5'000, [&] { ran_at = e.now(); }, 3, 0);
	const bool reused = late.slot == stale.slot;
	e.drain();
	const bool ok = reused && ran_at == 5'000;
	std::printf("slot reused: %s; the event at 5,000 ran at %llu: %s\n", reused ? "yes" : "no", (unsigned long long)ran_at, ok ? "OK" : "FAIL");
	return ok ? 0 : 1;
}
