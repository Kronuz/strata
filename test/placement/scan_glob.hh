// The reference glob matcher for the scan (quest placement A2): Xapiand's index patterns (its
// `index_pattern::Glob`, MIT) as a scan_matcher, plus an independent recursive matcher for the
// brute-force model.
//   *     a run of characters inside one segment       ?     one character (a UTF-8 sequence)
//   [..]  a class: `[abc]`, `[a-f]`, `[!x]`, `[^x]`; a `]` first is a member, an unclosed `[` is plain
//   **    any number of whole segments, none included   _all  is `**`
// An item `-p` removes what `p` matches; a path with a segment that starts with `.` is never listed.
#pragma once

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata::placement::test {

inline constexpr std::size_t max_pattern_size = 256;

// Where the class that opens at `pattern[open]` ends (just past its `]`), or npos when it is none.
inline std::size_t class_end(std::string_view pattern, std::size_t open) noexcept {
	std::size_t i = open + 1;
	if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
		++i;
	}
	if (i < pattern.size() && pattern[i] == ']') {
		++i;
	}
	const auto close = pattern.find(']', i);
	return close == std::string_view::npos ? close : close + 1;
}

// The length of the character at `s[i]`: a well-formed UTF-8 sequence, else the one byte.
inline std::size_t unit(std::string_view s, std::size_t i) noexcept {
	const auto c = static_cast<unsigned char>(s[i]);
	const std::size_t len = c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf) ? 2 : (c >= 0xe0 && c <= 0xef) ? 3 : (c >= 0xf0 && c <= 0xf4) ? 4 : 1;
	if (i + len > s.size()) {
		return 1;
	}
	for (std::size_t k = 1; k < len; ++k) {
		if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) {
			return 1;
		}
	}
	return len;
}

inline std::uint32_t code(std::string_view s, std::size_t i, std::size_t len) noexcept {
	const auto c = static_cast<unsigned char>(s[i]);
	if (len == 1) {
		return c;
	}
	std::uint32_t cp = c & (0xff >> (len + 1));
	for (std::size_t k = 1; k < len; ++k) {
		cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
	}
	return cp;
}

// Whether `cp` is a member of the class `pattern[open, end)`.
inline bool in_class(std::string_view pattern, std::size_t open, std::size_t end, std::uint32_t cp) noexcept {
	std::size_t i = open + 1;
	const std::size_t close = end - 1;
	bool negated = false;
	if (pattern[i] == '!' || pattern[i] == '^') {
		negated = true;
		++i;
	}
	bool found = false;
	while (i < close) {
		auto len = unit(pattern, i);
		const auto lo = code(pattern, i, len);
		auto hi = lo;
		i += len;
		if (i + 1 < close && pattern[i] == '-') {
			len = unit(pattern, i + 1);
			hi = code(pattern, i + 1, len);
			i += 1 + len;
		}
		found = found || (lo <= cp && cp <= hi);
	}
	return found != negated;
}

// One segment against one pattern segment: the two-pointer match that retries from the last `*`.
inline bool match_segment(std::string_view pattern, std::string_view name) noexcept {
	constexpr auto none = std::string_view::npos;
	std::size_t p = 0, n = 0, star = none, mark = 0;
	while (n < name.size()) {
		if (p < pattern.size()) {
			const char c = pattern[p];
			if (c == '*') {
				while (p < pattern.size() && pattern[p] == '*') {
					++p;
				}
				star = p;
				mark = n;
				continue;
			}
			const auto len = unit(name, n);
			if (c == '?') {
				++p;
				n += len;
				continue;
			}
			if (c == '[') {
				const auto end = class_end(pattern, p);
				if (end != none) {
					if (in_class(pattern, p, end, code(name, n, len))) {
						p = end;
						n += len;
						continue;
					}
					goto retry;
				}
			}
			if (c == name[n]) {
				++p;
				++n;
				continue;
			}
		}
	retry:
		if (star == none) {
			return false;
		}
		p = star;
		mark += unit(name, mark);
		n = mark;
	}
	while (p < pattern.size() && pattern[p] == '*') {
		++p;
	}
	return p == pattern.size();
}

