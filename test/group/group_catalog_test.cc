// A replication group on its catalog slot's index (D169; docs/mechanisms/group-catalog.md),
// on threads: the loop, the group's chore, the catalog's lane and the test's own each a thread.
//
// A1: a group the catalog's open_payload opens over the slot's index recovers from it and
// applies; closed by its host while the slot stays pinned, it leaves the index open and usable
// (read, committed, the slot not reopened), and a new group over the same index recovers from it
// and applies past it.
// A2: the catalog's shutdown closes the idle group through close_payload, whose checkpoint covers
// everything applied: a restarted process recovers its bundle at that position with every value.
// A3: that close completes with the group's close run outside the write lease. close_blocking
// refuses to run under the lease (it would wait on itself), and every step has a deadline: a
// close that never ends fails the test, which exits rather than hangs.
//
// The other closes and opens through the catalog, each a case (argv[1]; none runs them all):
// failed-open: a recover that fences (P_b > P_H) fails the pin; the group left the loop's table
//   and went idle before its throw, so the catalog destroys it on its lane while another group
//   writes on the loop.
// attach: groups open on the catalog's lane while another writes on the loop; only open() joins
//   the loop's table, on the loop.
// sweep, bound, replace: the sweep's eviction, the wake bound's close and a fenced index's
//   replace each close the group through close_payload; a reopen recovers it.
// bare-commit-crash: the catalog's bare commit makes P_H durable over file-set bytes not synced;
//   power goes off, and after a crash D16's replay rewrites them.
// TSan runs every case (CI's linux-tsan, and the quest's TSan job).
#include "blob_app.hh"
#include "host_io.hh"
#include "oracle.hh"

#include "faultlab/faultfs.h"
#include "strata/core/catalog.hh"
#include "strata/core/counted_io.hh"
#include "strata/core/owner.hh"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace sg = strata::group;
using group_test::group_type;

int failures = 0;  // the main thread's
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 30) { std::cerr << "FAIL: " << what << '\n'; }
	return ok;
}
std::string what(std::exception_ptr e) {
	if (!e) { return "no error"; }
	try {
		std::rethrow_exception(e);
	} catch (const std::exception& x) {
		return x.what();
	} catch (...) {}
	return "unknown";
}

// Every step has a deadline: past it the process exits failed, since a deadlocked thread
// cannot be joined (A3).
class watchdog {
public:
	watchdog() : t_([this] { loop(); }) {}
	~watchdog() {
		{
			std::lock_guard l(m_);
			quit_ = true;
		}
		cv_.notify_all();
		t_.join();
	}
	void arm(std::string step, std::chrono::seconds within) {
		std::lock_guard l(m_);
		step_ = std::move(step);
		until_ = std::chrono::steady_clock::now() + within;
		cv_.notify_all();
	}

private:
	void loop() {
		std::unique_lock l(m_);
		while (!quit_) {
			if (std::chrono::steady_clock::now() >= until_) {
				std::cerr << "FAIL: past the deadline at: " << step_ << '\n';
				std::fflush(stderr);
				std::_Exit(1);
			}
			cv_.wait_until(l, until_);
		}
	}
	std::mutex m_;
	std::condition_variable cv_;
	std::string step_ = "start";
	std::chrono::steady_clock::time_point until_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	bool quit_ = false;
	std::thread t_;
};
watchdog* dog = nullptr;
void step(const std::string& s, int seconds = 30) { dog->arm(s, std::chrono::seconds(seconds)); }

// A blocking_scheduler driven on its own thread until ended.
struct driven {
	task::blocking_scheduler s;
	std::atomic<bool> quit{false};
	std::thread t;
	void begin() {
		t = std::thread([this] { s.drive([this] { return quit.load(); }, [] { return true; }); });
	}
	void wake() { s.post_call([](void*) {}, nullptr); }
	void end() {
		quit.store(true);
		wake();
		if (t.joinable()) { t.join(); }
	}
	task::scheduler_ref ref() { return task::scheduler_ref::of(s); }
};

// The catalog's lane: every group it opens runs its work, one item at a time, on one thread.
class lane_executor final : public strata::executor {
public:
	explicit lane_executor(task::blocking_scheduler& lane) : lane_(&lane) {}
	std::unique_ptr<group> open() override { return std::make_unique<lane_group>(*this); }
	std::uint64_t now() override { return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		std::unique_lock l(mu_);
		cv_.wait_until(l, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(t)), [&] { return stop.requested(); });
		return !stop.requested();
	}

