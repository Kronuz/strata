// Probe F (rocksdb-parity D6, review F1): the cost of handing records from L loops to one log's
// owner, per turn. Each of L loop threads owns G groups, each with one ack=sync record in flight
// (a closed loop); one owner thread writes turns: it collects the open records, copies them into
// the turn, holds a barrier of D µs (a spin, standing in for a fast device flush), then marks them
// durable and wakes each loop that had one. Modes:
//   local  the owner is the only loop: no handoff (the floor)
//   pull   the owner takes each group's open record under the group's mutex (strata's log_state)
//   push   a loop seals each record into its own SPSC ring as it opens; the owner drains the rings
//   ask    the owner asks every loop with an open record to seal it, and waits for every answer
// After a turn the owner waits for the cohort (D5): until as many records opened as it released,
// or `cohort` µs. Reports commits/s, records per turn, and per turn the µs spent collecting and
// fanning out, the µs from the owner's wake to the loop running (wake latency), and the process's
// CPU per commit (the spin's cost included).
// Every wait spins up to SPIN_US before it sleeps, as RocksDB's write group yields first.
// Usage: handoff MODE L G D_US COHORT_US SPIN_US SECONDS

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/resource.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
std::uint64_t now_ns() { return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count()); }
void spin_until(std::uint64_t t) { while (now_ns() < t) {} }
// Waits for `a` to leave `old`: spins up to `spin` ns, then sleeps in the kernel.
void await_change(std::atomic<std::uint64_t>& a, std::uint64_t old, std::uint64_t spin) {
	const auto until = now_ns() + spin;
	while (a.load(std::memory_order_acquire) == old && now_ns() < until) {}
	a.wait(old, std::memory_order_acquire);
}

enum class mode { local, pull, push, ask };

struct alignas(64) group {
	std::mutex mu;                       // pull: the record's owner and the turn's
	bool open = false;                   // pull: an open record waits for a turn
	std::array<char, 100> body{};        // the record's bytes
	std::atomic<std::uint64_t> done{0};  // the last durable sequence
	std::uint64_t seq = 0;               // the loop's: the sequence in flight
	std::uint64_t pushed = 0;            // ask: the last sequence the loop sealed into its ring
	bool inflight = false;
	std::uint64_t start = 0;
	unsigned loop = 0;
};

// One producer (the loop), one consumer (the owner).
struct ring {
	static constexpr std::size_t size = 1024;
	std::array<group*, size> slots{};
	alignas(64) std::atomic<std::size_t> head{0};
	alignas(64) std::atomic<std::size_t> tail{0};
	void push(group* g) {
		const auto h = head.load(std::memory_order_relaxed);
		while (h - tail.load(std::memory_order_acquire) == size) {}
		slots[h % size] = g;
		head.store(h + 1, std::memory_order_release);
	}
	template <class F> std::size_t drain(F f) {
		const auto t = tail.load(std::memory_order_relaxed), h = head.load(std::memory_order_acquire);
		for (auto i = t; i < h; ++i) { f(slots[i % size]); }
		tail.store(h, std::memory_order_release);
		return h - t;
	}
};

struct alignas(64) loop_state {
	std::vector<group*> groups;
	alignas(64) std::atomic<std::uint64_t> bell{0};      // the owner rings it: completion or an ask
	alignas(64) std::atomic<std::uint64_t> asked{0};     // ask: the turn that asked
	std::atomic<std::uint64_t> answered{0};
	std::atomic<std::uint64_t> issued{0};                // ask: records this loop opened
	ring sealed;
	std::uint64_t ops = 0, latency = 0, wakes = 0, wake_ns = 0, wake_max = 0;
};

struct shared {
	mode m;
	unsigned loops, per_loop;
	std::uint64_t barrier_ns, cohort_ns, spin_ns;
	std::atomic<bool> stop{false};
	alignas(64) std::atomic<std::uint64_t> opened{0};      // records opened, all loops
	alignas(64) std::atomic<std::uint64_t> owner_bell{0};
	alignas(64) std::atomic<std::uint64_t> answers{0};     // ask: loops that answered
	alignas(64) std::atomic<std::uint64_t> rung_at{0};     // when the owner last rang the loops
	std::vector<group> groups;
	std::vector<loop_state> ls;
};

void ring_owner(shared& s) {
	s.owner_bell.fetch_add(1, std::memory_order_release);
	s.owner_bell.notify_one();
}

