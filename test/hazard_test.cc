// The hazard registry (shared-reader D5, D6; SR-D3, SR-D17): claim and release, on one thread and
// across threads; a claim never parks a record (`parking`) and never takes a lock when guards are
// released on other threads (`drift`); too_many_readers at H claimed records and not before;
// the claim-time table and high-water mark; then the stress, N readers acquiring and releasing,
// some released on another thread, while the owner publishes, retires, and scans. Run under
// TSan and ASan + UBSan; ordering is the litmus's (hazard_litmus_test.cc).
#include "strata/core/hazard.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

static std::atomic<int> failures{0};
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (failures.fetch_add(1) > 30) std::exit(1); } \
	} while (0)

using strata::reader_registry;
using strata::too_many_readers;

namespace {

// The owners publish at least `pubs` times, and go on until the readers have read `min_reads`
// times, so a loaded host that schedules readers late still exercises them; at most 120 s.
struct enough {
	std::uint64_t pubs, min_reads;
	std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
	bool more(std::uint64_t g, const std::atomic<std::uint64_t>& reads) const {
		return g <= pubs || (reads.load(std::memory_order_relaxed) < min_reads && std::chrono::steady_clock::now() < until);
	}
};

// A published value whose payload sums to `sum`: a free under a reader shows as a wrong sum,
// a use-after-free, or a race.
struct view {
	std::uint64_t gen;
	std::vector<std::uint64_t> payload;
	std::uint64_t sum = 0;
	explicit view(std::uint64_t g) : gen(g), payload(16 + g % 48) {
		for (std::size_t i = 0; i < payload.size(); ++i) {
			payload[i] = g * 1315423911u + i;
			sum += payload[i];
		}
	}
	bool intact() const {
		std::uint64_t s = 0;
		for (auto x : payload) { s += x; }
		return s == sum;
	}
};
using guard = reader_registry::guard<view>;

void claim_and_release() {
	reader_registry reg(4);
	std::atomic<const view*> current{nullptr};
	CHECK(!reg.acquire(current));  // nothing published: an empty guard, no record taken
	view a(1);
	reader_registry::publish(current, static_cast<const view*>(&a));
	std::vector<guard> held;
	for (int i = 0; i < 4; ++i) {
		held.push_back(reg.acquire(current));
		CHECK(held.back().get() == &a);
	}
	bool refused = false;
	try {
		(void)reg.acquire(current);
	} catch (const too_many_readers&) {
		refused = true;
	}
	CHECK(refused);  // H claimed: refused, never a wait
	held.pop_back();  // one release frees one record
	guard g = reg.acquire(current);
	CHECK(g.get() == &a);
	guard moved = std::move(g);
	CHECK(!g && moved.get() == &a);
	moved.release();
	CHECK(!moved);
	CHECK(reg.acquire(current).get() == &a);  // released twice over: the record is free again
	held.clear();
	// Released on another thread: the record is free for this one.
	{
		std::vector<guard> four;
		for (int i = 0; i < 4; ++i) { four.push_back(reg.acquire(current)); }
		std::thread([&] { four.clear(); }).join();
		for (int i = 0; i < 4; ++i) { four.push_back(reg.acquire(current)); }
		CHECK(four.size() == 4);
	}
	// The owner's diagnostics: the scan sees each claimed record once, and stamps its first sight.
	{
		guard x = reg.acquire(current);
		std::size_t named = 0;
		reg.scan(10, [&](const void* p) { named += p == &a; });
		CHECK(named == 1);
		reg.scan(20, [](const void*) {});
		const auto c = reg.claims();
		CHECK(c.size() == 1 && c[0].p == &a && c[0].since == 10);
		CHECK(reg.high_water() == 1);
		x.release();
		reg.scan(30, [](const void*) {});
		CHECK(reg.claims().empty());
	}
	// Closed: null published, acquires come back empty.
	reader_registry::publish(current, static_cast<const view*>(nullptr));
	CHECK(!reg.acquire(current));
	std::printf("claim and release: ok\n");
}

// SR-D3's `parking`: four idle threads that each used four guards and released them hold no
// record, so a fifth thread claims with H = 16.
void parking() {
	reader_registry reg(16);
	view a(1);
	std::atomic<const view*> current{&a};
	std::atomic<int> warmed{0};
	std::atomic<bool> leave{false};
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; ++t) {
		ts.emplace_back([&] {
			std::vector<guard> gs;
			for (int i = 0; i < 4; ++i) { gs.push_back(reg.acquire(current)); }
			gs.clear();
			warmed.fetch_add(1);
			while (!leave.load()) { std::this_thread::yield(); }
		});
	}
	while (warmed.load() != 4) { std::this_thread::yield(); }
	std::size_t held = 0;
	reg.scan(0, [&](const void*) { ++held; });
	bool refused = false;
	std::vector<guard> fifth;
	try {
		for (int i = 0; i < 16; ++i) { fifth.push_back(reg.acquire(current)); }  // every record, from one thread
	} catch (const too_many_readers&) {
		refused = true;
	}
	leave.store(true);
	for (auto& t : ts) { t.join(); }
	std::printf("parking: H 16, four idle threads, records held %zu, a fifth thread claims all 16: %s\n", held, refused ? "refused" : "yes");
	CHECK(held == 0 && !refused);
}

