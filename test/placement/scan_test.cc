// The seekable scan (quest placement A2): on random key sets and item lists it lists exactly what a
// brute-force filter lists, and a pattern confined to one subtree reads at most that subtree plus
// the path down to it and one record past it. Modes: `model`, `bound`, `cases`; none runs all.
// An optional second argument is the first seed, a third the iterations.
#include "scan_glob.hh"

#include "strata/placement/scan.hh"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sp = strata::placement;
namespace st = strata::placement::test;

int failures = 0;
void check(bool ok, const std::string& what) {
	if (!ok) {
		++failures;
		if (failures <= 20) {
			std::cerr << "FAIL: " << what << '\n';
		}
	}
}

// Path order: `/` sorts below every byte a name holds.
bool path_less(std::string_view a, std::string_view b) {
	std::size_t i = 0;
	while (i < a.size() && i < b.size() && a[i] == b[i]) {
		++i;
	}
	if (i == b.size()) {
		return false;
	}
	if (i == a.size()) {
		return true;
	}
	const auto rank = [](char c) { return c == '/' ? 0u : static_cast<unsigned char>(c) + 1u; };
	return rank(a[i]) < rank(b[i]);
}

bool under(std::string_view top, std::string_view path) {
	return path.size() > top.size() && path.starts_with(top) && path[top.size()] == '/';
}

// Records in path order, as a vector; it checks that every call moves forward.
class vector_source {
public:
	explicit vector_source(const std::vector<std::string>& keys) : keys_(keys) { }

	std::optional<std::string_view> seek(std::string_view path) {
		++seeks;
		return land(std::partition_point(keys_.begin(), keys_.end(), [&](const std::string& k) { return path_less(k, path); }));
	}
	std::optional<std::string_view> skip(std::string_view path) {
		++seeks;
		return land(std::partition_point(keys_.begin(), keys_.end(), [&](const std::string& k) { return !path_less(path, k) || under(path, k); }));
	}
	std::optional<std::string_view> next() {
		check(at_ != keys_.end(), "next() before any seek or past the end");
		return at_ == keys_.end() ? std::nullopt : land(std::next(at_));
	}

	std::size_t seeks = 0;
	bool backward = false;

private:
	using iter = std::vector<std::string>::const_iterator;
	std::optional<std::string_view> land(iter to) {
		if (started_ && to <= at_ && at_ != keys_.end()) {
			backward = true;
		}
		started_ = true;
		at_ = to;
		return to == keys_.end() ? std::nullopt : std::optional<std::string_view>(*to);
	}

	const std::vector<std::string>& keys_;
	iter at_ = keys_.end();
	bool started_ = false;
};

struct result {
	std::vector<std::string> listed;
	sp::scan_work work;
	std::size_t source_seeks = 0;
	bool backward = false;
};

result run(const std::vector<std::string>& keys, const std::vector<std::string>& list, std::size_t max_visits = SIZE_MAX) {
	vector_source source(keys);
	st::glob_matcher matcher(list);
	result r;
	r.work = sp::scan(source, matcher, max_visits, [&](std::string_view path) { r.listed.emplace_back(path); });
	r.source_seeks = source.seeks;
	r.backward = source.backward;
	return r;
}

std::vector<std::string> brute(const std::vector<std::string>& keys, const std::vector<std::string>& list) {
	std::vector<std::string> out;
	for (const auto& k : keys) {
		if (st::model::listed(list, k)) {
			out.push_back(k);
		}
	}
	return out;
}

std::vector<std::string> sorted(std::vector<std::string> keys) {
	std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) { return path_less(a, b); });
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return keys;
}

std::string show(const std::vector<std::string>& v) {
	std::string s = "[";
	for (const auto& x : v) {
		s += (s.size() > 1 ? ", " : "") + x;
	}
	return s + "]";
}

// --- random key sets and item lists -------------------------------------------------------------

struct gen {
	std::mt19937_64 rng;
	explicit gen(std::uint64_t seed) : rng(seed) { }

