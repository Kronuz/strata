// The cold tier's gate rows through the catalog (docs/design/cold-tier.md T1 to T4 and T2d), over the
// fixtures `idle_index build ROOT COUNT DOCS SEGMENTS` makes: a catalog<kv_index> on PosixIO directories,
// a work_pool_executor, a timer thread and the process's real resource budget, as a host runs it.
//   catalog_tiers tier  ROOT N hot|warm|cold [opts]   T1: memory per tier, metered as idle_index
//   catalog_tiers first ROOT N hot|warm|cold [opts]   T2 and T2d: one pin and get per index, random order,
//                                                     from one thread off the pool
//   catalog_tiers count ROOT N [opts]                 T3: every index once, then Zipf requests, 1 Hz samples
//   catalog_tiers zipf  ROOT N [opts]                 T4: hit ratio against the S3-FIFO models on one trace
//   catalog_tiers_count sites ROOT N TIER [opts]      T1's pieces: the live allocation sites per index
// opts: defer (defer_io), lazy (no eager_segments; ignored under defer), idle SECS, drop (drop the page
// cache before the timed loop: root), seed S, threads T, requests R, warm W, theta X, scan, soft_open A,
// soft_hot B, chores C (the catalog's chores at once).
// Tiers: hot is open with every segment mapped; warm is begin_demote() (`owed` under defer_io: the
// first request pin wakes it, promote() on the pool); cold is closed, no slot.
#include "../../test/s3fifo_model.hh"
#include "../idle_index/heap_count.hh"
#include "../idle_index/meters.hh"

#include "strata/core/budget.hh"
#include "strata/core/catalog.hh"
#include "strata/core/hazard.hh"
#include "strata/core/index.hh"
#include "strata/core/work_pool_executor.hh"

#include "completion/posix.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>

#include <set>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
#if defined(__APPLE__) && defined(IDLE_COUNT)
#include <mach-o/dyld.h>
#endif

using namespace strata;
using namespace idle_bench;
using kv_index = basic_index<document_layout>;
using cat_t = catalog<kv_index>;