private:
	class lane_group final : public group {
	public:
		explicit lane_group(lane_executor& e) : e_(&e) {}
		~lane_group() override { cancel(); }
		void post(work w) override {
			auto* it = new item{this, std::move(w), {}};
			{
				std::lock_guard l(m_);
				it->stop = source_.token();
				++pending_;
			}
			e_->lane_->post_call(&run, it);
		}
		void cancel() noexcept override {
			{
				std::lock_guard l(m_);
				source_.request();
			}
			e_->cv_.notify_all();
			wait();
			std::lock_guard l(m_);
			source_ = task::cancel_source();
		}
		void wait() override {
			std::unique_lock l(m_);
			cv_.wait(l, [this] { return pending_ == 0; });
		}

	private:
		struct item {
			lane_group* g;
			work fn;
			task::cancel_token stop;
		};
		static void run(void* p) {
			auto* it = static_cast<item*>(p);
			try {
				it->fn(it->stop);
			} catch (...) {}
			auto* g = it->g;
			delete it;
			std::lock_guard l(g->m_);
			if (--g->pending_ == 0) { g->cv_.notify_all(); }  // under the lock: a waiter may destroy g
		}
		lane_executor* e_;
		std::mutex m_;
		std::condition_variable cv_;
		std::size_t pending_ = 0;
		task::cancel_source source_;
	};
	task::blocking_scheduler* lane_;
	std::mutex mu_;
	std::condition_variable cv_;
};

// The host's timer: `fn` runs on its thread after `delay_ns`, and only posts a chore.
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
			q_.emplace(std::chrono::steady_clock::now() + std::chrono::nanoseconds(delay_ns), std::move(fn));
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
			} else if (q_.begin()->first <= std::chrono::steady_clock::now() || stop_) {
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
	std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> q_;
	bool stop_ = false;
	std::thread t_;
};

const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits admission{{256u << 20, 8192}, {256 * 1024, 3}, {64u << 20, 4}, 32, 3};
constexpr cs::GroupId gid = 3;
const std::array<char, 16> uuid{'c', 'a', 't', 'a', 'l', 'o', 'g'};

// A key "g<id>" holds the group <id>.
std::string key_of(cs::GroupId id) { return "g" + std::to_string(id); }
cs::GroupId id_of(std::string_view key) { return cs::GroupId(std::stoul(std::string(key.substr(1)))); }

