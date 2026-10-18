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
// matcher that runs one over keys, resuming at shared prefixes and reporting prefixes no
// key can match past and where the next possible match starts. Depends only on `unicode`.
//
// The automaton contract (D5): a state is words() plain 64-bit words, copyable as bytes;
// an automaton is immutable once built, so one serves many threads; next() never skips a
// character that keeps a state alive.

#pragma once

#include "unicode.hh"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::automata {

using word = std::uint64_t;

// Called with bytes before they are allocated; throws to refuse them (D11's budget).
using charge_fn = std::function<void(std::uint64_t bytes)>;

// The largest code point next_utf8 produces.
inline constexpr char32_t max_char = 0x10FFFF;

// Bytes unicode::next_utf8 examines to decode the character whose first byte is b (D1).
constexpr std::size_t lookahead(unsigned char b) noexcept {
	return b >= 0xC2 && b <= 0xDF ? 2 : b >= 0xE0 && b <= 0xEF ? 3 : b >= 0xF0 && b <= 0xF4 ? 4 : 1;
}

// A character and the bytes that spell it.
struct character {
	char32_t c;
	std::string_view bytes;
};

// The characters of s, decoded as analysis decodes terms (D1); their bytes view s.
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
std::vector<character> decode(std::string&&) = delete;  // its views would dangle

// Whether two byte forms decode to c: an invalid byte aliases U+0080 to U+00FF (D1).
constexpr bool aliased(char32_t c) noexcept { return c >= 0x80 && c <= 0xFF; }