namespace {

struct options {
	bool defer = false, lazy = false, drop = false, scan = false;
	double idle = 0, theta = 0.99;
	std::uint64_t seed = 1, threads = 8, requests = 1'000'000, warm = 0, soft_open = 0, soft_hot = 0, chores = 8;
};

// The host's timer: `fn` runs on the timer's thread after `delay_ns`, and only posts a chore.
class timer_thread {
public:
	timer_thread() : t_([this] { loop(); }) {}
	~timer_thread() {
		{
			std::lock_guard l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		t_.join();
	}
	void add(std::uint64_t delay_ns, std::function<void()> fn) {
		{
			std::lock_guard l(m_);
			q_.emplace(clk::now() + std::chrono::nanoseconds(delay_ns), std::move(fn));
		}
		cv_.notify_all();
	}

private:
	void loop() {
		std::unique_lock l(m_);
		for (;;) {
			if (q_.empty()) {
				if (stop_) { return; }
				cv_.wait(l);
			} else if (q_.begin()->first <= clk::now() || stop_) {
				auto fn = std::move(q_.begin()->second);
				q_.erase(q_.begin());
				l.unlock();
				fn();
				l.lock();
			} else {
				cv_.wait_until(l, q_.begin()->first);
			}
		}
	}
	std::mutex m_;
	std::condition_variable cv_;
	std::multimap<clk::time_point, std::function<void()>> q_;
	bool stop_ = false;
	std::thread t_;
};

// A request thread's scheduler: the catalog posts a resumed pin here from the thread that ended the transition.
class thread_sched {
public:
	void post(std::coroutine_handle<> h) {
		{
			std::lock_guard l(mu_);
			q_.push_back(h);
		}
		cv_.notify_one();
	}
	std::uint64_t now() const noexcept { return 0; }
	template <class Done>
	void run_until(Done&& done) {
		const task::running_on on(task::scheduler_ref::of(*this));
		for (;;) {
			std::coroutine_handle<> h;
			{
				std::unique_lock l(mu_);
				cv_.wait(l, [&] { return !q_.empty() || done(); });
				if (q_.empty()) { return; }
				h = q_.front();
				q_.pop_front();
			}
			h.resume();
		}
	}
	// Runs `body` to its end on the calling thread.
	void run(task::task<void> body) {
		std::atomic<bool> done{false};
		task::spawn(task::scheduler_ref::of(*this), std::move(body), [&](std::exception_ptr e) {
			if (e) { std::terminate(); }  // a request's errors are caught inside the body
			{
				std::lock_guard l(mu_);
				done = true;
			}
			cv_.notify_one();
		});
		run_until([&] { return done.load(); });
	}

private:
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<std::coroutine_handle<>> q_;
};

std::string name_of(std::uint64_t i) { return "i" + std::to_string(i); }

// Errors a request met, by kind: the kernel's (ENFILE, EMFILE, ENOMEM), the budget's too_many_open, others.
struct errors {
	std::atomic<std::uint64_t> enfile{0}, emfile{0}, enomem{0}, too_many_open{0}, other{0};
	void note(const std::exception& e) {
		if (dynamic_cast<const strata::too_many_open*>(&e)) {
			++too_many_open;
		} else if (const auto* se = dynamic_cast<const std::system_error*>(&e)) {
			const int c = se->code().value();
			++(c == ENFILE ? enfile : c == EMFILE ? emfile : c == ENOMEM ? enomem : other);
		} else {
			++other;
		}
		if (other.load() == 1) { std::printf("error: %s\n", e.what()); }
	}
	std::uint64_t kernel() const { return enfile.load() + emfile.load() + enomem.load(); }
	void print() const {
		std::printf("errors: ENFILE %llu EMFILE %llu ENOMEM %llu too_many_open %llu other %llu\n", (unsigned long long)enfile.load(), (unsigned long long)emfile.load(),
			(unsigned long long)enomem.load(), (unsigned long long)too_many_open.load(), (unsigned long long)other.load());
	}
};

long system_vnodes() {
	long v = -1;
#if defined(__APPLE__) || defined(__FreeBSD__)
	int n = 0;
	std::size_t len = sizeof n;
#ifdef __APPLE__
	const char* name = "kern.num_vnodes";
#else
	const char* name = "vfs.numvnodes";
#endif
	if (::sysctlbyname(name, &n, &len, nullptr, 0) == 0) { v = n; }
#endif
	return v;
}

// This process's vnodes (macOS): the distinct files behind its descriptors and its mapped regions.
// kern.num_vnodes is the system's vnode cache, which sits at kern.maxvnodes on a busy host; -1 elsewhere.
long process_vnodes() {
#ifdef __APPLE__
	std::set<std::pair<std::uint64_t, std::uint64_t>> seen;  // (device, inode)
	const pid_t pid = ::getpid();
	std::vector<proc_fdinfo> fds(std::size_t(proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0)) / sizeof(proc_fdinfo) + 64);
	const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), int(fds.size() * sizeof(proc_fdinfo))) / int(sizeof(proc_fdinfo));
	for (int i = 0; i < got; ++i) {
		vnode_fdinfo vi{};
		if (fds[std::size_t(i)].proc_fdtype == PROX_FDTYPE_VNODE && proc_pidfdinfo(pid, fds[std::size_t(i)].proc_fd, PROC_PIDFDVNODEINFO, &vi, sizeof vi) == int(sizeof vi)) {
			seen.emplace(std::uint64_t(vi.pvi.vi_stat.vst_dev), vi.pvi.vi_stat.vst_ino);
		}
	}
	for (std::uint64_t at = 0;;) {
		proc_regionwithpathinfo ri{};
		if (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, at, &ri, sizeof ri) != int(sizeof ri)) { break; }
		if (ri.prp_vip.vip_vi.vi_stat.vst_ino != 0) { seen.emplace(std::uint64_t(ri.prp_vip.vip_vi.vi_stat.vst_dev), ri.prp_vip.vip_vi.vi_stat.vst_ino); }
		at = ri.prp_prinfo.pri_address + ri.prp_prinfo.pri_size;
	}
	return long(seen.size());
#else
	return -1;
#endif
}

// The page cache dropped (root only): Linux drop_caches, macOS purge.
bool drop_caches() {
	::sync();
#ifdef __linux__
	if (FILE* f = std::fopen("/proc/sys/vm/drop_caches", "w")) {
		const bool ok = std::fputs("3\n", f) >= 0;
		return std::fclose(f) == 0 && ok;
	}
	return false;
#elif defined(__APPLE__)
	return std::system("purge 2>/dev/null") == 0;
#else
	return false;
#endif
}

// The host: one catalog over ROOT's indexes "i0".."i<N-1>".
struct bench_host {
	fs::path root;
	WorkPool pool;
	work_pool_executor exec;
	resource_budget budget;
	reader_registry registry{256};
	byte_budget bytes{std::uint64_t(16) << 30};
	timer_thread timer;
	std::unique_ptr<cat_t> cat;

