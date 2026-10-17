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
// frame's one-way latency comes from its distance class. Connections model the TCP peer
// transport: frames on one lane of a connection arrive in send order or not at all. A frame
// that reaches an unreachable link is lost and resets its connection, dropping what is in
// flight both ways and telling both ends, so no lane shows a hole. Sends report nothing: a
// sender learns of a partition only from a reset. A link's bandwidth paces its frames, a clog
// holds them (in flight too) until it clears, and a link's state lives only while it has
// frames in flight or a clog. Datagrams are lost, duplicated, and reordered independently.
// Partitions color nodes; one-way cuts, clogs, and gray nodes act on single links or nodes.
// Every draw comes from the sending node's stream.

#pragma once

#include "engine.hh"

#include <algorithm>
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
// zones within regions. The design's default is 5 × 4 × 125 × 40 = 100,000 nodes.
struct topology {
	std::uint32_t regions = 5, zones_per_region = 4, racks_per_zone = 125, nodes_per_rack = 40;
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

// A latency table with the given median: half of it at the floor, 4× at p99, 6× at p99.9,
// 8× at p99.99, and 10× at the top.
inline quantile_table latency_around(time_ns median) {
	return quantile_table({{0, median / 2}, {500'000, median}, {990'000, 4 * median}, {999'000, 6 * median}, {999'900, 8 * median}, {1'000'000, 10 * median}});
}

struct network_options {
	// By distance class: local, same rack, same zone, other zone, other region.
	std::array<quantile_table, 5> latency{latency_around(10'000), latency_around(50'000), latency_around(100'000), latency_around(500'000), latency_around(30'000'000)};
	std::array<std::uint32_t, 5> datagram_loss_ppm{}, datagram_duplicate_ppm{};
	std::uint64_t bandwidth_bits = 10'000'000'000;  // per directed link, shared by its lanes
	static constexpr std::uint8_t lanes = 4;
};

// dropped counts frames and datagrams lost to loss draws, partitions, and resets.
struct network_stats { std::uint64_t sent = 0, delivered = 0, dropped = 0, duplicated = 0, resets = 0; };

class network {
public:
	// Called at delivery; `lane` is 0 to 3 for frames and 255 for datagrams.
	using handler = std::function<void(node_id from, node_id to, std::uint8_t lane, std::string payload)>;
	// Called at a connection reset, once for each end: `node`'s connection to `peer` dropped
	// everything in flight.
	using reset_handler = std::function<void(node_id node, node_id peer)>;
	static constexpr std::uint8_t datagram = 255;
	static constexpr std::uint64_t trace_frame = 3, trace_datagram = 4, trace_reset = 5, trace_clog = 6;