// SR-D3's `drift`: acquire on one thread, release on another, as a resumed task does. The
// registry has no lock to take; every claim succeeds with H only above the guards in flight.
void drift() {
	reader_registry reg(8);
	view a(1);
	std::atomic<const view*> current{&a};
	const int n = 100000;
	std::mutex qmu;
	std::deque<guard> q;
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> released{0}, shed{0};
	std::thread b([&] {
		for (;;) {
			const bool done = stop.load();  // sampled first: no push follows it
			guard g;
			{
				std::lock_guard l(qmu);
				if (!q.empty()) {
					g = std::move(q.front());
					q.pop_front();
				}
			}
			if (g) {
				g.release();
				released.fetch_add(1, std::memory_order_relaxed);
			} else if (done) {
				break;
			} else {
				std::this_thread::yield();
			}
		}
	});
	for (int i = 0; i < n; ++i) {
		guard g;
		for (;;) {
			try {
				g = reg.acquire(current);
				break;
			} catch (const too_many_readers&) {
				shed.fetch_add(1, std::memory_order_relaxed);  // eight in flight: the releaser is behind
				std::this_thread::yield();
			}
		}
		CHECK(g.get() == &a);
		std::lock_guard l(qmu);
		q.push_back(std::move(g));
	}
	stop.store(true);
	b.join();
	std::size_t held = 0;
	reg.scan(0, [&](const void*) { ++held; });
	std::printf("drift: %d claims, %llu released on another thread, %llu shed while 8 were in flight, %zu held after\n", n,
		(unsigned long long)released.load(), (unsigned long long)shed.load(), held);
	CHECK(released.load() == std::uint64_t(n) && held == 0);
}

