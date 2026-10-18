// Automata (docs/design/automata.md, T1): prefix, glob, and Levenshtein automata against
// brute force over every term of random and adversarial sorted term sets, through
// intersect_sorted and the matcher key by key; every dead prefix the matcher reports is
// checked against random extensions of it.
#include "strata/automata.hh"

#include "automata_reference.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace automata = strata::automata;

namespace {

std::string printable(std::string_view s) {
	std::string out;
	for (unsigned char c : s) {
		if (c >= 0x20 && c < 0x7f) {
			out.push_back(char(c));
		} else {
			char b[5];
			std::snprintf(b, sizeof b, "\\x%02X", c);
			out += b;
		}
	}
	return out;
}

std::uint64_t checked = 0;

// Every match of a over terms equals ref's, through both walks; dead prefixes hold.
template <class A, class Ref>
void compare(const A& a, const std::vector<std::string>& terms, const Ref& ref, std::mt19937_64& rng, const std::string& what) {
	std::vector<std::string> want, got;
	for (const auto& t : terms) {
		if (ref(t)) { want.push_back(t); }
	}
	automata::intersect_sorted(a, terms.begin(), terms.end(), [&](std::string_view k) {
		got.emplace_back(k);
		return true;
	});
	if (got != want) {
		std::fprintf(stderr, "%s: %zu matches, want %zu\n", printable(what).c_str(), got.size(), want.size());
		for (const auto& t : want) {
			if (!std::binary_search(got.begin(), got.end(), t)) { std::fprintf(stderr, "  missing %s\n", printable(t).c_str()); }
		}
		for (const auto& t : got) {
			if (!std::binary_search(want.begin(), want.end(), t)) { std::fprintf(stderr, "  extra %s\n", printable(t).c_str()); }
		}
	}
	CHECK(got == want);
	for (const auto& t : want) { CHECK(t.substr(0, a.literal().size()) == a.literal()); }
	if (!want.empty()) {  // stopping after the first
		std::size_t n = 0;
		automata::intersect_sorted(a, terms.begin(), terms.end(), [&](std::string_view k) {
			CHECK(k == want[0]);
			++n;
			return false;
		});
		CHECK(n == 1);
	}
	if constexpr (!A::byte_range) {
		automata::matcher<A> m(a);
		for (const auto& t : terms) {
			const auto r = m(t);
			if (r.match != ref(t)) { std::fprintf(stderr, "%s: matcher on %s says %d\n", printable(what).c_str(), printable(t).c_str(), int(r.match)); }
			CHECK(r.match == ref(t));
			if (r.dead) {
				CHECK(!r.match && r.dead <= t.size());
				for (int e = 0; e < 3; ++e) {
					std::string ext = t.substr(0, r.dead);
					for (std::size_t i = 0, n = rng() % 6; i < n; ++i) { ext.push_back(char(rng() % 4 ? "ab\xC3\xE2"[rng() % 4] : char(rng()))); }
					if (ref(ext)) { std::fprintf(stderr, "%s: dead prefix %s of %s matches %s\n", printable(what).c_str(), printable(t.substr(0, r.dead)).c_str(), printable(t).c_str(), printable(ext).c_str()); }
					CHECK(!ref(ext));
				}
			}
		}
	}
	checked += terms.size();
}

const std::vector<std::string> ascii_pieces = {"a", "b", "c", "o", "m", "x", "0", "*", "?"};
// Valid 2-, 3-, and 4-byte characters; lone, truncated, overlong, and surrogate sequences;
// and both spellings of U+00E2 (C3 A2 and the lone byte E2).
const std::vector<std::string> unicode_pieces = {
	"a", "b", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9D\x84\x9E", "\xE2", "\x82", "\xC3", "\xFF", std::string(1, '\0'),
	"\xE2\x82", "\xE0\x80\x80", "\xED\xA0\x80", "\xC3\xA2", "\xF0", "*", "?"};

std::string random_string(std::mt19937_64& rng, const std::vector<std::string>& pieces, std::size_t max_pieces) {
	std::string s;
	for (std::size_t i = 0, n = rng() % (max_pieces + 1); i < n; ++i) { s += pieces[rng() % pieces.size()]; }
	return s;
}

std::vector<std::string> sorted(std::set<std::string> s) { return {s.begin(), s.end()}; }

void check_all(const std::vector<std::string>& terms, const std::vector<std::string>& pieces, std::mt19937_64& rng, int patterns) {
	auto any_term = [&] { return terms.empty() ? std::string() : terms[rng() % terms.size()]; };
	for (int i = 0; i < patterns; ++i) {
		// Prefixes: random, and real terms cut short.
		std::string p = rng() % 2 ? random_string(rng, pieces, 3) : any_term().substr(0, rng() % 4);
		compare(automata::prefix(p), terms, [&](std::string_view k) { return reference::prefix(p, k); }, rng, "prefix " + p);
		// Globs under every flag combination.
		std::string g = random_string(rng, pieces, 6);
		if (rng() % 3 == 0) {  // a term with characters replaced by metacharacters
			const std::string base = any_term();
			const auto chars = automata::decode(base);
			g.clear();
			for (const auto& c : chars) { g += rng() % 4 == 0 ? (rng() % 2 ? "*" : "?") : std::string(c.bytes); }
		}
		const unsigned flags = unsigned(rng() % 4);
		compare(automata::glob(g, flags), terms, [&](std::string_view k) { return reference::glob(g, flags, k); }, rng, "glob " + g + " flags " + std::to_string(flags));
		// Levenshtein around a term or a random target.
		const std::string t = rng() % 2 ? any_term() : random_string(rng, pieces, 6);
		const std::uint64_t k = rng() % 4;
		const std::size_t fixed = rng() % 3 == 0 ? 0 : rng() % (automata::decode(t).size() + 2);
		const bool swaps = rng() % 2;
		compare(automata::levenshtein(t, k, fixed, swaps), terms, [&](std::string_view key) { return reference::levenshtein(t, k, fixed, swaps, key); }, rng,
			"levenshtein " + t + " k " + std::to_string(k) + " fixed " + std::to_string(fixed) + " swaps " + std::to_string(swaps));
	}
}

}  // namespace

