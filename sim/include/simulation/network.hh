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

// The simulated network (simulator design D4). Nodes sit in a region, zone, and rack, and a
// frame's one-way latency comes from its distance class. Streams model the TCP peer
// transport: frames on one connection and lane arrive in order or not at all, a link's
// bandwidth paces them, and a reset drops what is in flight. Datagrams are lost, duplicated,
// and reordered independently. Partitions color nodes; one-way cuts, clogs, and gray nodes
// act on single links or nodes. Every draw comes from the sending node's stream.

#pragma once

#include "engine.hh"

#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kronuz::simulation {

using node_id = std::uint32_t;

// Node n is position n % nodes_per_rack in rack n / nodes_per_rack, racks numbered within
// zones within regions. The design's default is 5 × 2 × 250 × 40 = 100,000 nodes.
struct topology {
	std::uint32_t regions = 5, zones_per_region = 2, racks_per_zone = 250, nodes_per_rack = 40;
	std::uint32_t size() const noexcept { return regions * zones_per_region * racks_per_zone * nodes_per_rack; }
	std::uint32_t rack(node_id n) const noexcept { return n / nodes_per_rack; }
	std::uint32_t zone(node_id n) const noexcept { return rack(n) / racks_per_zone; }
	std::uint32_t region(node_id n) const noexcept { return zone(n) / zones_per_region; }
};

enum class distance : std::uint8_t { local, rack, zone, region, remote };
inline distance distance_between(const topology& t, node_id a, node_id b) noexcept {
	if (a == b) { return distance::local; }
	if (t.rack(a) == t.rack(b)) { return distance::rack; }
	if (t.zone(a) == t.zone(b)) { return distance::zone; }
	return t.region(a) == t.region(b) ? distance::region : distance::remote;
}

// A latency table with the given median: half of it at the floor, 4× at p99, 10× at the top.
inline quantile_table latency_around(time_ns median) {
	return quantile_table({{0, median / 2}, {500'000, median}, {990'000, 4 * median}, {1'000'000, 10 * median}});
}

struct network_options {
	// By distance class: local, same rack, same zone, other zone, other region.
	std::array<quantile_table, 5> latency{latency_around(10'000), latency_around(50'000), latency_around(100'000), latency_around(500'000), latency_around(30'000'000)};
	std::array<std::uint32_t, 5> datagram_loss_ppm{}, datagram_duplicate_ppm{};
	std::uint64_t bandwidth_bits = 10'000'000'000;  // per directed link, shared by its lanes
	static constexpr std::uint8_t lanes = 4;
};

struct network_stats { std::uint64_t sent = 0, delivered = 0, dropped = 0, duplicated = 0; };

class network {
public:
	// Called at delivery; `lane` is 0 to 3 for streams and 255 for datagrams.
	using handler = std::function<void(node_id from, node_id to, std::uint8_t lane, std::string payload)>;
	static constexpr std::uint8_t datagram = 255;
	static constexpr std::uint64_t trace_frame = 3, trace_datagram = 4;

	network(engine& e, topology t, network_options o, handler h) : engine_(e), topology_(t), options_(std::move(o)), handler_(std::move(h)) {
		const auto n = topology_.size();
		colors_.assign(n, 0); gray_loss_.assign(n, 0); gray_factor_.assign(n, 1);
		random_.reserve(n);
		const auto base = e.random().child("network");
		for (node_id i = 0; i < n; ++i) { random_.push_back(base.child(i)); }
	}
	network(const network&) = delete;
	network& operator=(const network&) = delete;

	const topology& layout() const noexcept { return topology_; }
	const network_stats& stats() const noexcept { return stats_; }

