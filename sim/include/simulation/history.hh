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

// The history file, version 1 (simulator design D9): one operation per line, shared by the
// simulator's clients, C5's harness, and the real-cluster gate, so one checker reads all of
// them. The reader rejects other versions and names the line of every malformed record,
// including a client whose operations overlap or that continues after an unknown outcome.

#pragma once

#include "checker.hh"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kronuz::simulation::check {

// simulated: simulated ns from 0. monotonic: one host's monotonic clock in ns, which takes
// both timestamps of every operation.
enum class time_base : std::uint8_t { simulated, monotonic };

struct history {
	time_base base = time_base::simulated;
	std::vector<operation> ops;
};

struct read_result {
	history h;
	std::size_t line = 0;  // the first malformed line, 1-based; 0 when none
	std::string error;
	bool ok() const noexcept { return error.empty(); }
};

inline constexpr std::string_view history_magic = "strata-history";
inline constexpr std::uint64_t history_version = 1;
inline constexpr std::size_t history_max_line = 256;

namespace detail {

inline constexpr std::string_view kind_names[] = {"put", "erase", "get"};
inline constexpr std::string_view outcome_names[] = {"ok", "conflict", "missing", "unknown"};

inline void append_number(std::string& out, std::uint64_t v) { out += std::to_string(v); }
inline void append_field(std::string& out, std::optional<std::uint64_t> v) {
	out += ' ';
	if (v) { append_number(out, *v); } else { out += '-'; }
}

// Unsigned decimal without sign or leading zeros, within 64 bits.
inline std::optional<std::uint64_t> parse_number(std::string_view s) {
	if (s.empty() || s.size() > 20 || (s.size() > 1 && s[0] == '0')) { return std::nullopt; }
	for (unsigned char c : s) { if (c < '0' || c > '9') { return std::nullopt; } }
	std::uint64_t v = 0;
	const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc() || end != s.data() + s.size()) { return std::nullopt; }
	return v;
}
// A field that may be "-".
inline bool parse_optional(std::string_view s, std::optional<std::uint64_t>& out) {
	if (s == "-") { out.reset(); return true; }
	out = parse_number(s);
	return out.has_value();
}
template <class E, std::size_t N> bool parse_name(std::string_view s, const std::string_view (&names)[N], E& out) {
	for (std::size_t i = 0; i < N; ++i) { if (s == names[i]) { out = E(i); return true; } }
	return false;
}

// One record's fields against D9's rules; null when well formed.
inline const char* parse_record(std::string_view line, operation& o) {
	std::string_view f[9];
	std::size_t n = 0;
	for (std::size_t start = 0;;) {
		const auto space = line.find(' ', start);
		if (n == 9) { return "more than 9 fields"; }
		f[n++] = line.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
		if (space == std::string_view::npos) { break; }
		start = space + 1;
	}
	if (n != 9) { return "fewer than 9 fields"; }
	std::optional<std::uint64_t> client, key, invoked, returned, value, expected, version;
	if (!(client = parse_number(f[0]))) { return "client"; }
	if (!(key = parse_number(f[1]))) { return "key"; }
	if (!parse_name(f[2], kind_names, o.op)) { return "op"; }
	if (!(invoked = parse_number(f[3]))) { return "invoked"; }
	if (!parse_optional(f[4], returned)) { return "returned"; }
	if (!parse_name(f[5], outcome_names, o.result)) { return "outcome"; }
	if (!parse_optional(f[6], value)) { return "value"; }
	if (!parse_optional(f[7], expected)) { return "expected"; }
	if (!(version = parse_number(f[8]))) { return "version"; }
	const bool unknown = o.result == outcome::unknown;
	if (returned.has_value() == unknown) { return "returned is - exactly when the outcome is unknown"; }
	switch (o.op) {
	case kind::put:
		if (o.result == outcome::missing) { return "a put cannot be missing"; }
		if (!value) { return "a put needs a value"; }
		break;
	case kind::erase:
		if (value) { return "an erase has no value"; }
		break;
	case kind::get:
		if (o.result == outcome::conflict) { return "a get cannot conflict"; }
		if (expected) { return "a get has no expected version"; }
		if (value.has_value() != (o.result == outcome::ok)) { return "a get has a value exactly when ok"; }
		break;
	}
	const bool versioned = o.result == outcome::conflict || (o.result == outcome::ok && o.op != kind::erase);
	if (!versioned && *version != 0) { return "version is 0 unless an ok put, an ok get, or a conflict"; }
	o.client = *client; o.key = *key; o.invoked = *invoked; o.returned = returned.value_or(0);
	o.value = value.value_or(0); o.expected = expected; o.version = *version;
	return nullptr;
}

}  // namespace detail

