// counted_io over a resource budget (docs/design/cold-tier.md D6; S3; CT-R8, CT-R27, CT-R32,
// CT-R37): every descriptor, vnode and map is reserved before the system call and returned with
// the kernel object, whatever fails; a cap refuses with too_many_open before the kernel is asked;
// two catalogs on one budget together stay under it; an EMFILE the cap did not predict lowers the
// soft cap, and the lowered cap survives the passes that follow. The kernel is a model
// (counted_hooks.hh); one case runs PosixIO against the real descriptor table.
#include "counted_hooks.hh"

#include "cold_hooks.hh"
#include "completion/posix.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>

namespace {

constexpr resource M = resource::maps, D = resource::descriptors, V = resource::vnodes;
using std::chrono::seconds;

// A faultfs, the kernel model, a budget and one catalog's counted IO.
struct fx {
	faultfs::filesystem fs;
	faultfs::io inner{fs};
	kernel kern;
	resource_counts catalog, slot;
	std::chrono::steady_clock::time_point now{seconds(1000)};
	resource_budget budget;
	std::unique_ptr<counted_io> io;

	static resource_options opts(fx* self, std::uint64_t fds, std::uint64_t maps, std::uint64_t vnodes, std::uint64_t reserve) {
		resource_options o;
		o.system = test_system(fds, maps, vnodes);
		o.descriptor_reserve = reserve;
		o.clock = [self] { return self->now; };
		return o;
	}
	// 256 descriptors (soft 144, hard 182 with the reserve of 64), and the given map and vnode tables.
	explicit fx(std::uint64_t fds = 256, std::uint64_t maps = 1000, std::uint64_t vnodes = 1000, std::uint64_t reserve = 64, bool build = true)
		: budget(opts(this, fds, maps, vnodes, reserve)) {
		fs.map_files(true);
		{
			auto f = inner.create_exclusive("a");
			f->write_at(0, std::string(4096, 'a'));
			f->sync();
		}
		if (build) { io = make_io(); }
	}
	io_counts counts() { return {&budget, &catalog, &slot}; }
	std::unique_ptr<counted_io> make_io() {
		return counted_io::make(counts(), [&] { return probe_io::make(kern, inner); });
	}
	std::uint64_t used(resource r) const { return budget.used(r); }
	// The budget's counts equal the kernel's (T7), and the holders' counts the budget's.
	bool agrees() const {
		return std::int64_t(budget.used(D)) == kern.fds.load() && std::int64_t(budget.used(M)) == kern.maps.load() &&
			   catalog.get(D) == budget.used(D) && catalog.get(M) == budget.used(M) && catalog.get(V) == budget.used(V);
	}
	bool empty() const { return budget.used(D) == 0 && budget.used(M) == 0 && budget.used(V) == 0 && kern.fds == 0 && kern.maps == 0; }
};

template <class F>
int code_of(F&& f) {
	try {
		f();
		return 0;
	} catch (const too_many_open&) {
		return -EMFILE;
	} catch (const std::system_error& e) {
		return e.code().value();
	} catch (const std::logic_error&) {
		return -2;
	} catch (const std::bad_alloc&) {
		return -1;
	}
}

// A barrier for threads that stop at quiescent points.
class sync_point {
public:
	explicit sync_point(int n) : n_(n) {}
	void wait() {
		std::unique_lock l(m_);
		const unsigned g = gen_;
		if (++waiting_ == n_) {
			waiting_ = 0;
			++gen_;
			cv_.notify_all();
		} else {
			cv_.wait(l, [&] { return g != gen_; });
		}
	}

private:
	std::mutex m_;
	std::condition_variable cv_;
	int n_, waiting_ = 0;
	unsigned gen_ = 0;
};

// --- CT-R32: the directory's descriptor ------------------------------------------------------------

// The directory descriptor and its vnode are reserved before the inner IO is built, returned if
// building throws, and released with the IO. The descriptor cap is reached by opens alone.
void directory_descriptor() {
	fx f(256, 1000, 1000, 64, false);
	int built = 0;
	{
		auto io = counted_io::make(f.counts(), [&] {
			++built;
			CHECK(f.used(D) == 1 && f.used(V) == 1 && f.kern.fds == 0);  // reserved, not yet opened
			return probe_io::make(f.kern, f.inner);
		});
		CHECK(f.used(D) == 1 && f.used(V) == 1 && f.kern.fds == 1 && f.agrees());
	}
	CHECK(f.empty());

	// Building throws: both go back, and an EMFILE is external pressure.
	f.kern.fail_fd = EIO;
	CHECK(code_of([&] { (void)f.make_io(); }) == EIO);
	CHECK(f.empty() && f.budget.external(D) == resource_budget::unlimited);
	f.kern.fail_fd = EMFILE;
	CHECK(code_of([&] { (void)f.make_io(); }) == EMFILE);
	CHECK(f.empty() && f.budget.external(D) == 0);
	f.kern.fail_fd = 0;

	// Nothing but directories: 182 IOs fit; the next is refused before the factory runs.
	fx g(256, 1000, 1000, 64, false);
	const std::uint64_t hard = g.budget.hard(D);
	CHECK(hard == 182);
	std::vector<std::unique_ptr<counted_io>> ios;
	int calls = 0;
	for (std::uint64_t i = 0; i < hard; ++i) {
		ios.push_back(counted_io::make(g.counts(), [&] {
			++calls;
			return probe_io::make(g.kern, g.inner);
		}));
	}
	CHECK(calls == int(hard) && g.kern.fds == std::int64_t(hard) && g.agrees());
	CHECK(code_of([&] {
		(void)counted_io::make(g.counts(), [&] {
			++calls;
			return probe_io::make(g.kern, g.inner);
		});
	}) == -EMFILE);
	CHECK(calls == int(hard) && g.kern.fd_calls == hard && g.used(D) == hard && g.used(V) == hard);
	CHECK(g.kern.refused == 0 && g.kern.fd_peak == std::int64_t(hard));
	ios.pop_back();  // one slot is free again
	(void)counted_io::make(g.counts(), [&] { return probe_io::make(g.kern, g.inner); });
	ios.clear();
	CHECK(g.empty());

	// A leaked IO keeps its reservation: the kernel still holds the descriptor.
	fx h(256, 1000, 1000, 64, false);
	counted_io* leaked = counted_io::make(h.counts(), [&] { return probe_io::make(h.kern, h.inner); }).release();
	CHECK(h.used(D) == 1 && h.kern.fds == 1);
	delete leaked;
	CHECK(h.empty());
	std::printf("directory descriptor: ok\n");
}

// --- CT-R8: what each object holds ----------------------------------------------------------------

void lifetimes() {
	fx f;
	CHECK(f.used(D) == 1 && f.used(V) == 1 && f.used(M) == 0);
	{
		auto a = f.io->open_existing("a");
		CHECK(f.used(D) == 2 && f.used(V) == 2 && f.slot.get(D) == 2 && f.agrees());
		auto b = f.io->create_exclusive("b");
		CHECK(f.used(D) == 3 && f.used(V) == 3 && f.agrees());
		auto m1 = a->map();
		auto m2 = a->map();
		CHECK(m1 && m2 && m1->bytes().size() == 4096 && f.used(M) == 2 && f.used(V) == 3 && f.agrees());
		// The file goes; the vnode stays while a mapping of it does; its descriptor is gone.
		a.reset();
		CHECK(f.used(D) == 2 && f.used(V) == 3 && f.kern.fds == 2 && f.used(M) == 2 && f.agrees());
		m1.reset();
		CHECK(f.used(V) == 3 && f.used(M) == 1);
		m2.reset();
		CHECK(f.used(V) == 2 && f.used(M) == 0 && f.agrees());
		b.reset();
		CHECK(f.used(D) == 1 && f.used(V) == 1);
	}
	{
		// close() releases the descriptor at once; the file object and its mappings keep the vnode.
		auto a = f.io->open_existing("a");
		auto m = a->map();
		a->close();
		CHECK(f.used(D) == 1 && f.kern.fds == 1 && f.used(V) == 2 && f.used(M) == 1 && f.agrees());
		a.reset();
		CHECK(f.used(V) == 2);
		m.reset();
		CHECK(f.used(V) == 1);
		// A backend with no explicit close keeps the descriptor and the reservation.
		f.kern.close_supported = false;
		auto b = f.io->open_existing("a");
		CHECK(code_of([&] { b->close(); }) != 0);
		CHECK(f.used(D) == 2 && f.kern.fds == 2 && f.agrees());
		b.reset();
		f.kern.close_supported = true;
		CHECK(f.used(D) == 1);
	}
	{
		// A backend that cannot map returns null and nothing stays reserved.
		f.fs.map_files(false);
		auto a = f.io->open_existing("a");
		CHECK(a->map() == nullptr && f.used(M) == 0 && f.kern.maps == 0);
		f.fs.map_files(true);
	}
	{
		// The owner lock file and a scan cursor hold a descriptor; the directory lock holds none.
		auto lock = f.io->acquire_owner(true);
		CHECK(f.used(D) == 2 && f.used(V) == 2 && f.agrees());
		auto dir = f.io->acquire_directory_owner();
		CHECK(f.used(D) == 2);
		auto cur = f.io->scan_directory();
		CHECK(f.used(D) == 3 && f.used(V) == 2 && cur->next());
		lock.reset();
		dir.reset();
		CHECK(f.used(D) == 2 && f.used(V) == 1);
		cur.reset();
		CHECK(f.used(D) == 1);
	}
	{
		// A reclaim candidate that is absent returns null; one that exists is a counted file.
		auto c = f.io->open_reclaim_candidate("a");
		CHECK(c && f.used(D) == 2);
		c.reset();
		CHECK(code_of([&] { (void)f.io->open_reclaim_candidate("missing"); }) == ENOENT && f.used(D) == 1 && f.agrees());
	}
	{
		// sync_files and the ordering calls reach the inner files through their wrappers.
		auto w = f.io->create_exclusive("w");
		w->write_at(0, "x");
		kronuz::journal::File* files[] = {w.get()};
		f.io->sync_files(files);
		f.io->order_files_and_directory(files);
		f.io->sync_files_and_directory(files);
		f.io->sync_directory();
		f.io->replace("w", "w2");
		f.io->remove("w2");
	}
	f.io.reset();
	CHECK(f.empty());
	CHECK(f.catalog.get(D) == 0 && f.catalog.get(V) == 0 && f.slot.get(M) == 0);
	std::printf("lifetimes: ok\n");
}

// --- S3: each cap driven to its hard value --------------------------------------------------------

// The call that passes a cap throws too_many_open before any system call, and the kernel's tables
// stay below the caps.
void caps_by_io() {
	for (const resource r : {D, M, V}) {
		fx f(256, 100, 100);  // descriptors 182, maps 90, vnodes 60
		auto a = f.io->open_existing("a");
		const std::uint64_t free = f.budget.hard(r) - f.used(r);
		auto pad = f.budget.reserve(r, free);
		CHECK(f.used(r) == f.budget.hard(r));
		const auto fd_calls = f.kern.fd_calls.load(), map_calls = f.kern.map_calls.load();
		const int map_code = code_of([&] { (void)a->map(); });
		const int open_code = code_of([&] { (void)f.io->open_existing("a"); });
		const int dir_code = code_of([&] { (void)f.make_io(); });
		if (r == M) {  // only mappings are refused
			CHECK(map_code == -EMFILE && open_code == 0 && dir_code == 0);
			CHECK(f.kern.map_calls == map_calls);
		} else {  // an open file maps; a new open or directory is refused
			CHECK(map_code == 0 && open_code == -EMFILE && dir_code == -EMFILE);
			CHECK(f.kern.fd_calls == fd_calls);
		}
		CHECK(f.kern.refused == 0 && f.budget.external_failures() == 0);
		CHECK(f.used(r) == f.budget.hard(r));
		pad = {};
		CHECK(code_of([&] { (void)f.io->open_existing("a"); }) == 0);
	}
	// Opens alone reach each of the caps' hard values with the kernel's tables below them.
	fx f(256, 100, 100);
	std::vector<std::unique_ptr<File>> files;
	while (code_of([&] { files.push_back(f.io->open_existing("a")); }) == 0) {}
	CHECK(f.used(V) == f.budget.hard(V) || f.used(D) == f.budget.hard(D));
	CHECK(f.kern.fd_peak <= std::int64_t(f.budget.hard(D)) && f.kern.refused == 0 && f.agrees());
	files.clear();
	std::printf("caps by io: ok\n");
}

// --- the kernel refuses: what comes back ---------------------------------------------------------

void kernel_failures() {
	fx f;
	auto a = f.io->open_existing("a");
	const std::uint64_t d = f.used(D), v = f.used(V);
	// EIO is nothing the budget acts on; EMFILE, ENFILE and ENOMEM are external pressure.
	for (const int e : {EIO, EACCES, ENOENT}) {
		f.kern.fail_fd = e;
		CHECK(code_of([&] { (void)f.io->open_existing("a"); }) == e);
		CHECK(code_of([&] { (void)f.io->create_exclusive("z"); }) == e);
		CHECK(f.used(D) == d && f.used(V) == v && f.agrees());
		f.kern.fail_fd = 0;
		f.kern.fail_map = e;
		CHECK(code_of([&] { (void)a->map(); }) == e);
		CHECK(f.used(M) == 0 && f.agrees());
		f.kern.fail_map = 0;
	}
	CHECK(f.budget.external_failures() == 0 && f.budget.external(D) == resource_budget::unlimited);
	std::uint64_t seen = 0;
	for (const int e : {EMFILE, ENFILE, ENOMEM}) {
		f.kern.fail_fd = e;
		CHECK(code_of([&] { (void)f.io->open_existing("a"); }) == e);
		CHECK(f.used(D) == d && f.used(V) == v && f.agrees());
		CHECK(f.budget.external_failures() == ++seen && f.budget.external(D) == d);
		f.kern.fail_fd = 0;
	}
	f.kern.fail_map = ENOMEM;
	CHECK(code_of([&] { (void)a->map(); }) == ENOMEM);
	CHECK(f.used(M) == 0 && f.budget.external(M) == 0 && f.budget.soft(M) == 0);
	f.kern.fail_map = 0;
	// The same failure out of the owner lock, the cursor and the creation.
	f.kern.fail_fd = EMFILE;
	CHECK(code_of([&] { (void)f.io->acquire_owner(true); }) == EMFILE);
	CHECK(code_of([&] { (void)f.io->scan_directory(); }) == EMFILE);
	CHECK(code_of([&] { (void)f.io->create_exclusive("q"); }) == EMFILE);
	f.kern.fail_fd = 0;
	CHECK(f.used(D) == d && f.used(V) == v && f.agrees());
	// A failure inside faultfs (after the descriptor) returns the descriptor too.
	CHECK(code_of([&] { (void)f.io->open_existing("missing"); }) == ENOENT);
	CHECK(code_of([&] { (void)f.io->create_exclusive("a"); }) == EEXIST);
	CHECK(f.used(D) == d && f.used(V) == v && f.agrees());
	a.reset();
	f.io.reset();
	CHECK(f.empty());
	std::printf("kernel failures: ok\n");
}

// --- exceptions after a successful call -----------------------------------------------------------

// Every allocation of a call fails in turn: whatever throws, the budget and the kernel's model
// are empty again; where the call returns, they agree.
template <class Call, class Live>
void sweep(const char* name, Call call, Live live, int expect_points) {
	for (int k = 1; k < 400; ++k) {
		fx f;
		bool ok = false;
		{
			using R = decltype(call(f));
			std::optional<R> r;
			try {
				alloc_arm arm(k);
				r.emplace(call(f));
				ok = true;
			} catch (const std::bad_alloc&) {
			}
			if (ok) { live(f, *r); }
		}
		if (!f.io) { CHECK(f.empty()); }
		f.io.reset();
		if (!(f.empty())) { std::fprintf(stderr, "%s: allocation %d left counts: D=%llu V=%llu M=%llu kernel fds=%lld maps=%lld\n", name, k, (unsigned long long)f.used(D), (unsigned long long)f.used(V), (unsigned long long)f.used(M), (long long)f.kern.fds.load(), (long long)f.kern.maps.load()); }
		CHECK(f.empty());
		if (ok) {
			CHECK(k - 1 >= expect_points);  // the call allocates at least this often
			return;
		}
	}
	CHECK(false);
}

[[maybe_unused]] void allocation_failures() {
	auto agree = [](fx& f, auto&) { CHECK(f.agrees()); };
	sweep("make", [](fx& f) { f.io.reset(); return f.make_io(); }, [](fx& f, std::unique_ptr<counted_io>& io) { CHECK(f.used(D) == 1 && f.used(V) == 1 && f.agrees()); f.io = std::move(io); }, 2);
	sweep("open_existing", [](fx& f) { return f.io->open_existing("a"); }, [](fx& f, auto&) { CHECK(f.used(D) == 2 && f.used(V) == 2 && f.agrees()); }, 2);
	sweep("create_exclusive", [](fx& f) { return f.io->create_exclusive("n"); }, agree, 2);
	sweep("open_reclaim_candidate", [](fx& f) { return f.io->open_reclaim_candidate("a"); }, agree, 2);
	{
		// The map itself, with its file held outside the armed call.
		for (int k = 1; k < 100; ++k) {
			fx f;
			auto file = f.io->open_existing("a");
			std::shared_ptr<const Mapping> m;
			bool ok = false;
			try {
				alloc_arm arm(k);
				m = file->map();
				ok = true;
			} catch (const std::bad_alloc&) {
			}
			if (ok) {
				CHECK(m && f.used(M) == 1 && f.kern.maps == 1 && f.agrees());
				CHECK(k > 1);
				break;
			}
			CHECK(f.used(M) == 0 && f.kern.maps == 0 && f.used(D) == 2 && f.used(V) == 2 && f.agrees());
		}
	}
	sweep("acquire_owner", [](fx& f) { return f.io->acquire_owner(true); }, [](fx& f, auto&) { CHECK(f.used(D) == 2 && f.used(V) == 2 && f.agrees()); }, 2);
	sweep("scan_directory", [](fx& f) { return f.io->scan_directory(); }, [](fx& f, auto&) { CHECK(f.used(D) == 2 && f.used(V) == 1 && f.agrees()); }, 1);
	std::printf("allocation failures: ok\n");
}

// --- CT-R27: two catalogs on one budget ----------------------------------------------------------

void two_catalogs() {
	fx a(256, 1000, 1000, 64, false);
	resource_counts catalog_b;
	faultfs::filesystem fs_b;
	fs_b.map_files(true);
	faultfs::io inner_b(fs_b);
	{
		auto f = inner_b.create_exclusive("a");
		f->write_at(0, std::string(100, 'b'));
	}
	// The kernel's table is as large as the budget's hard cap plus a few: a catalog that were
	// allowed its own cap would meet an EMFILE or pass the budget.
	a.kern.fd_limit = std::int64_t(a.budget.hard(D)) + 5;
	a.io = a.make_io();
	auto io_b = counted_io::make({&a.budget, &catalog_b, nullptr}, [&] { return probe_io::make(a.kern, inner_b); });
	std::vector<std::unique_ptr<File>> held_a, held_b;
	const std::uint64_t hard = a.budget.hard(D);
	for (std::uint64_t i = 0; i < hard; ++i) {
		auto& side = i % 3 == 0 ? held_b : held_a;
		auto& io = i % 3 == 0 ? static_cast<kronuz::journal::IO&>(*io_b) : static_cast<kronuz::journal::IO&>(*a.io);
		if (code_of([&] { side.push_back(io.open_existing("a")); }) != 0) { break; }
	}
	// 2 directories + files: the budget is at its hard value, split between the catalogs.
	CHECK(a.used(D) == hard && a.catalog.get(D) + catalog_b.get(D) == hard);
	CHECK(a.catalog.get(D) > 1 && catalog_b.get(D) > 1);
	CHECK(code_of([&] { (void)io_b->open_existing("a"); }) == -EMFILE);
	CHECK(code_of([&] { (void)a.io->open_existing("a"); }) == -EMFILE);
	CHECK(a.kern.fd_peak == std::int64_t(hard) && a.kern.refused == 0);
	// One catalog's release is the other's room.
	held_a.pop_back();
	CHECK(code_of([&] { held_b.push_back(io_b->open_existing("a")); }) == 0);
	CHECK(a.used(D) == hard && a.kern.fds == std::int64_t(hard));
	held_a.clear();
	held_b.clear();
	io_b.reset();
	a.io.reset();
	CHECK(a.empty() && catalog_b.get(D) == 0);
	std::printf("two catalogs: ok\n");
}

// --- the check that comes before the call ---------------------------------------------------------

// Two maps at the cap less one run together: the reservation is made before the call, so only one
// reaches the kernel; the kernel never holds more than the cap.
void reserve_before_call() {
	fx f(256, 100, 1000);  // maps: hard 90
	const std::uint64_t hard = f.budget.hard(M);
	auto pad = f.budget.reserve(M, hard - 1);
	auto a = f.io->open_existing("a");
	auto b = f.io->open_existing("a");
	std::mutex mu;
	std::condition_variable cv;
	int inside = 0;
	f.kern.map_hook = [&] {
		std::unique_lock l(mu);
		++inside;
		cv.notify_all();
		cv.wait_for(l, std::chrono::milliseconds(400), [&] { return inside >= 2; });
	};
	std::shared_ptr<const Mapping> ma, mb;
	int ca = 0, cb = 0;
	std::thread ta([&] { ca = code_of([&] { ma = a->map(); }); });
	std::thread tb([&] { cb = code_of([&] { mb = b->map(); }); });
	ta.join();
	tb.join();
	f.kern.map_hook = nullptr;
	CHECK((ca == 0) != (cb == 0));
	CHECK((ca == 0 ? cb : ca) == -EMFILE);
	CHECK(f.kern.maps <= 1 && f.kern.map_peak <= 1 && f.used(M) == hard);
	pad = {};
	ma.reset();
	mb.reset();
	a.reset();
	b.reset();
	f.io.reset();
	CHECK(f.empty());
	std::printf("reserve before the call: ok\n");
}

// --- CT-R37: another user of the descriptor limit -------------------------------------------------

// The kernel's table is smaller than the budget's caps because another user holds descriptors. The
// first EMFILE lowers the soft cap to the count reached; the sweep's passes (re-derivation, eviction
// to the soft cap, admission up to it) meet no second EMFILE while that user holds them; when it
// lets go, a quiet 60 s raises the cap an eighth of the gap per pass, to the derived value.
void external_user() {
	fx f(1024, 100000, 100000, 256);  // descriptors: soft 576, hard 729
	f.kern.fd_limit = 100;
	f.kern.external_fds = 60;
	const std::uint64_t derived = f.budget.derived_soft(D);
	CHECK(derived == 576);
	std::vector<std::unique_ptr<File>> held;
	auto admit = [&] {  // a catalog admits opens up to the soft cap
		int refused = 0;
		while (f.used(D) < f.budget.soft(D)) {
			try {
				held.push_back(f.io->open_existing("a"));
			} catch (const std::system_error& e) {
				CHECK(e.code().value() == EMFILE);
				++refused;
				break;
			}
		}
		return refused;
	};
	auto evict = [&] {  // and evicts to it
		while (f.used(D) > f.budget.soft(D) && !held.empty()) { held.pop_back(); }
	};
	CHECK(admit() == 1);
	CHECK(f.kern.refused == 1 && f.kern.fds == 40);
	CHECK(f.budget.soft(D) == 40 && f.budget.external(D) == 40 && f.budget.derived_soft(D) == derived);
	for (int pass = 0; pass < 20; ++pass) {
		f.now += seconds(1);
		f.budget.tick();
		evict();
		CHECK(admit() == 0);
	}
	CHECK(f.kern.refused == 1 && f.budget.soft(D) == 40 && f.agrees());
	// The other user lets go; the limit is not what changed. Quiet for 60 s, and the cap rises.
	f.kern.external_fds = 0;
	f.kern.fd_limit = 100000;
	f.now += seconds(60);
	std::uint64_t last = f.budget.soft(D);
	int passes = 0;
	while (f.budget.external(D) != resource_budget::unlimited && passes < 200) {
		f.budget.tick();
		CHECK(f.budget.soft(D) > last);
		last = f.budget.soft(D);
		evict();
		CHECK(admit() == 0);
		++passes;
	}
	CHECK(f.budget.soft(D) == derived && passes > 8);
	CHECK(f.used(D) == derived && f.kern.refused == 1);
	held.clear();
	f.io.reset();
	CHECK(f.empty());
	std::printf("external user: ok\n");
}

// --- index reads through the counted IO ---------------------------------------------------------

struct index_fx : fx {
	reader_registry reg{64};
	index_fx(std::uint64_t maps = 1000, std::uint64_t vnodes = 1000) : fx(1024, maps, vnodes, 256) {}
	kv_index::options opts(bool eager) {
		kv_index::options o;
		o.keyed = true;
		o.eager_segments = eager;
		o.readers = &reg;
		o.auto_merge = false;
		o.policy.segments_per_tier = 2;
		return o;
	}
};

// N threads reach the same record together and each maps: the losers' mappings, their descriptors
// and their reservations go with their bodies. The budget equals the kernel's at every point.
void lost_install_race() {
	index_fx f;
	{
		auto ix = kv_index::create(*f.io, f.opts(true), uuid_of("race"));
		fill(ix, 3);
	}
	CHECK(f.agrees() && f.used(M) == 0);
	auto ix = kv_index::open(*f.io, f.opts(false));
	CHECK(f.agrees() && f.used(M) == 0);
	const auto reader = ix.reader();
	constexpr int N = 8;
	std::mutex mu;
	std::condition_variable cv;
	int inside = 0, need = 0;
	f.kern.map_hook = [&] {
		std::unique_lock l(mu);
		if (need > 0) {
			++inside;
			cv.notify_all();
			cv.wait_for(l, seconds(3), [&] { return inside >= need; });
		}
	};
	for (int round = 0; round < 3; ++round) {
		{
			std::lock_guard l(mu);
			need = N;
			inside = 0;
		}
		std::vector<std::thread> ts;
		for (int t = 0; t < N; ++t) {
			ts.emplace_back([&] {
				const auto g = reader.acquire();
				CHECK(g->segments()[std::size_t(round)].seg()->core().doc_count() == 2);
			});
		}
		for (auto& t : ts) { t.join(); }
		{
			std::lock_guard l(mu);
			need = 0;
		}
		CHECK(f.kern.map_peak >= N || round > 0);
		CHECK(f.used(M) == std::uint64_t(round + 1) && f.kern.maps == round + 1 && f.agrees());
	}
	f.kern.map_hook = nullptr;
	CHECK(f.used(D) == std::uint64_t(f.kern.fds.load()));
	std::printf("lost install race: ok\n");
}

// A cap reached under a read is a host error: nothing marked, nothing fenced, no kernel call; the
// read works once there is room.
void index_at_the_caps() {
	for (const resource r : {D, M, V}) {
		index_fx f;
		{
			auto ix = kv_index::create(*f.io, f.opts(true), uuid_of("caps"));
			fill(ix, 3);
		}
		auto ix = kv_index::open(*f.io, f.opts(false));
		auto pad = f.budget.reserve(r, f.budget.hard(r) - f.used(r), &f.catalog);
		const auto fd_calls = f.kern.fd_calls.load(), map_calls = f.kern.map_calls.load();
		int code = 0;
		CHECK(run([&] { (void)ix.get(key_of(0)); }, &code) == outcome::host && code == EMFILE);
		CHECK(f.kern.fd_calls == fd_calls || r == M);
		CHECK(f.kern.map_calls == map_calls && f.kern.refused == 0);
		CHECK(!ix.needs_heal() && ix.media_retries() == 0);
		CHECK(f.used(r) == f.budget.hard(r) && f.catalog.get(r) == f.used(r));
		pad = {};
		CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0"));
		CHECK(ix.apply(tx({add("knew", "x")}), {}).result == verdict::accepted);
		ix.commit();
		CHECK(f.agrees());
	}
	std::printf("index at the caps: ok\n");
}

// --- T7 on the real kernel ------------------------------------------------------------------------

// Probes the low descriptor numbers: the same on every platform (FreeBSD has no /dev/fd
// listing without fdescfs), and it opens nothing itself.
std::size_t count_open_descriptors() {
	std::size_t n = 0;
	for (int fd = 0; fd < 1024; ++fd) {
		if (::fcntl(fd, F_GETFD) != -1) { ++n; }
	}
	return n;
}

// A sanitizer's allocator maps memory of its own between two counts, so its builds skip them.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STRATA_NO_MAP_COUNT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define STRATA_NO_MAP_COUNT 1
#endif
#endif

#if defined(__linux__) && !defined(STRATA_NO_MAP_COUNT)
#define STRATA_MAP_COUNT 1
std::size_t count_map_lines() {
	std::size_t n = 0;
	std::ifstream in("/proc/self/maps");
	std::string line;
	while (std::getline(in, line)) { ++n; }
	return n;
}
#endif

void posix_counts() {
	namespace fs = std::filesystem;
	std::string tmpl = (fs::temp_directory_path() / "strata-counted-XXXXXX").string();
	CHECK(::mkdtemp(tmpl.data()) != nullptr);
	const fs::path dir = tmpl;
	{
		resource_options o;
		o.system = test_system(4096, 60000, 100000);
		resource_budget budget(std::move(o));
		resource_counts catalog;
		const std::size_t base_fds = count_open_descriptors();
#ifdef STRATA_MAP_COUNT
		const std::size_t base_maps = count_map_lines();
#endif
		auto io = counted_io::make({&budget, &catalog, nullptr}, [&] { return std::make_unique<kronuz::journal::PosixIO>(dir); });
		CHECK(count_open_descriptors() - base_fds == budget.used(D) && budget.used(D) == 1);
		{
			auto f = io->create_exclusive("seg");
			f->write_at(0, std::string(8192, 'x'));
			f->sync();
			auto m1 = f->map();
			CHECK(m1 && m1->bytes().size() == 8192);
			auto g = io->open_existing("seg");
			auto m2 = g->map();
			auto lock = io->acquire_owner(true);
			auto cur = io->scan_directory();
			CHECK(budget.used(D) == 5 && budget.used(M) == 2);
			CHECK(count_open_descriptors() - base_fds == budget.used(D));
#ifdef STRATA_MAP_COUNT
			CHECK(count_map_lines() - base_maps == budget.used(M));
#endif
			f->close();
			CHECK(budget.used(D) == 4 && count_open_descriptors() - base_fds == budget.used(D));
			CHECK(m1->bytes()[0] == 'x');  // the mapping outlives the descriptor
			CHECK(budget.used(V) == 4);    // directory, f's (held by m1), g, the lock
		}
		CHECK(budget.used(D) == 1 && budget.used(M) == 0 && budget.used(V) == 1);
		CHECK(count_open_descriptors() - base_fds == 1);
#ifdef STRATA_MAP_COUNT
		CHECK(count_map_lines() == base_maps);
#endif
		// A descriptor limit the kernel enforces: EMFILE comes back as the host's error.
		io.reset();
		CHECK(count_open_descriptors() == base_fds);
	}
	std::filesystem::remove_all(dir);
	std::printf("posix counts: ok\n");
}

// --- a race of many threads, failures everywhere ------------------------------------------------

// Eight threads over two catalogs on one budget, the kernel failing 5% of the calls with random
// errors and allocations failing in turn: no count passes a hard cap at any time, and at every
// quiescent point the budget equals the kernel's.
void stress() {
	fx f(100, 40, 100, 8, false);  // descriptors 87, maps 36, vnodes 60
	const std::uint64_t hard_d = f.budget.hard(D), hard_m = f.budget.hard(M), hard_v = f.budget.hard(V);
	faultfs::filesystem fs_b;
	fs_b.map_files(true);
	faultfs::io inner_b(fs_b);
	{
		auto w = inner_b.create_exclusive("a");
		w->write_at(0, std::string(512, 'b'));
	}
	resource_counts catalog_b;
	f.kern.fd_limit = std::int64_t(hard_d) + 10;
	f.kern.map_limit = std::int64_t(hard_m) + 10;
	f.io = f.make_io();
	auto io_b = counted_io::make({&f.budget, &catalog_b, nullptr}, [&] { return probe_io::make(f.kern, inner_b); });
	f.kern.chaos = 50;

	constexpr int T = 8, rounds = 6;
	const int iters = 1500;
	std::atomic<bool> stop{false};
	std::atomic<int> over{0}, odd{0};
	std::atomic<std::uint64_t> too_many{0}, kernel_errors{0}, alloc_failures{0}, done{0};
	sync_point sync(T);
	std::vector<std::thread> ts;
	std::thread sampler([&] {
		while (!stop) {
			if (f.used(D) > hard_d || f.used(M) > hard_m || f.used(V) > hard_v) { ++over; }
			if (f.kern.fd_peak > std::int64_t(hard_d) || f.kern.map_peak > std::int64_t(hard_m)) { ++over; }
		}
	});
	for (int t = 0; t < T; ++t) {
		ts.emplace_back([&, t] {
			kronuz::journal::IO& io = t % 2 ? static_cast<kronuz::journal::IO&>(*io_b) : static_cast<kronuz::journal::IO&>(*f.io);
			std::mt19937 rng(unsigned(t) * 7919 + 1);
			std::vector<std::unique_ptr<File>> files;
			std::vector<std::shared_ptr<const Mapping>> maps;
			std::vector<std::unique_ptr<DirectoryCursor>> cursors;
			std::vector<std::unique_ptr<resource_reservation>> pads;
			for (int round = 0; round < rounds; ++round) {
				for (int i = 0; i < iters; ++i) {
					const unsigned op = rng() % 10;
					const bool inject = rng() % 5 == 0;
					try {
						alloc_arm arm(inject ? int(1 + rng() % 8) : 0);
						if (op < 3) {
							files.push_back(io.open_existing("a"));
						} else if (op < 6 && !files.empty()) {
							maps.push_back(files[rng() % files.size()]->map());
						} else if (op == 6) {
							cursors.push_back(io.scan_directory());
						} else if (op == 7) {
							auto c = io.create_exclusive("t" + std::to_string(t) + "-" + std::to_string(rng() % 4));
							(void)c;
						} else if (op == 8) {
							auto lk = io.acquire_directory_owner();
						}
					} catch (const too_many_open&) {
						++too_many;
					} catch (const std::system_error& e) {
						const int c = e.code().value();
						if (c == EMFILE || c == ENFILE || c == ENOMEM || c == EIO || c == EEXIST || c == EAGAIN || c == EWOULDBLOCK) {
							++kernel_errors;
						} else {
							++odd;
						}
					} catch (const std::bad_alloc&) {
						++alloc_failures;
					}
					++done;
					if (files.size() > 12 || (rng() % 6 == 0 && !files.empty())) { files.erase(files.begin() + std::ptrdiff_t(rng() % files.size())); }
					if (maps.size() > 12 || (rng() % 6 == 0 && !maps.empty())) { maps.erase(maps.begin() + std::ptrdiff_t(rng() % maps.size())); }
					if (cursors.size() > 3) { cursors.erase(cursors.begin()); }
					if (t == 0 && i % 300 == 0) { f.budget.tick(); }
				}
				// A quiescent point: every thread holds what it holds and none is in a call.
				sync.wait();
				if (t == 0) {
					bool same = std::int64_t(f.used(D)) == f.kern.fds.load() && std::int64_t(f.used(M)) == f.kern.maps.load();
					for (const resource r : {D, M, V}) { same = same && f.catalog.get(r) + catalog_b.get(r) == f.used(r); }
					if (!same) { ++odd; }
				}
				sync.wait();
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	stop = true;
	sampler.join();
	CHECK(over == 0 && odd == 0);
	CHECK(too_many > 0 && kernel_errors > 0);
#ifndef STRATA_NO_ALLOC_HOOK
	CHECK(alloc_failures > 0);
#endif
	CHECK(f.kern.refused == 0);  // the kernel's own limits (above the caps) never answered
	f.io.reset();
	io_b.reset();
	CHECK(f.empty() && catalog_b.get(D) == 0 && catalog_b.get(V) == 0);
	std::printf("stress: ok (%llu calls: %llu cap, %llu kernel, %llu allocation failures)\n", (unsigned long long)done.load(), (unsigned long long)too_many.load(),
				(unsigned long long)kernel_errors.load(), (unsigned long long)alloc_failures.load());
}

}  // namespace

int main() {
	directory_descriptor();
	lifetimes();
	caps_by_io();
	kernel_failures();
#ifndef STRATA_NO_ALLOC_HOOK
	allocation_failures();
#endif
	two_catalogs();
	reserve_before_call();
	external_user();
	lost_install_race();
	index_at_the_caps();
	posix_counts();
	stress();
	if (failures) {
		std::printf("counted_io: FAILED (%d)\n", failures);
		return 1;
	}
	std::printf("counted_io: ok\n");
	return 0;
}