// A loop's records: one per group with none in flight.
std::uint64_t issue(shared& s, loop_state& l) {
	std::uint64_t n = 0;
	for (auto* g : l.groups) {
		if (g->inflight) { continue; }
		g->inflight = true;
		g->start = now_ns();
		++g->seq;
		if (s.m == mode::pull) {
			const std::lock_guard hold(g->mu);
			std::memset(g->body.data(), int(g->seq), g->body.size());
			g->open = true;
		} else {
			std::memset(g->body.data(), int(g->seq), g->body.size());
			if (s.m == mode::push) { l.sealed.push(g); }
		}
		++n;
	}
	return n;
}
void reap(loop_state& l) {
	const auto t = now_ns();
	for (auto* g : l.groups) {
		if (g->inflight && g->done.load(std::memory_order_acquire) >= g->seq) {
			g->inflight = false;
			++l.ops;
			l.latency += t - g->start;
		}
	}
}

void loop_main(shared& s, loop_state& l) {
	std::uint64_t seen = l.bell.load(std::memory_order_acquire);
	for (;;) {
		reap(l);
		if (s.stop.load(std::memory_order_acquire)) { return; }
		if (const auto n = issue(s, l); n) {
			l.issued.fetch_add(n, std::memory_order_release);
			s.opened.fetch_add(n, std::memory_order_release);
			ring_owner(s);
		}
		if (s.m == mode::ask) {
			const auto a = l.asked.load(std::memory_order_acquire);
			if (a != l.answered.load(std::memory_order_relaxed)) {
				for (auto* g : l.groups) { if (g->inflight && g->pushed != g->seq) { g->pushed = g->seq; l.sealed.push(g); } }
				l.answered.store(a, std::memory_order_release);
				s.answers.fetch_add(1, std::memory_order_release);
				ring_owner(s);
			}
		}
		await_change(l.bell, seen, s.spin_ns);
		seen = l.bell.load(std::memory_order_acquire);
		const auto rung = s.rung_at.load(std::memory_order_acquire), t = now_ns();
		if (rung && t > rung) {
			const auto w = t - rung;
			++l.wakes;
			l.wake_ns += w;
			l.wake_max = std::max(l.wake_max, w);
		}
	}
}

struct owner_stats { std::uint64_t turns = 0, records = 0, collect = 0, fanout = 0, cohort_wait = 0; };