int main() {
	std::mt19937_64 rng(5);

	// Known answers.
	{
		const std::vector<std::string> terms = sorted({"bama", "oabma", "obama", "obamaxyz", "obam", "obma", "obxma", "ob\xC3\xA9ma", "xobamax"});
		std::vector<std::string> got;
		auto collect = [&](std::string_view k) {
			got.emplace_back(k);
			return true;
		};
		automata::intersect_sorted(automata::levenshtein("obama", 2), terms.begin(), terms.end(), collect);
		CHECK((got == std::vector<std::string>{"bama", "oabma", "obam", "obama", "obma", "obxma", "ob\xC3\xA9ma", "xobamax"}));
		got.clear();
		automata::intersect_sorted(automata::levenshtein("obama", 1, 0, false), terms.begin(), terms.end(), collect);
		CHECK((got == std::vector<std::string>{"bama", "obam", "obama", "obma", "obxma", "ob\xC3\xA9ma"}));  // "oabma" is two edits without swaps
		got.clear();
		automata::intersect_sorted(automata::glob("ob?ma"), terms.begin(), terms.end(), collect);
		CHECK((got == std::vector<std::string>{"obama", "obxma", "ob\xC3\xA9ma"}));
		got.clear();
		automata::intersect_sorted(automata::prefix("obam"), terms.begin(), terms.end(), collect);
		CHECK((got == std::vector<std::string>{"obam", "obama", "obamaxyz"}));
		CHECK(automata::glob("ob?ma").literal() == "ob");
		CHECK(automata::glob("caf\xC3\xA9*").literal() == "caf");  // U+00E9 has two byte forms
		CHECK(automata::levenshtein("obama", 2, 3).literal() == "oba");
		CHECK(automata::glob("a*b", 0).literal() == "a*b");
		bool threw = false;
		try {
			automata::levenshtein("x", automata::levenshtein::max_distance + 1);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		CHECK(threw);
		CHECK(!automata::successor("") && !automata::successor("\xFF\xFF") && *automata::successor("a\xFF") == "b");
	}

	// Every string over {a, b} up to 10 characters: dense matches and long dead runs.
	{
		std::set<std::string> all = {""};
		std::vector<std::string> level = {""};
		for (int len = 1; len <= 10; ++len) {
			std::vector<std::string> next;
			for (const auto& s : level) {
				for (char c : {'a', 'b'}) {
					next.push_back(s + c);
					all.insert(next.back());
				}
			}
			level = std::move(next);
		}
		const auto terms = sorted(all);
		check_all(terms, {"a", "b", "*", "?"}, rng, 40);
		for (const char* g : {"", "*", "?", "**", "*a*a*a*b", "*a?????", "a*b*a", "?*?", "*ab", "ba*"}) {
			compare(automata::glob(g), terms, [&](std::string_view k) { return reference::glob(g, 3, k); }, rng, std::string("glob ") + g);
		}
	}

	// Random sets: ASCII and Unicode pieces, with long terms and long shared prefixes.
	for (int round = 0; round < 60; ++round) {
		const auto& pieces = round % 2 ? unicode_pieces : ascii_pieces;
		std::set<std::string> s;
		for (std::size_t i = 0, n = 1 + rng() % 400; i < n; ++i) { s.insert(random_string(rng, pieces, 1 + rng() % 9)); }
		if (round % 10 == 0) {  // past resume_depth: 5,000 characters, some sharing 4,500
			std::string base = random_string(rng, pieces, 4500);
			for (int j = 0; j < 4; ++j) { s.insert(base + random_string(rng, pieces, 500)); }
			s.insert(std::string(5000, 'a'));
		}
		check_all(sorted(s), pieces, rng, 12);
	}

	// Adversarial patterns over a mixed set.
	{
		std::set<std::string> s;
		for (int i = 0; i < 600; ++i) { s.insert(random_string(rng, i % 2 ? unicode_pieces : ascii_pieces, 14)); }
		s.insert(std::string(80, 'a'));
		s.insert(std::string(80, 'a') + "b");
		const auto terms = sorted(s);
		std::vector<std::string> globs = {"*a*a*a*b", "*a??????????", std::string(70, '?'), "*\xC3\xA9*", "\xE2*", "\xE2\x82\xAC?", "\xC3\xA2*", "?\x82*"};
		std::string many;
		for (int i = 0; i < 40; ++i) { many += "a*"; }
		globs.push_back(many);  // 80 elements: two words of state
		globs.push_back(std::string(80, 'a') + "?");
		for (const auto& g : globs) {
			for (unsigned flags = 0; flags < 4; ++flags) {
				compare(automata::glob(g, flags), terms, [&](std::string_view k) { return reference::glob(g, flags, k); }, rng, "glob " + g);
			}
		}
		for (const std::string& t : {std::string(), std::string("a"), std::string("\xE2"), std::string(30, 'a'), std::string("\xC3\xA2\xE2")}) {
			for (std::uint64_t k = 0; k <= 3; ++k) {
				for (bool swaps : {false, true}) {
					compare(automata::levenshtein(t, k, 0, swaps), terms, [&](std::string_view key) { return reference::levenshtein(t, k, 0, swaps, key); }, rng, "levenshtein " + t);
				}
			}
		}
		// A distance past the term lengths matches everything with the fixed prefix.
		compare(automata::levenshtein("ab", 200, 1), terms, [&](std::string_view key) { return reference::levenshtein("ab", 200, 1, true, key); }, rng, "levenshtein k 200");
	}

	std::printf("automata: %llu term checks, %d failures\n", (unsigned long long)checked, failures);
	return failures ? 1 : 0;
}