	std::size_t below(std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); }
	bool chance(unsigned percent) { return below(100) < percent; }

	// Names around `/` in byte order (`-` 0x2d, `0` 0x30), a two-byte character, hidden segments.
	std::string name() {
		static const char* const chars[] = {"a", "b", "c", "-", "0", "\xc3\xa9", "z"};
		std::string s = chance(6) ? "." : "";
		const auto n = 1 + below(3);
		for (std::size_t i = 0; i < n; ++i) {
			s += chars[below(std::size(chars))];
		}
		return s;
	}

	std::vector<std::string> keys() {
		std::vector<std::string> out;
		const auto n = below(150);
		for (std::size_t i = 0; i < n; ++i) {
			std::string path = !out.empty() && chance(60) ? out[below(out.size())] + "/" : "";
			path += name();
			while (chance(25) && std::count(path.begin(), path.end(), '/') < 5) {
				path += "/" + name();
			}
			out.push_back(path);
		}
		return sorted(out);
	}

	std::string wild_segment() {
		static const char* const parts[] = {"a", "b", "c", "-", "0", "\xc3\xa9", "*", "?", "[ab]", "[!a]", "[^a-b]", "[]a]", "[a-]", "[", "**", "[\xc3\xa0-\xc3\xaf]"};
		std::string s;
		const auto n = 1 + below(3);
		for (std::size_t i = 0; i < n; ++i) {
			s += parts[below(std::size(parts))];
		}
		return s;
	}

	// A pattern from a key (some segments made wild) or from nothing.
	std::string pattern(const std::vector<std::string>& keys) {
		std::vector<std::string> segments;
		if (!keys.empty() && chance(60)) {
			for (const auto s : st::model::split(keys[below(keys.size())])) {
				segments.emplace_back(s);
			}
			for (auto& s : segments) {
				if (chance(30)) {
					s = chance(30) ? "**" : chance(50) ? "*" : wild_segment();
				} else if (chance(15) && s.size() > 1) {
					s = s.substr(0, s.size() / 2) + "*";
				}
			}
			if (chance(15)) {
				segments.insert(segments.begin(), chance(50) ? "**" : "*");
			}
			if (chance(15)) {
				segments.emplace_back(chance(50) ? "**" : "*");
			}
		} else {
			const auto n = 1 + below(4);
			for (std::size_t i = 0; i < n; ++i) {
				segments.push_back(chance(25) ? "**" : chance(40) ? name() : wild_segment());
			}
		}
		std::string out;
		for (const auto& s : segments) {
			out += (out.empty() ? "" : "/") + s;
		}
		return out;
	}

	std::vector<std::string> list(const std::vector<std::string>& keys) {
		std::vector<std::string> out;
		const auto n = 1 + below(3);
		for (std::size_t i = 0; i < n; ++i) {
			if (chance(5)) {
				out.emplace_back(chance(50) ? "_all" : "-");
			} else {
				out.push_back((i > 0 && chance(35) ? "-" : "") + pattern(keys));
			}
		}
		return out;
	}
};

// --- the model: the same listing as the brute-force filter ---------------------------------------

void model(std::uint64_t seed, std::size_t iterations) {
	std::size_t listed = 0, visits = 0, keys_seen = 0, seeks = 0;
	for (std::size_t it = 0; it < iterations; ++it) {
		gen g(seed + it);
		const auto keys = g.keys();
		const auto list = g.list(keys);
		const auto r = run(keys, list);
		const auto want = brute(keys, list);
		const std::string at = " (seed " + std::to_string(seed + it) + ", items " + show(list) + ")";
		check(r.listed == want, "scan " + show(r.listed) + " != brute force " + show(want) + at);
		check(r.work.complete, "a scan without a bound completes" + at);
		check(!r.backward, "the scan never moves backward" + at);
		check(r.work.visits <= keys.size(), "the scan reads a record at most once" + at);
		check(r.work.seeks == r.source_seeks, "the scan counts its seeks" + at);
		check(r.work.seeks <= r.work.visits + 1, "at most one seek per record read, plus the first" + at);
		// Stopped at a bound: what it listed is the start of the full listing.
		if (r.work.visits > 1) {
			const auto bound = 1 + g.below(r.work.visits - 1);
			const auto cut = run(keys, list, bound);
			check(!cut.work.complete && cut.work.visits == bound, "a scan stops at its bound" + at);
			check(cut.listed.size() <= want.size() && std::equal(cut.listed.begin(), cut.listed.end(), want.begin()), "a bounded scan lists a prefix" + at);
		}
		listed += want.size();
		visits += r.work.visits;
		keys_seen += keys.size();
		seeks += r.work.seeks;
	}
	std::cout << "model: " << iterations << " key sets and item lists, " << keys_seen << " records, " << listed << " listed, " << visits << " read, " << seeks << " seeks\n";
}