// The protocol under load: one owner publishing `pubs` views, retiring the old and freeing what no
// record names; readers acquiring, checking the view, and releasing, one guard in 16 on another
// thread; then close (null) and the last frees.
void stress(int readers, std::uint64_t pubs, std::size_t h) {
	reader_registry reg(h);
	strata::basic_retired<view> retired;
	std::atomic<const view*> current{nullptr};
	auto first = std::make_unique<const view>(1);
	reader_registry::publish(current, first.get());
	std::unique_ptr<const view> owned = std::move(first);
	std::atomic<std::uint64_t> bad_sum{0}, bad_gen{0}, reads{0}, shed{0}, cross{0};
	std::size_t max_retired = 0;

	std::mutex qmu;
	std::deque<guard> q;
	std::atomic<bool> stop{false};
	std::thread releaser([&] {
		for (;;) {
			const bool done = stop.load();  // sampled first: no push follows it
			guard g;
			{
				std::lock_guard l(qmu);
				if (!q.empty()) {
					g = std::move(q.front());
					q.pop_front();
				}
			}
			if (g) {
				if (!g->intact()) { bad_sum.fetch_add(1); }
				g.release();
				cross.fetch_add(1, std::memory_order_relaxed);
			} else if (done) {
				break;
			} else {
				std::this_thread::yield();
			}
		}
	});
	std::vector<std::thread> ts;
	for (int t = 0; t < readers; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(std::uint64_t(t) + 1);
			std::uint64_t last = 0;
			for (;;) {
				guard g;
				try {
					g = reg.acquire(current);
				} catch (const too_many_readers&) {
					shed.fetch_add(1, std::memory_order_relaxed);
					std::this_thread::yield();
					continue;
				}
				if (!g) { break; }  // closed
				if (g->gen < last) { bad_gen.fetch_add(1); }
				last = g->gen;
				if (!g->intact()) { bad_sum.fetch_add(1); }
				reads.fetch_add(1, std::memory_order_relaxed);
				if (rng() % 16 == 0) {
					std::lock_guard l(qmu);
					q.push_back(std::move(g));
					continue;
				}
				if (rng() % 64 == 0) { std::this_thread::yield(); }
				if (!g->intact()) { bad_sum.fetch_add(1); }
			}
		});
	}
	const enough go{pubs, std::uint64_t(readers) * 500};
	for (std::uint64_t g = 2; go.more(g, reads); ++g) {
		auto next = std::make_unique<const view>(g);
		retired.reserve();
		reader_registry::publish(current, next.get());
		retired.retire(std::exchange(owned, std::move(next)));
		retired.reclaim(reg, g);
		max_retired = std::max(max_retired, retired.size());
		if (retired.size() > h) {
			std::fprintf(stderr, "retired %zu > H %zu\n", retired.size(), h);
			std::abort();
		}
	}
	retired.reserve();
	reader_registry::publish(current, static_cast<const view*>(nullptr));
	retired.retire(std::move(owned));
	for (auto& t : ts) { t.join(); }
	stop.store(true);
	releaser.join();
	retired.reclaim(reg, pubs + 1);
	std::printf("stress: %d readers, H %zu, %llu publications, %llu reads, %llu released on another thread, %llu shed, max retired %zu, high water %zu\n", readers,
		h, (unsigned long long)pubs, (unsigned long long)reads.load(), (unsigned long long)cross.load(), (unsigned long long)shed.load(), max_retired, reg.high_water());
	CHECK(bad_sum.load() == 0 && bad_gen.load() == 0);
	CHECK(retired.empty());  // every guard released: nothing is held
	CHECK(reads.load() >= go.min_reads);
}


// Revision 4 (D5): the registry is empty until the first reader and grows a chunk at a time.
// Chunks never move: a record's address is the same before and after growth. The capacity is
// exact, not rounded to the chunk, and a scan and the claim table cover every chunk.
void chunks() {
	constexpr std::size_t k = reader_registry::chunk_records();
	reader_registry reg(10);
	CHECK(reg.capacity() == 10);
	CHECK(reg.allocated() == 0);
	view v(1);
	std::atomic<const view*> cur{&v};
	std::size_t named = 0;
	reg.scan(1, [&](const void*) { ++named; });
	CHECK(named == 0 && reg.claims().empty() && reg.allocated() == 0);  // a scan allocates nothing

	std::vector<guard> gs;
	const void* first_record = nullptr;
	for (std::size_t i = 0; i < 10; ++i) {
		gs.push_back(reg.acquire(cur));
		CHECK(gs.back() && gs.back().get() == &v);
		CHECK(reg.allocated() == (i / k + 1) * k);  // a chunk when the claimed ones fill what exists
		if (i == 0) { first_record = &reg.slot(0); }
		CHECK(first_record == &reg.slot(0));
	}
	bool threw = false;
	try {
		(void)reg.acquire(cur);
	} catch (const too_many_readers&) {
		threw = true;
	}
	CHECK(threw);
	named = 0;
	reg.scan(2, [&](const void* p) { named += p == &v; });
	CHECK(named == 10 && reg.high_water() == 10);
	std::vector<bool> got(reg.allocated());
	for (const auto& c : reg.claims()) {
		CHECK(c.record < 10 && !got[c.record] && c.p == &v && c.since == 2);
		got[c.record] = true;
	}
	std::size_t seen = 0;
	for (bool b : got) { seen += b; }
	CHECK(seen == 10);
	const std::size_t grown = reg.allocated();
	gs.clear();
	named = 0;
	reg.scan(3, [&](const void*) { ++named; });
	CHECK(named == 0 && reg.claims().empty());
	for (std::size_t i = 0; i < 10; ++i) { gs.push_back(reg.acquire(cur)); }
	CHECK(reg.allocated() == grown);  // released records are reused, not appended after

	reader_registry open;  // no capacity: it grows with the readers
	CHECK(open.capacity() == strata::unbounded_records);
	std::vector<guard> many;
	for (std::size_t i = 0; i < 1000; ++i) { many.push_back(open.acquire(cur)); }
	CHECK(open.allocated() == 1000 / k * k + (1000 % k ? k : 0));
	std::size_t all = 0;
	open.scan(1, [&](const void*) { ++all; });
	CHECK(all == 1000);
}

