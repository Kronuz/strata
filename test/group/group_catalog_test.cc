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
#include "blob_app.hh"
#include "host_io.hh"
#include "oracle.hh"

#include "faultlab/faultfs.h"
#include "strata/core/catalog.hh"
#include "strata/core/counted_io.hh"
#include "strata/core/owner.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

group_type::config config() {
	group_type::config c;
	c.id = gid;
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
	c.identity = j::Identity{'C', 'A', 'T'};
	return c;
}

// What survives a restart: the journal host's directory, the index's, the storage budget.
struct disks {
	fl::filesystem host, dir;
	strata::byte_budget storage{std::uint64_t(1) << 30};
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
	std::atomic<int> payload_closes{0};
	std::atomic<std::uint64_t> closed_applied{0};
	std::mutex record_mu;
	std::string payload_error;

	explicit process(disks& disk) : d(disk) {
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
		h.resolve = [this](const cat_t::opening& op) { return strata::counted_io::make(op.counts, [this] { return std::make_unique<fl::io>(d.dir); }); };
		h.open_index = [](j::IO& io, const blob::index_type::options& o, const cat_t::opening&) { return group_type::open_index(io, o, uuid); };
		// On the catalog's lane: the group's open waits on the loop and the group's chore, both elsewhere.
		h.open_payload = [this](const cat_t::opening& op, blob::index_type& ix, std::optional<hosted>& out) {
			out.emplace();
			out->io = op.io;
			out->g = std::make_unique<group_type>(*lp, config(), *op.io, ix, app, chore_t.ref());
			out->g->open_blocking();
		};
		// The group's close, outside the write lease, before the index's (D169).
		h.close_payload = [this](hosted& p, blob::index_type&) {
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
		h.sweep_period_ns = 3'600'000'000'000ull;  // no sweep closes in this test: shutdown's alone
		cat = std::make_unique<cat_t>(std::move(h));
		cat->start();
	}

	// Pins `key` from the test's thread, waiting for its open.
	cat_t::pin pin(strata::open_intent intent = strata::open_intent::open) {
		step("pin");
		for (;;) {
			auto r = cat->try_pin("g", strata::pin_kind::request, true, intent);
			if (r.status == cat_t::try_status::pinned) { return std::move(r.p); }
			if (r.status == cat_t::try_status::failed) {
				check(false, "the pin's open: " + what(r.error));
				return {};
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
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

void run() {
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

}  // namespace

int main() {
	watchdog w;
	dog = &w;
	run();
	step("exit");
	if (failures) {
		std::printf("group-catalog: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("group-catalog: ok\n");
	return 0;
}