// --- the bound: a one-subtree pattern reads its subtree, the path to it and one record ----------

std::size_t depth_of(std::string_view path) { return 1 + std::count(path.begin(), path.end(), '/'); }

void bound(std::uint64_t seed, std::size_t iterations) {
	std::size_t cases = 0, total_visits = 0, total_subtree = 0, total_keys = 0, child_cases = 0;
	double worst = 0;
	for (std::size_t it = 0; it < iterations; ++it) {
		gen g(seed + it);
		const auto keys = g.keys();
		if (keys.empty()) {
			continue;
		}
		// P: a key or a proper prefix of one.
		auto segments = st::model::split(keys[g.below(keys.size())]);
		segments.resize(1 + g.below(segments.size()));
		std::string top;
		for (const auto s : segments) {
			top += (top.empty() ? "" : "/") + std::string(s);
		}
		const std::size_t subtree = std::count_if(keys.begin(), keys.end(), [&](const std::string& k) { return k == top || under(top, k); });
		const std::size_t depth = depth_of(top);
		const std::string at = " (seed " + std::to_string(seed + it) + ", P " + top + ")";

		// Any pattern under P, with exclusions.
		std::vector<std::string> list = {top + "/" + g.pattern({})};
		if (g.chance(30)) {
			list.push_back("-" + top + "/" + g.pattern({}));
		}
		const auto r = run(keys, list);
		check(r.listed == brute(keys, list), "the bound's scan lists what the brute force does" + at);
		check(r.work.visits <= subtree + depth + 1, "P/<pattern> " + show(list) + " reads " + std::to_string(r.work.visits) + " > subtree " + std::to_string(subtree) + " + depth " + std::to_string(depth) + " + 1" + at);
		++cases;
		total_visits += r.work.visits;
		total_subtree += subtree;
		total_keys += keys.size();
		worst = std::max(worst, static_cast<double>(r.work.visits) / static_cast<double>(subtree + depth + 1));

		// P/* reads one record per child of P, not their subtrees.
		std::set<std::string> children;
		for (const auto& k : keys) {
			if (under(top, k)) {
				const auto rest = std::string_view(k).substr(top.size() + 1);
				children.emplace(rest.substr(0, rest.find('/')));
			}
		}
		const auto c = run(keys, {top + "/*"});
		check(c.work.visits <= children.size() + depth + 1, "P/* reads " + std::to_string(c.work.visits) + " > children " + std::to_string(children.size()) + " + depth " + std::to_string(depth) + " + 1" + at);
		child_cases += children.size() < subtree - (std::find(keys.begin(), keys.end(), top) != keys.end() ? 1 : 0) ? 1 : 0;
	}
	std::cout << "bound: " << cases << " one-subtree patterns over " << total_keys << " records: " << total_visits << " read for subtrees of " << total_subtree
	          << " (worst " << worst << " of subtree + depth + 1); " << child_cases << " P/* cases where a child's subtree holds more than the child\n";
}

// --- named cases, with values ---------------------------------------------------------------------