// Appends the byte forms that decode to c (D1): its UTF-8 encoding, and for U+0080 to
// U+00FF also the lone byte. Each form is passed to f.
template <class F>
void spellings(char32_t c, F&& f) {
	char b[4];
	if (c < 0x80) {
		b[0] = char(c);
		f(std::string_view(b, 1));
		return;
	}
	if (c < 0x800) {
		b[0] = char(0xC0 | (c >> 6));
		b[1] = char(0x80 | (c & 0x3F));
		f(std::string_view(b, 2));
		if (c <= 0xFF) {
			b[0] = char(c);
			f(std::string_view(b, 1));
		}
		return;
	}
	if (c < 0x10000) {
		b[0] = char(0xE0 | (c >> 12));
		b[1] = char(0x80 | ((c >> 6) & 0x3F));
		b[2] = char(0x80 | (c & 0x3F));
		f(std::string_view(b, 3));
		return;
	}
	if (c <= max_char) {
		b[0] = char(0xF0 | (c >> 18));
		b[1] = char(0x80 | ((c >> 12) & 0x3F));
		b[2] = char(0x80 | ((c >> 6) & 0x3F));
		b[3] = char(0x80 | (c & 0x3F));
		f(std::string_view(b, 4));
	}
}

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
	std::optional<char32_t> next(const word*, char32_t c) const noexcept { return c; }
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

	explicit glob(std::string_view pattern, unsigned flags = multi | single, const charge_fn& charge = {}) {
		if (charge) { charge(pattern.size() * (sizeof(character) + 2 * sizeof(char32_t) + sizeof(word))); }  // decoding and elements
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
		if (charge) { charge((chars_.size() + 2) * words_ * sizeof(word)); }  // the masks
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

	// Any character while a `?` or `*` position is live; else the smallest live literal >= c.
	std::optional<char32_t> next(const word* s, char32_t c) const noexcept {
		bool live = false;
		for (std::size_t w = 0; w < words_; ++w) {
			if (s[w] & (any_[w] | star_[w])) { return c; }
			live = live || s[w];
		}
		if (!live) { return std::nullopt; }
		for (auto it = std::lower_bound(chars_.begin(), chars_.end(), c); it != chars_.end(); ++it) {
			const word* lit = masks_.data() + std::size_t(it - chars_.begin()) * words_;
			for (std::size_t w = 0; w < words_; ++w) {
				if (s[w] & lit[w]) { return *it; }
			}
		}
		return std::nullopt;
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
// [characters read][band: w cells][swap band: w cells, with transpositions]. Every k is
// accepted: past any key's length it changes no answer, so it clamps there.
class levenshtein {
public:
	static constexpr bool byte_range = false;
	static constexpr std::uint64_t clamp_distance = std::uint64_t(1) << 62;

	levenshtein(std::string_view target, std::uint64_t k, std::size_t prefix_chars = 0, bool transpositions = true, const charge_fn& charge = {})
		: k_(std::min(k, clamp_distance)), k1_(k_ + 1), swaps_(transpositions) {
		if (charge) { charge(target.size() * (sizeof(character) + 2 * sizeof(char32_t))); }  // decoding and the target
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
		w_ = std::size_t(std::min<std::uint64_t>(2 * k_ + 1, n_ + 1));
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

	// Any character while a cell is below k; else the smallest >= c among the target
	// characters after cells at k and the swap band's live cells. Conservative.
	std::optional<char32_t> next(const word* s, char32_t c) const noexcept {
		const word r = s[0];
		if (r < f_) { return fixed_[r] >= c ? std::optional<char32_t>(fixed_[r]) : std::nullopt; }
		if (!can_match(s)) { return std::nullopt; }
		const std::uint64_t lo0 = lo(r - f_);
		std::optional<char32_t> best;
		auto consider = [&](char32_t t) {
			if (t >= c && (!best || t < *best)) { best = t; }
		};
		for (std::size_t x = 0; x < w_; ++x) {
			const std::uint64_t i = lo0 + x, v = s[1 + x];
			if (v < k_) { return c; }
			if (v == k_ && i < n_) { consider(p_[i]); }
			if (swaps_ && s[1 + w_ + x] <= k_ && i >= 2) { consider(p_[i - 2]); }
		}
		return best;
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

// Runs an automaton over keys, resuming at the bytes each shares with the previous one;
// correct in any order, fastest in increasing order. Not thread-safe: one per walk.
template <class A>
class matcher {
public:
	struct result {
		bool match = false;
		std::size_t dead = 0;  // every key starting with this key's first `dead` bytes fails (0: none known)
		bool known = false;    // answered from the previous key's dead prefix, undecoded
		bool died = false;     // the automaton died on this key (or on the one `known` refers to)
	};

	explicit matcher(const A& a, std::size_t resume_depth = default_resume_depth, charge_fn charge = {})
		: a_(&a), words_(a.words()), depth_(resume_depth), charge_(std::move(charge)) {
		if (charge_) { charge_(3 * words_ * sizeof(word)); }
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
		if (dead_ && l >= dead_) { return {false, dead_, true, true}; }
		std::size_t b = offset_.size() - 1;
		while (seen_[b] > l) { --b; }
		offset_.resize(b + 1);
		seen_.resize(b + 1);
		std::copy_n(states_.data() + b * words_, words_, cur_.data());
		std::size_t seen = seen_[b];
		if (charge_ && key.size() > prev_.capacity()) { charge_(key.size() - prev_.capacity()); }
		prev_.assign(key);
		dead_ = 0;
		died_ = false;
		seek_ready_ = false;
		const char* base = key.data();
		const char* p = base + offset_[b];
		const char* end = base + key.size();
		while (p < end) {
			const std::size_t at = std::size_t(p - base), before = seen;
			seen = std::max(seen, at + lookahead(std::uint8_t(*p)));
			const char32_t c = unicode::next_utf8(p, end);
			a_->step(cur_.data(), c, next_.data());
			cur_.swap(next_);
			if (!a_->can_match(cur_.data())) {
				// next_ keeps the state before c, for seek().
				dead_ = seen <= key.size() ? seen : 0;
				died_ = true;
				died_at_ = at;
				died_on_ = c;
				fixed_ = before <= at;
				return {false, dead_, false, true};
			}
			if (offset_.size() <= depth_) {
				reserve(offset_.size() + 1);
				offset_.push_back(std::size_t(p - base));
				seen_.push_back(seen);
				states_.resize(offset_.size() * words_);
				std::copy_n(cur_.data(), words_, states_.data() + (offset_.size() - 1) * words_);
			}
		}
		return {a_->is_match(cur_.data()), 0, false, false};
	}

	// After a result that did not match: the smallest key above it that can match (D6), or
	// none when no later key can. Valid until the next key is decoded.
	std::optional<std::string> seek() {
		if (seek_ready_) { return seek_; }
		seek_ready_ = true;
		const std::string_view key = prev_;
		std::optional<std::string> lo = std::string(key) + '\0';  // the next key, no skip
		if (dead_) {
			lo = successor(key.substr(0, dead_));
			if (!lo) { return seek_ = std::nullopt; }
		}
		seek_ = lo;
		if (!died_ || !fixed_) { return seek_; }
		// Every live key after this one starting with P starts with P + a spelling of a
		// character that keeps the state before the dead one alive.
		const std::string_view P = key.substr(0, died_at_);
		std::optional<std::string> best = successor(P);
		const auto at_most = [&](std::string t) {
			if (!best || t < *best) { best = std::move(t); }
		};
		const auto first = a_->next(next_.data(), 0);
		if (first && a_->next(next_.data(), died_on_) != died_on_) {  // else next() cannot tell
			std::size_t asked = 0;
			for (auto a = first; a && asked < max_alive; a = *a < max_char ? a_->next(next_.data(), *a + 1) : std::nullopt, ++asked) {
				spellings(*a, [&](std::string_view sp) {
					std::string t(P);
					t += sp;
					if (key.starts_with(t)) {
						at_most(std::string(key) + '\0');
					} else if (std::string_view(t) > key) {
						at_most(std::move(t));
					}
				});
			}
			if (asked == max_alive) { return seek_; }  // too many to list: the dead prefix's bound
		}
		if (!best) { return seek_ = std::nullopt; }
		if (*best > *seek_) { seek_ = std::move(best); }
		return seek_;
	}

private:
	// Characters seek() lists before settling for the dead prefix's bound.
	static constexpr std::size_t max_alive = 256;

	// Room for n recorded boundaries, charged before it is allocated.
	void reserve(std::size_t n) {
		if (n <= reserved_) { return; }
		const std::size_t to = std::min(std::max(n, 2 * reserved_ + 16), depth_ + 1);
		if (charge_ && to > reserved_) { charge_((to - reserved_) * (words_ + 2) * sizeof(word)); }
		reserved_ = std::max(to, n);
		offset_.reserve(reserved_);
		seen_.reserve(reserved_);
		states_.reserve(reserved_ * words_);
	}

	const A* a_;
	std::size_t words_, depth_;
	charge_fn charge_;
	std::string prev_;                       // the last key decoded
	std::vector<std::size_t> offset_, seen_;  // per recorded boundary of prev_
	std::vector<word> states_;               // boundary b's state at [b * words, (b + 1) * words)
	std::vector<word> cur_, next_;
	std::size_t reserved_ = 1;
	std::size_t dead_ = 0;  // prev_'s dead prefix, 0 when none
	// Where prev_ died: the dead character's offset and value, and whether the bytes before
	// it decided every earlier character.
	bool died_ = false, fixed_ = false;
	std::size_t died_at_ = 0;
	char32_t died_on_ = 0;
	bool seek_ready_ = false;
	std::optional<std::string> seek_;
};

// Calls f(key) for every key of the sorted range [first, last) the automaton accepts, in
// order, until f returns false; jumps to each dead key's seek target by binary search.
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
			if (r.died) {
				const auto next = m.seek();
				if (!next) { return; }
				it = std::lower_bound(it, last, std::string_view(*next), less);
			} else {
				++it;
			}
		}
	}
}

}  // namespace strata::automata