owner_stats owner_main(shared& s, std::uint64_t end) {
	owner_stats st;
	std::string turn;
	std::vector<group*> taken;
	std::uint64_t consumed = 0, released = 0;
	std::vector<std::uint64_t> taken_from(s.loops, 0);  // ask: records taken per loop
	std::uint64_t turn_no = 0;
	auto local = s.m == mode::local ? &s.ls[0] : nullptr;
	for (;;) {
		if (now_ns() >= end) { break; }
		if (local) {
			reap(*local);
			if (const auto n = issue(s, *local); n) { s.opened.fetch_add(n, std::memory_order_relaxed); }
		}
		// Work: records opened past what turns took.
		auto bell = s.owner_bell.load(std::memory_order_acquire);
		if (s.opened.load(std::memory_order_acquire) == consumed) {
			await_change(s.owner_bell, bell, s.spin_ns);
			continue;
		}
		// The cohort: as many records as the last turn released, or the bound.
		const auto c0 = now_ns();
		while (s.opened.load(std::memory_order_acquire) - consumed < released && now_ns() - c0 < s.cohort_ns) {}
		st.cohort_wait += now_ns() - c0;
		const auto t0 = now_ns();
		taken.clear();
		++turn_no;
		switch (s.m) {
		case mode::local:
			for (auto* g : local->groups) { if (g->inflight && g->done.load(std::memory_order_relaxed) < g->seq) { taken.push_back(g); } }
			break;
		case mode::pull:
			for (auto& g : s.groups) {
				const std::lock_guard hold(g.mu);
				if (g.open) { g.open = false; taken.push_back(&g); }
			}
			break;
		case mode::push:
			for (auto& l : s.ls) { l.sealed.drain([&](group* g) { taken.push_back(g); }); }
			break;
		case mode::ask: {
			std::uint64_t want = 0;
			s.answers.store(0, std::memory_order_relaxed);
			for (unsigned i = 0; i < s.loops; ++i) {
				auto& l = s.ls[i];
				if (l.issued.load(std::memory_order_acquire) == taken_from[i]) { continue; }
				++want;
				l.asked.store(turn_no, std::memory_order_release);
				s.rung_at.store(now_ns(), std::memory_order_release);
				l.bell.fetch_add(1, std::memory_order_release);
				l.bell.notify_one();
			}
			while (s.answers.load(std::memory_order_acquire) < want) {
				const auto b = s.owner_bell.load(std::memory_order_acquire);
				if (s.answers.load(std::memory_order_acquire) >= want) { break; }
				await_change(s.owner_bell, b, s.spin_ns);
			}
			for (unsigned i = 0; i < s.loops; ++i) { taken_from[i] += s.ls[i].sealed.drain([&](group* g) { taken.push_back(g); }); }
			break;
		}
		}
		const auto t1 = now_ns();
		consumed += taken.size();
		if (taken.empty()) { continue; }
		turn.clear();
		for (auto* g : taken) { turn.append(g->body.data(), g->body.size()); }
		spin_until(now_ns() + s.barrier_ns);
		const auto t2 = now_ns();
		for (auto* g : taken) { g->done.store(g->seq, std::memory_order_release); }
		if (!local) {
			std::vector<bool> ring(s.loops, false);
			for (auto* g : taken) { ring[g->loop] = true; }
			s.rung_at.store(now_ns(), std::memory_order_release);
			for (unsigned i = 0; i < s.loops; ++i) {
				if (!ring[i]) { continue; }
				s.ls[i].bell.fetch_add(1, std::memory_order_release);
				s.ls[i].bell.notify_one();
			}
		}
		const auto t3 = now_ns();
		released = taken.size();
		++st.turns;
		st.records += taken.size();
		st.collect += t1 - t0;
		st.fanout += t3 - t2;
	}
	return st;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 8) {
		std::fprintf(stderr, "usage: handoff local|pull|push|ask L G D_US COHORT_US SPIN_US SECONDS\n");
		return 2;
	}
	const std::string m = argv[1];
	shared s;
	s.m = m == "local" ? mode::local : m == "pull" ? mode::pull : m == "push" ? mode::push : mode::ask;
	s.loops = unsigned(std::atoi(argv[2]));
	s.per_loop = unsigned(std::atoi(argv[3]));
	s.barrier_ns = std::uint64_t(std::atof(argv[4]) * 1000);
	s.cohort_ns = std::uint64_t(std::atof(argv[5]) * 1000);
	s.spin_ns = std::uint64_t(std::atof(argv[6]) * 1000);
	const double seconds = std::atof(argv[7]);
	if (s.m == mode::local) { s.per_loop *= s.loops; s.loops = 1; }
	s.groups = std::vector<group>(s.loops * s.per_loop);
	s.ls = std::vector<loop_state>(s.loops);
	for (unsigned i = 0; i < s.groups.size(); ++i) {
		s.groups[i].loop = i / s.per_loop;
		s.ls[i / s.per_loop].groups.push_back(&s.groups[i]);
	}
	std::vector<std::thread> threads;
	if (s.m != mode::local) {
		for (auto& l : s.ls) { threads.emplace_back([&s, &l] { loop_main(s, l); }); }
	}
	const auto begin = now_ns();
	const auto st = owner_main(s, begin + std::uint64_t(seconds * 1e9));
	const auto elapsed = double(now_ns() - begin) / 1e9;
	s.stop.store(true, std::memory_order_release);
	for (auto& l : s.ls) {
		l.bell.fetch_add(1, std::memory_order_release);
		l.bell.notify_all();
	}
	for (auto& t : threads) { t.join(); }
	std::uint64_t ops = 0, latency = 0, wakes = 0, wake_ns = 0, wake_max = 0;
	for (auto& l : s.ls) {
		ops += l.ops;
		latency += l.latency;
		wakes += l.wakes;
		wake_ns += l.wake_ns;
		wake_max = std::max(wake_max, l.wake_max);
	}
	const double turns = double(std::max<std::uint64_t>(st.turns, 1));
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	const double cpu = double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e6 + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
	std::printf("%-5s L=%u G=%u D=%gus cohort=%gus spin=%gus: %.0f commits/s, %.2f records/turn (of %zu), turn %.1fus, collect %.2fus, fanout %.2fus, cohort wait %.2fus, mean latency %.1fus, wake %.2fus mean %.1fus max, cpu %.2fus/commit\n",
		m.c_str(), s.loops, s.per_loop, double(s.barrier_ns) / 1000, double(s.cohort_ns) / 1000, double(s.spin_ns) / 1000, double(ops) / elapsed, double(st.records) / turns, s.groups.size(),
		elapsed * 1e6 / turns, double(st.collect) / turns / 1000, double(st.fanout) / turns / 1000, double(st.cohort_wait) / turns / 1000,
		ops ? double(latency) / double(ops) / 1000 : 0.0, wakes ? double(wake_ns) / double(wakes) / 1000 : 0.0, double(wake_max) / 1000, ops ? cpu / double(ops) : 0.0);
	return 0;
}