	network(engine& e, topology t, network_options o, handler h, reset_handler r = {})
		: engine_(e), topology_(t), options_(std::move(o)), handler_(std::move(h)), reset_handler_(std::move(r)) {
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
	// Link records alive: those with frames in flight or a clog.
	std::size_t links() const noexcept { return links_.size(); }

	// A frame on lane `lane` of the from–to connection. It arrives in order after the lane's
	// earlier frames, or is dropped with them by a reset.
	void send(node_id from, node_id to, std::uint8_t lane, std::string payload) {
		check(from, to);
		if (lane >= network_options::lanes) { throw std::invalid_argument("lane"); }
		++stats_.sent;
		auto& l = links_[key(from, to)];
		const time_ns now = engine_.now();
		l.busy_until = std::max(now, l.busy_until) + serialization(payload.size());
		const time_ns arrival = std::max(l.busy_until + latency(from, to), l.last_arrival[lane]);
		l.last_arrival[lane] = arrival;
		auto& q = l.queue[lane];
		q.push({arrival, std::move(payload)});
		if (q.size() == 1) { arm(from, to, lane, l); }
	}
	// A datagram: may be lost, duplicated, or overtaken by later ones.
	void send_datagram(node_id from, node_id to, std::string payload) {
		check(from, to);
		++stats_.sent;
		auto& r = random_[from];
		const auto d = std::size_t(distance_between(topology_, from, to));
		if (r.chance_ppm(options_.datagram_loss_ppm[d]) || r.chance_ppm(gray_loss_[from]) || r.chance_ppm(gray_loss_[to])) { ++stats_.dropped; return; }
		const int copies = r.chance_ppm(options_.datagram_duplicate_ppm[d]) ? 2 : 1;
		stats_.duplicated += std::uint64_t(copies - 1);
		for (int c = 0; c < copies; ++c) {
			schedule_datagram(engine_.now() + latency(from, to), from, to, c + 1 == copies ? std::move(payload) : payload);
		}
	}

	// Resets the a–b connection: drops every frame in flight both ways, on every lane, and
	// tells both ends.
	void reset(node_id a, node_id b) { check(a, b); reset_connection(a, b); }
	// Partitions: nodes of different colors cannot talk; a cut stops one direction only.
	void color(node_id n, std::uint8_t c) { check(n, n); colors_[n] = c; }
	void cut(node_id from, node_id to) { check(from, to); cuts_.insert(key(from, to)); }
	void uncut(node_id from, node_id to) { cuts_.erase(key(from, to)); }
	void heal() { std::fill(colors_.begin(), colors_.end(), 0); cuts_.clear(); }
	// The simulator's view, for oracles and the fault planner; a sender never sees it.
	bool reachable(node_id from, node_id to) const { return colors_[from] == colors_[to] && !cuts_.contains(key(from, to)); }
	// Holds every frame and datagram on from → to, in flight or later, until `until`, then
	// releases them in order.
	void clog(node_id from, node_id to, time_ns until) {
		check(from, to);
		if (until <= engine_.now()) { return; }
		auto& l = links_[key(from, to)];
		if (until <= l.clogged_until) { return; }
		l.clogged_until = until;
		engine_.at(until, [this, from, to] { if (auto found = links_.find(key(from, to)); found != links_.end()) { evict_if_idle(found); } }, trace_clog, from);
	}
	// A gray node: its datagrams are lost at loss_ppm and its latencies multiplied by factor.
	void gray(node_id n, std::uint32_t loss_ppm, std::uint32_t factor) { check(n, n); gray_loss_[n] = loss_ppm; gray_factor_[n] = factor ? factor : 1; }

private:
	struct frame { time_ns arrival; std::string payload; };
	// A lane's frames in flight, oldest first.
	struct fifo {
		std::vector<frame> items;
		std::size_t head = 0;
		bool empty() const noexcept { return head == items.size(); }
		std::size_t size() const noexcept { return items.size() - head; }
		frame& front() noexcept { return items[head]; }
		void push(frame f) { items.push_back(std::move(f)); }
		void pop() {
			if (++head == items.size()) { items.clear(); head = 0; }
			else if (head > 64 && 2 * head > items.size()) { items.erase(items.begin(), items.begin() + std::ptrdiff_t(head)); head = 0; }
		}
		void clear() noexcept { items.clear(); head = 0; }
	};
	struct link {
		time_ns busy_until = 0, clogged_until = 0;
		std::array<time_ns, network_options::lanes> last_arrival{};
		std::array<fifo, network_options::lanes> queue;
		std::array<engine::id, network_options::lanes> timer{};
		std::array<bool, network_options::lanes> armed{};
		bool idle() const noexcept { return std::all_of(queue.begin(), queue.end(), [](const fifo& q) { return q.empty(); }); }
	};
	using links_map = std::unordered_map<std::uint64_t, link>;

	static std::uint64_t key(node_id from, node_id to) noexcept { return (std::uint64_t(from) << 32) | to; }
	void check(node_id a, node_id b) const { if (a >= topology_.size() || b >= topology_.size()) { throw std::out_of_range("node"); } }
	time_ns serialization(std::size_t bytes) const noexcept { return time_ns((unsigned __int128)bytes * 8 * 1'000'000'000 / options_.bandwidth_bits); }
	time_ns latency(node_id from, node_id to) {
		const auto base = options_.latency[std::size_t(distance_between(topology_, from, to))].draw(random_[from]);
		return base * std::max(gray_factor_[from], gray_factor_[to]);
	}
	// Schedules the lane's head frame at its arrival, or when the clog clears.
	void arm(node_id from, node_id to, std::uint8_t lane, link& l) {
		const time_ns when = std::max({l.queue[lane].front().arrival, l.clogged_until, engine_.now()});
		l.timer[lane] = engine_.at(when, [this, from, to, lane] { arrive(from, to, lane); }, trace_frame, to);
		l.armed[lane] = true;
	}
	void arrive(node_id from, node_id to, std::uint8_t lane) {
		const auto found = links_.find(key(from, to));
		auto& l = found->second;
		l.armed[lane] = false;
		if (l.clogged_until > engine_.now()) { arm(from, to, lane, l); return; }  // clogged after it was armed
		if (!reachable(from, to)) { reset_connection(from, to); return; }  // lost in flight
		auto f = std::move(l.queue[lane].front());
		l.queue[lane].pop();
		if (!l.queue[lane].empty()) { arm(from, to, lane, l); } else { evict_if_idle(found); }
		deliver(from, to, lane, std::move(f.payload));
	}
	void reset_connection(node_id a, node_id b) {
		for (const auto k : {key(a, b), key(b, a)}) {
			const auto found = links_.find(k);
			if (found == links_.end()) { continue; }
			auto& l = found->second;
			for (std::uint8_t lane = 0; lane < network_options::lanes; ++lane) {
				if (l.armed[lane]) { engine_.cancel(l.timer[lane]); l.armed[lane] = false; }
				stats_.dropped += l.queue[lane].size();
				l.queue[lane].clear();
			}
			l.last_arrival = {}; l.busy_until = 0;
			evict_if_idle(found);
			if (a == b) { break; }
		}
		++stats_.resets;
		for (const auto& end : {std::pair{a, b}, std::pair{b, a}}) {
			const node_id node = end.first, peer = end.second;
			engine_.at(engine_.now(), [this, node, peer] {
				engine_.trace(mix(node) ^ mix(std::uint64_t(peer) << 1 | 1));
				if (reset_handler_) { reset_handler_(node, peer); }
			}, trace_reset, node);
			if (a == b) { break; }
		}
	}
	void evict_if_idle(links_map::iterator found) {
		if (found->second.idle() && found->second.clogged_until <= engine_.now()) { links_.erase(found); }
	}
	void schedule_datagram(time_ns when, node_id from, node_id to, std::string payload) {
		engine_.at(when, [this, from, to, p = std::move(payload)]() mutable {
			if (auto found = links_.find(key(from, to)); found != links_.end() && found->second.clogged_until > engine_.now()) {
				schedule_datagram(found->second.clogged_until, from, to, std::move(p));
				return;
			}
			if (!reachable(from, to)) { ++stats_.dropped; return; }
			deliver(from, to, datagram, std::move(p));
		}, trace_datagram, to);
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
	reset_handler reset_handler_;
	network_stats stats_;
	std::vector<stream> random_;
	std::vector<std::uint8_t> colors_;
	std::vector<std::uint32_t> gray_loss_, gray_factor_;
	links_map links_;
	std::unordered_set<std::uint64_t> cuts_;
};

}  // namespace kronuz::simulation