// The bytes every segment the pattern segment matches starts with, and whether it matches only them.
struct literal_prefix {
	std::string_view text;
	bool whole;
};
inline literal_prefix prefix_of(std::string_view pattern) noexcept {
	for (std::size_t i = 0; i < pattern.size(); ++i) {
		if (pattern[i] == '*' || pattern[i] == '?' || (pattern[i] == '[' && class_end(pattern, i) != std::string_view::npos)) {
			return {pattern.substr(0, i), false};
		}
	}
	return {pattern, true};
}

// One pattern as an automaton over segments: a position per pattern segment, `**` a loop.
class glob {
public:
	static constexpr std::size_t max_segments = max_pattern_size + 1;
	using states = std::bitset<max_segments + 1>;

	explicit glob(std::string pattern) : pattern_(std::move(pattern)) {
		if (pattern_.size() > max_pattern_size) {
			oversize_ = true;
			return;
		}
		const std::string_view all(pattern_);
		for (std::size_t from = 0;;) {
			const auto cut = all.find('/', from);
			const auto text = all.substr(from, cut == std::string_view::npos ? cut : cut - from);
			const bool globstar = text == "**";
			if (!(globstar && !segments_.empty() && segments_.back().globstar)) {
				segments_.push_back({text, globstar});
			}
			if (cut == std::string_view::npos) {
				break;
			}
			from = cut + 1;
		}
	}
	glob(const glob&) = delete;
	glob& operator=(const glob&) = delete;

	states start() const {
		states s;
		if (!oversize_) {
			add(s, 0);
		}
		return s;
	}
	states step(const states& from, std::string_view segment) const {
		states next;
		for (std::size_t i = 0; i < segments_.size(); ++i) {
			if (!from.test(i)) {
				continue;
			}
			if (segments_[i].globstar) {
				add(next, i);
			} else if (match_segment(segments_[i].text, segment)) {
				add(next, i + 1);
			}
		}
		return next;
	}
	bool accepts(const states& s) const noexcept { return s.test(segments_.size()); }
	bool live(const states& s) const noexcept { return s.count() > (accepts(s) ? 1u : 0u); }

	// The least segment greater than `after` that a live position may match; `after` when a `**`
	// or a prefix match leaves it unknown; nullopt when none may.
	std::optional<std::string> skip_to(const states& s, std::string_view after) const {
		std::optional<std::string_view> least;
		for (std::size_t i = 0; i < segments_.size(); ++i) {
			if (!s.test(i)) {
				continue;
			}
			if (segments_[i].globstar) {
				return std::string(after);
			}
			const auto [lead, whole] = prefix_of(segments_[i].text);
			std::optional<std::string_view> candidate;
			if (lead > after) {
				candidate = lead;
			} else if (!whole && after.starts_with(lead)) {
				return std::string(after);
			}
			if (candidate && (!least || *candidate < *least)) {
				least = candidate;
			}
		}
		return least ? std::optional<std::string>(std::string(*least)) : std::nullopt;
	}

private:
	struct segment {
		std::string_view text;
		bool globstar;
	};
	void add(states& s, std::size_t position) const noexcept {
		s.set(position);
		while (position < segments_.size() && segments_[position].globstar) {
			s.set(++position);
		}
	}

	std::string pattern_;
	std::vector<segment> segments_;
	bool oversize_ = false;
};

// A search's items as Xapiand splits them: `-p` an exclusion (when longer than `-`), `_all` is `**`.
struct items {
	std::vector<std::string> wanted;
	std::vector<std::string> excluded;
	explicit items(std::span<const std::string> list) {
		for (const auto& item : list) {
			if (item.size() > 1 && item[0] == '-') {
				excluded.push_back(item.substr(1));
			} else {
				wanted.push_back(item == "_all" ? std::string("**") : item);
			}
		}
	}
};

// The items as a scan_matcher: listed when a wanted pattern accepts and no exclusion does; live
// while a wanted pattern is; a segment that starts with `.` is dead.
class glob_matcher {
public:
	using state = std::vector<glob::states>;  // wanted first