group_type::config config(cs::GroupId id = gid) {
	group_type::config c;
	c.id = id;
	c.options.configuration.local = 1;
	c.options.configuration.voters = {1};
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 8192;
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{20, 40, 100, 200, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	c.identity = j::Identity{'C', 'A', char('0' + id % 64)};
	return c;
}

// What survives a restart: the journal host's directory, each key's, the storage budget.
struct disks {
	fl::filesystem host;
	std::mutex mu;
	std::map<std::string, std::unique_ptr<fl::filesystem>> dirs;
	strata::byte_budget storage{std::uint64_t(1) << 30};
	fl::filesystem& of(std::string_view key) {
		std::lock_guard l(mu);
		auto& fs = dirs[std::string(key)];
		if (!fs) { fs = std::make_unique<fl::filesystem>(); }
		return *fs;
	}
	// The key's directory lost: a fresh one in its place.
	void lose(std::string_view key) {
		std::lock_guard l(mu);
		dirs[std::string(key)] = std::make_unique<fl::filesystem>();
	}
};

// A directory whose segment files fail to open with `fail` (the host's error, not the medium's),
// so a wake's promote fails (D5's bound). Files are the inner IO's, unwrapped.
class segment_fault_io final : public j::IO {
public:
	segment_fault_io(fl::filesystem& fs, const std::atomic<int>& fail) : own_(fs), fail_(&fail) {}
	using j::IO::acquire_owner;
	j::FilesystemClass directory_filesystem() const override { return in().directory_filesystem(); }
	j::DirectoryTrust directory_trust() const override { return in().directory_trust(); }
	std::unique_ptr<j::OwnerLock> acquire_owner(bool create) override { return in().acquire_owner(create); }
	std::unique_ptr<j::OwnerLock> acquire_directory_owner() override { return in().acquire_directory_owner(); }
	std::unique_ptr<j::File> open_existing(std::string_view n) override {
		check_segment(n);
		return in().open_existing(n);
	}
	std::unique_ptr<j::File> open_existing_read_only(std::string_view n) override {
		check_segment(n);
		return in().open_existing_read_only(n);
	}
	void open_existing_read_only_into(std::string_view n, j::File& f) override { in().open_existing_read_only_into(n, f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, j::File& f) override { return in().open_reclaim_candidate_read_only_into(n, f); }
	std::unique_ptr<j::File> open_reclaim_candidate_read_only(std::string_view n) override { return in().open_reclaim_candidate_read_only(n); }
	std::unique_ptr<j::File> open_reclaim_candidate(std::string_view n) override { return in().open_reclaim_candidate(n); }
	std::unique_ptr<j::File> create_exclusive(std::string_view n) override { return in().create_exclusive(n); }
	void replace(std::string_view s, std::string_view d) override { in().replace(s, d); }
	void link(std::string_view s, std::string_view d) override { in().link(s, d); }
	void remove(std::string_view n) override { in().remove(n); }
	std::unique_ptr<j::DirectoryCursor> scan_directory() override { return in().scan_directory(); }
	std::optional<j::EntryFootprint> entry_footprint(std::string_view n) override { return in().entry_footprint(n); }
	std::optional<j::EntryInspection> inspect_entry(std::string_view n) override { return in().inspect_entry(n); }
	void sync_directory() override { in().sync_directory(); }
	void sync_files(std::span<j::File* const> f) override { in().sync_files(f); }
	void sync_data_files(std::span<j::File* const> f) override { in().sync_data_files(f); }
	void sync_files_and_directory(std::span<j::File* const> f) override { in().sync_files_and_directory(f); }
	void order_files_and_directory(std::span<j::File* const> f) override { in().order_files_and_directory(f); }

protected:
	std::unique_ptr<j::OwnerLock> lock_owner(const j::OwnerLockOptions& o) override { return in().acquire_owner(o); }

private:
	void check_segment(std::string_view n) const {
		if (const int e = fail_->load(); e && n.starts_with("seg-")) { throw std::system_error(e, std::generic_category(), "test: open " + std::string(n)); }
	}
	j::IO& in() { return own_; }  // through the base: faultfs's overrides may be protected
	const j::IO& in() const { return own_; }
	fl::io own_;
	const std::atomic<int>* fail_;
};

// The catalog's knobs a case turns: by default no sweep closes, only shutdown's.
struct tuning {
	std::uint64_t sweep_ns = 3'600'000'000'000ull, close_idle_ns = 600'000'000'000ull, demote_idle_ns = 30'000'000'000ull;
	bool defer_io = false;
};

// H: the group the slot's index is lent to.
struct hosted {
	std::unique_ptr<group_type> g;
	kronuz::journal::IO* io = nullptr;
};
using cat_t = strata::catalog<blob::index_type, hosted>;

// A call's result, written where it finishes, read once `done` is.
template <class T>
struct started {
	std::atomic<bool> done{false};
	std::exception_ptr error;
	std::optional<T> value;
};
template <class T>
std::shared_ptr<started<T>> start(task::scheduler_ref on, task::task<T> work) {
	auto s = std::make_shared<started<T>>();
	task::spawn(on, std::move(work), [s](std::exception_ptr e, std::optional<T> v) {
		s->error = e;
		s->value = std::move(v);
		s->done.store(true, std::memory_order_release);
	});
	return s;
}
template <class T>
bool await(const std::shared_ptr<started<T>>& s, const std::string& what_for) {
	step(what_for);
	while (!s->done.load(std::memory_order_acquire)) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
	return check(!s->error, what_for + ": " + what(s->error));
}

// One process: its threads, journal host, loop and catalog.
struct process {
	disks& d;
	blob::app app;
	driven loop_t, chore_t, lane_t, request_t;
	j::InlineCompletion inline_io;
	std::unique_ptr<j::Host> host;
	std::unique_ptr<sg::loop> lp;
	task::cancel_source stop;
	std::atomic<bool> turns_done{false}, driver_done{false};
	strata::resource_budget budget;
	std::unique_ptr<lane_executor> exec;
	std::unique_ptr<timer_thread> timer;
	std::unique_ptr<cat_t> cat;
	// close_payload's record.
	std::atomic<int> payload_closes{0}, payload_calls{0};
	std::atomic<std::uint64_t> closed_applied{0};
	std::mutex record_mu;
	std::string payload_error;
	// open_payload's record: groups found in the loop's table before their open or after a failed one.
	std::atomic<int> early_joins{0}, failed_stays{0}, failed_opens{0};
	std::string open_error;
	std::atomic<int> fail_segments{0};  // segment_fault_io's errno, for every key's directory

	explicit process(disks& disk, tuning knobs = {}) : d(disk) {
		host = std::make_unique<j::Host>(std::make_shared<group_test::host_io>(d.host), j::Identity{'H', 'C', 'T'}, j::HostOptions{1, turn_options});
		lp = std::make_unique<sg::loop>(loop_t.ref(), j::completion_ref::of(inline_io), *host, 0, 5);
		task::spawn(loop_t.ref(), host->driver(0).run(j::completion_ref::of(inline_io), stop.token()), [this](std::exception_ptr) { turns_done.store(true); });
		task::spawn(loop_t.ref(), lp->run(stop.token()), [this](std::exception_ptr) { driver_done.store(true); });
		loop_t.begin();
		chore_t.begin();
		lane_t.begin();
		request_t.begin();
		exec = std::make_unique<lane_executor>(lane_t.s);
		timer = std::make_unique<timer_thread>();

		cat_t::host h;
		h.exec = exec.get();
		h.budget = &budget;
		h.timer = [t = timer.get()](std::uint64_t delay, std::function<void()> fn) { t->add(delay, std::move(fn)); };
		h.index_options.keyed = true;
		h.index_options.storage = &d.storage;
		h.index_options.defer_io = knobs.defer_io;
		h.resolve = [this](const cat_t::opening& op) {
			auto* fs = &d.of(op.key);
			return strata::counted_io::make(op.counts, [this, fs] { return std::make_unique<segment_fault_io>(*fs, fail_segments); });
		};
		h.open_index = [](j::IO& io, const blob::index_type::options& o, const cat_t::opening&) { return group_type::open_index(io, o, uuid); };
		// On the catalog's lane: the group's open waits on the loop and the group's chore, both elsewhere.
		// The loop's table is the loop's: the constructor leaves it alone, and a failed open has left
		// it before it throws, so the lane may destroy the group.
		h.open_payload = [this](const cat_t::opening& op, blob::index_type& ix, std::optional<hosted>& out) {
			const auto id = id_of(op.key);
			out.emplace();
			out->io = op.io;
			out->g = std::make_unique<group_type>(*lp, config(id), *op.io, ix, app, chore_t.ref());
			if (in_table(id)) { early_joins.fetch_add(1); }
			try {
				out->g->open_blocking();
			} catch (...) {
				failed_opens.fetch_add(1);
				if (in_table(id)) { failed_stays.fetch_add(1); }
				std::lock_guard l(record_mu);
				open_error = what(std::current_exception());
				throw;
			}
		};
		// The group's close, outside the write lease, before the index's (D169).
		h.close_payload = [this](hosted& p, blob::index_type&) {
			payload_calls.fetch_add(1);
			try {
				p.g->close_blocking();
				closed_applied.store(p.g->applied());
			} catch (...) {
				std::lock_guard l(record_mu);
				payload_error = what(std::current_exception());
				throw;
			}
			payload_closes.fetch_add(1);
		};
		h.sweep_period_ns = knobs.sweep_ns;
		h.close_idle_ns = knobs.close_idle_ns;
		h.demote_idle_ns = knobs.demote_idle_ns;
		cat = std::make_unique<cat_t>(std::move(h));
		cat->start();
	}

	// Whether the loop's table holds `id`, read on the loop.
	bool in_table(cs::GroupId id) {
		std::mutex m;
		std::condition_variable cv;
		bool done = false, found = false;
		task::spawn(loop_t.ref(), [](sg::loop& l, cs::GroupId g, bool& out) -> task::task<void> {
			out = l.find(g) != nullptr;
			co_return;
		}(*lp, id, found), [&](std::exception_ptr) {
			std::lock_guard l(m);
			done = true;
			cv.notify_one();  // under the lock: the waiter's frame outlives it
		});
		std::unique_lock l(m);
		cv.wait(l, [&] { return done; });
		return found;
	}

	// Pins `key` from the test's thread, waiting for its open; a failed open is `failed`'s, if given.
	cat_t::pin pin(strata::open_intent intent = strata::open_intent::open, const std::string& key = key_of(gid), std::exception_ptr* failed = nullptr) {
		step("pin " + key);
		for (;;) {
			auto r = cat->try_pin(key, strata::pin_kind::request, true, intent);
			if (r.status == cat_t::try_status::pinned) { return std::move(r.p); }
			if (r.status == cat_t::try_status::failed) {
				if (failed) {
					*failed = r.error;
					return {};
				}
				check(false, "the pin's open: " + what(r.error));
				return {};
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	// The catalog's commit chore, as its bound, shutdown and replace closes run it: a bare strata
	// commit under the lease, which syncs no file-set byte (D169). False when it threw. The lease
	// is the group's chore's while it applies or checkpoints: tried again shortly.
	bool bare_commit(cat_t::pin& p) {
		for (int i = 0; i < 5000; ++i) {
			const strata::chore_scope c{p.index().lease()};
			if (!c) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			try {
				p.index().commit();
			} catch (...) {
				return false;
			}
			return true;
		}
		return check(false, "the bare commit took the index's lease");
	}
	template <class F>
	bool wait_for(const std::string& what_for, F&& done, int seconds = 30) {
		step(what_for, seconds + 5);
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (!done()) {
			if (std::chrono::steady_clock::now() >= until) { return check(false, what_for); }
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return true;
	}
	bool write(group_type& g, const std::string& command) {
		auto w = start(loop_t.ref(), g.propose(command));
		return await(w, "write " + command) && check(w->value && w->value->applied && w->value->applied->result == strata::verdict::accepted, "write " + command + " accepted");
	}
	// Every key's version and bytes, read once applied (D4).
	std::map<std::string, std::pair<std::uint64_t, std::string>> read(group_type& g) {
		auto r = start(loop_t.ref(), g.read_applied([&g](blob::index_type&) {
			group_test::digest out;
			group_test::documents(g.strata(), [&](const std::string& n, std::uint64_t o, std::size_t l) { return g.files().read(n, o, l); }, out);
			return out.documents;
		}));
		if (!await(r, "read")) { return {}; }
		return *r->value;
	}
	strata::shutdown_result shut() {
		auto s = start(request_t.ref(), [](cat_t& c) -> task::task<strata::shutdown_result> { co_return co_await c.shutdown(); }(*cat));
		await(s, "the catalog's shutdown");
		return s->value.value_or(strata::shutdown_result{});
	}
	~process() {
		step("the process ends");
		check(early_joins.load() == 0, "a group joined the loop's table before its open: " + std::to_string(early_joins.load()));
		check(failed_stays.load() == 0, "a failed open left its group in the loop's table: " + std::to_string(failed_stays.load()));
		cat.reset();  // after shutdown: every slot closed, every chore finished
		stop.request(task::cancel_reason::shutdown);
		loop_t.wake();
		while (!(turns_done.load() && driver_done.load())) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
		request_t.end();
		lane_t.end();
		chore_t.end();
		loop_t.end();
		timer.reset();
		exec.reset();
		lp.reset();
		host.reset();
	}
};

using values = std::map<std::string, std::pair<std::uint64_t, std::string>>;
// The oracle reads keys "a".."n".
std::string key(int i) { return std::string(1, char('a' + i - 1)); }
std::string show(const values& v) {
	std::string out;
	for (const auto& [k, x] : v) { out += k + "=" + std::to_string(x.first) + ":" + x.second + " "; }
	return out;
}
values expected(int through) {
	values v;
	for (int i = 1; i <= through; ++i) { v[key(i)] = {1, "v" + std::to_string(i)}; }
	return v;
}

// A1, A2, A3.
void borrow() {
	disks d;
	std::uint64_t applied_at_shutdown = 0;
	{
		process p(d);
		auto pin = p.pin(strata::open_intent::create);
		if (!pin) { return; }
		auto& slot = pin.payload();
		group_type* g = slot.g.get();

		// A1: the group recovers from the slot's index (a deferred create: nothing durable yet) and applies.
		check(g->readable() && g->recovered().position == 0, "a fresh group opens on the slot's index with no bundle: applied " + std::to_string(g->applied()));
		for (int i = 1; i <= 5; ++i) { p.write(*g, "P|" + key(i) + "|v" + std::to_string(i)); }
		check(p.read(*g) == expected(5), "the group applied five writes into the slot's index: " + show(p.read(*g)));
		const auto applied5 = g->applied();

		// A3: a blocking close under the index's write lease would wait on itself: refused, nothing begun.
		{
			const strata::chore_scope leased{pin.index().lease()};
			check(bool(leased), "the test holds the index's lease");
			bool refused = false;
			try {
				g->close_blocking();
			} catch (const std::logic_error&) {
				refused = true;
			}
			check(refused, "close_blocking under the write lease is refused");
		}
		check(!g->fenced() && p.read(*g) == expected(5), "the refused close changed nothing");

		// A1: the host closes the group with the slot pinned; the index stays open and the catalog's.
		step("the group's close");
		g->close_blocking();
		const auto record = g->closed_record();
		check(record.has_value(), "the close rested (a closed record for the reopen)");
		check(!pin.index().fenced() && !pin.index().closing(), "the group's close leaves the index open");
		check(pin.index().applied() == strata::files::position{applied5, 1}, "the index is at the group's applied position: " + std::to_string(pin.index().applied().index));
		const auto found = pin.index().find_key(key(3));
		check(found && found->second == 1, "the catalog reads the index the group closed over");
		pin.index().commit();
		check(!pin.index().uncommitted(), "the catalog commits the index the group closed over");

		// A1: a new group over the same open index recovers from it and applies past it.
		{
			auto c = config();
			c.closed = record;
			slot.g.reset();
			slot.g = std::make_unique<group_type>(*p.lp, std::move(c), *slot.io, pin.index(), p.app, p.chore_t.ref());
			step("the reopen");
			slot.g->open_blocking();
			g = slot.g.get();
		}
		check(g->recovered().position == applied5, "the reopen recovers the close's bundle: " + std::to_string(g->recovered().position));
		check(g->readable() && g->applied() == applied5, "the reopened group is readable at the close's position");
		for (int i = 6; i <= 8; ++i) { p.write(*g, "P|" + key(i) + "|v" + std::to_string(i)); }
		check(p.read(*g) == expected(8), "the reopened group applies past the index it recovered from");
		applied_at_shutdown = g->applied();
		check(pin.index().uncommitted(), "the last writes are not committed before the shutdown");
		pin.release();
		check(p.cat->stats().opens == 1, "the slot opened once: the group's close left it to the catalog");

		// A2, A3: the shutdown closes the idle group through close_payload, outside the lease.
		const auto r = p.shut();
		check(r.uncommitted.empty(), "the shutdown names no key");
		{
			std::lock_guard l(p.record_mu);
			check(p.payload_closes.load() == 1, "close_payload closed the group once: " + std::to_string(p.payload_closes.load()) + " " + p.payload_error);
		}
		check(p.cat->stats().payload_close_failures == 0, "no close_payload failed");
		check(p.closed_applied.load() == applied_at_shutdown, "the group closed at everything it applied");
		std::printf("group-catalog: %llu entries applied, closed by the catalog's shutdown\n", (unsigned long long)applied_at_shutdown);
	}
	// A2: a restarted process recovers the shutdown close's bundle, which covers everything applied.
	{
		process p(d);
		auto pin = p.pin();
		if (!pin) { return; }
		group_type& g = *pin.payload().g;
		check(g.recovered().position == applied_at_shutdown, "the bundle covers everything applied: P_b " + std::to_string(g.recovered().position) + " of " + std::to_string(applied_at_shutdown));
		check(p.read(g) == expected(8), "the restarted group reads every value");
		pin.release();
		(void)p.shut();
		check(p.payload_closes.load() == 1 && p.cat->stats().payload_close_failures == 0, "the restarted group closes too");
	}
}

constexpr std::uint64_t ms = 1'000'000;

// Writes entries from..to as P|<key>|v<i>.
bool write_range(process& p, group_type& g, int from, int to) {
	bool ok = true;
	for (int i = from; i <= to; ++i) { ok = p.write(g, "P|" + key(i) + "|v" + std::to_string(i)) && ok; }
	return ok;
}

// Proposes on `g` from its own thread until stopped: the loop finds `g` for each Committed notice.
class writer {
public:
	writer(process& p, group_type& g) : t_([this, &p, &g] {
		for (int i = 0; !stop_.load() && i < 4000; ++i) {
			auto w = start(p.loop_t.ref(), g.propose("P|" + key(1 + i % 10) + "|w" + std::to_string(i)));
			while (!w->done.load(std::memory_order_acquire)) { std::this_thread::sleep_for(std::chrono::microseconds(20)); }
			if (w->error) { return; }
			written_.fetch_add(1);
		}
	}) {}
	~writer() { stop(); }
	int stop() {
		stop_.store(true);
		if (t_.joinable()) { t_.join(); }
		return written_.load();
	}

private:
	std::atomic<bool> stop_{false};
	std::atomic<int> written_{0};
	std::thread t_;
};

// A recover that fences (strata's index lost under a bundle: P_b > P_H) fails the pin. The group
// left the loop's table and went idle before open_blocking threw, so the lane destroys it while
// another group on the loop writes; a second pin opens and fails the same way.
void failed_open() {
	disks d;
	const std::string k = key_of(4);
	{
		process p(d);
		auto pin = p.pin(strata::open_intent::create, k);
		if (!pin) { return; }
		write_range(p, *pin.payload().g, 1, 3);
		pin.release();
		(void)p.shut();  // its close publishes a bundle at 3
	}
	d.lose(k);
	process p(d);
	auto busy = p.pin(strata::open_intent::create, key_of(5));
	if (!busy) { return; }
	writer w(p, *busy.payload().g);
	for (int i = 0; i < 2; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));  // past the failed slot's backoff
		std::exception_ptr e;
		auto pin = p.pin(strata::open_intent::open, k, &e);
		if (!check(!pin, "the open over a lost index failed")) { break; }
		check(what(e).find("P_b > P_H") != std::string::npos, "the pin fails on the bundle past strata's head: " + what(e));
	}
	check(p.failed_opens.load() == 2, "two pins opened the group and failed: " + std::to_string(p.failed_opens.load()));
	check(w.stop() > 0, "the other group wrote meanwhile");
	busy.release();
	(void)p.shut();
	std::printf("failed-open: %d opens failed, none left a group in the loop's table\n", p.failed_opens.load());
}

// Groups open on the catalog's lane while another writes on the loop: the constructor leaves the
// loop's table alone and open() joins it on the loop.
void attach() {
	disks d;
	process p(d);
	auto first = p.pin(strata::open_intent::create, key_of(11));
	if (!first) { return; }
	writer w(p, *first.payload().g);
	std::vector<cat_t::pin> pins;
	for (cs::GroupId id = 12; id <= 16; ++id) {
		auto pk = p.pin(strata::open_intent::create, key_of(id));
		if (!pk) { break; }
		pins.push_back(std::move(pk));
	}
	const int written = w.stop();
	check(pins.size() == 5 && written > 0, "five groups opened while the first wrote: " + std::to_string(pins.size()) + ", " + std::to_string(written) + " writes");
	for (auto& x : pins) { check(p.write(*x.payload().g, "P|a|v1"), "a group opened beside a writing one writes"); }
	pins.clear();
	first.release();
	(void)p.shut();
	check(p.payload_closes.load() == 6, "the shutdown closed every group through close_payload: " + std::to_string(p.payload_closes.load()));
	std::printf("attach: %d writes on the first group while five opened\n", written);
}

// After a reopen: the close's bundle recovered, every value read, and one more write.
void reopens(process& p, const std::string& k, std::uint64_t bundle, int values) {
	auto pin = p.pin(strata::open_intent::open, k);
	if (!pin) { return; }
	auto& g = *pin.payload().g;
	check(g.recovered().position == bundle, "the reopen recovers the bundle at " + std::to_string(bundle) + ": " + std::to_string(g.recovered().position));
	const auto got = p.read(g);
	check(got == expected(values), "the reopened group reads every value: " + show(got));
	check(p.write(g, "P|n|v14"), "the reopened group writes");
}

// The sweep's eviction (the common close): an idle group with nothing uncommitted is closed through
// close_payload, whose checkpoint covers everything applied.
void sweep() {
	disks d;
	tuning t;
	t.sweep_ns = 20 * ms, t.close_idle_ns = 100 * ms;
	process p(d, t);
	const std::string k = key_of(21);
	std::uint64_t applied = 0;
	{
		auto pin = p.pin(strata::open_intent::create, k);
		if (!pin) { return; }
		write_range(p, *pin.payload().g, 1, 5);
		applied = pin.payload().g->applied();
		check(p.bare_commit(pin), "the host's commit: the sweep closes only an index with nothing uncommitted");
	}
	if (!p.wait_for("the sweep closes the idle group", [&] { return p.payload_calls.load() >= 1 && p.cat->table_size() == 0; })) { return; }
	{
		std::lock_guard l(p.record_mu);
		check(p.payload_closes.load() == 1 && p.cat->stats().payload_close_failures == 0, "the sweep closed the group through close_payload: " + p.payload_error);
	}
	check(p.closed_applied.load() == applied, "the sweep's close covered everything applied");
	check(p.cat->stats().pass_closes >= 1, "the close was the sweep's");
	reopens(p, k, applied, 5);
	(void)p.shut();
	std::printf("sweep: closed at %llu applied, reopened from its bundle\n", (unsigned long long)applied);
}

// The wake bound's close (D5): a demoted slot whose wakes fail `wake_bound` times is closed through
// close_payload, whose checkpoint covers everything applied.
void bound() {
	disks d;
	tuning t;
	t.defer_io = true;  // a demoted index owes a promote, which the wake runs
	process p(d, t);
	const std::string k = key_of(31);
	std::uint64_t applied = 0;
	std::uint32_t idx = 0;
	{
		auto pin = p.pin(strata::open_intent::create, k);
		if (!pin) { return; }
		write_range(p, *pin.payload().g, 1, 5);
		applied = pin.payload().g->applied();
		check(p.bare_commit(pin), "the host's commit: a segment for the wake to promote");
		idx = pin.where().slot;
	}
	step("the demotion");
	check(p.cat->begin_demote(idx), "the idle slot demotes");
	if (!p.wait_for("the slot is owed", [&] { return cat_t::state_of(p.cat->word_of(idx)) == strata::slot_state::owed; })) { return; }
	check(p.cat->index_of(idx)->promote_owed(), "the demoted index owes its promote");
	std::printf("bound: demoted with %llu maps, probe in %llu ms\n", (unsigned long long)p.cat->count_of(idx, strata::resource::maps), (unsigned long long)(p.cat->probe_at_of(idx) > p.exec->now() ? (p.cat->probe_at_of(idx) - p.exec->now()) / ms : 0));
	p.fail_segments.store(EMFILE);
	const bool reached = p.wait_for("the wakes fail to the bound", [&] {
		if (auto r = p.cat->try_pin(k); r.status == cat_t::try_status::pinned) { return !check(false, "a wake promoted with its segments failing"); }
		return p.cat->wake_failures_of(idx) >= 8;
	});
	p.fail_segments.store(0);
	if (!reached) { return; }
	std::printf("bound: the wakes failed to the bound; passes %llu, bound closes %llu\n", (unsigned long long)p.cat->stats().passes, (unsigned long long)p.cat->stats().bound_closes);
	if (!p.wait_for("the bound close begins", [&] { return p.cat->begin_bound_close(idx); })) {
		const std::uint64_t w = p.cat->word_of(idx), at = p.exec->now(), probe = p.cat->probe_at_of(idx);
		const auto st = p.cat->stats();
		std::string edges;
		for (unsigned i = 0; i < strata::slot_states; ++i) {
			for (unsigned j = 0; j < strata::slot_states; ++j) {
				if (st.edges[i][j]) { edges += " " + std::to_string(i) + ">" + std::to_string(j) + ":" + std::to_string(st.edges[i][j]); }
			}
		}
		check(false, "the slot at the bound: state " + std::to_string(unsigned(cat_t::state_of(w))) + ", pins " + std::to_string(cat_t::pins_of(w)) + ", failures " + std::to_string(p.cat->wake_failures_of(idx)) + ", probe in " + std::to_string(probe > at ? (probe - at) / ms : 0) + " ms, maps " + std::to_string(p.cat->count_of(idx, strata::resource::maps)) + "; passes " + std::to_string(st.passes) + ", bound closes " + std::to_string(st.bound_closes) + ", replaces " + std::to_string(st.replaces) + ", pass closes " + std::to_string(st.pass_closes) + ", close_payload calls " + std::to_string(p.payload_calls.load()) + ", edges" + edges);
		return;
	}
	if (!p.wait_for("the bound close ends", [&] { return p.payload_calls.load() >= 1 && p.cat->table_size() == 0; })) { return; }
	{
		std::lock_guard l(p.record_mu);
		check(p.payload_closes.load() == 1 && p.cat->stats().payload_close_failures == 0, "the bound close closed the group through close_payload: " + p.payload_error);
	}
	check(p.cat->stats().bound_closes == 1, "the close was the bound's");
	check(p.closed_applied.load() == applied, "the bound close covered everything applied");
	reopens(p, k, applied, 5);
	(void)p.shut();
	std::printf("bound: %u wakes failed, closed at %llu applied, reopened from its bundle\n", 8u, (unsigned long long)applied);
}

// A fenced index's replace close (F4): close_payload runs, the group's checkpoint cannot commit
// over the fenced index (counted), and the reopen replays everything from the log (D16).
void replace() {
	disks d;
	tuning t;
	t.sweep_ns = 20 * ms, t.close_idle_ns = 100 * ms;
	process p(d, t);
	const std::string k = key_of(41);
	{
		auto pin = p.pin(strata::open_intent::create, k);
		if (!pin) { return; }
		write_range(p, *pin.payload().g, 1, 3);
		check(p.bare_commit(pin), "the first commit");
		write_range(p, *pin.payload().g, 4, 5);
		d.of(k).add_fault({fl::op::sync, 1, EIO});  // the log's sync: a durability failure (SR4)
		const bool committed = p.bare_commit(pin);
		d.of(k).clear_faults();
		check(!committed && pin.index().fenced(), "a failed sync under the host's commit fences the index");
	}
	if (!p.wait_for("the sweep replaces the fenced index", [&] { return p.cat->stats().replaces >= 1 && p.cat->table_size() == 0; })) { return; }
	{
		std::lock_guard l(p.record_mu);
		check(p.payload_calls.load() == 1, "the replace close ran close_payload: " + std::to_string(p.payload_calls.load()));
		check(p.payload_closes.load() == 0 && p.cat->stats().payload_close_failures == 1, "the group's close over a fenced index fails, counted: " + p.payload_error);
	}
	reopens(p, k, 0, 5);
	(void)p.shut();
	std::printf("replace: the fenced index replaced, its group closed through close_payload, all replayed\n");
}

// The catalog's bare commit makes P_H durable over file-set bytes no sync covered (D169); power goes
// off, so the shutdown's checkpoint fails. After a crash, the index stands at P_H with no bundle,
// and D16's replay from P_b rewrites the file effects strata skips.
void bare_commit_crash(std::uint64_t seed) {
	disks d;
	const std::string k = key_of(51), at = "seed " + std::to_string(seed) + ": ";
	std::uint64_t applied = 0;
	{
		process p(d);
		auto pin = p.pin(strata::open_intent::create, k);
		if (!pin) { return; }
		write_range(p, *pin.payload().g, 1, 5);
		applied = pin.payload().g->applied();  // the five writes and the leader's entry
		check(pin.index().uncommitted() && p.bare_commit(pin), at + "the bare commit over unsynced file effects");
		d.of(k).power_off_after(0);
		pin.release();
		const auto r = p.shut();
		check(r.uncommitted.size() == 1 && r.uncommitted[0] == k, at + "the shutdown names the key whose close failed");
	}
	d.host.crash(seed);
	d.of(k).crash(seed * 3 + 1);
	{
		fl::io io(d.of(k));
		blob::index_type::options o;
		o.keyed = true;
		o.storage = &d.storage;
		auto ix = group_type::open_index(io, o, uuid);
		check(ix.applied().index == applied, at + "the bare commit's P_H is durable: " + std::to_string(ix.applied().index) + " of " + std::to_string(applied));
		ix.close();
	}
	process p(d);
	auto pin = p.pin(strata::open_intent::open, k);
	if (!pin) { return; }
	auto& g = *pin.payload().g;
	check(g.recovered().position == 0, at + "no bundle: " + std::to_string(g.recovered().position));
	const auto got = p.read(g);
	check(got == expected(5), at + "every acknowledged value after the crash: " + show(got));
	pin.release();
	(void)p.shut();
	std::lock_guard l(p.record_mu);
	check(p.payload_closes.load() == 1 && p.cat->stats().payload_close_failures == 0, at + "the recovered group closes through close_payload: " + p.payload_error);
}

void bare_commit_crashes() {
	for (std::uint64_t seed = 1; seed <= 16; ++seed) { bare_commit_crash(seed); }
	std::printf("bare-commit-crash: 16 seeds recovered every value\n");
}

}  // namespace

int main(int argc, char** argv) {
	watchdog w;
	dog = &w;
	const std::string only = argc > 1 ? argv[1] : "";
	const std::pair<const char*, void (*)()> cases[] = {
		{"borrow", borrow}, {"failed-open", failed_open}, {"attach", attach}, {"sweep", sweep}, {"bound", bound}, {"replace", replace}, {"bare-commit-crash", bare_commit_crashes},
	};
	bool ran = false;
	for (const auto& [name, fn] : cases) {
		if (!only.empty() && only != name) { continue; }
		ran = true;
		const int before = failures;
		fn();
		std::printf("group-catalog %s: %s\n", name, failures == before ? "ok" : "FAIL");
	}
	if (!ran) {
		std::fprintf(stderr, "group-catalog: no case %s\n", only.c_str());
		return 2;
	}
	step("exit");
	if (failures) {
		std::printf("group-catalog: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("group-catalog: ok\n");
	return 0;
}
