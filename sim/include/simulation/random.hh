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

// Deterministic randomness (simulator design D2): splittable streams keyed by labels, each a
// xoshiro256** generator seeded through splitmix64; bounded integers by Lemire's method;
// probabilities in parts per million; latencies from integer quantile tables. Only fully
// specified integer arithmetic, so every platform draws the same values.

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace kronuz::simulation {

// splitmix64's finalizer: a bijection that spreads every input bit over the output.
constexpr std::uint64_t mix(std::uint64_t z) noexcept {
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}
constexpr std::uint64_t splitmix64(std::uint64_t& state) noexcept { return mix(state += 0x9e3779b97f4a7c15ULL); }

// FNV-1a over a label's bytes, mixed: the key step of a child stream.
constexpr std::uint64_t label_hash(std::string_view label) noexcept {
	std::uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned char c : label) { h = (h ^ c) * 0x100000001b3ULL; }
	return mix(h);
}

// One draw of Lemire's method in width U with product width W: out is floor(x·n / 2^bits),
// accepted unless x falls in the rejected sliver, so accepted draws are exactly uniform. The
// threshold is below n, so its modulo runs only when the low word is.
template <class U, class W> constexpr bool bounded_draw(U x, U n, U& out) noexcept {
	constexpr unsigned bits = std::numeric_limits<U>::digits;
	const W m = W(x) * W(n);
	const U low = U(m);
	out = U(m >> bits);
	return low >= n || low >= U(U(U(0) - n) % n);
}

class stream {
public:
	explicit stream(std::uint64_t key) noexcept : key_(key) {
		std::uint64_t s = key;
		for (auto& word : state_) { word = splitmix64(s); }
	}
	std::uint64_t key() const noexcept { return key_; }
	// A child's draws do not depend on how many values the parent or a sibling drew.
	stream child(std::string_view label) const noexcept { return stream(mix(key_ ^ label_hash(label))); }
	stream child(std::uint64_t index) const noexcept { return stream(mix(key_ ^ mix(index + 0x632be59bd9b4e019ULL))); }

	std::uint64_t next() noexcept {
		const auto result = rotl(state_[1] * 5, 7) * 9, t = state_[1] << 17;
		state_[2] ^= state_[0]; state_[3] ^= state_[1]; state_[1] ^= state_[2]; state_[0] ^= state_[3];
		state_[2] ^= t; state_[3] = rotl(state_[3], 45);
		return result;
	}
	// Uniform in [0, n); n > 0.
	std::uint64_t below(std::uint64_t n) {
		if (n == 0) { throw std::invalid_argument("below(0)"); }
		std::uint64_t out;
		while (!bounded_draw<std::uint64_t, unsigned __int128>(next(), n, out)) {}
		return out;
	}
	// Uniform in [low, high].
	std::uint64_t between(std::uint64_t low, std::uint64_t high) {
		if (high < low) { throw std::invalid_argument("between"); }
		return high - low == std::numeric_limits<std::uint64_t>::max() ? next() : low + below(high - low + 1);
	}
	// True with probability ppm / 10^6.
	bool chance_ppm(std::uint32_t ppm) { return below(1'000'000) < ppm; }

private:
	static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
	std::uint64_t key_;
	std::array<std::uint64_t, 4> state_{};
};

// A distribution by its quantiles: points (q, value) with q in parts per million, from
// q = 0 to q = 10^6, both coordinates nondecreasing; draws interpolate linearly between them.
class quantile_table {
public:
	struct point { std::uint32_t q; std::uint64_t value; };
	explicit quantile_table(std::vector<point> points) : points_(std::move(points)) {
		if (points_.size() < 2 || points_.front().q != 0 || points_.back().q != 1'000'000) { throw std::invalid_argument("quantile table ends"); }
		for (std::size_t i = 1; i < points_.size(); ++i) {
			if (points_[i].q < points_[i - 1].q || points_[i].value < points_[i - 1].value) { throw std::invalid_argument("quantile table order"); }
		}
	}
	// The value at quantile q (parts per million).
	std::uint64_t at(std::uint32_t q) const {
		if (q > 1'000'000) { throw std::invalid_argument("quantile"); }
		std::size_t i = 1;
		while (points_[i].q < q) { ++i; }
		const auto& a = points_[i - 1]; const auto& b = points_[i];
		if (b.q == a.q) { return b.value; }
		return a.value + std::uint64_t((unsigned __int128)(b.value - a.value) * (q - a.q) / (b.q - a.q));
	}
	std::uint64_t draw(stream& s) const { return at(std::uint32_t(s.below(1'000'001))); }

private:
	std::vector<point> points_;
};

}  // namespace kronuz::simulation
