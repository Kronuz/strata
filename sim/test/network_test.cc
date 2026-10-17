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

// The simulated network (simulator design D4): distance classes from the topology; frames on
// one lane of a connection arrive in order or not at all, while lanes and connections
// reorder; a frame lost in flight resets its connection, so no lane shows a hole across a
// heal, and both ends are told; sends report nothing; clogs hold frames already in flight;
// idle links are evicted; datagrams are lost, duplicated, and reordered; color partitions,
// one-way cuts, resets, gray nodes, and bandwidth pacing; and a randomized workload checked
// for in-order-or-not-at-all, with a pinned delivery-trace hash (gate row T1).
#include "simulation/network.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;

// SIM-R7: a sender sees nothing of the network's state.
static_assert(std::is_void_v<decltype(std::declval<network&>().send(0, 1, 0, std::string()))>);
static_assert(std::is_void_v<decltype(std::declval<network&>().send_datagram(0, 1, std::string()))>);

namespace {

struct delivery { node_id from, to; std::uint8_t lane; std::string payload; time_ns at; };
struct notice { node_id node, peer; time_ns at; };
const topology small{2, 2, 2, 4};  // 32 nodes: 4 per rack, 2 racks per zone, 2 zones per region

struct harness {
	engine e;
	std::vector<delivery> log;
	std::vector<notice> resets;
	std::map<std::pair<node_id, node_id>, std::uint64_t> told;  // resets each node heard of, by peer
	network net;
	std::uint64_t heard(node_id n, node_id p) const { const auto f = told.find({n, p}); return f == told.end() ? 0 : f->second; }
	explicit harness(std::uint64_t seed, network_options o = {}, topology t = small)
		: e(seed), net(e, t, std::move(o),
			[this](node_id f, node_id to, std::uint8_t l, std::string p) { log.push_back({f, to, l, std::move(p), e.now()}); },
			[this](node_id n, node_id p) { resets.push_back({n, p, e.now()}); ++told[{n, p}]; }) {}
};

network_options zero_latency(std::uint64_t bandwidth_bits) {
	network_options o; o.bandwidth_bits = bandwidth_bits; o.latency.fill(latency_around(0));
	return o;
}

// Sends, datagrams, partitions, resets, clogs, and heals at random. Every frame carries the
// number of resets its sender had heard of for that peer; on each lane, frames must arrive
// in send order, and none may arrive after an earlier frame of the same connection was lost.
struct mixed_result { std::uint64_t hash, frames, delivered, resets, disorder, holes; };
mixed_result mixed(std::uint64_t seed) {
	network_options o; o.datagram_loss_ppm.fill(20'000); o.datagram_duplicate_ppm.fill(10'000);
	harness h(seed, o);
	struct sent { int id; std::uint64_t stamp; };
	std::map<std::uint64_t, std::vector<sent>> lanes;  // (from, to, lane): frames in send order
	auto lane_key = [](node_id a, node_id b, std::uint8_t l) { return (std::uint64_t(a) << 40) | (std::uint64_t(b) << 8) | l; };
	auto s = h.e.random().child("workload");
	mixed_result r{};
	for (int i = 0; i < 20'000; ++i) {
		const node_id a = node_id(s.below(32)), b = node_id(s.below(32));
		const auto pick = s.below(100);
		if (pick < 60) {
			const auto lane = std::uint8_t(s.below(2));
			lanes[lane_key(a, b, lane)].push_back({i, h.told[{a, b}]});
			h.net.send(a, b, lane, std::to_string(i));
			++r.frames;
		}
		else if (pick < 95) { h.net.send_datagram(a, b, std::to_string(i)); }
		else if (pick < 97) { h.net.color(a, std::uint8_t(s.below(2))); }
		else if (pick < 98) { h.net.reset(a, b); }
		else if (pick < 99) { h.net.clog(a, b, h.e.now() + s.below(1'000'000)); }
		else { h.net.heal(); }
		h.e.run_until(h.e.now() + s.below(20'000));  // runs reset notices before the next send
	}
	h.e.drain();
	std::map<std::uint64_t, std::pair<std::map<int, std::size_t>, std::vector<bool>>> seen;
	for (auto& [k, frames] : lanes) {
		auto& [index, got] = seen[k];
		for (std::size_t j = 0; j < frames.size(); ++j) { index[frames[j].id] = j; }
		got.assign(frames.size(), false);
	}
	std::map<std::uint64_t, long> previous;
	for (const auto& d : h.log) {
		if (d.lane == network::datagram) { continue; }
		const auto k = lane_key(d.from, d.to, d.lane);
		auto& [index, got] = seen[k];
		const auto& frames = lanes[k];
		const auto j = index.at(std::stoi(d.payload));
		const auto p = previous.try_emplace(k, -1).first;
		if (long(j) <= p->second) { ++r.disorder; }
		p->second = long(j);
		got[j] = true;
		++r.delivered;
		// Stamps never decrease along a lane, so the frames of j's connection precede it.
		for (std::size_t m = j; m-- > 0 && frames[m].stamp == frames[j].stamp;) { r.holes += !got[m]; }
	}
	r.hash = h.e.trace_hash();
	r.resets = h.net.stats().resets;
	CHECK(h.net.links() == 0);
	return r;
}

}  // namespace

int main() {
	// Distance classes over the default 5 × 4 × 125 × 40 topology.
	{
		const topology t;
		CHECK(t.size() == 100'000);
		CHECK(distance_between(t, 0, 0) == distance::local && distance_between(t, 0, 39) == distance::rack);
		CHECK(distance_between(t, 0, 40) == distance::zone && distance_between(t, 0, 4'999) == distance::zone);
		CHECK(distance_between(t, 0, 5'000) == distance::region && distance_between(t, 0, 19'999) == distance::region);
		CHECK(distance_between(t, 0, 20'000) == distance::remote);
		CHECK(t.region(99'999) == 4 && t.zone(99'999) == 19 && t.rack(99'999) == 2'499);
	}
	// FIFO per connection and lane; lanes and other connections overtake.
	{
		harness h(1);
		for (int i = 0; i < 1'000; ++i) { h.net.send(0, 20, std::uint8_t(i % 2), std::to_string(i)); h.e.run_until(h.e.now() + 1'000); }
		h.e.drain();
		CHECK(h.log.size() == 1'000);
		std::map<int, int> last{{0, -1}, {1, -1}}; bool crossed = false; int previous = -1;
		for (const auto& d : h.log) {
			const int i = std::stoi(d.payload);
			CHECK(i > last[d.lane]); last[d.lane] = i;
			crossed |= i < previous; previous = i;
		}
		CHECK(crossed && h.resets.empty() && h.net.links() == 0);
	}
	// Datagrams: loss and duplication near their rates, and reordering; a partition drops them
	// at arrival, with no reset.
	{
		network_options o; o.datagram_loss_ppm.fill(100'000); o.datagram_duplicate_ppm.fill(50'000);
		harness h(2, o);
		for (int i = 0; i < 10'000; ++i) { h.net.send_datagram(1, 30, std::to_string(i)); }
		h.e.drain();
		const auto& st = h.net.stats();
		CHECK(st.dropped > 900 && st.dropped < 1'100 && st.duplicated > 350 && st.duplicated < 600);
		CHECK(h.log.size() == 10'000 - st.dropped + st.duplicated);
		bool reordered = false;
		for (std::size_t i = 1; i < h.log.size(); ++i) { reordered |= std::stoi(h.log[i].payload) < std::stoi(h.log[i - 1].payload); }
		CHECK(reordered);
		const auto before = h.log.size();
		h.net.color(30, 1);
		for (int i = 0; i < 100; ++i) { h.net.send_datagram(1, 30, "cut"); }
		h.e.drain();
		CHECK(h.log.size() == before && h.resets.empty());
	}
	// Partitions: a frame lost in flight resets its connection and both ends hear of it; a
	// frame sent into a partition is accepted and lost the same way; one-way cuts; heal.
	{
		harness h(3);
		h.net.send(0, 1, 0, "before");
		h.net.color(1, 1);
		h.e.drain();
		CHECK(h.log.empty() && !h.net.reachable(0, 1) && h.net.stats().resets == 1);
		CHECK(h.resets.size() == 2 && h.heard(0, 1) == 1 && h.heard(1, 0) == 1);
		h.net.send(0, 1, 0, "during");
		h.e.drain();
		CHECK(h.log.empty() && h.resets.size() == 4);
		h.net.heal();
		h.net.cut(0, 1);
		h.net.send(1, 0, 0, "back");
		h.e.drain();
		CHECK(h.log.size() == 1 && h.log[0].payload == "back");
		h.net.send(0, 1, 0, "cut");
		h.e.drain();
		CHECK(h.log.size() == 1 && h.resets.size() == 6);
		h.net.uncut(0, 1);
		h.net.send(0, 1, 0, "healed");
		h.e.drain();
		CHECK(h.log.size() == 2 && h.log[1].payload == "healed" && h.net.links() == 0);
	}
	// No hole across a heal: a and b are in flight on one lane; a arrives during a partition
	// and is lost; b would arrive after the heal, but the reset dropped it.
	{
		harness h(8, zero_latency(8'000'000'000));
		h.net.send(0, 1, 0, std::string(1'000'000, 'a'));  // arrives at 1 ms
		h.net.send(0, 1, 0, std::string(1'000'000, 'b'));  // at 2 ms
		h.net.color(1, 1);
		h.e.run_until(1'500'000);
		h.net.heal();
		h.e.drain();
		CHECK(h.log.empty() && h.net.stats().dropped == 2);
		CHECK(h.resets.size() == 2 && h.resets[0].at == 1'000'000 && h.resets[1].at == 1'000'000);
		h.net.send(0, 1, 0, "c");
		h.e.drain();
		CHECK(h.log.size() == 1 && h.log[0].payload == "c" && h.net.links() == 0);
	}
	// A reset drops what is in flight both ways and tells both ends; later frames go through.
	{
		harness h(4);
		h.net.send(0, 5, 0, "a"); h.net.send(5, 0, 1, "b");
		h.net.reset(0, 5);
		h.net.send(0, 5, 0, "c");
		h.e.drain();
		CHECK(h.log.size() == 1 && h.log[0].payload == "c");
		CHECK(h.resets.size() == 2 && h.heard(0, 5) == 1 && h.heard(5, 0) == 1);
	}
	// Bandwidth: ten 1 MB frames over an 8 Gb/s link take 10 ms to serialize.
	{
		harness h(5, zero_latency(8'000'000'000));
		for (int i = 0; i < 10; ++i) { h.net.send(0, 1, 1, std::string(1'000'000, 'x')); }
		h.e.drain();
		CHECK(h.log.size() == 10 && h.log.front().at == 1'000'000 && h.log.back().at == 10'000'000);
	}
	// A clog holds deliveries until it clears, in order.
	{
		harness h(6);
		h.net.clog(0, 9, 5'000'000);
		for (int i = 0; i < 5; ++i) { h.net.send(0, 9, 0, std::to_string(i)); }
		h.net.send(9, 0, 0, "free");
		h.e.drain();
		CHECK(h.log.size() == 6 && h.log[0].payload == "free");
		for (int i = 0; i < 5; ++i) { CHECK(h.log[std::size_t(i) + 1].payload == std::to_string(i) && h.log[std::size_t(i) + 1].at >= 5'000'000); }
	}
	// A clog holds frames already in flight too, and releases them in lane order.
	{
		harness h(9, zero_latency(8'000'000'000));
		h.net.send(0, 1, 0, std::string(1'000'000, 'a'));  // would arrive at 1 ms
		h.net.send(0, 1, 1, "x");
		h.net.clog(0, 1, 5'000'000);
		h.net.send(0, 1, 0, "b");
		h.e.drain();
		CHECK(h.log.size() == 3 && h.net.links() == 0);
		std::size_t a = 9, b = 9;
		for (std::size_t i = 0; i < h.log.size(); ++i) {
			CHECK(h.log[i].at >= 5'000'000);
			if (h.log[i].payload.size() == 1'000'000) { a = i; }
			if (h.log[i].payload == "b") { b = i; }
		}
		CHECK(a < b && b < 3);
	}
	// Idle links are evicted: after a clog clears, and after their frames arrive.
	{
		harness h(10);
		h.net.clog(2, 3, 1'000'000);
		CHECK(h.net.links() == 1);
		h.e.run_until(2'000'000);
		CHECK(h.net.links() == 0);
		for (int i = 0; i < 100; ++i) { h.net.send(node_id(i % 32), node_id((i * 7) % 32), std::uint8_t(i % 4), "x"); }
		CHECK(h.net.links() > 0);
		h.e.drain();
		CHECK(h.net.links() == 0 && h.log.size() == 100);
	}
	// A gray node multiplies its latencies.
	{
		network_options o; o.latency.fill(latency_around(1'000));
		harness h(7, o);
		h.net.gray(3, 0, 10);
		h.net.send(0, 3, 0, "slow"); h.net.send(0, 2, 0, "fast");
		h.e.drain();
		CHECK(h.log.size() == 2 && h.log[0].payload == "fast" && h.log[1].at >= 5'000);
	}
	// The randomized workload: in order or not at all on every lane, and deterministic; the
	// trace hash is pinned for seed 1.
	{
		const auto x = mixed(1), y = mixed(2);
		CHECK(x.disorder == 0 && x.holes == 0 && y.disorder == 0 && y.holes == 0);
		CHECK(x.resets > 100 && x.delivered > 1'000 && x.delivered < x.frames);
		CHECK(x.hash == mixed(1).hash && x.hash != y.hash);
		std::printf("mixed seed 1: %llu frames, %llu delivered, %llu resets\n", (unsigned long long)x.frames, (unsigned long long)x.delivered, (unsigned long long)x.resets);
		constexpr std::uint64_t pinned = 0;
		if (x.hash != pinned) { std::fprintf(stderr, "seed 1 network trace hash 0x%016llx\n", (unsigned long long)x.hash); }
		CHECK(x.hash == pinned);
	}
	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