	static WorkPool::Options pool_opts(const options& o) {
		WorkPool::Options w;
		w.threads = unsigned(o.chores) + 1;  // chores are merge-class: one worker stays in reserve
		w.name_format = "tiers{:02}";
		return w;
	}
	bench_host(fs::path r, const options& o, bool sweep) : root(std::move(r)), pool(pool_opts(o)), exec(pool, unsigned(o.chores)) {
		cat_t::host h;
		h.exec = &exec;
		h.budget = &budget;
		h.index_options.keyed = true;
		h.index_options.eager_segments = !o.lazy;
		h.index_options.defer_io = o.defer;
		h.index_options.budget = &bytes;
		h.index_options.buffer_share = std::uint64_t(1) << 30;
		h.index_options.readers = &registry;
		h.soft_open = o.soft_open;
		h.soft_hot = o.soft_hot;
		h.resolve = [this](const cat_t::opening& op) -> std::unique_ptr<kronuz::journal::IO> {
			const fs::path dir = dir_of(root, std::strtoull(std::string(op.key.substr(1)).c_str(), nullptr, 10));
			return counted_io::make(op.counts, [&] { return std::make_unique<kronuz::journal::PosixIO>(dir, 0600, kronuz::journal::PosixIOOptions{true, true}); });
		};
		h.timer = [this](std::uint64_t delay, std::function<void()> fn) { timer.add(delay, std::move(fn)); };
		cat = std::make_unique<cat_t>(std::move(h));
		if (sweep) { cat->start(); }
		const resource_limits lim = budget.limits();
		std::printf("RLIMIT_NOFILE soft %llu hard %llu; budget caps: descriptors %llu/%llu maps %llu/%llu vnodes %llu/%llu (soft/hard)\n", (unsigned long long)lim.soft,
			(unsigned long long)lim.hard, (unsigned long long)budget.soft(resource::descriptors), (unsigned long long)budget.hard(resource::descriptors),
			(unsigned long long)budget.soft(resource::maps), (unsigned long long)budget.hard(resource::maps), (unsigned long long)budget.soft(resource::vnodes),
			(unsigned long long)budget.hard(resource::vnodes));
	}
	~bench_host() {
		cat->stop_sweep();
		thread_sched s;
		cat_t* c = cat.get();
		s.run([c]() -> task::task<void> { (void)co_await c->shutdown(); }());
		cat.reset();
	}

	std::uint64_t count(slot_state want) const {
		std::uint64_t n = 0;
		for (std::uint32_t i = 0; i < cat->slots_made(); ++i) { n += cat_t::state_of(cat->word_of(i)) == want; }
		return n;
	}
	void print_states(const char* when) const {
		static const char* names[] = {"free", "opening", "open", "owed", "waking", "demoting", "closing", "failed"};
		std::printf("%s: slots %u", when, cat->slots_made());
		for (unsigned s = 0; s < slot_states; ++s) {
			if (const std::uint64_t n = count(slot_state(s))) { std::printf(" %s %llu", names[s], (unsigned long long)n); }
		}
		const auto& c = cat->counts();
		std::printf("; held: maps %llu descriptors %llu vnodes %llu, hot %llu\n", (unsigned long long)c.get(resource::maps), (unsigned long long)c.get(resource::descriptors),
			(unsigned long long)c.get(resource::vnodes), (unsigned long long)c.holders_mapped.load());
	}

	// Opens indexes [0, n) through try_pin, the opens on the pool, `batch` in flight at a time.
	void open_all(std::uint64_t n) {
		for (std::uint64_t i = 0; i < n; ++i) {
			const auto r = cat->try_pin(name_of(i), pin_kind::request, false);
			if (r.status == cat_t::try_status::failed) { std::rethrow_exception(r.error); }
			if (i % 512 == 511) { cat->wait_chores(); }
		}
		cat->wait_chores();
	}
	// Every slot in `from` through `begin` (begin_demote or begin_close), then the chores.
	template <class Begin>
	std::uint64_t move_all(slot_state from, Begin&& begin) {
		std::uint64_t n = 0;
		for (std::uint32_t i = 0; i < cat->slots_made(); ++i) {
			if (cat_t::state_of(cat->word_of(i)) == from && begin(i)) { ++n; }
			if (i % 512 == 511) { cat->wait_chores(); }
		}
		cat->wait_chores();
		return n;
	}
	// The tier forced: every index opened, then demoted (warm) or closed (cold).
	void force(std::uint64_t n, const std::string& tier) {
		const auto t0 = clk::now();
		open_all(n);
		std::printf("opened %llu in %.2f s\n", (unsigned long long)count(slot_state::open), us(t0, clk::now()) / 1e6);
		if (tier == "warm" || tier == "cold") {
			const auto t1 = clk::now();
			const std::uint64_t d = move_all(slot_state::open, [&](std::uint32_t i) { return cat->begin_demote(i); });
			std::printf("demoted %llu in %.2f s\n", (unsigned long long)d, us(t1, clk::now()) / 1e6);
		}
		if (tier == "cold") {  // a close needs the slot's maps at zero: the sweep's order, demote then close
			const auto t1 = clk::now();
			const slot_state warm = cat_t::state_of(cat->word_of(0)) == slot_state::owed ? slot_state::owed : slot_state::open;
			const std::uint64_t d = move_all(warm, [&](std::uint32_t i) { return cat->begin_close(i); });
			std::printf("closed %llu in %.2f s\n", (unsigned long long)d, us(t1, clk::now()) / 1e6);
		}
		print_states("forced");
	}
};

