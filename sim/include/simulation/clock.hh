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

// A node's clock (simulator design D3): local(t) = offset + t × rate_ppm / 10^6 over global
// simulated nanoseconds, in 128-bit integer math, so it is monotone at every rate.

#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace kronuz::simulation {

using time_ns = std::uint64_t;
inline constexpr time_ns never = std::numeric_limits<time_ns>::max();

class node_clock {
public:
	// rate_ppm: 10^6 runs at the global rate; T2's range 0.5x to 2x is 500,000 to 2,000,000.
	explicit node_clock(std::uint64_t offset = 0, std::uint64_t rate_ppm = 1'000'000) : offset_(offset), rate_(rate_ppm) {
		if (rate_ == 0) { throw std::invalid_argument("clock rate"); }
	}
	time_ns local(time_ns global) const noexcept {
		const auto v = (unsigned __int128)global * rate_ / 1'000'000 + offset_;
		return v > never ? never : time_ns(v);
	}
	// The earliest global time at which local() reaches `target`; never past the range.
	time_ns global_at(time_ns target) const noexcept {
		if (target <= offset_) { return 0; }
		const auto v = ((unsigned __int128)(target - offset_) * 1'000'000 + rate_ - 1) / rate_;
		return v >= never ? never : time_ns(v);
	}
	std::uint64_t rate_ppm() const noexcept { return rate_; }
	std::uint64_t offset() const noexcept { return offset_; }

private:
	std::uint64_t offset_, rate_;
};

}  // namespace kronuz::simulation
