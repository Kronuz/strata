// sim_run [--plant lost-write] [--expect VERDICT] [--history FILE] [--keys N] [--ms N] SEED...:
// the simulator end to end. Per seed, one register server and seven clients on the simulated
// network, under partitions, clogs, connection resets and server outages; clients record a D9
// history (an unanswered operation is unknown and retires its client) and the checker judges
// it. The server is strata's register held in one place, so every seed is linearizable unless a
// fault is planted: lost-write acknowledges one put it never stores. Prints one line per seed
// and exits 0 when every verdict is VERDICT (default linearizable), 1 otherwise, 3 on bad usage.
#include "simulation/history.hh"
#include "simulation/network.hh"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace kronuz::simulation;
using check::kind;
using check::outcome;

namespace {

constexpr node_id server = 0;
constexpr node_id clients = 7;  // nodes 1 to 7
constexpr time_ns timeout = 3'000'000;
// The run's size: keys, and simulated ms of workload; lost-write drops the first put after a quarter.
struct shape { std::uint64_t keys = 8; time_ns workload = 200'000'000; };
constexpr std::uint64_t trace_client = 16, trace_fault = 17;

// A request "id op key value expected" and its reply "id outcome value version"; expected
// and value are "-" when absent.
std::string field(std::optional<std::uint64_t> v) { return v ? std::to_string(*v) : "-"; }
std::vector<std::optional<std::uint64_t>> fields(std::string_view s) {
	std::vector<std::optional<std::uint64_t>> out;
	while (!s.empty()) {
		const auto space = s.find(' ');
		const auto word = s.substr(0, space);
		std::uint64_t v = 0;
		if (word == "-") { out.emplace_back(); }
		else if (std::from_chars(word.data(), word.data() + word.size(), v).ec == std::errc()) { out.emplace_back(v); }
		else { throw std::runtime_error("malformed frame"); }
		s = space == std::string_view::npos ? std::string_view() : s.substr(space + 1);
	}
	return out;
}

struct run {
	bool plant = false;
	shape size;
	engine e;
	network net;
	stream workload, faults;
	check::history h;
	// The server: strata's register per key, and when it is down (it drops what arrives).
	std::map<std::uint64_t, check::state> store;
	time_ns down_until = 0;
	bool planted = false;
	// Each client node runs one client at a time; an unknown outcome retires it.
	struct client {
		std::uint64_t id = 0, incarnation = 0, request = 0;
		bool waiting = false;
		check::operation op;
		engine::id timer;
	};
	std::vector<client> cs;
	std::uint64_t next_value = 1;

	run(std::uint64_t seed, bool plant_lost_write, shape sz)
		: plant(plant_lost_write), size(sz), e(seed),
		  net(e, topology{1, 1, 2, 4}, network_options{},
		      [this](node_id from, node_id to, std::uint8_t, std::string p) { receive(from, to, p); },
		      [this](node_id node, node_id peer) { if (node != server && peer == server) { lose(node); } }),
		  workload(e.random().child("workload")), faults(e.random().child("faults")), cs(clients + 1) {}

