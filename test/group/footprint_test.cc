// What an idle open strata::group holds (quest group-core A6), on real directories: COUNT lone
// voters on one loop and one chore (inline schedulers, one thread), each with a strata index and
// a file set in their own PosixIO directories, one journal host for all. Phases: COUNT bare
// indexes (the index's own cost, as the group's holds it: one document, committed); COUNT groups
// open and idle after one write; the clock advanced an idle minute; COUNT closes; the groups
// destroyed. Per group it prints and checks descriptors, heap, threads, timers, coroutine frames
// and idle resumes: the group's own descriptors are its file set's directory alone (its files
// stay closed), it adds no thread, timer or frame, an idle minute resumes nothing, a close's
// checkpoint syncs are counted, and a closed group holds no descriptor beyond its two
// directories, which its host owns. `group_footprint_test [count [closes [root]]]`: count
// groups (64), the first `closes` of them closed (all; a close costs full flushes), the
// directories under `root` (a new one under the current directory, removed at the end).
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

struct member {
	std::unique_ptr<j::PosixIO> strata_io, files_io;
	std::unique_ptr<group_type> g;
};

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

	// COUNT groups, open and idle after one `quorum` write each, in waves that share turns.
	const meter before;
	const auto timers_before = loop_s.armed_timers();
	std::vector<member> members(count);
	const std::size_t wave = std::getenv("GROUP_FOOTPRINT_WAVE") ? std::stoull(std::getenv("GROUP_FOOTPRINT_WAVE")) : 1;
	for (std::size_t first = 0; first < count; first += wave) {
		const std::size_t last = std::min(count, first + wave);
		std::size_t opened = 0, written = 0;
		for (std::size_t i = first; i < last; ++i) {
			auto& m = members[i];
			const auto dir = root / "groups" / std::to_string(i);
			m.strata_io = directory(dir / "strata");
			m.files_io = directory(dir / "files");
			m.g = std::make_unique<group_type>(*lp, config(cs::GroupId(i + 1), storage), *m.strata_io, *m.files_io, app, chore_ref);
			task::spawn(loop_ref, m.g->open(), [&](std::exception_ptr e) {
				check(!e, "open: " + why(e));
				++opened;
			});
		}
		wait([&] { return opened == last - first; }, "open");
		for (std::size_t i = first; i < last; ++i) {
			task::spawn(loop_ref, members[i].g->propose("P|a|alpha"), [&](std::exception_ptr e, std::optional<sg::answer>) {
				check(!e, "the write: " + why(e));
				++written;
			});
		}
		wait([&] { return written == last - first; }, "write");
	}
	settle();
	const meter idle;
	const auto group_timers = lp->driver().armed_group_timers();
	report("group, open idle", before, idle, count);
	std::printf("timers: loop %zu (consensus's rows %zu), IO and turn watches in flight %zu\n", loop_s.armed_timers() - timers_before, group_timers, lp->driver().in_flight());
	std::printf("sizeof: group %zu, file_set %zu\n", sizeof(group_type), sizeof(sg::file_set));

	// The group's own: no thread, no timer past consensus's rows, no frame past its index's, no
	// IO in flight, and the file set's directory its only descriptor beside the index's.
	check(idle.threads_ == before.threads_, "an idle group adds no thread");
	check(loop_s.armed_timers() - timers_before <= group_timers, "an idle group arms no timer of its own");
	check(idle.frames - before.frames == index_frames * std::ptrdiff_t(count), "an idle group holds no coroutine frame beside its index's: " + std::to_string(idle.frames - before.frames) + " against " + std::to_string(index_frames) + " each");
	// The journal host may open a turn-log file meanwhile: at most two, never one per group.
	const long extra = idle.fds - before.fds - (index_fds + 1) * long(count);
	check(extra >= 0 && extra <= 2, "an idle group's descriptors are its index's and its file set's directory: " + std::to_string(idle.fds - before.fds) + " against " + std::to_string(index_fds) + " + 1 each");
	check(lp->driver().in_flight() == 0, "an idle group holds no IO or turn watch");
	for (auto& m : members) { check(!m.g->stalled() && !m.g->fenced() && m.g->applied() >= 1, "the group applied its write"); }

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

	// Closes: each checkpoints once (nothing past the write is applied) and closes its index.
	{
		const auto synced = syncs.load();
		const auto t0 = std::chrono::steady_clock::now();
		for (std::size_t i = 0; i < closes; ++i) {
			auto& m = members[i];
			bool closed = false;
			task::spawn(loop_ref, m.g->close(), [&](std::exception_ptr e) {
				check(!e, "close: " + why(e));
				closed = true;
			});
			wait([&] { return closed; }, "close");
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("close: %.2f syncs, %.3f ms per group (%zu closed)\n", double(syncs.load() - synced) / double(closes), ms / double(closes), closes);
	}
	settle();
	const meter closed;
	report("group, closed (all)", before, closed, count);
	// The open ones' descriptors, as measured idle, come off before the closed ones' are counted.
	const long open_fds = long(count - closes) * (index_fds + 1);
	check(closed.frames - before.frames == std::ptrdiff_t(count - closes) * index_frames, "a closed group holds no coroutine frame");
	for (auto& m : members) {
		m.g.reset();
		m.strata_io.reset();
		m.files_io.reset();
	}
	const meter gone;
	report("group, destroyed", before, gone, count);
	// What stays once every group and directory is gone is the journal host's (a turn-log file),
	// not per group; a closed group holds only the two directories its host handed it.
	check(gone.fds - before.fds <= 2, "destroyed groups and their directories hold nothing: " + std::to_string(gone.fds - before.fds));
	check(closed.fds - gone.fds - open_fds == long(2 * closes), "a closed group holds no descriptor of its own: " + std::to_string(closed.fds - gone.fds - open_fds) + " for " + std::to_string(closes) + " groups' two directories");

	stop.request(task::cancel_reason::shutdown);
	for (int i = 0; i < 1000 && !(turns_done && driver_done); ++i) { settle(); }
	lp.reset();
	host.reset();
	fs::remove_all(root);
	std::printf("footprint: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