// One request: a pin by key (a use) and one get; with `twice`, a re-pin by handle and a second get, as a Xapiand
// read does (CT-R31). Returns whether the key was found.
task::task<bool> request(cat_t& c, std::string key, bool twice) {
	static const std::string q = key_of(0);
	bool found = false;
	cat_t::handle h;
	{
		auto p = co_await c.pin_request(key);
		h = p.where();
		const auto g = p.reader().acquire();
		found = g->get(q).has_value();
	}
	if (twice) {
		auto p = co_await c.repin(h, key);
		const auto g = p.reader().acquire();
		found &= g->get(q).has_value();
	}
	co_return found;
}

// --- T1 ---------------------------------------------------------------------------------------

int tier_mode(const fs::path& root, std::uint64_t n, const std::string& tier, const options& o) {
	bench_host b(root, o, false);
	const meters m0 = take();
#ifdef IDLE_COUNT
	const double heap0 = double(live_bytes), count0 = double(live_count);
#endif
	b.force(n, tier);
	if (o.idle > 0) { std::this_thread::sleep_for(std::chrono::duration<double>(o.idle)); }
	const meters m1 = take();
	report(("tier " + tier).c_str(), m0, m1, double(n));
#ifdef IDLE_COUNT
	std::printf("heap live: %.1f B/index in %.2f allocations\n", (double(live_bytes) - heap0) / double(n), (double(live_count) - count0) / double(n));
#endif
	return 0;
}

#ifdef IDLE_COUNT
// T1's pieces: the allocation sites live after forcing `tier`, per index, largest first, with the image's slide so
// `atos` can name them. One index opened and closed first, so one-time statics are not charged.
int sites_mode(const fs::path& root, std::uint64_t n, const std::string& tier, const options& o) {
	bench_host b(root, o, false);
	b.force(1, "cold");
	b.cat->wait_chores();
	sites = new std::map<std::uint64_t, site_rec>;
	const double heap0 = double(live_bytes), count0 = double(live_count);
	profile = true;
	b.force(n, tier);
	profile = false;
	const double per = double(n);
	std::printf("heap live: %.1f B/index in %.2f allocations\n", (double(live_bytes) - heap0) / per, (double(live_count) - count0) / per);
#ifdef __APPLE__
	std::printf("slide 0x%llx\n", (unsigned long long)_dyld_get_image_vmaddr_slide(0));
#endif
	std::vector<std::pair<std::size_t, const site_rec*>> v;
	for (auto& [k, r] : *sites) {
		if (r.bytes) { v.emplace_back(r.bytes, &r); }
	}
	std::sort(v.rbegin(), v.rend());
	for (auto& [bytes, r] : v) {
		std::printf("SITE %.1f B %.2f allocs:", double(bytes) / per, double(r->count) / per);
		for (int i = 0; i < r->n; ++i) { std::printf(" %p", r->pcs[i]); }
		std::printf("\n");
	}
	return 0;
}
#endif

// --- T2 and T2d -------------------------------------------------------------------------------

// promote()'s own time per index (T2d), on indexes opened alone under defer_io and demoted.
void promote_probe(const fs::path& root, std::uint64_t n) {
	byte_budget bytes(std::uint64_t(16) << 30);
	std::vector<double> t;
	std::size_t segments = 0;
	for (std::uint64_t i = 0; i < n; ++i) {
		kronuz::journal::PosixIO io(dir_of(root, i), 0600, kronuz::journal::PosixIOOptions{true, true});
		kv_index::options ko;
		ko.keyed = true;
		ko.defer_io = true;
		ko.budget = &bytes;
		kv_index ix = kv_index::open(io, ko);
		segments = ix.segments().size();
		if (!ix.demote()) { continue; }
		const strata::lease_scope hold{ix.lease()};
		const auto a = clk::now();
		ix.promote();
		t.push_back(us(a, clk::now()));
	}
	const pct p = summarize(t);
	print_pct("promote()", p);
	std::printf("promote() per segment (%zu segments): p50 %.1f us p99 %.1f us (estimate 33 to 50 us)\n", segments, p.p50 / double(segments ? segments : 1), p.p99 / double(segments ? segments : 1));
}