// A registry destroyed with a record claimed keeps its chunks: the guard's release writes there
// (ASan sees a freed chunk). One destroyed with none claimed frees them (LSan sees a leak).
void claimed_query() {
	view v(1);
	std::atomic<const view*> cur{&v};
	reader_registry reg(4096);
	CHECK(!reg.claimed() && reg.allocated() == 0);  // the query allocates nothing, however large the capacity
	std::vector<guard> gs;
	for (int i = 0; i < 9; ++i) { gs.push_back(reg.acquire(cur)); }
	const std::size_t grown = reg.allocated();
	CHECK(reg.claimed() && reg.allocated() == grown);
	gs.clear();
	CHECK(!reg.claimed() && reg.allocated() == grown);
	reader_registry open;
	CHECK(!open.claimed() && open.allocated() == 0);  // no capacity to walk
}

// The destructor's check against releases on other threads (A4): each round destroys a registry
// while guards are released on other threads. Under TSan the free after a clean check must follow
// the last release.
void destroy_racing_releases(int rounds) {
	view v(1);
	std::atomic<const view*> cur{&v};
	for (int round = 0; round < rounds; ++round) {
		auto reg = std::make_unique<reader_registry>(8);
		std::vector<guard> gs;
		for (int i = 0; i < 6; ++i) { gs.push_back(reg->acquire(cur)); }
		std::atomic<bool> go{false};
		std::vector<std::thread> ts;
		for (std::size_t i = 0; i < gs.size(); ++i) {
			ts.emplace_back([&, g = std::make_shared<guard>(std::move(gs[i]))] {
				while (!go.load()) { std::this_thread::yield(); }
				g->release();
			});
		}
		go.store(true);
		while (reg->claimed()) { std::this_thread::yield(); }
		reg.reset();  // frees, or keeps if a release is still to come: either is safe
		for (auto& t : ts) { t.join(); }
	}
}

void kept_chunks() {
	view v(1);
	std::atomic<const view*> cur{&v};
	auto reg = std::make_unique<reader_registry>(16);
	std::vector<guard> gs;
	for (int i = 0; i < 9; ++i) { gs.push_back(reg->acquire(cur)); }  // three chunks
	reg.reset();
	for (auto& g : gs) { CHECK(g.get() == &v); }
	gs.clear();  // each release is a store into a chunk
	reader_registry done(16);
	{
		std::vector<guard> tmp;
		for (int i = 0; i < 9; ++i) { tmp.push_back(done.acquire(cur)); }
	}
}

// Readers racing to append: n threads acquire from an empty registry at once, so several build
// a chunk for the same link and the losers discard theirs. Every thread gets its own record,
// whatever the interleaving, and the scan names every one.
void racing_appenders(int n, int rounds) {
	view v(1);
	std::atomic<const view*> cur{&v};
	for (int round = 0; round < rounds; ++round) {
		reader_registry reg;
		std::atomic<int> ready{0}, held{0};
		std::atomic<bool> go{false}, release{false};
		std::vector<std::thread> ts;
		for (int t = 0; t < n; ++t) {
			ts.emplace_back([&] {
				ready.fetch_add(1);
				while (!go.load()) { std::this_thread::yield(); }
				guard g = reg.acquire(cur);
				held.fetch_add(1);
				while (!release.load()) { std::this_thread::yield(); }
			});
		}
		while (ready.load() < n) { std::this_thread::yield(); }
		go.store(true);
		while (held.load() < n) { std::this_thread::yield(); }
		std::size_t named = 0;
		reg.scan(1, [&](const void*) { ++named; });
		CHECK(named == std::size_t(n));
		CHECK(reg.claims().size() == std::size_t(n));
		CHECK(reg.allocated() >= std::size_t(n) && reg.allocated() < std::size_t(n) + reader_registry::chunk_records());
		release.store(true);
		for (auto& t : ts) { t.join(); }
	}
}