inline std::string write_history(const history& h) {
	std::string out(history_magic);
	out += ' '; detail::append_number(out, history_version);
	out += h.base == time_base::simulated ? " simulated\n" : " monotonic\n";
	for (const auto& o : h.ops) {
		const bool unknown = o.result == outcome::unknown;
		detail::append_number(out, o.client);
		out += ' '; detail::append_number(out, o.key);
		out += ' '; out += detail::kind_names[std::size_t(o.op)];
		out += ' '; detail::append_number(out, o.invoked);
		detail::append_field(out, unknown ? std::nullopt : std::optional(o.returned));
		out += ' '; out += detail::outcome_names[std::size_t(o.result)];
		const bool valued = o.op == kind::put || (o.op == kind::get && o.result == outcome::ok);
		detail::append_field(out, valued ? std::optional(o.value) : std::nullopt);
		detail::append_field(out, o.op == kind::get ? std::nullopt : o.expected);
		out += ' '; detail::append_number(out, o.version);
		out += '\n';
	}
	return out;
}

inline read_result read_history(std::string_view text) {
	read_result r;
	auto fail = [&](std::size_t line, std::string error) { r.line = line; r.error = std::move(error); r.h.ops.clear(); return r; };
	std::vector<std::size_t> lines;  // each operation's line
	std::size_t number = 0;
	for (std::size_t start = 0; start < text.size();) {
		++number;
		const auto end = text.find('\n', start);
		if (end == std::string_view::npos) { return fail(number, "the last line has no newline"); }
		const auto line = text.substr(start, end - start);
		start = end + 1;
		if (line.size() > history_max_line) { return fail(number, "line longer than 256 bytes"); }
		if (number == 1) {
			if (line.substr(0, history_magic.size()) != history_magic || line.size() == history_magic.size() || line[history_magic.size()] != ' ') { return fail(1, "not a strata history"); }
			const auto rest = line.substr(history_magic.size() + 1);
			const auto space = rest.find(' ');
			const auto version = detail::parse_number(rest.substr(0, space));
			if (!version) { return fail(1, "malformed version"); }
			if (*version != history_version) { return fail(1, "unsupported version " + std::to_string(*version)); }
			if (space == std::string_view::npos) { return fail(1, "no time base"); }
			const auto base = rest.substr(space + 1);
			if (base == "simulated") { r.h.base = time_base::simulated; }
			else if (base == "monotonic") { r.h.base = time_base::monotonic; }
			else { return fail(1, "unknown time base"); }
			continue;
		}
		if (!line.empty() && line[0] == '#') { continue; }
		operation o;
		if (const char* error = detail::parse_record(line, o)) { return fail(number, error); }
		r.h.ops.push_back(o);
		lines.push_back(number);
	}
	if (number == 0) { return fail(1, "empty"); }
	// Each client is sequential, and an unknown outcome is its last operation.
	std::map<std::uint64_t, std::vector<std::size_t>> by_client;
	for (std::size_t i = 0; i < r.h.ops.size(); ++i) { by_client[r.h.ops[i].client].push_back(i); }
	for (auto& [client, mine] : by_client) {
		std::stable_sort(mine.begin(), mine.end(), [&](std::size_t a, std::size_t b) { return r.h.ops[a].invoked < r.h.ops[b].invoked; });
		for (std::size_t k = 1; k < mine.size(); ++k) {
			const auto& before = r.h.ops[mine[k - 1]];
			const auto& after = r.h.ops[mine[k]];
			if (before.result == outcome::unknown) { return fail(lines[mine[k]], "client " + std::to_string(client) + " continues after an unknown outcome"); }
			if (after.invoked < before.returned) { return fail(lines[mine[k]], "client " + std::to_string(client) + " overlaps its previous operation"); }
		}
	}
	return r;
}

}  // namespace kronuz::simulation::check
