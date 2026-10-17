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

// Term automata over characters (docs/design/automata.md): prefixes, globs with `*` and
// `?`, and Levenshtein distance with optional transpositions and a fixed prefix; and a
// matcher that runs one over keys in increasing order, resuming at shared prefixes and
// reporting prefixes no key can match past. Depends only on `unicode`.

#pragma once

#include "unicode.hh"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::automata {

using word = std::uint64_t;

// Bytes unicode::next_utf8 examines to decode the character whose first byte is b (D1).
constexpr std::size_t lookahead(unsigned char b) noexcept {
	return b >= 0xC2 && b <= 0xDF ? 2 : b >= 0xE0 && b <= 0xEF ? 3 : b >= 0xF0 && b <= 0xF4 ? 4 : 1;
}

// A character and the bytes that spell it.
struct character {
	char32_t c;
	std::string_view bytes;
};

// The characters of s, decoded as analysis decodes terms (D1).
inline std::vector<character> decode(std::string_view s) {
	std::vector<character> out;
	const char* p = s.data();
	const char* end = p + s.size();
	while (p < end) {
		const char* at = p;
		const char32_t c = unicode::next_utf8(p, end);
		out.push_back({c, std::string_view(at, std::size_t(p - at))});
	}
	return out;
}

// Whether two byte forms decode to c: an invalid byte aliases U+0080 to U+00FF (D1).
constexpr bool aliased(char32_t c) noexcept { return c >= 0x80 && c <= 0xFF; }

// The smallest string above every string starting with p; none when p is empty or all 0xFF.
inline std::optional<std::string> successor(std::string_view p) {
	while (!p.empty() && std::uint8_t(p.back()) == 0xFF) { p.remove_suffix(1); }
	if (p.empty()) { return std::nullopt; }
	std::string s(p);
	s.back() = char(std::uint8_t(s.back()) + 1);
	return s;
}

// --- prefix (D2) --------------------------------------------------------------------------------

// Every key starting with the bytes `prefix`: a byte range, decoded by no one.
class prefix {
public:
	static constexpr bool byte_range = true;
	explicit prefix(std::string_view p) : prefix_(p) {}
	std::size_t words() const noexcept { return 0; }
	void start(word*) const noexcept {}
	void step(const word*, char32_t, word*) const noexcept {}
	bool is_match(const word*) const noexcept { return true; }
	bool can_match(const word*) const noexcept { return true; }
	std::string_view literal() const noexcept { return prefix_; }

private:
	std::string prefix_;
};

// --- glob (D3) ----------------------------------------------------------------------------------

// `*` matches any run of characters and `?` exactly one, each only under its flag; with
// neither, the pattern is literal. Simulated over a bitset of pattern positions.
class glob {
public:
	static constexpr bool byte_range = false;
	enum flags : unsigned { multi = 1, single = 2 };

	explicit glob(std::string_view pattern, unsigned flags = multi | single) {
		enum kind { literal, any, star };
		std::vector<std::pair<kind, char32_t>> elems;
		bool literal_open = true;
		for (const auto& ch : decode(pattern)) {
			kind k = (flags & multi) && ch.c == U'*' ? star : (flags & single) && ch.c == U'?' ? any : literal;
			if (k == star && !elems.empty() && elems.back().first == star) { continue; }
			if (k != literal || aliased(ch.c)) { literal_open = false; }
			if (literal_open) { literal_.append(ch.bytes); }
			elems.push_back({k, ch.c});
		}
		m_ = elems.size();
		words_ = (m_ + 1 + 63) / 64;
		star_.assign(words_, 0);
		any_.assign(words_, 0);
		for (std::size_t i = 0; i < m_; ++i) {
			if (elems[i].first == literal) { chars_.push_back(elems[i].second); }
		}
		std::sort(chars_.begin(), chars_.end());
		chars_.erase(std::unique(chars_.begin(), chars_.end()), chars_.end());
		masks_.assign(chars_.size() * words_, 0);
		ascii_.fill(-1);
		for (std::size_t j = 0; j < chars_.size(); ++j) {
			if (chars_[j] < 128) { ascii_[chars_[j]] = std::int32_t(j); }
		}
		for (std::size_t i = 0; i < m_; ++i) {
			const word bit = word(1) << (i % 64);
			switch (elems[i].first) {
			case star: star_[i / 64] |= bit; break;
			case any: any_[i / 64] |= bit; break;
			case literal: {
				const std::size_t j = std::size_t(std::lower_bound(chars_.begin(), chars_.end(), elems[i].second) - chars_.begin());
				masks_[j * words_ + i / 64] |= bit;
				break;
			}
			}
		}
	}

	std::size_t words() const noexcept { return words_; }
	std::size_t elements() const noexcept { return m_; }
	std::string_view literal() const noexcept { return literal_; }

	void start(word* s) const noexcept {
		std::fill(s, s + words_, word(0));
		s[0] = 1;
		close(s);
	}

