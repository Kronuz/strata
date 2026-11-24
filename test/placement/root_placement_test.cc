// Placement (quest placement A1, cluster-release D7): a property test over seeded creation
// sequences on a fixed set of active nodes, every shard checked for `min(rf, nodes)` distinct
// data members, D7's roles and a voter first, and every node's leaderships at most one above the
// mean; and, with `--stdin`, the choices for a scenario read from stdin, which
// root_placement_model.py checks against its own model.
//   stdin: `node ID GROUPS LEADERS` lines, then `place RF SHARDS` lines; stdout: one line per
//   shard, `ID:ROLE ...` with the preferred leader first, or `error` when placement refuses.
#include "strata/placement/placement.hh"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>

namespace {

namespace sp = strata::placement;

int failures = 0;
std::string context;
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 30) { std::cerr << "FAIL [" << context << "]: " << what << '\n'; }
	return ok;
}

void property(std::uint64_t seed) {
	std::mt19937_64 rng(seed);
	const std::size_t n = 1 + rng() % 12;
	context = "seed " + std::to_string(seed) + ", " + std::to_string(n) + " nodes";
	std::vector<sp::candidate> nodes;
	for (std::size_t i = 0; i < n; ++i) { nodes.push_back({sp::node_id(10 + 3 * i), 0, 0}); }
	std::map<sp::node_id, std::pair<std::uint64_t, std::uint64_t>> counted;  // from the shards
	for (std::size_t step = 0, steps = 1 + rng() % 60; step < steps; ++step) {
		const auto rf = std::uint32_t(1 + rng() % 5);
		const auto shards = 1 + rng() % 6;
		const auto placed = sp::place(nodes, shards, rf);
		check(placed.size() == shards, "every shard placed");
		const std::size_t data = std::min<std::size_t>(rf, n);
		for (const auto& members : placed) {
			std::set<sp::node_id> distinct;
			std::size_t data_members = 0, witnesses = 0, learners = 0;
			for (const auto& m : members) {
				distinct.insert(m.node);
				data_members += m.role != sp::role::witness;
				witnesses += m.role == sp::role::witness;
				learners += m.role == sp::role::learner;
				++counted[m.node].first;
			}
			++counted[members.front().node].second;
			check(distinct.size() == members.size(), "members are distinct nodes");
			check(data_members == data, "min(rf, nodes) data members: " + std::to_string(data_members) + " at rf " + std::to_string(rf));
			check(members.front().role == sp::role::voter, "the preferred leader is a voter");
			check(witnesses == (data == 2 && n > 2 ? 1u : 0u), "a witness exactly when two data members have a third node");
			check(learners == (data == 2 && n == 2 ? 1u : 0u), "a learner exactly when two data members are all the nodes");
		}
		std::uint64_t total = 0, most = 0, least = ~std::uint64_t(0);
		for (const auto& c : nodes) {
			total += c.leaders;
			most = std::max(most, c.leaders);
			least = std::min(least, c.leaders);
			check(counted[c.node] == std::pair{c.groups, c.leaders}, "the loads count the shards placed");
		}
		check(double(most) <= double(total) / double(n) + 1.0, "leaderships at most one above the mean: " + std::to_string(most) + " of " + std::to_string(total) + " on " + std::to_string(n));
		check(most - least <= 1, "leaderships within one of each other");
	}
}

int from_stdin() {
	std::vector<sp::candidate> nodes;
	std::string line;
	while (std::getline(std::cin, line)) {
		std::istringstream in(line);
		std::string what;
		in >> what;
		if (what == "node") {
			sp::candidate c;
			in >> c.node >> c.groups >> c.leaders;
			nodes.push_back(c);
		} else if (what == "place") {
			std::uint32_t rf = 0;
			std::size_t shards = 0;
			in >> rf >> shards;
			try {
				for (const auto& members : sp::place(nodes, shards, rf)) {
					for (std::size_t i = 0; i < members.size(); ++i) { std::cout << (i ? " " : "") << members[i].node << ':' << int(members[i].role); }
					std::cout << '\n';
				}
			} catch (const std::invalid_argument&) {
				std::cout << "error\n";
			}
		}
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc > 1 && std::string(argv[1]) == "--stdin") { return from_stdin(); }
	const std::uint64_t seeds = argc > 1 ? std::stoull(argv[1]) : 2000;
	for (std::uint64_t seed = 1; seed <= seeds; ++seed) { property(seed); }
	std::printf("root placement: %llu seeded sequences: %s\n", (unsigned long long)seeds, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
