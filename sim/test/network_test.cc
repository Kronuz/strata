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
// one connection and lane arrive in order, while lanes and connections reorder; datagrams are
// lost, duplicated, and reordered; color partitions, one-way cuts, resets, clogs, gray nodes,
// and bandwidth pacing; and a pinned delivery-trace hash for a mixed workload (gate row T1).
#include "simulation/network.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;

namespace {

struct delivery { node_id from, to; std::uint8_t lane; std::string payload; time_ns at; };
const topology small{2, 2, 2, 4};  // 32 nodes: 4 per rack, 2 racks per zone, 2 zones per region

struct harness {
	engine e;
	std::vector<delivery> log;
	network net;
	explicit harness(std::uint64_t seed, network_options o = {}, topology t = small)
		: e(seed), net(e, t, std::move(o), [this](node_id f, node_id to, std::uint8_t l, std::string p) { log.push_back({f, to, l, std::move(p), e.now()}); }) {}
};

std::uint64_t mixed(std::uint64_t seed) {
	network_options o; o.datagram_loss_ppm.fill(20'000); o.datagram_duplicate_ppm.fill(10'000);
	harness h(seed, o);
	auto s = h.e.random().child("workload");
	for (int i = 0; i < 20'000; ++i) {
		const node_id a = node_id(s.below(32)), b = node_id(s.below(32));
		const auto pick = s.below(100);
		if (pick < 60) { h.net.send(a, b, std::uint8_t(s.below(2)), std::to_string(i)); }
		else if (pick < 95) { h.net.send_datagram(a, b, std::to_string(i)); }
		else if (pick < 97) { h.net.color(a, std::uint8_t(s.below(2))); }
		else if (pick < 98) { h.net.reset(a, b); }
		else if (pick < 99) { h.net.clog(a, b, h.e.now() + s.below(1'000'000)); }
		else { h.net.heal(); }
		h.e.run_until(h.e.now() + s.below(20'000));
	}
	h.e.drain();
	return h.e.trace_hash();
}

}  // namespace

int main() {
	// Distance classes.
	{
		const topology t;
		CHECK(t.size() == 100'000);
		CHECK(distance_between(t, 0, 0) == distance::local && distance_between(t, 0, 39) == distance::rack);
		CHECK(distance_between(t, 0, 40) == distance::zone && distance_between(t, 0, 9'999) == distance::zone);
		CHECK(distance_between(t, 0, 10'000) == distance::region && distance_between(t, 0, 20'000) == distance::remote);
		CHECK(t.region(99'999) == 4 && t.zone(99'999) == 9 && t.rack(99'999) == 2'499);
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
		CHECK(crossed);
	}
	// Datagrams: loss and duplication near their rates, and reordering.
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
	}
	// Partitions by color drop frames sent across and in flight; one-way cuts; heal.
	{
		harness h(3);
		h.net.send(0, 1, 0, "before");
		h.net.color(1, 1);
		h.e.drain();
		CHECK(h.log.empty() && !h.net.reachable(0, 1) && !h.net.send(0, 1, 0, "during"));
		h.net.heal();
		h.net.cut(0, 1);
		CHECK(!h.net.send(0, 1, 0, "cut") && h.net.send(1, 0, 0, "back"));
		h.e.drain();
		CHECK(h.log.size() == 1 && h.log[0].payload == "back");
		h.net.uncut(0, 1);
		CHECK(h.net.send(0, 1, 0, "healed"));
		h.e.drain();
		CHECK(h.log.size() == 2 && h.log[1].payload == "healed");
	}
	// A reset drops what is in flight both ways; later frames go through.
	{
		harness h(4);
		h.net.send(0, 5, 0, "a"); h.net.send(5, 0, 1, "b");
		h.net.reset(0, 5);
		h.net.send(0, 5, 0, "c");
		h.e.drain();
		CHECK(h.log.size() == 1 && h.log[0].payload == "c");
	}
	// Bandwidth: ten 1 MB frames over an 8 Gb/s link take 10 ms to serialize.
	{
		network_options o; o.bandwidth_bits = 8'000'000'000; o.latency.fill(latency_around(0));
		harness h(5, o);
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
	// A gray node multiplies its latencies.
	{
		network_options o; o.latency.fill(latency_around(1'000));
		harness h(7, o);
		h.net.gray(3, 0, 10);
		h.net.send(0, 3, 0, "slow"); h.net.send(0, 2, 0, "fast");
		h.e.drain();
		CHECK(h.log.size() == 2 && h.log[0].payload == "fast" && h.log[1].at >= 5'000);
	}
	// Determinism: a mixed workload's trace hash, pinned for seed 1.
	{
		const auto x = mixed(1);
		CHECK(x == mixed(1) && x != mixed(2));
		constexpr std::uint64_t pinned = 0x3b66823c41cfce59ULL;
		if (x != pinned) { std::fprintf(stderr, "seed 1 network trace hash 0x%016llx\n", (unsigned long long)x); }
		CHECK(x == pinned);
	}
	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