	explicit glob_matcher(std::span<const std::string> list) {
		const items parsed(list);
		wanted_ = parsed.wanted.size();
		globs_.reserve(parsed.wanted.size() + parsed.excluded.size());
		for (const auto& p : parsed.wanted) {
			globs_.emplace_back(std::make_unique<glob>(p));
		}
		for (const auto& p : parsed.excluded) {
			globs_.emplace_back(std::make_unique<glob>(p));
		}
	}

	std::size_t steps = 0;  // segments stepped, for a test's count

	state start() {
		state s;
		for (const auto& g : globs_) {
			s.push_back(g->start());
		}
		return s;
	}
	state step(const state& from, std::string_view segment) {
		++steps;
		state next(from.size());
		if (segment.empty() || segment[0] == '.') {
			return next;
		}
		for (std::size_t i = 0; i < globs_.size(); ++i) {
			next[i] = globs_[i]->step(from[i], segment);
		}
		return next;
	}
	bool accepts(const state& s) const {
		bool wanted = false;
		for (std::size_t i = 0; i < globs_.size(); ++i) {
			if (globs_[i]->accepts(s[i])) {
				if (i >= wanted_) {
					return false;
				}
				wanted = true;
			}
		}
		return wanted;
	}
	bool live(const state& s) const {
		for (std::size_t i = 0; i < wanted_; ++i) {
			if (globs_[i]->live(s[i])) {
				return true;
			}
		}
		return false;
	}
	std::optional<std::string> skip_to(const state& s, std::string_view after) const {
		std::optional<std::string> least;
		for (std::size_t i = 0; i < wanted_; ++i) {
			auto next = globs_[i]->skip_to(s[i], after);
			if (next && (!least || *next < *least)) {
				least = std::move(next);
			}
		}
		return least;
	}

private:
	std::vector<std::unique_ptr<glob>> globs_;
	std::size_t wanted_ = 0;
};

// The brute-force model's matcher: plain recursion over characters and segments, no automaton.
namespace model {

inline bool segment(std::string_view p, std::size_t pi, std::string_view n, std::size_t ni) {
	if (pi == p.size()) {
		return ni == n.size();
	}
	const char c = p[pi];
	if (c == '*') {
		return segment(p, pi + 1, n, ni) || (ni < n.size() && segment(p, pi, n, ni + unit(n, ni)));
	}
	if (ni == n.size()) {
		return false;
	}
	const auto len = unit(n, ni);
	if (c == '?') {
		return segment(p, pi + 1, n, ni + len);
	}
	if (c == '[') {
		if (const auto end = class_end(p, pi); end != std::string_view::npos) {
			return in_class(p, pi, end, code(n, ni, len)) && segment(p, end, n, ni + len);
		}
	}
	return n[ni] == c && segment(p, pi + 1, n, ni + 1);
}

inline std::vector<std::string_view> split(std::string_view s) {
	std::vector<std::string_view> out;
	for (std::size_t from = 0;;) {
		const auto cut = s.find('/', from);
		out.push_back(s.substr(from, cut == std::string_view::npos ? cut : cut - from));
		if (cut == std::string_view::npos) {
			return out;
		}
		from = cut + 1;
	}
}

inline bool path(const std::vector<std::string_view>& p, std::size_t i, const std::vector<std::string_view>& n, std::size_t j) {
	if (i == p.size()) {
		return j == n.size();
	}
	if (p[i] == "**") {
		return path(p, i + 1, n, j) || (j < n.size() && path(p, i, n, j + 1));
	}
	return j < n.size() && segment(p[i], 0, n[j], 0) && path(p, i + 1, n, j + 1);
}

inline bool matches(std::string_view pattern, std::string_view name) {
	return pattern.size() <= max_pattern_size && path(split(pattern), 0, split(name), 0);
}

// Whether the items list `name`.
inline bool listed(std::span<const std::string> list, std::string_view name) {
	const auto segments = split(name);
	if (std::any_of(segments.begin(), segments.end(), [](std::string_view s) { return s.empty() || s[0] == '.'; })) {
		return false;
	}
	const items parsed(list);
	const auto hit = [&](const std::string& p) { return matches(p, name); };
	return std::any_of(parsed.wanted.begin(), parsed.wanted.end(), hit) && std::none_of(parsed.excluded.begin(), parsed.excluded.end(), hit);
}

}  // namespace model

}  // namespace strata::placement::test
