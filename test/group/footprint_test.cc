// What an idle open strata::group holds (quest group-core A6, D111), on real directories: COUNT
// lone voters on one loop and one chore (inline schedulers, one thread), each with one PosixIO
// directory holding its strata index and its file set under the set's prefix, one journal host
// for all. Phases: bare indexes (the index's own cost, as the group's holds it: one document,
// committed); bare consensus rows (Driver open, create, one write applied: consensus's and the
// journal's cost); COUNT groups open and idle after one write; the clock advanced an idle minute;
// closes; the groups destroyed. Per group it prints and checks descriptors, heap by owner,
// threads, timers, coroutine frames and idle resumes: an idle group's descriptors are its index's
// alone (the file set's files stay closed), its own heap is at most `own_heap`, it adds no
// thread, timer or frame, an idle minute resumes nothing, a close's checkpoint syncs are counted,
// and a closed group holds no descriptor: its directory closes with it. `group_footprint_test
// [count [closes [root]]]`: count groups (64), the first `closes` of them closed (all; a close
// costs full flushes), the directories under `root` (a new one under the current directory,
// removed at the end).
#include "blob_app.hh"

#include "completion/posix.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

// Live heap, requested bytes: every plain new in this program.
static std::atomic<std::int64_t> live_bytes{0};
void* operator new(std::size_t n) {
	auto* p = static_cast<std::size_t*>(std::malloc(n + 16));
	if (!p) { throw std::bad_alloc(); }
	p[0] = n;
	live_bytes.fetch_add(std::int64_t(n), std::memory_order_relaxed);
	return p + 2;
}
void operator delete(void* q) noexcept {
	if (!q) { return; }
	auto* p = static_cast<std::size_t*>(q) - 2;
	live_bytes.fetch_sub(std::int64_t(p[0]), std::memory_order_relaxed);
	std::free(p);
}
void operator delete(void* q, std::size_t) noexcept { operator delete(q); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* q) noexcept { operator delete(q); }
void operator delete[](void* q, std::size_t) noexcept { operator delete(q); }

