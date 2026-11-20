// What an idle open strata::group holds (quest group-core A6, D111), on real directories: COUNT
// lone voters on one loop and one chore (inline schedulers, one thread), each with one PosixIO
// directory holding its strata index and its file set under the set's prefix, one journal host for
// all. Phases: bare indexes (the index's own cost, as the group's holds it: one document, created
// deferred, not committed); bare consensus rows (Driver open, create, one write applied:
// consensus's and the journal's cost); COUNT groups open and idle after one write; the clock
// advanced an idle minute; closes; the closed groups destroyed, then the open ones fenced and
// destroyed; the bare rows checkpointed and closed (consensus's and the journal's part of a closed
// group). Per group it prints and checks descriptors, heap by owner, threads, timers, coroutine
// frames and idle resumes: an idle group's descriptors are its index's alone (the file set's files
// stay closed), its own heap is at most `own_heap`, it adds no thread, timer or frame, an idle
// minute resumes nothing, a close's checkpoint syncs are counted, a closed group holds no
// descriptor or frame (its directory and index close with it) and past consensus's closed row only
// its object, and a destroyed one leaves nothing of ours. Closed and destroyed rows are per closed
// group.
// `group_footprint_test [count [closes [root]]]`: count groups (64), the first `closes` of them
// closed (all; a close costs full flushes), the directories under `root` (a new one under the
// current directory, removed at the end). GROUP_FOOTPRINT_SIZES=1 prints live blocks by size, the
// groups' window against the rows'; GROUP_FOOTPRINT_TRACE=SIZE the stacks of live blocks of SIZE.
#include "blob_app.hh"

#include "completion/posix.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