// The protocol under growth: readers hold a handful of guards each, so the registry grows to
// many chunks while the owner publishes, retires, and scans; the retired views stay bounded by
// the records that exist, the reads are intact, and the high-water mark covers what was held.
void growth_stress(int readers, std::uint64_t pubs) {
	reader_registry reg;  // unbounded, empty
	strata::basic_retired<view> retired;
	std::atomic<const view*> current{nullptr};
	auto first = std::make_unique<const view>(1);
	reader_registry::publish(current, first.get());
	std::unique_ptr<const view> owned = std::move(first);
	std::atomic<std::uint64_t> bad{0}, reads{0};
	std::size_t max_retired = 0;
	std::vector<std::thread> ts;
	for (int t = 0; t < readers; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(std::uint64_t(t) + 7);
			std::vector<guard> held;
			for (;;) {
				const std::size_t want = 1 + rng() % 9;
				bool closed = false;
				while (held.size() < want) {
					guard g = reg.acquire(current);
					if (!g) { closed = true; break; }
					if (!g->intact()) { bad.fetch_add(1); }
					reads.fetch_add(1, std::memory_order_relaxed);
					held.push_back(std::move(g));
				}
				if (closed) { break; }
				for (const auto& g : held) {
					if (!g->intact()) { bad.fetch_add(1); }
				}
				if (rng() % 4 == 0) { std::this_thread::yield(); }
				held.erase(held.begin() + std::ptrdiff_t(rng() % held.size()));  // out of order
				if (rng() % 3 == 0) { held.clear(); }
			}
		});
	}
	const enough go{pubs, std::uint64_t(readers) * 500};
	for (std::uint64_t g = 2; go.more(g, reads); ++g) {
		auto next = std::make_unique<const view>(g);
		retired.reserve();
		reader_registry::publish(current, next.get());
		retired.retire(std::exchange(owned, std::move(next)));
		retired.reclaim(reg, g);
		max_retired = std::max(max_retired, retired.size());
		if (retired.size() > reg.allocated()) {
			std::fprintf(stderr, "retired %zu > records %zu\n", retired.size(), reg.allocated());
			std::abort();
		}
	}
	retired.reserve();
	reader_registry::publish(current, static_cast<const view*>(nullptr));
	retired.retire(std::move(owned));
	for (auto& t : ts) { t.join(); }
	retired.reclaim(reg, pubs + 1);
	std::printf("growth: %d readers, %llu publications, %llu reads, %zu records in %zu chunks, max retired %zu, high water %zu\n", readers, (unsigned long long)pubs,
		(unsigned long long)reads.load(), reg.allocated(), reg.allocated() / reader_registry::chunk_records(), max_retired, reg.high_water());
	CHECK(bad.load() == 0);
	CHECK(retired.empty());
	CHECK(reads.load() >= go.min_reads);
	CHECK(reg.high_water() <= reg.allocated());
}