namespace {

namespace cs = kronuz::consensus;
namespace j = kronuz::journal;
namespace sg = strata::group;
namespace fs = std::filesystem;
using group_type = sg::group<blob::app>;

int failures = 0;
bool check(bool ok, const std::string& what) {
	if (!ok) {
		++failures;
		std::cerr << "FAIL: " << what << '\n';
	}
	return ok;
}

std::string why(std::exception_ptr e) {
	if (!e) { return {}; }
	try {
		std::rethrow_exception(e);
	} catch (const std::exception& x) {
		return x.what();
	} catch (...) {}
	return "unknown";
}

long descriptors() {
	DIR* d = ::opendir("/dev/fd");
	if (!d) { return -1; }
	long n = 0;
	while (auto* e = ::readdir(d)) { n += e->d_name[0] != '.'; }
	::closedir(d);
	return n - 1;  // the scan's own
}
long threads() {
#ifdef __APPLE__
	thread_act_array_t list;
	mach_msg_type_number_t n = 0;
	if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) { return -1; }
	for (mach_msg_type_number_t i = 0; i < n; ++i) { mach_port_deallocate(mach_task_self(), list[i]); }
	vm_deallocate(mach_task_self(), vm_address_t(list), n * sizeof(thread_act_t));
	return long(n);
#else
	long n = 0;
	if (DIR* d = ::opendir("/proc/self/task")) {
		while (auto* e = ::readdir(d)) { n += e->d_name[0] != '.'; }
		::closedir(d);
	}
	return n;
#endif
}
double cpu_seconds() {
	rusage u{};
	::getrusage(RUSAGE_SELF, &u);
	return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

// Every sync PosixIO issues, counted; GROUP_FOOTPRINT_NOSYNC skips them (a footprint at scale,
// where the syncs alone would take the run's hour; they change no count but the time).
std::atomic<std::uint64_t> syncs{0};
const bool nosync = std::getenv("GROUP_FOOTPRINT_NOSYNC") != nullptr;
int counted_fsync(int fd) {
	++syncs;
	return nosync ? 0 : ::fsync(fd);
}
#ifdef __APPLE__
int counted_fcntl(int fd, int command) {
	++syncs;
	return nosync ? 0 : ::fcntl(fd, command);
}
#elif defined(__linux__)
int counted_writeback(int fd) {
	++syncs;
	return nosync ? 0 : ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
}
#endif
std::unique_ptr<j::PosixIO> directory(const fs::path& p) {
	fs::create_directories(p);
	auto io = std::make_unique<j::PosixIO>(p, 0600, j::PosixIOOptions{true, true});
#ifdef __APPLE__
	io->replace_sync_calls(&counted_fsync, &counted_fcntl);
#elif defined(__linux__)
	io->replace_sync_calls(&counted_fsync, &counted_writeback);
#else
	io->replace_sync_calls(&counted_fsync);
#endif
	return io;
}

struct meter {
	long fds = descriptors(), threads_ = threads();
	std::int64_t heap = live_bytes.load();
	std::ptrdiff_t frames = task::detail::live_frames();
};
void report(const char* what, const meter& a, const meter& b, std::size_t count) {
	const double n = double(count);
	std::printf("%-28s descriptors %+.2f  heap %+.0f B  threads %+ld  frames %+.2f  (per group, %zu)\n", what, double(b.fds - a.fds) / n, double(b.heap - a.heap) / n,
		b.threads_ - a.threads_, double(b.frames - a.frames) / n, count);
}

const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits admission{{256u << 20, 8192}, {256 * 1024, 3}, {64u << 20, 4}, 32, 3};

group_type::config config(cs::GroupId id, strata::byte_budget& storage) {
	group_type::config c;
	c.id = id;
	c.options.configuration.local = 1;
	c.options.configuration.voters = {1};
	c.options.configuration.cluster[0] = 'C';
	c.options.configuration.configuration[0] = 'G';
	c.options.admission = admission;
	c.options.limits.command_bytes = 4096;
	c.options.limits.rpc_bytes = 16384;
	c.options.limits.log_bytes = 64 * 1024;
	c.options.limits.log_entries = 64;
	c.options.limits.control_entries = 4;
	c.options.limits.rpc_entries = 8;
	c.options.timing = cs::Timing{20, 40, 100, 200, 60000};
	c.options.scheduling = cs::WorkerLimits{16, 128};
	for (unsigned b = 0; b < 8; ++b) { c.identity[b] = char(id >> (8 * b)); }
	c.identity[15] = 'F';
	c.uuid = {'f', 'o', 'o', 't'};
	c.strata.keyed = true;
	c.strata.storage = &storage;
	return c;
}

// The group's own heap beyond its index and its consensus row, per idle group: the object past
// its index (sizeof) and its file set's one entry. A reserved block or a kept config exceeds it.
constexpr std::int64_t own_heap = 1024;

// A consensus row alone, as the group's: it reports each committed range applied.
struct row final : sg::member {
	sg::loop* lp;
	cs::GroupId id;
	cs::Index applied = 0;
	row(sg::loop& l, cs::GroupId i) : lp(&l), id(i) { lp->attach(id, *this); }
	~row() override { lp->detach(id); }
	static task::task<void> report(sg::loop* l, cs::GroupId id, cs::Index through) {
		l->driver().applied(id, cs::Applied{through});
		co_return;
	}
	void committed(cs::Committed range) override {
		if (range.entries.empty()) { return; }
		applied = range.entries.back().index;
		task::spawn(lp->scheduler(), report(lp, id, applied));
	}
	void fenced(std::string) override {}
	void install_needed(cs::NodeId) override {}
};
task::task<void> open_row(sg::loop& lp, cs::GroupId id, cs::GroupOptions options, j::Identity identity) {
	lp.driver().open(id, std::move(options));
	co_await lp.driver().create(id, identity);
	co_await lp.driver().propose(id, "P|a|alpha");
}

}  // namespace