	void step(const word* s, char32_t c, word* out) const noexcept {
		const word* lit = find(c);
		word carry = 0;
		for (std::size_t w = 0; w < words_; ++w) {
			const word moved = s[w] & (any_[w] | (lit ? lit[w] : 0));
			out[w] = (moved << 1) | carry | (s[w] & star_[w]);
			carry = moved >> 63;
		}
		close(out);
	}

	bool is_match(const word* s) const noexcept { return (s[m_ / 64] >> (m_ % 64)) & 1; }
	bool can_match(const word* s) const noexcept {
		for (std::size_t w = 0; w < words_; ++w) {
			if (s[w]) { return true; }
		}
		return false;
	}

private:
	// A star's position also reaches the next one: it may match nothing. Stars never repeat,
	// so one pass closes the set.
	void close(word* s) const noexcept {
		word carry = 0;
		for (std::size_t w = 0; w < words_; ++w) {
			const word skip = s[w] & star_[w];
			s[w] |= (skip << 1) | carry;
			carry = skip >> 63;
		}
	}
	const word* find(char32_t c) const noexcept {
		std::ptrdiff_t j = -1;
		if (c < 128) {
			j = ascii_[c];
		} else {
			auto it = std::lower_bound(chars_.begin(), chars_.end(), c);
			if (it != chars_.end() && *it == c) { j = it - chars_.begin(); }
		}
		return j < 0 ? nullptr : masks_.data() + std::size_t(j) * words_;
	}

	std::size_t m_ = 0, words_ = 1;
	std::vector<word> star_, any_, masks_;  // masks_: per distinct literal character
	std::vector<char32_t> chars_;           // distinct literal characters, increasing
	std::array<std::int32_t, 128> ascii_{};
	std::string literal_;
};

// --- levenshtein (D4) ---------------------------------------------------------------------------

// Keys within k edits of a target (insert, delete, substitute, and with transpositions a
// swap of adjacent characters), whose first prefix_chars characters equal the target's.
// Simulates Ukkonen's band of the dynamic program; the state is
// [characters read][band: w cells][swap band: w cells, with transpositions].
class levenshtein {
public:
	static constexpr bool byte_range = false;
	static constexpr std::uint64_t max_distance = std::numeric_limits<std::uint32_t>::max() - 1;

	levenshtein(std::string_view target, std::uint64_t k, std::size_t prefix_chars = 0, bool transpositions = true)
		: k_(k), k1_(k + 1), swaps_(transpositions) {
		if (k > max_distance) { throw std::invalid_argument("strata: edit distance above 2^32 - 2"); }
		const auto chars = decode(target);
		f_ = std::min(prefix_chars, chars.size());
		bool literal_open = true;
		for (std::size_t i = 0; i < chars.size(); ++i) {
			if (i < f_) {
				fixed_.push_back(chars[i].c);
				if (aliased(chars[i].c)) { literal_open = false; }
				if (literal_open) { literal_.append(chars[i].bytes); }
			} else {
				p_.push_back(chars[i].c);
			}
		}
		n_ = p_.size();
		w_ = std::size_t(std::min<std::uint64_t>(2 * k + 1, n_ + 1));
	}

	std::size_t words() const noexcept { return 1 + (swaps_ ? 2 : 1) * w_; }
	std::string_view literal() const noexcept { return literal_; }

	void start(word* s) const noexcept {
		s[0] = 0;
		if (f_ == 0) {
			row0(s);
		} else {
			fill(s);
		}
	}

	void step(const word* s, char32_t c, word* out) const noexcept {
		const word r = s[0];
		if (r < f_) {
			if (c == fixed_[r]) {
				out[0] = r + 1;
				if (r + 1 == f_) {
					row0(out);
				} else {
					fill(out);
				}
			} else {
				out[0] = f_;  // dead: every cell past k
				fill(out);
			}
			return;
		}
		const std::uint64_t j = r - f_;
		const std::uint64_t lo0 = lo(j), lo1 = lo(j + 1);
		const word* b0 = s + 1;
		const word* x0 = s + 1 + w_;
		word* b1 = out + 1;
		word* x1 = out + 1 + w_;
		auto get = [&](const word* band, std::uint64_t i) { return i >= lo0 && i < lo0 + w_ ? band[i - lo0] : k1_; };
		out[0] = r + 1;
		for (std::size_t x = 0; x < w_; ++x) {
			const std::uint64_t i = lo1 + x;
			std::uint64_t v = get(b0, i) + 1;  // the term's character inserted
			if (i >= 1) {
				v = std::min(v, get(b0, i - 1) + (p_[i - 1] != c ? 1 : 0));  // matched or substituted
				v = std::min(v, (x ? b1[x - 1] : k1_) + 1);                  // the target's character deleted
				if (swaps_ && i >= 2 && p_[i - 2] == c) { v = std::min(v, get(x0, i)); }
			}
			b1[x] = std::min(v, k1_);
			if (swaps_) { x1[x] = i >= 2 && p_[i - 1] == c ? std::min(get(b0, i - 2) + 1, k1_) : k1_; }
		}
	}

	bool is_match(const word* s) const noexcept {
		if (s[0] < f_) { return false; }
		const std::uint64_t lo0 = lo(s[0] - f_);
		return n_ >= lo0 && n_ < lo0 + w_ && s[1 + (n_ - lo0)] <= k_;
	}