int first_mode(const fs::path& root, std::uint64_t n, const std::string& tier, const options& o) {
	{
		bench_host b(root, o, false);
		b.force(n, tier);
		if (o.drop) { std::printf("page cache dropped: %s\n", drop_caches() ? "yes" : "no (needs root)"); }
		std::vector<std::uint64_t> order(n);
		std::iota(order.begin(), order.end(), 0);
		std::shuffle(order.begin(), order.end(), std::mt19937_64(o.seed));
		std::vector<double> t;
		t.reserve(n);
		std::uint64_t found = 0;
		errors err;
		const auto before = b.cat->stats();
		thread_sched s;
		cat_t& c = *b.cat;
		s.run([&]() -> task::task<void> {
			for (const std::uint64_t i : order) {
				const auto a = clk::now();
				try {
					found += co_await request(c, name_of(i), false);
				} catch (const std::exception& e) {
					err.note(e);
				}
				t.push_back(us(a, clk::now()));
			}
		}());
		const auto after = b.cat->stats();
		std::printf("found %llu of %llu; wakes %llu, opens %llu\n", (unsigned long long)found, (unsigned long long)n, (unsigned long long)(after.wakes_posted - before.wakes_posted),
			(unsigned long long)(after.opens - before.opens));
		print_pct(("first " + tier).c_str(), summarize(t));
		err.print();
		b.print_states("after");
	}
	if (o.defer && tier == "warm") { promote_probe(root, std::min<std::uint64_t>(n, 10'000)); }
	return 0;
}

// --- T3 and T4 --------------------------------------------------------------------------------

// The kernel's counts at 1 Hz beside the budget's and the catalog's, with their peaks.
class sampler {
public:
	sampler(bench_host& b, bool print) : b_(b), print_(print), t_([this] { loop(); }) {}
	~sampler() { stop(); }
	void stop() {
		{
			std::lock_guard l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		if (t_.joinable()) { t_.join(); }
	}
	struct sample {
		long maps = 0, fds = 0, vnodes = -1, sys_vnodes = -1;
		std::uint64_t open = 0, hot = 0, used_fds = 0, used_maps = 0, used_vnodes = 0;
		long fd_excess = 0;  // the kernel's descriptors beyond the larger budget count read around them
	};
	// The budget's descriptors are read before and after the kernel's, so an index closing between
	// the reads counts on the budget's side; process_vnodes() opens descriptors, so it runs after.
	sample now() const {
		sample s;
		const std::uint64_t fds_before = b_.budget.used(resource::descriptors);
		const meters m = take();
		const std::uint64_t fds_after = b_.budget.used(resource::descriptors);
		s.fd_excess = m.fds - long(std::max(fds_before, fds_after));
		s.maps = m.maps, s.fds = m.fds, s.vnodes = process_vnodes(), s.sys_vnodes = system_vnodes();
		s.open = b_.cat->open_slots(), s.hot = b_.cat->counts().holders_mapped.load();
		s.used_fds = fds_after, s.used_maps = b_.budget.used(resource::maps), s.used_vnodes = b_.budget.used(resource::vnodes);
		return s;
	}
	static void print(const char* what, const sample& s) {
		std::printf("%s: kernel maps %ld fds %ld vnodes %ld (system %ld) | open %llu hot %llu | budget descriptors %llu maps %llu vnodes %llu\n", what, s.maps, s.fds, s.vnodes, s.sys_vnodes,
			(unsigned long long)s.open, (unsigned long long)s.hot, (unsigned long long)s.used_fds, (unsigned long long)s.used_maps, (unsigned long long)s.used_vnodes);
	}
	sample peak;
	long fd_gap = 0;  // the most descriptors the kernel showed beyond the budget's count at one sample (T7)

private:
	void loop() {
		std::unique_lock l(m_);
		for (int sec = 0;; ++sec) {
			if (cv_.wait_for(l, std::chrono::seconds(1), [&] { return stop_; })) { return; }
			const sample s = now();
			peak.maps = std::max(peak.maps, s.maps), peak.fds = std::max(peak.fds, s.fds), peak.vnodes = std::max(peak.vnodes, s.vnodes), peak.sys_vnodes = std::max(peak.sys_vnodes, s.sys_vnodes);
			peak.open = std::max(peak.open, s.open), peak.hot = std::max(peak.hot, s.hot);
			peak.used_fds = std::max(peak.used_fds, s.used_fds), peak.used_maps = std::max(peak.used_maps, s.used_maps), peak.used_vnodes = std::max(peak.used_vnodes, s.used_vnodes);
			fd_gap = std::max(fd_gap, s.fd_excess);
			if (print_) {
				print(("t+" + std::to_string(sec + 1) + "s").c_str(), s);
				flow(sec + 1);
			}
		}
	}
	// Where the counts over the soft caps sit: slots in each transition (begun: its chore queued or running), the relief
	// booked, the counts net of both, and the sweep's and the requests' work over the last second.
	void flow(int sec) {
		const cat_t::stats_t st = b_.cat->stats();
		const auto ended = [](const cat_t::stats_t& z, slot_state x) {
			std::uint64_t n = 0;
			for (unsigned t = 0; t < slot_states; ++t) { n += z.edges[unsigned(x)][t]; }
			return n;
		};
		const auto in = [&](slot_state x) {
			std::uint64_t into = 0;
			for (unsigned f = 0; f < slot_states; ++f) { into += st.edges[f][unsigned(x)]; }
			return (long long)(into - ended(st, x));
		};
		const auto d = [&](std::uint64_t cat_t::stats_t::*m) { return (unsigned long long)(st.*m - last_.*m); };
		const auto ends = [&](slot_state x) { return (unsigned long long)(ended(st, x) - ended(last_, x)); };
		const long long open = (long long)b_.cat->open_slots(), hot = (long long)b_.cat->hot_indexes(), closing = in(slot_state::closing);
		const long long booked = (long long)b_.cat->pending_open(), demoting = (long long)b_.cat->pending_hot();
		std::printf("flow t+%ds: in flight opening %lld waking %lld demoting %lld (booked hot %lld) closing %lld booked closes %lld | net open %lld hot %lld | per s: passes %llu "
					"reposts %llu demotes %llu (evict %llu) closes %llu busy %llu opens %llu wakes %llu; ended demotes %llu closes %llu\n",
			sec, in(slot_state::opening), in(slot_state::waking), in(slot_state::demoting), demoting, closing, booked, open - closing - booked, hot - demoting,
			d(&cat_t::stats_t::passes), d(&cat_t::stats_t::pass_reposts), d(&cat_t::stats_t::pass_demotes), d(&cat_t::stats_t::evict_demotes),
			d(&cat_t::stats_t::pass_closes), d(&cat_t::stats_t::busy), d(&cat_t::stats_t::opens), d(&cat_t::stats_t::wakes_posted), ends(slot_state::demoting),
			ends(slot_state::closing));
		last_ = st;
	}
	bench_host& b_;
	bool print_;
	cat_t::stats_t last_{};  // the stats at the last flow line
	std::mutex m_;
	std::condition_variable cv_;
	bool stop_ = false;
	std::thread t_;
};

struct phase_result {
	double secs = 0;
	std::vector<double> lat;
	std::uint64_t found = 0;
};

// `keys[from, to)` as requests on `threads` threads, each taking the next key from one cursor, so the order
// across threads follows the sequence.
phase_result run_phase(bench_host& b, const std::vector<std::uint32_t>& keys, std::size_t from, std::size_t to, std::uint64_t threads, bool twice, errors& err) {
	phase_result r;
	std::atomic<std::size_t> cursor{from};
	std::atomic<std::uint64_t> found{0};
	std::vector<std::vector<double>> lat(threads);
	std::vector<std::thread> ts;
	const auto t0 = clk::now();
	for (std::uint64_t t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			thread_sched s;
			cat_t& c = *b.cat;
			auto& mine = lat[t];
			mine.reserve((to - from) / threads + 1);
			s.run([&]() -> task::task<void> {
				for (;;) {
					const std::size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
					if (i >= to) { break; }
					const auto a = clk::now();
					try {
						found.fetch_add(co_await request(c, name_of(keys[i]), twice), std::memory_order_relaxed);
					} catch (const std::exception& e) {
						err.note(e);
					}
					mine.push_back(us(a, clk::now()));
				}
			}());
		});
	}
	for (auto& t : ts) { t.join(); }
	r.secs = us(t0, clk::now()) / 1e6;
	for (auto& v : lat) { r.lat.insert(r.lat.end(), v.begin(), v.end()); }
	r.found = found.load();
	return r;
}

std::uint64_t misses_of(const cat_t::stats_t& st) { return st.misses + st.retries + st.edges[unsigned(slot_state::closing)][unsigned(slot_state::opening)]; }
std::uint64_t closes_of(const cat_t::stats_t& st) { return st.edges[unsigned(slot_state::closing)][unsigned(slot_state::free)]; }

void print_rates(const char* what, const cat_t::stats_t& a, const cat_t::stats_t& z, double secs, std::size_t requests) {
	std::printf("%s: %zu requests in %.1f s (%.0f/s); opens %.0f/s, closes %.0f/s, wakes %.0f/s, ghost hits %.0f/s, ghost adds %.0f/s, misses %llu\n", what, requests, secs,
		double(requests) / secs, double(z.opens - a.opens) / secs, double(closes_of(z) - closes_of(a)) / secs, double(z.wakes_posted - a.wakes_posted) / secs,
		double(z.ghost_hits - a.ghost_hits) / secs, double(z.ghost_adds - a.ghost_adds) / secs, (unsigned long long)(misses_of(z) - misses_of(a)));
}

void print_latency(const char* what, phase_result& r, std::uint64_t threads) {
	const pct p = summarize(r.lat);
	std::printf("%s per request at %llu threads: p50 %.1f us p99 %.1f us mean %.1f us max %.1f us\n", what, (unsigned long long)threads, p.p50, p.p99, p.mean, p.max);
}

int count_mode(const fs::path& root, std::uint64_t n, const options& o) {
	bench_host b(root, o, true);
	errors err;
	std::vector<std::uint32_t> keys(n);
	std::iota(keys.begin(), keys.end(), 0u);
	std::shuffle(keys.begin(), keys.end(), std::mt19937_64(o.seed));
	{
		const s8::zipf z(n, o.theta);
		std::mt19937_64 rng(o.seed + 1);
		keys.reserve(n + o.requests);
		for (std::uint64_t i = 0; i < o.requests; ++i) { keys.push_back(std::uint32_t(z.draw(rng))); }
	}
	sampler smp(b, true);
	auto s0 = b.cat->stats();
	auto touch = run_phase(b, keys, 0, n, o.threads, false, err);
	auto s1 = b.cat->stats();
	print_rates("touch every index", s0, s1, touch.secs, n);
	print_latency("touch", touch, o.threads);
	auto zipf = run_phase(b, keys, n, keys.size(), o.threads, false, err);
	auto s2 = b.cat->stats();
	print_rates("zipf", s1, s2, zipf.secs, keys.size() - n);
	print_latency("zipf", zipf, o.threads);
	std::printf("found %llu of %zu\n", (unsigned long long)(touch.found + zipf.found), keys.size());
	// After the touch: the sweep's passes bring the counts to the soft caps.
	std::this_thread::sleep_for(std::chrono::seconds(3));
	smp.stop();
	const auto end = smp.now();
	sampler::print("peak", smp.peak);
	std::printf("descriptors the kernel showed beyond the budget's count, at most: %ld\n", smp.fd_gap);
	sampler::print("3 s after", end);
	err.print();
	const auto st = b.cat->stats();
	std::printf("sweep: %llu passes, %llu reposts, %llu demotes, %llu closes; %llu wake failures, %llu open failures\n", (unsigned long long)st.passes,
		(unsigned long long)st.pass_reposts, (unsigned long long)st.pass_demotes, (unsigned long long)st.pass_closes, (unsigned long long)st.wake_failures,
		(unsigned long long)st.open_failures);
	// The row: no kernel error and no too_many_open; the kernel's peaks below the hard caps; within a pass of the soft caps after.
	const auto below = [](std::uint64_t v, std::uint64_t cap) { return cap == 0 || v <= cap; };
	const std::uint64_t posts = 16;  // host::pass_posts: what one pass may close or demote
	const bool hard_ok = below(std::uint64_t(smp.peak.fds), b.budget.hard(resource::descriptors)) && (b.budget.hard(resource::maps) == resource_budget::unlimited || below(std::uint64_t(smp.peak.maps), b.budget.hard(resource::maps))) &&
		(smp.peak.vnodes < 0 || b.budget.hard(resource::vnodes) == resource_budget::unlimited || below(std::uint64_t(smp.peak.vnodes), b.budget.hard(resource::vnodes)));
	const bool soft_ok = (o.soft_open == 0 || end.open <= o.soft_open + posts) && (o.soft_hot == 0 || end.hot <= o.soft_hot + posts);
	std::printf("T3 %s: kernel errors %llu, too_many_open %llu, peaks below the hard caps %s, within a pass of the soft caps after %s (open %llu of %llu, hot %llu of %llu)\n",
		err.kernel() == 0 && err.too_many_open.load() == 0 && hard_ok && soft_ok ? "PASS" : "FAIL", (unsigned long long)err.kernel(), (unsigned long long)err.too_many_open.load(),
		hard_ok ? "yes" : "no", soft_ok ? "yes" : "no", (unsigned long long)end.open, (unsigned long long)o.soft_open, (unsigned long long)end.hot, (unsigned long long)o.soft_hot);
	return 0;
}

// The hottest key's pin and unpin, by key and by handle, on `threads` threads at once (T4, reported).
void hottest(bench_host& b, std::uint64_t threads, std::uint64_t pairs) {
	std::vector<double> by_key(threads), by_handle(threads);
	std::vector<std::thread> ts;
	for (std::uint64_t t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			thread_sched s;
			cat_t& c = *b.cat;
			s.run([&]() -> task::task<void> {
				const std::string k = name_of(0);
				cat_t::handle h;
				{
					auto p = co_await c.pin_request(k);
					h = p.where();
				}
				auto a = clk::now();
				for (std::uint64_t i = 0; i < pairs; ++i) { auto p = co_await c.pin_request(k); }
				by_key[t] = std::chrono::duration<double, std::nano>(clk::now() - a).count() / double(pairs);
				a = clk::now();
				for (std::uint64_t i = 0; i < pairs; ++i) { auto p = co_await c.repin(h, k); }
				by_handle[t] = std::chrono::duration<double, std::nano>(clk::now() - a).count() / double(pairs);
			}());
		});
	}
	for (auto& t : ts) { t.join(); }
	std::sort(by_key.begin(), by_key.end());
	std::sort(by_handle.begin(), by_handle.end());
	std::printf("hottest key at %llu threads, ns per pin and unpin (median of threads): by key %.1f, by handle %.1f\n", (unsigned long long)threads, by_key[threads / 2], by_handle[threads / 2]);
}