// Live heap, requested bytes: every plain new in this program; live blocks by requested size too
// (the last slot holds the larger ones), for GROUP_FOOTPRINT_SIZES.
static std::atomic<std::int64_t> live_bytes{0};
constexpr std::size_t size_slots = 16384;
static std::atomic<std::int32_t> live_sizes[size_slots + 1];
// GROUP_FOOTPRINT_TRACE=SIZE: the stacks of live blocks of that size, by the window they came in.
struct traced {
	void* at = nullptr;
	int window = 0, depth = 0;
	void* frames[32];
};
static traced traces[256];
static int trace_window = 0;
static std::size_t trace_size() {
	static const std::size_t size = std::getenv("GROUP_FOOTPRINT_TRACE") ? std::strtoull(std::getenv("GROUP_FOOTPRINT_TRACE"), nullptr, 10) : 0;
	return size;
}
void* operator new(std::size_t n) {
	auto* p = static_cast<std::size_t*>(std::malloc(n + 16));
	if (!p) { throw std::bad_alloc(); }
	p[0] = n;
	live_bytes.fetch_add(std::int64_t(n), std::memory_order_relaxed);
	live_sizes[std::min(n, size_slots)].fetch_add(1, std::memory_order_relaxed);
	if (n == trace_size()) {
		for (auto& t : traces) {
			if (!t.at) {
				t = {p + 2, trace_window, 0, {}};
				t.depth = ::backtrace(t.frames, 32);
				break;
			}
		}
	}
	return p + 2;
}
void operator delete(void* q) noexcept {
	if (!q) { return; }
	auto* p = static_cast<std::size_t*>(q) - 2;
	live_bytes.fetch_sub(std::int64_t(p[0]), std::memory_order_relaxed);
	live_sizes[std::min(p[0], size_slots)].fetch_sub(1, std::memory_order_relaxed);
	if (p[0] == trace_size()) {
		for (auto& t : traces) {
			if (t.at == q) { t.at = nullptr; }
		}
	}
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
	c.options.limits.rpc_bytes = 8192;  // a batch fits one 16 KiB turn-log segment (DRIVER.md)
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
// Bare indexes and rows measured: their cost per unit is the same at any count.
constexpr std::size_t sampled = 256;
// What a destroyed group may leave of ours, per group: the loop's bucket array, grown for every
// group and not shrunk. A member entry kept (32 B) exceeds it.
constexpr std::int64_t gone_slack = 16;

// A consensus row alone, as the group's: it reports each committed range applied.
struct row final : sg::member {
	sg::loop* lp;
	cs::GroupId id;
	cs::Index applied = 0;
	cs::Term term = 0;
	row(sg::loop& l, cs::GroupId i) : lp(&l), id(i) { lp->attach(id, *this); }
	~row() override { lp->detach(id); }
	static task::task<void> report(sg::loop* l, cs::GroupId id, cs::Index through) {
		l->driver().applied(id, cs::Applied{through});
		co_return;
	}
	void committed(cs::Committed range) override {
		if (range.entries.empty()) { return; }
		applied = range.entries.back().index;
		term = range.entries.back().term;
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
// Closed as a group closes: one checkpoint covering the write, then the Driver's close.
task::task<void> close_row(sg::loop& lp, const row& r) {
	struct source {
		std::string bytes;
		std::size_t at = 0;
		cs::Index through;
		cs::Term term;
		cs::CaptureMetadata metadata() const { return {0, through, term, 0}; }
		std::size_t read(std::span<char> out) {
			const auto n = std::min(out.size(), bytes.size() - at);
			std::copy_n(bytes.data() + at, n, out.data());
			at += n;
			return n;
		}
	};
	co_await lp.driver().checkpoint(r.id, 3, source{"row", 0, r.applied, r.term});
	co_await lp.driver().close(r.id);
}

}  // namespace

// footprint_owners.py stops here, at the phase GROUP_FOOTPRINT_PAUSE names, to read the heap.
extern "C" __attribute__((noinline)) void footprint_phase(const char* name) { asm volatile("" ::"r"(name)); }
// GROUP_FOOTPRINT_SIZES: live blocks by size at four phases, no heap of their own; `sizes()`
// prints, per size, the groups' window (before to destroyed) against the bare rows' (rows-before
// to before, then destroyed to rows-closed): what a destroyed group leaves past a closed row.
constexpr std::array<const char*, 4> sized{"rows-before", "before", "destroyed", "rows-closed"};
static std::int32_t snapshots[sized.size()][size_slots + 1];
static void phase(const char* name) {
	if (const char* at = std::getenv("GROUP_FOOTPRINT_PAUSE"); at && std::string_view(at) == name) { footprint_phase(name); }
	for (std::size_t k = 0; k < sized.size(); ++k) {
		if (std::string_view(sized[k]) == name) { trace_window = int(k) + 1; }
	}
	if (trace_size() && std::string_view(name) == "destroyed") {
		for (const auto& t : traces) {
			if (!t.at) { continue; }
			std::fprintf(stderr, "live %zu B block from the window after %s:\n", trace_size(), t.window ? sized[std::size_t(t.window) - 1] : "the start");
			::backtrace_symbols_fd(t.frames, t.depth, 2);
		}
	}
	if (!std::getenv("GROUP_FOOTPRINT_SIZES")) { return; }
	for (std::size_t k = 0; k < sized.size(); ++k) {
		if (std::string_view(sized[k]) != name) { continue; }
		for (std::size_t n = 0; n <= size_slots; ++n) { snapshots[k][n] = live_sizes[n].load(std::memory_order_relaxed); }
	}
}
static void sizes(std::size_t closes) {
	if (!std::getenv("GROUP_FOOTPRINT_SIZES")) { return; }
	const auto& [rows_before, before, destroyed, rows_closed] = snapshots;
	std::printf("live blocks by size, groups' window against the rows' (%zu each; sizes over %zu B in the last):\n", closes, size_slots);
	for (std::size_t n = 0; n <= size_slots; ++n) {
		const std::int64_t groups = destroyed[n] - before[n], rows = (before[n] - rows_before[n]) + (rows_closed[n] - destroyed[n]);
		if (groups != rows) { std::printf("  %6zu B  groups %+lld  rows %+lld  difference %+lld B\n", n, (long long)groups, (long long)rows, (long long)((groups - rows) * std::int64_t(n))); }
	}
}

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

	// Both schedulers' ready queues warmed first: task's inline_scheduler queues in a std::deque
	// that keeps its 4 KiB blocks once grown, the host's, not a group's or a row's. Cold, the
	// chore's first two blocks fell in the groups' window (no bare row runs on the chore).
	for (auto s : {loop_ref, chore_ref}) {
		for (int i = 0; i < 1024; ++i) {
			task::spawn(s, []() -> task::task<void> { co_return; }(), [](std::exception_ptr) {});
		}
	}
	settle();

	// The index's own cost, as the group's holds it: created deferred (D112), one document applied,
	// not yet committed (the group commits at its first checkpoint), idle; per
	// index, from at most `sampled` of them.
	long index_fds = 0;  // per index
	std::ptrdiff_t index_frames = 0;
	std::int64_t index_heap = 0, index_closed_heap = 0;
	{
		const std::size_t bare_count = std::min<std::size_t>(count, sampled);
		const meter before;
		std::vector<std::pair<std::unique_ptr<j::PosixIO>, std::optional<blob::index_type>>> bare(bare_count);
		for (std::size_t i = 0; i < bare_count; ++i) {
			bare[i].first = directory(root / "bare" / std::to_string(i));
			strata::basic_index_options<strata::document_layout> o;
			o.keyed = true;
			o.storage = &storage;
			bare[i].second.emplace(blob::index_type::create_deferred(*bare[i].first, o, {'b', 'a', 'r', 'e'}));
			blob::transaction t;
			blob::operation op;
			op.type = blob::operation::replace_key;
			op.key = op.doc.key = "a";
			op.doc.stored = blob::locator(0, 0, 5);
			t.ops.push_back(op);
			bare[i].second->apply(t, strata::files::position{1, 1});
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
		decltype(bare)().swap(bare);  // its storage too: what the closed indexes keep is what stays past it
		phase("bare-closed");
		const meter after;
		report("bare index, closed", before, after, bare_count);
		index_closed_heap = (after.heap - before.heap) / std::int64_t(bare_count);
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
	// journal's cost per group, from at most `sampled`. They stay open, idle, to the end.
	const std::size_t wave = std::getenv("GROUP_FOOTPRINT_WAVE") ? std::stoull(std::getenv("GROUP_FOOTPRINT_WAVE")) : 1;
	std::int64_t row_heap = 0;
	std::vector<std::unique_ptr<row>> rows;
	{
		const std::size_t row_count = std::min<std::size_t>(count, sampled);
		rows.reserve(row_count);
		phase("rows-before");
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
	std::vector<std::unique_ptr<group_type>> members(count);
	phase("before");
	const meter before;
	const auto timers_before = loop_s.armed_timers();
	const auto opening = std::chrono::steady_clock::now();
	const auto synced_open = syncs.load();
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
	phase("idle");
	const meter idle;
	const auto group_timers = lp->driver().armed_group_timers();
	report("group, open idle", before, idle, count);
	std::printf("open and first write: %.3f ms per group, %.2f syncs per group (%s)\n", open_ms / double(count), double(syncs.load() - synced_open) / double(count), nosync ? "skipped" : "issued");
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
	// Per closed group: what the closes added to the idle groups, plus an idle group's holdings.
	phase("closed");
	const meter closed;
	const auto per_closed = [&](const meter& a, const meter& b) {
		const double n = double(closes);
		return std::array<double, 3>{double(b.fds - a.fds) / n, double(b.heap - a.heap) / n, double(b.frames - a.frames) / n};
	};
	const double idle_fds = double(idle.fds - before.fds) / double(count), idle_frames = double(idle.frames - before.frames) / double(count);
	const auto c = per_closed(idle, closed);
	const double closed_heap = double(group_heap) + c[1];
	if (closes) {
		std::printf("%-28s descriptors %+.2f  heap %+.0f B  threads %+ld  frames %+.2f  (per group, %zu of %zu)\n", "group, closed", idle_fds + c[0], closed_heap, closed.threads_ - idle.threads_,
			idle_frames + c[2], closes, count);
	}
	// The open ones' descriptors, as measured idle, come off before the closed ones' are counted.
	const long open_fds = long(count - closes) * index_fds;
	check(closed.frames - before.frames == std::ptrdiff_t(count - closes) * index_frames, "a closed group holds no coroutine frame");
	const long closed_fds = closed.fds - before.fds - open_fds;
	check(closed_fds >= 0 && closed_fds <= 2, "a closed group holds no descriptor: " + std::to_string(closed_fds) + " for " + std::to_string(closes) + " groups");

	// The closed groups destroyed: what stays is consensus's and the journal's closed row (below)
	// and anything of ours outside the object.
	for (std::size_t i = 0; i < closes; ++i) {
		members[i].reset();
		check(!lp->find(cs::GroupId(i + 1)), "a destroyed group leaves its loop's table");
	}
	phase("destroyed");
	const meter gone;
	const double gone_heap = closed_heap + per_closed(closed, gone)[1];
	if (closes) { std::printf("%-28s descriptors %+.2f  heap %+.0f B  (per group, %zu)\n", "group, closed and destroyed", idle_fds + per_closed(idle, gone)[0], gone_heap, closes); }
	// What stays once every group is gone is the journal host's (a turn-log file), not per group.
	check(gone.fds - before.fds - open_fds <= 2, "destroyed groups hold nothing: " + std::to_string(gone.fds - before.fds - open_fds));
	// The open ones end as in a stopped process (the lifetime's other case): fenced, then
	// destroyed. Their consensus rows stay open in the Driver until `Driver::close` or the Driver ends.
	for (std::size_t i = closes; i < count; ++i) { members[i]->fenced("the process stopped"); }
	settle();
	for (std::size_t i = closes; i < count; ++i) { members[i].reset(); }
	const meter gone_open;
	if (count > closes) {
		std::printf("%-28s heap %+.0f B  (per group, %zu; consensus's open row, %lld, stays in the Driver)\n", "group, fenced and destroyed", double(gone_open.heap - gone.heap) / double(count - closes) + double(group_heap),
			count - closes, (long long)row_heap);
	}

	// As many bare rows as groups closed, checkpointed, closed and destroyed: consensus's and the
	// journal's part of a closed group.
	if (closes) {
		const std::size_t row_count = std::min(rows.size(), closes);
		const meter open_rows;
		for (std::size_t i = 0; i < row_count; ++i) {
			bool done = false;
			task::spawn(loop_ref, close_row(*lp, *rows[i]), [&](std::exception_ptr e) {
				check(!e, "a row's close: " + why(e));
				done = true;
			});
			wait([&] { return done; }, "row close");
			rows[i].reset();
		}
		settle();
		phase("rows-closed");
		const meter closed_rows;
		const double row_closed = double(row_heap) + double(closed_rows.heap - open_rows.heap) / double(row_count);
		std::printf("%-28s descriptors %+.2f  heap %+.0f B  frames %+.2f  (per group, %zu)\n", "consensus row, closed", double(closed_rows.fds - open_rows.fds) / double(row_count), row_closed,
			double(closed_rows.frames - open_rows.frames) / double(row_count), row_count);
		// Ours past a closed index and the closed row: the object while it lives, nothing once gone.
		const double ours_closed = closed_heap - row_closed - double(index_closed_heap), ours_gone = gone_heap - row_closed - double(index_closed_heap);
		std::printf("heap per closed group: %.0f B = consensus row and journal %.0f + index %lld + the group's own %.0f (the object, sizeof %zu); destroyed: %.0f B = consensus row and journal %.0f + index %lld + ours %.0f\n",
			closed_heap, row_closed, (long long)index_closed_heap, ours_closed, sizeof(group_type), gone_heap, row_closed, (long long)index_closed_heap, ours_gone);
		check(ours_closed <= double(sizeof(group_type) + own_heap), "a closed group holds at most its object and own heap: " + std::to_string(ours_closed));
		check(ours_gone <= double(gone_slack), "a destroyed group leaves nothing of ours: " + std::to_string(ours_gone) + " B");
	}

	sizes(closes);
	stop.request(task::cancel_reason::shutdown);
	for (int i = 0; i < 1000 && !(turns_done && driver_done); ++i) { settle(); }
	rows.clear();
	lp.reset();
	host.reset();
	fs::remove_all(root);
	std::printf("footprint: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
