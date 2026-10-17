// Brute-force references for the automata (docs/design/automata.md, T1): each term decoded
// with unicode::next_utf8, a dynamic-programming glob matcher, and the full
// optimal-string-alignment distance. Independent of automata.hh's simulations.
#pragma once

#include "unicode.hh"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace reference {

inline std::vector<char32_t> chars(std::string_view s) {
	std::vector<char32_t> out;
	const char* p = s.data();
	const char* end = p + s.size();
	while (p < end) { out.push_back(unicode::next_utf8(p, end)); }
	return out;
}

inline bool prefix(std::string_view p, std::string_view key) { return key.substr(0, p.size()) == p; }

// `*` (under multi) and `?` (under single) over characters; others literal.
inline bool glob(std::string_view pattern, unsigned flags, std::string_view key) {
	const auto p = chars(pattern), t = chars(key);
	const bool multi = flags & 1, single = flags & 2;
	// ok[i][j]: the first i pattern characters match the first j term characters.
	std::vector<std::vector<char>> ok(p.size() + 1, std::vector<char>(t.size() + 1, 0));
	ok[0][0] = 1;
	for (std::size_t i = 1; i <= p.size(); ++i) {
		const bool star = multi && p[i - 1] == U'*', any = single && p[i - 1] == U'?';
		for (std::size_t j = 0; j <= t.size(); ++j) {
			if (star) {
				ok[i][j] = ok[i - 1][j] || (j && ok[i][j - 1]);
			} else if (j) {
				ok[i][j] = ok[i - 1][j - 1] && (any || p[i - 1] == t[j - 1]);
			}
		}
	}
	return ok[p.size()][t.size()];
}

// Edit distance with adjacent transpositions as one edit (optimal string alignment).
inline std::uint64_t distance(const std::vector<char32_t>& a, const std::vector<char32_t>& b, bool swaps) {
	std::vector<std::vector<std::uint64_t>> d(a.size() + 1, std::vector<std::uint64_t>(b.size() + 1));
	for (std::size_t i = 0; i <= a.size(); ++i) { d[i][0] = i; }
	for (std::size_t j = 0; j <= b.size(); ++j) { d[0][j] = j; }
	for (std::size_t i = 1; i <= a.size(); ++i) {
		for (std::size_t j = 1; j <= b.size(); ++j) {
			d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] != b[j - 1])});
			if (swaps && i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) { d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1); }
		}
	}
	return d[a.size()][b.size()];
}

inline bool levenshtein(std::string_view target, std::uint64_t k, std::size_t fixed, bool swaps, std::string_view key) {
	auto a = chars(target), b = chars(key);
	fixed = std::min(fixed, a.size());
	if (b.size() < fixed || !std::equal(a.begin(), a.begin() + std::ptrdiff_t(fixed), b.begin())) { return false; }
	a.erase(a.begin(), a.begin() + std::ptrdiff_t(fixed));
	b.erase(b.begin(), b.begin() + std::ptrdiff_t(fixed));
	// Length alone rules a pair out, which keeps long random terms cheap.
	const std::uint64_t gap = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
	return gap <= k && distance(a, b, swaps) <= k;
}

}  // namespace reference