int zipf_mode(const fs::path& root, std::uint64_t n, const options& o) {
	const auto tt = clk::now();
	const s8::trace tr(n, o.theta, o.warm, o.requests, o.scan);
	std::printf("trace: %zu requests (%llu warm-up%s, %llu measured), theta %.2f, built in %.1f s\n", tr.keys.size(), (unsigned long long)o.warm, o.scan ? " + a scan" : "",
		(unsigned long long)o.requests, o.theta, us(tt, clk::now()) / 1e6);
	const std::uint64_t cap = o.soft_open;
	{
		s8::model m(cap);
		s8::model_total mt(cap);
		std::uint64_t hm = 0, ht = 0;
		for (std::size_t i = 0; i < tr.keys.size(); ++i) {
			const bool a = m.access(tr.keys[i]), c = mt.access(tr.keys[i]);
			if (i >= tr.measure_from) { hm += a, ht += c; }
		}
		std::printf("models at capacity %llu: s8_variants %.4f, model_total %.4f\n", (unsigned long long)cap, double(hm) / double(o.requests), double(ht) / double(o.requests));
		bench_host b(root, o, true);
		errors err;
		sampler smp(b, false);
		auto s0 = b.cat->stats();
		auto w = run_phase(b, tr.keys, 0, tr.measure_from, o.threads, true, err);
		auto s1 = b.cat->stats();
		print_rates("warm-up", s0, s1, w.secs, tr.measure_from);
		auto r = run_phase(b, tr.keys, tr.measure_from, tr.keys.size(), o.threads, true, err);
		auto s2 = b.cat->stats();
		print_rates("measured", s1, s2, r.secs, o.requests);
		print_latency("measured", r, o.threads);
		const double hit = 1.0 - double(misses_of(s2) - misses_of(s1)) / double(o.requests), ref = double(hm) / double(o.requests);
		smp.stop();
		sampler::print("peak", smp.peak);
		err.print();
		std::printf("T4 %s: hit ratio %.4f against s8_variants %.4f less 1 point (model_total %.4f); kernel errors %llu, too_many_open %llu\n",
			hit >= ref - 0.01 && err.kernel() == 0 ? "PASS" : "FAIL", hit, ref, double(ht) / double(o.requests), (unsigned long long)err.kernel(), (unsigned long long)err.too_many_open.load());
		hottest(b, o.threads, 200'000);
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: catalog_tiers tier|first ROOT N hot|warm|cold [opts] | count|zipf ROOT N [opts]\n");
		return 2;
	}
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const std::string cmd = argv[1];
	const fs::path root = argv[2];
	const std::uint64_t n = std::strtoull(argv[3], nullptr, 10);
	int at = 4;
	std::string tier;
	if (cmd == "tier" || cmd == "first" || cmd == "sites") {
		if (argc < 5) { return 2; }
		tier = argv[at++];
		if (tier != "hot" && tier != "warm" && tier != "cold") {
			std::fprintf(stderr, "tier: hot, warm or cold\n");
			return 2;
		}
	}
	options o;
	for (int i = at; i < argc; ++i) {
		const std::string a = argv[i];
		const auto num = [&] { return i + 1 < argc ? std::strtod(argv[++i], nullptr) : 0.0; };
		if (a == "defer") { o.defer = true; }
		else if (a == "lazy") { o.lazy = true; }
		else if (a == "drop") { o.drop = true; }
		else if (a == "scan") { o.scan = true; }
		else if (a == "idle") { o.idle = num(); }
		else if (a == "theta") { o.theta = num(); }
		else if (a == "seed") { o.seed = std::uint64_t(num()); }
		else if (a == "threads") { o.threads = std::uint64_t(num()); }
		else if (a == "requests") { o.requests = std::uint64_t(num()); }
		else if (a == "warm") { o.warm = std::uint64_t(num()); }
		else if (a == "soft_open") { o.soft_open = std::uint64_t(num()); }
		else if (a == "soft_hot") { o.soft_hot = std::uint64_t(num()); }
		else if (a == "chores") { o.chores = std::uint64_t(num()); }
		else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}
	std::printf("catalog_tiers %s %s %llu %s defer %d lazy %d\n", cmd.c_str(), root.c_str(), (unsigned long long)n, tier.c_str(), int(o.defer), int(o.lazy));
	if (cmd == "tier") { return tier_mode(root, n, tier, o); }
#ifdef IDLE_COUNT
	if (cmd == "sites") { return sites_mode(root, n, tier, o); }
#endif
	if (cmd == "first") { return first_mode(root, n, tier, o); }
	if (cmd == "count") { return count_mode(root, n, o); }
	if (cmd == "zipf") {
		if (o.soft_open == 0) {
			std::fprintf(stderr, "zipf needs soft_open (the model's capacity)\n");
			return 2;
		}
		return zipf_mode(root, n, o);
	}
	std::fprintf(stderr, "unknown mode %s\n", cmd.c_str());
	return 2;
}