// SR-D18: one registry shared by a partition's indexes, two lease holders publishing and scanning
// it at once from two threads, readers on both, and a third thread reading the diagnostics. Under
// TSan the scan's claim-time table and high-water mark must not race.
void two_scanners() {
	reader_registry reg(64);
	view v[2][2] = {{view(1), view(2)}, {view(3), view(4)}};
	std::atomic<const view*> cur[2];
	cur[0].store(&v[0][0]);
	cur[1].store(&v[1][0]);
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> bad{0};
	std::vector<std::thread> readers;
	for (int r = 0; r < 4; ++r) {
		readers.emplace_back([&, r] {
			while (!stop.load(std::memory_order_relaxed)) {
				auto g = reg.acquire(cur[r & 1]);
				if (!g || !g->intact()) { bad.fetch_add(1); }
			}
		});
	}
	std::thread diagnostics([&] {
		while (!stop.load(std::memory_order_relaxed)) {
			for (const auto& c : reg.claims()) {
				if (!c.p || c.record >= reg.capacity()) { bad.fetch_add(1); }
			}
			(void)reg.high_water();
		}
	});
	std::vector<std::thread> owners;
	std::atomic<std::uint64_t> named{0};
	for (int ix = 0; ix < 2; ++ix) {
		owners.emplace_back([&, ix] {
			for (int i = 0; i < 20000; ++i) {
				reader_registry::publish(cur[ix], static_cast<const view*>(&v[ix][i & 1]));
				reg.scan(std::uint64_t(i), [&](const void*) { named.fetch_add(1, std::memory_order_relaxed); });
			}
		});
	}
	for (auto& t : owners) { t.join(); }
	stop.store(true);
	for (auto& t : readers) { t.join(); }
	diagnostics.join();
	reg.scan(0, [](const void*) {});
	std::printf("two scanners: %llu records named, high water %zu, %zu claimed after\n", (unsigned long long)named.load(), reg.high_water(), reg.claims().size());
	CHECK(bad.load() == 0);
	CHECK(reg.high_water() <= 4);  // 4 readers, one guard each; 0 when every scan lost the stamping lock to claims()
	CHECK(reg.claims().empty());
	// The stamp itself, once no claims() contends: a held guard is stamped and counted.
	{
		auto g = reg.acquire(cur[0]);
		reg.scan(1, [](const void*) {});
		const auto c = reg.claims();
		CHECK(c.size() == 1 && c[0].p == g.get() && reg.high_water() >= 1);
	}
}

// CR2 (the review's stress2.cc): K lease holders, each with its own pointer and retired list,
// publish, retire, and reclaim against one registry, while R readers acquire from a random one,
// 1 guard in 8 released on another thread, and a thread spins on claims() so most scans lose the
// stamping lock: a scan that loses it must still protect. Views are the same size and poisoned
// when freed, so addresses recycle and a free under a reader shows.
struct pview {
	static constexpr std::uint64_t live = 0x600dc0de600dc0deull, dead = 0xdeaddeaddeaddeadull;
	volatile std::uint64_t magic = live;
	std::uint64_t gen;
	std::uint64_t payload[24];
	std::uint64_t sum = 0;
	explicit pview(std::uint64_t g) : gen(g) {
		for (int i = 0; i < 24; ++i) {
			payload[i] = g * 2654435761u + std::uint64_t(i);
			sum += payload[i];
		}
	}
	~pview() {
		for (auto& p : payload) { p = 0; }
		magic = dead;
	}
	bool intact() const {
		if (magic != live) { return false; }
		std::uint64_t s = 0;
		for (auto x : payload) { s += x; }
		return s == sum && magic == live;
	}
};