void cases() {
	const auto keys = sorted({".xapiand", ".xapiand/nodes", "a/.hidden/b", "logs", "logs/2026", "logs/2026/a", "twitter", "twitter/user", "twitter/user/x", "twitter-a", "twitter0", "\xc3\xa9"});
	check(keys[7] == "twitter/user" && keys[9] == "twitter-a", "path order puts twitter/user before twitter-a");
	const auto expect = [&](std::vector<std::string> list, std::vector<std::string> want) {
		const auto r = run(keys, list);
		check(r.listed == want, show(list) + " lists " + show(r.listed) + ", not " + show(want));
		check(want == brute(keys, list), show(list) + ": the brute force agrees with the case");
	};
	expect({"twitter"}, {"twitter"});
	expect({"twitter*"}, {"twitter", "twitter-a", "twitter0"});
	expect({"twitter/**"}, {"twitter", "twitter/user", "twitter/user/x"});
	expect({"twitter/*"}, {"twitter/user"});
	expect({"**"}, {"logs", "logs/2026", "logs/2026/a", "twitter", "twitter/user", "twitter/user/x", "twitter-a", "twitter0", "\xc3\xa9"});
	expect({"_all"}, {"logs", "logs/2026", "logs/2026/a", "twitter", "twitter/user", "twitter/user/x", "twitter-a", "twitter0", "\xc3\xa9"});
	expect({"_all", "-twitter/**"}, {"logs", "logs/2026", "logs/2026/a", "twitter-a", "twitter0", "\xc3\xa9"});
	expect({"**/x"}, {"twitter/user/x"});
	expect({"*"}, {"logs", "twitter", "twitter-a", "twitter0", "\xc3\xa9"});
	expect({"*/2026"}, {"logs/2026"});
	expect({"[lt]*", "-*-*"}, {"logs", "twitter", "twitter0"});
	expect({"?"}, {"\xc3\xa9"});
	expect({"a/**"}, {});
	expect({"**/b"}, {});
	expect({".xapiand"}, {});

	// The work, counted: twitter/* reads twitter, twitter/user and twitter-a, and skips
	// twitter/user/x; the scan seeks to twitter and past twitter/user.
	const auto r = run(keys, {"twitter/*"});
	check(r.work.visits == 3 && r.work.seeks == 2, "twitter/* reads 3 records with 2 seeks, not " + std::to_string(r.work.visits) + " with " + std::to_string(r.work.seeks));

	// A wide subtree: 16 children with 16 records each under one P, beside 63 more such trees.
	std::vector<std::string> wide;
	for (int t = 0; t < 64; ++t) {
		const std::string top = "t" + std::to_string(t);
		wide.push_back(top);
		for (int c = 0; c < 16; ++c) {
			wide.push_back(top + "/c" + std::to_string(c));
			for (int d = 0; d < 16; ++d) {
				wide.push_back(top + "/c" + std::to_string(c) + "/d" + std::to_string(d));
			}
		}
	}
	wide = sorted(wide);
	const auto one = run(wide, {"t7/*"});
	check(one.listed.size() == 16 && one.work.visits == 18, "t7/* reads t7, its 16 children and one record past, 18, not " + std::to_string(one.work.visits));
	const auto all_of = run(wide, {"t7/**"});
	check(all_of.listed.size() == 273 && all_of.work.visits == 274, "t7/** reads its 273 records and one past, 274, not " + std::to_string(all_of.work.visits));
	const auto deep = run(wide, {"t7/c1*/d3"});
	check(deep.listed == std::vector<std::string>{"t7/c1/d3", "t7/c10/d3", "t7/c11/d3", "t7/c12/d3", "t7/c13/d3", "t7/c14/d3", "t7/c15/d3"}, "t7/c1*/d3 lists 7 records, not " + show(deep.listed));
	check(deep.work.visits <= 1 + 7 + 7 + 1, "t7/c1*/d3 reads at most t7, the 7 c1* children, their 7 d3 and one past: " + std::to_string(deep.work.visits));
	std::cout << "cases: t7/* read " << one.work.visits << " of " << wide.size() << " records with " << one.work.seeks << " seeks; t7/** read " << all_of.work.visits
	          << "; t7/c1*/d3 read " << deep.work.visits << " with " << deep.work.seeks << " seeks\n";
}

}  // namespace

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "all";
	const std::uint64_t seed = argc > 2 ? std::stoull(argv[2]) : 1;
	const std::size_t iterations = argc > 3 ? std::stoull(argv[3]) : 20000;
	if (mode == "all" || mode == "cases") {
		cases();
	}
	if (mode == "all" || mode == "model") {
		model(seed, iterations);
	}
	if (mode == "all" || mode == "bound") {
		bound(seed, iterations);
	}
	if (failures) {
		std::cerr << failures << " failures\n";
		return 1;
	}
	std::cout << "ok\n";
	return 0;
}
