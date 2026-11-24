// Placement (cluster-release D7; docs/mechanisms/placement-root.md): an index's shards on the
// active nodes, from their loads, one shard at a time, each choice counted before the next.
#pragma once

#include "strata/placement/records.hh"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace strata::placement {

// An active node and its load as the root holds it.
struct candidate {
	node_id node = 0;
	std::uint64_t groups = 0, leaders = 0;
	bool operator==(const candidate&) const = default;
};

// Places one shard of `rf` on `nodes` and counts it in their loads. The preferred leader is the
// node with the fewest leaderships, so leaderships stay within one of each other under
// creations; the other data members are the nodes with the fewest groups. Two data members are
// never a lasting two-voter group (D7): a third node takes a witness, else the second is a learner.
inline std::vector<member> place_shard(std::vector<candidate>& nodes, std::uint32_t rf) {
	if (nodes.empty() || rf == 0) { throw std::invalid_argument("strata::placement: no nodes or rf 0"); }
	const auto by_leaders = [](const candidate& a, const candidate& b) {
		return a.leaders != b.leaders ? a.leaders < b.leaders : a.groups != b.groups ? a.groups < b.groups : a.node < b.node;
	};
	const auto by_groups = [](const candidate& a, const candidate& b) {
		return a.groups != b.groups ? a.groups < b.groups : a.leaders != b.leaders ? a.leaders < b.leaders : a.node < b.node;
	};
	const std::size_t data = std::min<std::size_t>(rf, nodes.size());
	auto leader = std::min_element(nodes.begin(), nodes.end(), by_leaders);
	std::iter_swap(nodes.begin(), leader);
	// The rest by groups: the data members, then the witness if one joins.
	const std::size_t wanted = std::min(nodes.size(), data == 2 ? std::size_t(3) : data);
	std::partial_sort(nodes.begin() + 1, nodes.begin() + std::ptrdiff_t(wanted), nodes.end(), by_groups);
	std::vector<member> out;
	out.push_back({nodes[0].node, role::voter});
	for (std::size_t i = 1; i < data; ++i) { out.push_back({nodes[i].node, data == 2 && nodes.size() == 2 ? role::learner : role::voter}); }
	if (data == 2 && nodes.size() > 2) { out.push_back({nodes[2].node, role::witness}); }
	for (std::size_t i = 0; i < out.size(); ++i) { ++nodes[i].groups; }
	++nodes[0].leaders;
	return out;
}

// `shards` shards placed in turn, each counted before the next; `nodes` is reordered.
inline std::vector<std::vector<member>> place(std::vector<candidate>& nodes, std::size_t shards, std::uint32_t rf) {
	std::vector<node_id> ids;
	ids.reserve(nodes.size());
	for (const auto& c : nodes) { ids.push_back(c.node); }
	std::sort(ids.begin(), ids.end());
	if ((!ids.empty() && ids.front() == 0) || std::adjacent_find(ids.begin(), ids.end()) != ids.end()) { throw std::invalid_argument("strata::placement: node 0 or repeated"); }
	std::vector<std::vector<member>> out;
	out.reserve(shards);
	for (std::size_t s = 0; s < shards; ++s) { out.push_back(place_shard(nodes, rf)); }
	return out;
}

}  // namespace strata::placement