void shared_registry_stress(int k, int readers, std::size_t h, std::uint64_t pubs) {
	using pguard = reader_registry::guard<pview>;
	reader_registry reg(h);
	const std::size_t ks = std::size_t(k);
	std::vector<std::atomic<const pview*>> cur(ks);
	std::vector<std::unique_ptr<const pview>> owned(ks);
	std::vector<strata::basic_retired<pview>> retired(ks);
	for (std::size_t i = 0; i < ks; ++i) {
		owned[i] = std::make_unique<const pview>(1);
		cur[i].store(owned[i].get());
	}
	std::atomic<std::uint64_t> bad{0}, reads{0}, cross{0}, over{0};
	std::atomic<bool> stop{false};
	std::mutex qmu;
	std::deque<pguard> q;
	std::thread releaser([&] {
		for (;;) {
			const bool done = stop.load();  // sampled first: no push follows it
			pguard g;
			{
				std::lock_guard l(qmu);
				if (!q.empty()) {
					g = std::move(q.front());
					q.pop_front();
				}
			}
			if (g) {
				if (!g->intact()) { bad.fetch_add(1); }
				g.release();
				cross.fetch_add(1, std::memory_order_relaxed);
			} else if (done) {
				break;
			} else {
				std::this_thread::yield();
			}
		}
	});
	std::thread diag([&] {
		while (!stop.load(std::memory_order_relaxed)) {
			(void)reg.claims();
			(void)reg.high_water();
		}
	});
	std::atomic<int> owners_done{0};
	std::vector<std::thread> rs;
	for (int t = 0; t < readers; ++t) {
		rs.emplace_back([&, t] {
			std::mt19937_64 rng(std::uint64_t(t) + 7);
			while (owners_done.load(std::memory_order_acquire) < k) {
				pguard g;
				try {
					g = reg.acquire(cur[rng() % ks]);
				} catch (const too_many_readers&) {
					std::this_thread::yield();
					continue;
				}
				if (!g) { continue; }
				if (!g->intact()) { bad.fetch_add(1); }
				reads.fetch_add(1, std::memory_order_relaxed);
				if (rng() % 8 == 0) {
					std::lock_guard l(qmu);
					q.push_back(std::move(g));
					continue;
				}
				if (rng() % 32 == 0) { std::this_thread::yield(); }
				if (!g->intact()) { bad.fetch_add(1); }
			}
		});
	}
	const enough go{pubs, std::uint64_t(readers) * 500};
	std::vector<std::thread> os;
	for (std::size_t i = 0; i < ks; ++i) {
		os.emplace_back([&, i] {
			for (std::uint64_t g = 2; go.more(g, reads); ++g) {
				auto next = std::make_unique<const pview>(g);
				retired[i].reserve();
				reader_registry::publish(cur[i], next.get());
				retired[i].retire(std::exchange(owned[i], std::move(next)));
				retired[i].reclaim(reg, g);
				if (retired[i].size() > h) { over.fetch_add(1); }
			}
			owners_done.fetch_add(1, std::memory_order_release);
		});
	}
	for (auto& t : os) { t.join(); }
	for (auto& t : rs) { t.join(); }
	stop.store(true);
	releaser.join();
	diag.join();
	std::size_t left = 0;
	for (std::size_t i = 0; i < ks; ++i) {
		retired[i].reserve();
		reader_registry::publish(cur[i], static_cast<const pview*>(nullptr));
		retired[i].retire(std::move(owned[i]));
		retired[i].reclaim(reg, pubs + 1);
		left += retired[i].size();
	}
	std::printf("shared registry: %d lease holders, %d readers, H %zu, %llu publications each, %llu reads, %llu released on another thread, high water %zu\n", k,
		readers, h, (unsigned long long)pubs, (unsigned long long)reads.load(), (unsigned long long)cross.load(), reg.high_water());
	if (left) {  // the evidence a rare CI failure has to carry: who still names what
		for (std::size_t i = 0; i < ks; ++i) { std::fprintf(stderr, "left claimed: holder %zu keeps %zu retired\n", i, retired[i].size()); }
		for (std::size_t r = 0; r < h; ++r) {
			// a named view was kept by its retired list, unless the record was set after its free
			if (const auto* p = static_cast<const pview*>(reg.slot(r).p.load())) {
				std::fprintf(stderr, "left claimed: record %zu names %p, generation %llu, %s\n", r, static_cast<const void*>(p), (unsigned long long)p->gen, p->magic == pview::live ? "live" : "freed");
			}
		}
		for (const auto& c : reg.claims()) { std::fprintf(stderr, "left claimed: record %zu first seen naming %p at scan %llu\n", c.record, c.p, (unsigned long long)c.since); }
	}
	CHECK(bad.load() == 0);
	CHECK(over.load() == 0);
	CHECK(left == 0);
	CHECK(reads.load() >= go.min_reads);
}
}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t pubs = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
	claim_and_release();
	parking();
	drift();
	chunks();
	kept_chunks();
	claimed_query();
	destroy_racing_releases(500);
	racing_appenders(8, 500);
	growth_stress(12, pubs);
	two_scanners();
	shared_registry_stress(2, 8, 32, pubs);
	shared_registry_stress(3, 8, 16, pubs);
	for (int readers : {1, 4, 8}) { stress(readers, pubs, 64); }
	stress(8, pubs / 4, 4);  // H below the readers: shedding, and retired views bounded by H
	if (failures.load()) {
		std::fprintf(stderr, "%d failures\n", failures.load());
		return 1;
	}
	std::printf("hazard: ok\n");
	return 0;
}