	// A frame on the (from, to, lane) connection; false when the link is cut now.
	bool send(node_id from, node_id to, std::uint8_t lane, std::string payload) {
		check(from, to);
		if (lane >= network_options::lanes) { throw std::invalid_argument("lane"); }
		++stats_.sent;
		if (!reachable(from, to)) { ++stats_.dropped; return false; }
		auto& l = links_[key(from, to)];
		const time_ns now = engine_.now();
		const time_ns start = std::max(now, l.busy_until);
		l.busy_until = start + serialization(payload.size());
		time_ns arrival = l.busy_until + latency(from, to);
		arrival = std::max({arrival, l.last_arrival[lane], l.clogged_until});
		l.last_arrival[lane] = arrival;
		const auto epoch = l.epoch;
		engine_.at(arrival, [this, from, to, lane, epoch, p = std::move(payload)]() mutable {
			const auto found = links_.find(key(from, to));
			if (found == links_.end() || found->second.epoch != epoch || !reachable(from, to)) { ++stats_.dropped; return; }
			deliver(from, to, lane, std::move(p));
		}, trace_frame, to);
		return true;
	}
	// A datagram: may be lost, duplicated, or overtaken by later ones.
	bool send_datagram(node_id from, node_id to, std::string payload) {
		check(from, to);
		++stats_.sent;
		auto& r = random_[from];
		const auto d = std::size_t(distance_between(topology_, from, to));
		if (!reachable(from, to) || r.chance_ppm(options_.datagram_loss_ppm[d]) || r.chance_ppm(gray_loss_[from]) || r.chance_ppm(gray_loss_[to])) { ++stats_.dropped; return false; }
		const int copies = r.chance_ppm(options_.datagram_duplicate_ppm[d]) ? 2 : 1;
		stats_.duplicated += copies - 1;
		for (int c = 0; c < copies; ++c) {
			time_ns arrival = engine_.now() + latency(from, to);
			if (auto found = links_.find(key(from, to)); found != links_.end()) { arrival = std::max(arrival, found->second.clogged_until); }
			engine_.at(arrival, [this, from, to, p = c + 1 == copies ? std::move(payload) : payload]() mutable {
				if (!reachable(from, to)) { ++stats_.dropped; return; }
				deliver(from, to, datagram, std::move(p));
			}, trace_datagram, to);
		}
		return true;
	}

	// Drops every frame in flight between a and b, both ways, on every lane.
	void reset(node_id a, node_id b) {
		check(a, b);
		for (auto k : {key(a, b), key(b, a)}) { if (auto found = links_.find(k); found != links_.end()) { ++found->second.epoch; found->second.last_arrival = {}; } }
	}
	// Partitions: nodes of different colors cannot talk; a cut stops one direction only.
	void color(node_id n, std::uint8_t c) { check(n, n); colors_[n] = c; }
	void cut(node_id from, node_id to) { check(from, to); cuts_.insert(key(from, to)); }
	void uncut(node_id from, node_id to) { cuts_.erase(key(from, to)); }
	void heal() { std::fill(colors_.begin(), colors_.end(), 0); cuts_.clear(); }
	bool reachable(node_id from, node_id to) const { return colors_[from] == colors_[to] && !cuts_.contains(key(from, to)); }
	// Holds deliveries on from → to until `until`, then releases them in order.
	void clog(node_id from, node_id to, time_ns until) { check(from, to); auto& l = links_[key(from, to)]; l.clogged_until = std::max(l.clogged_until, until); }
	// A gray node: its datagrams are lost at loss_ppm and its latencies multiplied by factor.
	void gray(node_id n, std::uint32_t loss_ppm, std::uint32_t factor) { check(n, n); gray_loss_[n] = loss_ppm; gray_factor_[n] = factor ? factor : 1; }

private:
	struct link {
		time_ns busy_until = 0, clogged_until = 0;
		std::array<time_ns, network_options::lanes> last_arrival{};
		std::uint64_t epoch = 0;
	};
	static std::uint64_t key(node_id from, node_id to) noexcept { return (std::uint64_t(from) << 32) | to; }
	void check(node_id a, node_id b) const { if (a >= topology_.size() || b >= topology_.size()) { throw std::out_of_range("node"); } }
	time_ns serialization(std::size_t bytes) const noexcept { return time_ns((unsigned __int128)bytes * 8 * 1'000'000'000 / options_.bandwidth_bits); }
	time_ns latency(node_id from, node_id to) {
		const auto base = options_.latency[std::size_t(distance_between(topology_, from, to))].draw(random_[from]);
		return base * std::max(gray_factor_[from], gray_factor_[to]);
	}
	void deliver(node_id from, node_id to, std::uint8_t lane, std::string p) {
		++stats_.delivered;
		engine_.trace(mix(from) ^ mix(std::uint64_t(to) << 8 | lane) ^ label_hash(p));
		handler_(from, to, lane, std::move(p));
	}

	engine& engine_;
	topology topology_;
	network_options options_;
	handler handler_;
	network_stats stats_;
	std::vector<stream> random_;
	std::vector<std::uint8_t> colors_;
	std::vector<std::uint32_t> gray_loss_, gray_factor_;
	std::unordered_map<std::uint64_t, link> links_;
	std::unordered_set<std::uint64_t> cuts_;
};

}  // namespace kronuz::simulation