	bool can_match(const word* s) const noexcept {
		if (s[0] < f_) { return true; }
		for (std::size_t x = 0; x < w_; ++x) {
			if (s[1 + x] <= k_) { return true; }
		}
		return false;
	}

private:
	// The band's first target position after j characters: it always holds [j - k, j + k].
	std::uint64_t lo(std::uint64_t j) const noexcept {
		const std::uint64_t base = j > k_ ? j - k_ : 0;
		return std::min<std::uint64_t>(base, n_ + 1 - w_);
	}
	void row0(word* s) const noexcept {
		for (std::size_t x = 0; x < w_; ++x) { s[1 + x] = std::min<std::uint64_t>(x, k1_); }
		if (swaps_) { std::fill(s + 1 + w_, s + 1 + 2 * w_, k1_); }
	}
	void fill(word* s) const noexcept { std::fill(s + 1, s + words(), k1_); }

	std::uint64_t k_, k1_;
	bool swaps_;
	std::size_t f_ = 0, n_ = 0, w_ = 1;
	std::vector<char32_t> fixed_, p_;  // the fixed prefix and the rest of the target
	std::string literal_;
};

// --- the walk over sorted keys (D6) ---------------------------------------------------------------

// Boundaries recorded per key; past them a key's tail is decoded on every visit (a preset).
inline constexpr std::size_t default_resume_depth = 4096;

// Runs an automaton over keys given in increasing order. Not thread-safe: one per walk.
template <class A>
class matcher {
public:
	struct result {
		bool match = false;
		std::size_t dead = 0;  // every key starting with this key's first `dead` bytes fails (0: none known)
		bool known = false;    // answered from the previous key's dead prefix, undecoded
	};

	explicit matcher(const A& a, std::size_t resume_depth = default_resume_depth) : a_(&a), words_(a.words()), depth_(resume_depth) {
		offset_.push_back(0);
		seen_.push_back(0);
		states_.resize(words_);
		a.start(states_.data());
		cur_.resize(words_);
		next_.resize(words_);
	}

	result operator()(std::string_view key) {
		std::size_t l = 0;
		const std::size_t shared = std::min(prev_.size(), key.size());
		while (l < shared && prev_[l] == key[l]) { ++l; }
		if (dead_ && l >= dead_) { return {false, dead_, true}; }
		std::size_t b = offset_.size() - 1;
		while (seen_[b] > l) { --b; }
		offset_.resize(b + 1);
		seen_.resize(b + 1);
		std::copy_n(states_.data() + b * words_, words_, cur_.data());
		std::size_t seen = seen_[b];
		prev_.assign(key);
		dead_ = 0;
		const char* base = key.data();
		const char* p = base + offset_[b];
		const char* end = base + key.size();
		while (p < end) {
			seen = std::max(seen, std::size_t(p - base) + lookahead(std::uint8_t(*p)));
			const char32_t c = unicode::next_utf8(p, end);
			a_->step(cur_.data(), c, next_.data());
			cur_.swap(next_);
			if (!a_->can_match(cur_.data())) {
				dead_ = seen <= key.size() ? seen : 0;
				return {false, dead_, false};
			}
			if (offset_.size() <= depth_) {
				offset_.push_back(std::size_t(p - base));
				seen_.push_back(seen);
				states_.resize(offset_.size() * words_);
				std::copy_n(cur_.data(), words_, states_.data() + (offset_.size() - 1) * words_);
			}
		}
		return {a_->is_match(cur_.data()), 0, false};
	}

private:
	const A* a_;
	std::size_t words_, depth_;
	std::string prev_;                       // the last key decoded
	std::vector<std::size_t> offset_, seen_;  // per recorded boundary of prev_
	std::vector<word> states_;               // boundary b's state at [b * words, (b + 1) * words)
	std::vector<word> cur_, next_;
	std::size_t dead_ = 0;  // prev_'s dead prefix, 0 when none
};

// Calls f(key) for every key of the sorted range [first, last) the automaton accepts, in
// order, until f returns false; jumps past dead prefixes by binary search.
template <class A, class It, class F>
void intersect_sorted(const A& a, It first, It last, F&& f) {
	const std::string_view lo = a.literal();
	const auto hi = successor(lo);
	auto less = [](const auto& k, std::string_view v) { return std::string_view(k) < v; };
	It it = std::lower_bound(first, last, lo, less);
	if constexpr (A::byte_range) {
		for (; it != last && (!hi || std::string_view(*it) < *hi); ++it) {
			if (!f(std::string_view(*it))) { return; }
		}
	} else {
		matcher<A> m(a);
		while (it != last) {
			const std::string_view key(*it);
			if (hi && key >= *hi) { return; }
			const auto r = m(key);
			if (r.match && !f(key)) { return; }
			if (r.dead) {
				const auto next = successor(key.substr(0, r.dead));
				if (!next) { return; }
				it = std::lower_bound(it, last, std::string_view(*next), less);
			} else {
				++it;
			}
		}
	}
}

}  // namespace strata::automata