	void start(node_id n) {
		auto& c = cs[n];
		c.id = std::uint64_t(n) * 1'000'000 + ++c.incarnation;
		think(n);
	}
	void think(node_id n) {
		if (e.now() >= size.workload) { return; }
		e.after(workload.below(100'000), [this, n] { issue(n); }, trace_client, n);
	}
	void issue(node_id n) {
		auto& c = cs[n];
		auto& o = c.op;
		o = {};
		o.client = c.id; o.key = workload.below(size.keys); o.invoked = e.now();
		const auto roll = workload.below(100);
		if (roll < 40) { o.op = kind::put; o.value = next_value++; }
		else if (roll < 55) { o.op = kind::erase; }
		else { o.op = kind::get; }
		if (o.op != kind::get && workload.chance_ppm(250'000)) { o.expected = workload.below(4); }
		c.waiting = true; ++c.request;
		c.timer = e.after(timeout, [this, n] { lose(n); }, trace_client, n);
		net.send(n, server, 0, std::to_string(c.request) + ' ' + std::to_string(int(o.op)) + ' ' + std::to_string(o.key) + ' ' + field(o.op == kind::put ? std::optional(o.value) : std::nullopt) + ' ' + field(o.expected));
	}
	// No answer (a timeout or a reset): the operation is unknown, its client retires.
	void lose(node_id n) {
		auto& c = cs[n];
		if (!c.waiting) { return; }
		c.waiting = false;
		e.cancel(c.timer);
		auto o = c.op;
		o.result = outcome::unknown; o.returned = 0; o.version = 0;
		if (o.op == kind::get) { o.value = 0; }
		h.ops.push_back(o);
		e.after(500'000, [this, n] { start(n); }, trace_client, n);
	}
	void receive(node_id from, node_id to, std::string_view p) {
		const auto f = fields(p);
		if (to == server) { serve(from, f); return; }
		auto& c = cs[to];
		if (!c.waiting || *f[0] != c.request) { return; }  // a reply to an operation already given up
		c.waiting = false;
		e.cancel(c.timer);
		auto o = c.op;
		o.returned = e.now(); o.result = outcome(*f[1]); o.version = *f[3];
		if (o.op == kind::get) { o.value = f[2].value_or(0); }
		h.ops.push_back(o);
		e.trace(o.value ^ (o.version << 32) ^ std::uint64_t(o.result));
		think(to);
	}
	void serve(node_id from, const std::vector<std::optional<std::uint64_t>>& f) {
		if (e.now() < down_until) { return; }
		const auto op = kind(*f[1]);
		const auto key = *f[2];
		auto& st = store[key];
		outcome result = outcome::ok;
		std::optional<std::uint64_t> value;
		std::uint64_t version = 0;
		if (op == kind::get) {
			if (st.present) { value = st.value; version = st.version; } else { result = outcome::missing; }
		} else if (f[4] && *f[4] != st.version) {
			result = outcome::conflict; version = st.version;
		} else if (op == kind::put) {
			version = st.version + 1;
			if (plant && !planted && e.now() >= size.workload / 4) { planted = true; }
			else { st = {version, *f[3], true}; }
		} else {
			result = st.present ? outcome::ok : outcome::missing;
			st = {};
		}
		net.send(server, from, 0, std::to_string(*f[0]) + ' ' + std::to_string(int(result)) + ' ' + field(value) + ' ' + std::to_string(version));
	}
	// One fault every 2 to 20 ms: a client partitioned from the server, a clogged link, a
	// reset connection, or a server outage, each for 0.5 to 10 ms.
	void fault() {
		if (e.now() >= size.workload) { return; }
		const auto n = node_id(1 + faults.below(clients));
		const auto span = faults.between(500'000, 10'000'000);
		switch (faults.below(4)) {
		case 0: net.color(n, 1); e.after(span, [this, n] { net.color(n, 0); }, trace_fault, n); break;
		case 1: faults.below(2) ? net.clog(n, server, e.now() + span) : net.clog(server, n, e.now() + span); break;
		case 2: net.reset(server, n); break;
		default: down_until = std::max(down_until, e.now() + span); break;
		}
		e.after(faults.between(2'000'000, 20'000'000), [this] { fault(); }, trace_fault, server);
	}
	check::report go() {
		for (node_id n = 1; n <= clients; ++n) { start(n); }
		fault();
		e.drain();
		return check::check(h.ops);
	}
};

}  // namespace

int main(int argc, char** argv) {
	bool plant = false;
	shape size;
	const char* history = nullptr;
	std::string expect = "linearizable";
	std::vector<std::uint64_t> seeds;
	for (int i = 1; i < argc; ++i) {
		const std::string_view a = argv[i];
		if (a == "--plant" && i + 1 < argc && std::strcmp(argv[i + 1], "lost-write") == 0) { plant = true; ++i; }
		else if (a == "--expect" && i + 1 < argc) { expect = argv[++i]; }
		else if (a == "--history" && i + 1 < argc) { history = argv[++i]; }
		else if (a == "--keys" && i + 1 < argc) { size.keys = std::strtoull(argv[++i], nullptr, 10); }
		else if (a == "--ms" && i + 1 < argc) { size.workload = std::strtoull(argv[++i], nullptr, 10) * 1'000'000; }
		else {
			std::uint64_t s = 0;
			if (std::from_chars(a.data(), a.data() + a.size(), s).ec != std::errc()) { seeds.clear(); break; }
			seeds.push_back(s);
		}
	}
	if (seeds.empty() || (history && seeds.size() != 1) || size.keys == 0 || size.workload == 0) {
		std::fprintf(stderr, "usage: sim_run [--plant lost-write] [--expect linearizable|violation|inconclusive] [--history FILE] [--keys N] [--ms N] SEED...\n");
		return 3;
	}
	static constexpr const char* names[] = {"linearizable", "violation", "inconclusive"};
	int status = 0;
	for (const auto seed : seeds) {
		run r(seed, plant, size);
		const auto report = r.go();
		std::size_t unknown = 0;
		for (const auto& o : r.h.ops) { unknown += o.result == outcome::unknown; }
		const auto& s = r.net.stats();
		std::printf("seed %llu: %s; %zu operations (%zu unknown), %llu events, trace %016llx, %llu frames sent, %llu dropped, %llu resets, %llu search nodes",
			(unsigned long long)seed, names[std::size_t(report.result)], r.h.ops.size(), unknown, (unsigned long long)r.e.executed(),
			(unsigned long long)r.e.trace_hash(), (unsigned long long)s.sent, (unsigned long long)s.dropped, (unsigned long long)s.resets,
			(unsigned long long)report.nodes);
		if (report.result != check::verdict::linearizable) { std::printf("; key %llu: %s", (unsigned long long)report.key, report.detail.c_str()); }
		std::printf("\n");
		if (plant && !r.planted) { std::printf("seed %llu: the fault was never planted\n", (unsigned long long)seed); status = 1; }
		if (expect != names[std::size_t(report.result)]) { status = 1; }
		if (history) {
			std::ofstream out(history, std::ios::binary);
			out << check::write_history(r.h);
			if (!out) { std::fprintf(stderr, "sim_run: cannot write %s\n", history); return 3; }
		}
	}
	return status;
}
