// The process-wide resource budget (docs/design/cold-tier.md D6; S3): the caps and how they are
// derived, the raise of the descriptor limit, the hard cap that refuses before the kernel is
// asked, the external cap an EMFILE lowers, and the counts that never drift.
#include "strata/core/resource_budget.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;
using std::chrono::seconds;

namespace {

// A kernel the test controls: the descriptor limit, what raising it was asked for.
struct fake_kernel {
	resource_limits limits{1024, 1024};
	std::uint64_t raised_to = 0;
	bool refuse_raise = false;
	std::uint64_t max_files = ~std::uint64_t(0);
	std::uint64_t max_maps = 65530, max_vnodes = 263168;

	resource_system system(bool maps = true, bool vnodes = true) {
		resource_system s;
		s.get_limits = [this] { return limits; };
		s.set_soft = [this](std::uint64_t n) {
			raised_to = n;
			if (refuse_raise) { return false; }
			limits.soft = n;
			return true;
		};
		s.max_files = max_files;
		s.count_maps = maps;
		s.max_maps = max_maps;
		s.count_vnodes = vnodes;
		s.max_vnodes = max_vnodes;
		return s;
	}
};

// An injected clock.
struct fake_clock {
	std::chrono::steady_clock::time_point now{seconds(1000)};
	std::function<std::chrono::steady_clock::time_point()> fn() {
		return [this] { return now; };
	}
};

resource_options options(fake_kernel& k, fake_clock* c = nullptr, std::uint64_t reserve = 0) {
	resource_options o;
	o.system = k.system();
	o.descriptor_reserve = reserve;
	if (c) { o.clock = c->fn(); }
	return o;
}

constexpr resource M = resource::maps, D = resource::descriptors, V = resource::vnodes;

// The caps are the design's: the stock values give the table's numbers.
void caps() {
	fake_kernel k;
	k.limits = {4096, 4096};
	resource_budget b(options(k));
	CHECK(b.soft(M) == 32765 && b.hard(M) == 58977);
	CHECK(b.soft(V) == 105267 && b.hard(V) == 157900);
	// 4096 less the default reserve (a quarter, under 1,024): 3072 at 75% and 95%.
	CHECK(b.descriptor_reserve() == 1024);
	CHECK(b.soft(D) == 2304 && b.hard(D) == 2918);
	CHECK(b.limits().soft == 4096);

	// A small limit takes a quarter as the reserve.
	fake_kernel s;
	s.limits = {256, 256};
	resource_budget c(options(s));
	CHECK(c.descriptor_reserve() == 64 && c.soft(D) == 144 && c.hard(D) == 182);

	// Maps less the process's own at construction.
	auto sys = k.system();
	sys.process_maps = 530;
	resource_options o;
	o.system = sys;
	resource_budget m(std::move(o));
	CHECK(m.soft(M) == 32500 && m.hard(M) == 58500);

	// A platform that does not count a resource has no cap on it.
	resource_options o2;
	o2.system = k.system(false, true);
	resource_budget nm(std::move(o2));
	CHECK(nm.hard(M) == resource_budget::unlimited && nm.soft(M) == resource_budget::unlimited);
	std::printf("caps: ok\n");
}

// Fewer than 64 indexes' descriptors fails at construction with EMFILE.
void too_few_descriptors() {
	for (const auto [limit, ok] : {std::pair<std::uint64_t, bool>{100, true}, {90, true}, {89, false}, {64, false}}) {
		fake_kernel k;
		k.limits = {limit, limit};
		int code = 0;
		try {
			resource_budget b(options(k));
		} catch (const std::system_error& e) {
			code = e.code().value();
		}
		CHECK(ok == (code == 0));
		CHECK(ok || code == EMFILE);
	}
	std::printf("too few descriptors: ok\n");
}

// The soft limit is raised to the hard limit (or the system's ceiling) and the caps come from the
// raised value: 256 soft with a higher hard limit.
void raise() {
	fake_kernel k;
	k.limits = {256, 4096};
	resource_budget b(options(k));
	CHECK(k.raised_to == 4096 && b.limits().soft == 4096);
	CHECK(b.soft(D) == 2304 && b.hard(D) == 2918);

	// The ceiling (kern.maxfilesperproc) bounds the raise.
	fake_kernel c;
	c.limits = {256, ~std::uint64_t(0)};
	c.max_files = 2048;
	resource_budget cb(options(c));
	CHECK(c.raised_to == 2048 && cb.limits().soft == 2048);
	CHECK(cb.descriptor_reserve() == 512 && cb.soft(D) == 1152 && cb.hard(D) == 1459);

	// Already at the hard limit: nothing asked.
	fake_kernel h;
	h.limits = {1024, 1024};
	resource_budget hb(options(h));
	CHECK(h.raised_to == 0);

	// A refused raise caps from the limit in force.
	fake_kernel r;
	r.limits = {256, 4096};
	r.refuse_raise = true;
	resource_budget rb(options(r));
	CHECK(r.raised_to == 4096 && rb.limits().soft == 256 && rb.hard(D) == 182);
	std::printf("raise: ok\n");
}

// The real getrlimit: a soft limit of 256 under a higher hard one is raised.
void real_raise() {
	rlimit before{};
	CHECK(::getrlimit(RLIMIT_NOFILE, &before) == 0);
	if (before.rlim_max != RLIM_INFINITY && before.rlim_max <= 512) {
		std::printf("real raise: skipped (hard limit %llu)\n", static_cast<unsigned long long>(before.rlim_max));
		return;
	}
	rlimit low = before;
	low.rlim_cur = 256;
	CHECK(::setrlimit(RLIMIT_NOFILE, &low) == 0);
	{
		resource_budget b;
		rlimit now{};
		CHECK(::getrlimit(RLIMIT_NOFILE, &now) == 0);
		CHECK(now.rlim_cur > 256);
		CHECK(b.limits().soft == static_cast<std::uint64_t>(now.rlim_cur));
		CHECK(b.hard(D) > 182);
		// A limit lowered afterwards is picked up at the next pass.
		rlimit lower = now;
		lower.rlim_cur = 300;
		CHECK(::setrlimit(RLIMIT_NOFILE, &lower) == 0);
		b.tick();
		CHECK(b.limits().soft == 300 && b.descriptor_reserve() == 75 && b.hard(D) == 213);
	}
	CHECK(::setrlimit(RLIMIT_NOFILE, &before) == 0);
	std::printf("real raise: ok\n");
}

// Each cap driven to its hard value: the reservation that passes it throws too_many_open and
// nothing it touched has changed.
void hard_caps() {
	for (const resource r : {M, D, V}) {
		fake_kernel k;
		k.limits = {256, 256};
		k.max_maps = k.max_vnodes = 100;
		resource_budget b(options(k));
		resource_counts catalog, slot;
		const std::uint64_t hard = b.hard(r);
		CHECK(hard < 1000);
		std::vector<resource_reservation> held;
		for (std::uint64_t i = 0; i < hard; ++i) { held.push_back(b.reserve(r, 1, &catalog, &slot)); }
		CHECK(b.used(r) == hard && catalog.get(r) == hard && slot.get(r) == hard);
		bool threw = false;
		try {
			(void)b.reserve(r, 1, &catalog, &slot);
		} catch (const too_many_open& e) {
			threw = e.code().value() == EMFILE;
		}
		CHECK(threw);
		CHECK(b.used(r) == hard && catalog.get(r) == hard && slot.get(r) == hard);
		// Several units at once: all or nothing.
		held.pop_back();
		held.pop_back();
		CHECK(b.used(r) == hard - 2);
		threw = false;
		try {
			(void)b.reserve(r, 3);
		} catch (const too_many_open&) {
			threw = true;
		}
		CHECK(threw && b.used(r) == hard - 2);
		{
			auto two = b.reserve(r, 2);
			CHECK(b.used(r) == hard && two.held() == 2);
		}
		CHECK(b.used(r) == hard - 2);
		held.clear();
		CHECK(b.used(r) == 0 && catalog.get(r) == 0 && slot.get(r) == 0);
		// A request larger than the cap itself.
		bool huge = false;
		try {
			(void)b.reserve(r, ~std::uint64_t(0));
		} catch (const too_many_open&) {
			huge = true;
		}
		CHECK(huge && b.used(r) == 0);
	}
	std::printf("hard caps: ok\n");
}

// A reservation moves and ends once.
void moves() {
	fake_kernel k;
	resource_budget b(options(k));
	resource_counts c;
	resource_reservation a = b.reserve(D, 1, &c);
	resource_reservation m = std::move(a);
	CHECK(!a && m && b.used(D) == 1 && c.get(D) == 1);
	resource_reservation e;
	e = std::move(m);
	CHECK(b.used(D) == 1);
	e = b.reserve(D, 2, &c);  // the one it held goes back
	CHECK(b.used(D) == 2 && c.get(D) == 2);
	e.release();
	e.release();
	CHECK(b.used(D) == 0 && c.get(D) == 0);
	std::printf("moves: ok\n");
}

// A reservation that crosses a soft cap posts to every listener, once per crossing; the pass sees
// over_soft() and excess().
void crossing() {
	fake_kernel k;
	k.limits = {256, 256};
	resource_budget b(options(k));
	std::atomic<int> a{0}, c{0};
	const auto ida = b.add_listener([&] { ++a; });
	b.add_listener([&] { ++c; });
	const std::uint64_t soft = b.soft(D);
	std::vector<resource_reservation> held;
	for (std::uint64_t i = 0; i < soft; ++i) { held.push_back(b.reserve(D)); }
	CHECK(a == 0 && c == 0 && !b.over_soft());
	held.push_back(b.reserve(D));  // soft + 1 crosses
	CHECK(a == 1 && c == 1 && b.over_soft() && b.excess(D) == 1);
	held.push_back(b.reserve(D));  // already over: no new crossing
	CHECK(a == 1 && b.excess(D) == 2);
	held.resize(soft - 1);
	CHECK(!b.over_soft() && b.excess(D) == 0);
	held.push_back(b.reserve(D));  // back at the soft cap
	held.push_back(b.reserve(D));  // crosses again
	CHECK(a == 2 && c == 2);
	b.remove_listener(ida);
	held.clear();
	held.push_back(b.reserve(D, soft + 1));  // one reservation crossing in one step
	CHECK(a == 2 && c == 3);
	std::printf("crossing: ok\n");
}

// A counted call the kernel refused with EMFILE, ENFILE or ENOMEM lowers the soft cap to the count
// before it; other errors do not. The lowered cap survives later passes.
void external_pressure() {
	fake_kernel k;
	k.limits = {1024, 1024};
	fake_clock clock;
	resource_budget b(options(k, &clock));
	int posts = 0;
	b.add_listener([&] { ++posts; });
	const std::uint64_t derived = b.soft(D);
	CHECK(derived == 576 && b.external(D) == resource_budget::unlimited);
	std::vector<resource_reservation> held;
	for (int i = 0; i < 100; ++i) { held.push_back(b.reserve(D)); }
	{
		auto res = b.reserve(D);
		b.note_failure(res, EIO);
		b.note_failure(res, EACCES);
		CHECK(b.soft(D) == derived && b.external_failures() == 0 && posts == 0);
		b.note_failure(res, EMFILE);
		CHECK(b.soft(D) == 100 && b.external(D) == 100 && b.derived_soft(D) == derived && b.external_failures() == 1);
		CHECK(posts == 1);
	}
	// The pass re-derives the caps from the same limit and the lowered cap stands.
	for (int pass = 0; pass < 5; ++pass) {
		clock.now += seconds(1);
		b.tick();
		CHECK(b.soft(D) == 100 && b.derived_soft(D) == derived);
	}
	// A second failure at a lower count lowers it again; a higher one does not raise it.
	held.resize(80);
	{
		auto res = b.reserve(D);
		b.note_failure(res, ENFILE);
		CHECK(b.soft(D) == 80);
	}
	held.push_back(b.reserve(D, 10));
	{
		auto res = b.reserve(D);
		b.note_failure(res, ENOMEM);
		CHECK(b.soft(D) == 80);
	}
	// Another resource keeps its own cap.
	CHECK(b.external(M) == resource_budget::unlimited);
	std::printf("external pressure: ok\n");
}

// The lowered cap is cleared by a changed limit, and rises an eighth of its gap per pass after 60 s
// without an external failure.
void external_recovery() {
	fake_kernel k;
	k.limits = {1024, 1024};
	fake_clock clock;
	resource_budget b(options(k, &clock));
	const std::uint64_t derived = b.soft(D);
	auto fail_at = [&](std::uint64_t count) {
		std::vector<resource_reservation> held;
		for (std::uint64_t i = 0; i < count; ++i) { held.push_back(b.reserve(D)); }
		b.note_failure(b.reserve(D), EMFILE);
	};

	// A changed limit clears it at the next pass.
	fail_at(100);
	CHECK(b.soft(D) == 100);
	k.limits = {2048, 2048};
	b.tick();
	CHECK(b.external(D) == resource_budget::unlimited && b.soft(D) == b.derived_soft(D) && b.derived_soft(D) > derived);
	k.limits = {1024, 1024};
	b.tick();
	CHECK(b.soft(D) == derived);

	// Quiet for 60 s: each pass raises it by an eighth of the gap, at least 1.
	fail_at(100);
	CHECK(b.soft(D) == 100);
	clock.now += seconds(59);
	b.tick();
	CHECK(b.soft(D) == 100);
	clock.now += seconds(1);
	std::uint64_t at = 100;
	int passes = 0;
	while (b.external(D) != resource_budget::unlimited && passes < 200) {
		b.tick();
		const std::uint64_t gap = derived - at;
		const std::uint64_t up = gap / 8 ? gap / 8 : 1;
		at = at + up >= derived ? derived : at + up;
		CHECK(b.soft(D) == at);
		++passes;
	}
	CHECK(b.soft(D) == derived && b.external(D) == resource_budget::unlimited && passes > 8 && passes < 100);

	// A failure while it is rising lowers it again and restarts the 60 s.
	fail_at(100);
	clock.now += seconds(61);
	b.tick();
	const std::uint64_t rising = b.soft(D);
	CHECK(rising > 100 && rising < derived);
	fail_at(50);
	CHECK(b.soft(D) == 50);
	clock.now += seconds(30);
	b.tick();
	CHECK(b.soft(D) == 50);
	std::printf("external recovery: ok\n");
}

// Many threads reserve, fail and release: the count never passes the hard cap while they run (a
// sampler watches) and is exactly zero after; the holders' counts agree.
void stress() {
	fake_kernel k;
	k.limits = {256, 256};
	resource_budget b(options(k));
	resource_counts catalogs[2], slots[4];
	constexpr int T = 8;
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> peak{0}, refused{0}, granted{0};
	std::thread sampler([&] {
		while (!stop) {
			for (const resource r : {M, D, V}) {
				const std::uint64_t u = b.used(r);
				if (u > b.hard(r)) { peak = u; }
			}
		}
	});
	std::vector<std::thread> ts;
	for (int t = 0; t < T; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937 rng(unsigned(t) + 1);
			std::vector<resource_reservation> mine;
			for (int i = 0; i < 20000; ++i) {
				const resource r = rng() % 3 == 0 ? M : (rng() % 2 ? D : V);
				if (rng() % 3 && mine.size() < 40) {
					try {
						mine.push_back(b.reserve(r, 1 + rng() % 3, &catalogs[t % 2], &slots[t % 4]));
						++granted;
					} catch (const too_many_open&) {
						++refused;
					}
				} else if (!mine.empty()) {
					const std::size_t at = rng() % mine.size();
					mine.erase(mine.begin() + static_cast<std::ptrdiff_t>(at));
				}
				if (i % 4000 == 0) { b.tick(); }
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	stop = true;
	sampler.join();
	CHECK(peak == 0);
	CHECK(refused > 0 && granted > 0);
	for (const resource r : {M, D, V}) {
		CHECK(b.used(r) == 0);
		CHECK(catalogs[0].get(r) == 0 && catalogs[1].get(r) == 0);
		for (auto& s : slots) { CHECK(s.get(r) == 0); }
	}
	std::printf("stress: ok (%llu granted, %llu refused)\n", static_cast<unsigned long long>(granted.load()), static_cast<unsigned long long>(refused.load()));
}

}  // namespace

int main() {
	caps();
	too_few_descriptors();
	raise();
	real_raise();
	hard_caps();
	moves();
	crossing();
	external_pressure();
	external_recovery();
	stress();
	if (failures) {
		std::printf("resource_budget: FAILED (%d)\n", failures);
		return 1;
	}
	std::printf("resource_budget: ok\n");
	return 0;
}