int main(int argc, char** argv) {
	const std::size_t count = argc > 1 ? std::stoull(argv[1]) : 64;
	const std::size_t closes = std::min<std::size_t>(count, argc > 2 ? std::stoull(argv[2]) : count);
	const fs::path root = argc > 3 ? fs::path(argv[3]) : fs::current_path() / ("group-footprint-" + std::to_string(::getpid()));
	fs::remove_all(root);
	fs::create_directories(root);
	strata::byte_budget storage{std::uint64_t(1) << 40};
	blob::app app;

	task::inline_scheduler loop_s, chore_s;
	auto loop_ref = task::scheduler_ref::of(loop_s), chore_ref = task::scheduler_ref::of(chore_s);
	const auto settle = [&] {
		std::size_t n = 0;
		for (std::size_t ran = 1; ran;) {
			ran = loop_s.run() + chore_s.run();
			n += ran;
		}
		return n;
	};
	const auto wait = [&](const std::function<bool()>& done, const char* what) {
		settle();
		for (int i = 0; i < 10000 && !done(); ++i) {
			loop_s.advance(10'000'000);
			settle();
		}
		if (!done()) {
			std::cerr << "stuck: " << what << '\n';
			std::exit(2);
		}
	};

	// The index's own cost, as the group's holds it: one document applied, committed, idle; per
	// index, from at most 1,000 of them.
	long index_fds = 0;  // per index
	std::ptrdiff_t index_frames = 0;
	std::int64_t index_heap = 0;
	{
		const std::size_t bare_count = std::min<std::size_t>(count, 1000);
		const meter before;
		std::vector<std::pair<std::unique_ptr<j::PosixIO>, std::optional<blob::index_type>>> bare(bare_count);
		for (std::size_t i = 0; i < bare_count; ++i) {
			bare[i].first = directory(root / "bare" / std::to_string(i));
			strata::basic_index_options<strata::document_layout> o;
			o.keyed = true;
			o.storage = &storage;
			bare[i].second.emplace(blob::index_type::create(*bare[i].first, o, {'b', 'a', 'r', 'e'}));
			blob::transaction t;
			blob::operation op;
			op.type = blob::operation::replace_key;
			op.key = op.doc.key = "a";
			op.doc.stored = blob::locator(0, 0, 5);
			t.ops.push_back(op);
			bare[i].second->apply(t, strata::files::position{1, 1});
			bare[i].second->commit();
		}
		const meter open;
		report("bare index, idle", before, open, bare_count);
		index_fds = (open.fds - before.fds) / long(bare_count);
		index_frames = (open.frames - before.frames) / std::ptrdiff_t(bare_count);
		index_heap = (open.heap - before.heap) / std::int64_t(bare_count);
		for (auto& [io, ix] : bare) {
			ix->close();
			ix.reset();
		}
		bare.clear();
		const meter after;
		report("bare index, closed", before, after, bare_count);
		fs::remove_all(root / "bare");
	}

	j::InlineCompletion inline_io;
	task::cancel_source stop;
	bool turns_done = false, driver_done = false;
	auto host_io = directory(root / "host");
	auto host = std::make_unique<j::Host>(std::shared_ptr<j::IO>(std::move(host_io)), j::Identity{'F', 'O', 'O', 'T'}, j::HostOptions{1, turn_options});
	auto lp = std::make_unique<sg::loop>(loop_ref, j::completion_ref::of(inline_io), *host, 0, 7);
	task::spawn(loop_ref, host->driver(0).run(j::completion_ref::of(inline_io), stop.token()), [&](std::exception_ptr) { turns_done = true; });
	task::spawn(loop_ref, lp->run(stop.token()), [&](std::exception_ptr) { driver_done = true; });
	settle();

	// Bare consensus rows, as the groups' (their own ids), in the groups' waves: consensus's and the
	// journal's cost per group, from at most 1,000. They stay open, idle, to the end.
	const std::size_t wave = std::getenv("GROUP_FOOTPRINT_WAVE") ? std::stoull(std::getenv("GROUP_FOOTPRINT_WAVE")) : 1;
	std::int64_t row_heap = 0;
	std::vector<std::unique_ptr<row>> rows;
	{
		const std::size_t row_count = std::min<std::size_t>(count, 1000);
		const meter before_rows;
		for (std::size_t first = 0; first < row_count; first += wave) {
			const std::size_t last = std::min(row_count, first + wave);
			std::size_t written = 0;
			for (std::size_t i = first; i < last; ++i) {
				const auto id = cs::GroupId(1'000'000'000 + i);
				rows.push_back(std::make_unique<row>(*lp, id));
				auto c = config(id, storage);
				task::spawn(loop_ref, open_row(*lp, id, c.options, c.identity), [&](std::exception_ptr e) {
					check(!e, "a row: " + why(e));
					++written;
				});
			}
			wait([&] { return written == last - first && std::all_of(rows.begin() + std::ptrdiff_t(first), rows.end(), [](const auto& r) { return r->applied >= 1; }); }, "rows");
		}
		settle();
		const meter open;
		report("consensus row, idle", before_rows, open, row_count);
		row_heap = (open.heap - before_rows.heap) / std::int64_t(row_count);
	}

	// COUNT groups, open and idle after one `quorum` write each, in waves that share turns.
	const meter before;
	const auto timers_before = loop_s.armed_timers();
	std::vector<std::unique_ptr<group_type>> members(count);
	const auto opening = std::chrono::steady_clock::now();
	for (std::size_t first = 0; first < count; first += wave) {
		const std::size_t last = std::min(count, first + wave);
		std::size_t opened = 0, written = 0;
		for (std::size_t i = first; i < last; ++i) {
			members[i] = std::make_unique<group_type>(*lp, config(cs::GroupId(i + 1), storage), directory(root / "groups" / std::to_string(i)), app, chore_ref);
			task::spawn(loop_ref, members[i]->open(), [&](std::exception_ptr e) {
				check(!e, "open: " + why(e));
				++opened;
			});
		}
		wait([&] { return opened == last - first; }, "open");
		for (std::size_t i = first; i < last; ++i) {
			task::spawn(loop_ref, members[i]->propose("P|a|alpha"), [&](std::exception_ptr e, std::optional<sg::answer>) {
				check(!e, "the write: " + why(e));
				++written;
			});
		}
		wait([&] { return written == last - first; }, "write");
	}
	const double open_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - opening).count();
	settle();
	const meter idle;
	const auto group_timers = lp->driver().armed_group_timers();
	report("group, open idle", before, idle, count);
	std::printf("open and first write: %.3f ms per group, %.2f syncs per group (%s)\n", open_ms / double(count), double(syncs.load()) / double(count + std::min<std::size_t>(count, 1000)), nosync ? "skipped" : "issued");
	std::printf("timers: loop %zu (consensus's rows %zu), IO and turn watches in flight %zu\n", loop_s.armed_timers() - timers_before, group_timers, lp->driver().in_flight());
	const std::int64_t group_heap = (idle.heap - before.heap) / std::int64_t(count), own = group_heap - index_heap - row_heap;
	std::printf("heap per idle group: %lld B = index %lld + consensus row and journal %lld + the group's own %lld (sizeof group %zu, of it the index %zu)\n", (long long)group_heap,
		(long long)index_heap, (long long)row_heap, (long long)own, sizeof(group_type), sizeof(std::optional<blob::index_type>));

	// The group's own: no thread, no timer past consensus's rows, no frame past its index's, no
	// IO in flight, no descriptor beside its index's, and little heap past its index and row.
	check(idle.threads_ == before.threads_, "an idle group adds no thread");
	check(loop_s.armed_timers() - timers_before <= group_timers, "an idle group arms no timer of its own");
	check(idle.frames - before.frames == index_frames * std::ptrdiff_t(count), "an idle group holds no coroutine frame beside its index's: " + std::to_string(idle.frames - before.frames) + " against " + std::to_string(index_frames) + " each");
	// The journal host may open a turn-log file meanwhile: at most two, never one per group.
	const long extra = idle.fds - before.fds - index_fds * long(count);
	check(extra >= 0 && extra <= 2, "an idle group's descriptors are its index's: " + std::to_string(idle.fds - before.fds) + " against " + std::to_string(index_fds) + " each");
	check(own <= own_heap, "an idle group's own heap is at most " + std::to_string(own_heap) + " B: " + std::to_string(own));
	check(lp->driver().in_flight() == 0, "an idle group holds no IO or turn watch");
	for (auto& g : members) { check(!g->stalled() && !g->fenced() && g->applied() >= 1, "the group applied its write"); }

	// An idle minute on the manual clock: what wakes.
	{
		const double cpu = cpu_seconds();
		std::size_t resumes = 0;
		for (int step = 0; step < 6000; ++step) {
			loop_s.advance(10'000'000);
			resumes += settle();
		}
		const double used = cpu_seconds() - cpu;
		std::printf("idle minute: %zu resumes, %.3f ms CPU (%.2f us per group)\n", resumes, used * 1e3, used * 1e6 / double(count));
		check(resumes == 0, "an idle minute resumes nothing: " + std::to_string(resumes));
	}

	// Closes: each checkpoints once (nothing past the write is applied), closes its index, and
	// lets its directory go.
	{
		const auto synced = syncs.load();
		const auto t0 = std::chrono::steady_clock::now();
		for (std::size_t i = 0; i < closes; ++i) {
			bool closed = false;
			task::spawn(loop_ref, members[i]->close(), [&](std::exception_ptr e) {
				check(!e, "close: " + why(e));
				closed = true;
			});
			wait([&] { return closed; }, "close");
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (closes) { std::printf("close: %.2f syncs, %.3f ms per group (%zu closed)\n", double(syncs.load() - synced) / double(closes), ms / double(closes), closes); }
	}
	settle();
	const meter closed;
	report("group, closed (all)", before, closed, count);
	// The open ones' descriptors, as measured idle, come off before the closed ones' are counted.
	const long open_fds = long(count - closes) * index_fds;
	check(closed.frames - before.frames == std::ptrdiff_t(count - closes) * index_frames, "a closed group holds no coroutine frame");
	const long closed_fds = closed.fds - before.fds - open_fds;
	check(closed_fds >= 0 && closed_fds <= 2, "a closed group holds no descriptor: " + std::to_string(closed_fds) + " for " + std::to_string(closes) + " groups");
	for (auto& g : members) { g.reset(); }
	const meter gone;
	report("group, destroyed", before, gone, count);
	// What stays once every group is gone is the journal host's (a turn-log file), not per group.
	check(gone.fds - before.fds <= 2, "destroyed groups hold nothing: " + std::to_string(gone.fds - before.fds));

	stop.request(task::cancel_reason::shutdown);
	for (int i = 0; i < 1000 && !(turns_done && driver_done); ++i) { settle(); }
	rows.clear();
	lp.reset();
	host.reset();
	fs::remove_all(root);
	std::printf("footprint: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
