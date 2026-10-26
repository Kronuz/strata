// The shared reader's step 1, items 3 and 5 (docs/design/shared-reader.md D1, D4, D6, D7, D9,
// D11; SR-D1, SR-D2, SR-D6, SR-D15, SR-D16, SR-D19, SR-D20): views published at the flush points
// and read from other threads, retirement through the hazard registry, retention of what views
// map, the non-blocking close, destruction and move-assignment with views, publish_through, and
// the write lease as the owner check, with the worker mark as the second.
#include "kv_test.hh"

#include "strata/core/owner.hh"
#include "strata/core/pool_executor.hh"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

namespace strata {
struct index_test_access {
	static lease_scope hold(const kv_index& ix) { return lease_scope{ix.lease_}; }
	static const write_lease& lease(const kv_index& ix) { return ix.lease_; }
	// A share of segment i's live bits, as a view holds one. Under the lease.
	static deleted_bits share(const kv_index& ix, std::size_t i) { return ix.segs_[i]->deleted.share(); }
	static void probe(kv_index& ix, std::function<void(int)> f) { ix.pub_->rs->probe = std::move(f); }
	static std::size_t retired(const kv_index& ix) { return ix.pub_ ? ix.pub_->retired.size() : 0; }
};
}  // namespace strata

namespace {

std::string key_of(std::size_t i) {
	char k[16];
	std::snprintf(k, sizeof k, "k%05zu", i);
	return k;
}

using model_t = std::map<std::string, std::string>;

template <class Reader>
model_t scanned(const Reader& r) {
	model_t out;
	r.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace(std::string(k), std::string(v)); });
	return out;
}

kv_index::options with(reader_registry& reg) {
	kv_index::options o;
	o.key_prefix = "k";
	o.readers = &reg;
	return o;
}

std::size_t files_named(const faultfs::filesystem& fs, const std::string& name) {
	const auto l = fs.list();
	return std::size_t(std::count(l.begin(), l.end(), name));
}

// Counts the ownership breaks a call provokes, without aborting.
template <class F>
std::uint64_t breaks(F&& f) {
	detail::abort_on_owner_break().store(false);
	const std::uint64_t before = detail::off_owner_changes().load();
	f();
	const std::uint64_t n = detail::off_owner_changes().load() - before;
	detail::abort_on_owner_break().store(true);
	return n;
}

// D1, D4, D8: the first reader() refreshes; a guard reads the view's state, equal to the index's
// at publication, digest included; no registry, no reader.
void views_basic() {
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.key_prefix = "k";
		auto ix = kv_index::create(io, o, uuid_of("noreg"));
		CHECK(throws([&] { (void)ix.reader(); }));
	}
	reader_registry reg(8);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("basic"));
	model_t model;
	for (int i = 0; i < 40; ++i) {
		model[key_of(std::size_t(i))] = "v" + std::to_string(i);
		CHECK(ix.apply(tx({replace(key_of(std::size_t(i)), model[key_of(std::size_t(i))]), set_meta("m", std::to_string(i))}), {1, std::uint64_t(i + 1)}).result == verdict::accepted);
	}
	CHECK(ix.buffered_count() == 40);
	const auto h = ix.reader();
	CHECK(ix.buffered_count() == 0 && h.generation() == 1);  // reader() refreshed and published
	CHECK((h.published_position() == files::position{1, 40}));
	{
		const auto g = h.acquire();
		CHECK(g->generation() == 1 && (g->position() == files::position{1, 40}));
		CHECK(scanned(*g) == model && g->get(key_of(3)) == std::optional<std::string>("v3") && !g->get("k99999"));
		CHECK(g->metadata().at("m") == "39" && g->last_docid() == ix.last_docid() && g->key_prefix() == "k");
		CHECK(g->segments().size() == ix.segments().size() && g->digest() == ix.digest());
	}
	const auto again = ix.reader();  // later calls publish nothing new
	CHECK(again.generation() == 1);
	std::printf("views basic: ok\n");
}

// SR-D1 and SR-D16 at every point of D1's table: publish when the state changed and no document
// is buffered; a pending replace keeps the old view through a merge and a heal.
void publication_points() {
	// The source of the install below: its files back the installed segments' mappings, so it
	// outlives the index.
	faultfs::filesystem sfs;
	faultfs::io sio(sfs);
	kv_index::options plain;
	plain.key_prefix = "k";
	auto source = kv_index::create(sio, plain, uuid_of("points"));
	reader_registry reg(8);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("points"));
	for (int i = 0; i < 30; ++i) { CHECK(ix.apply(tx({replace(key_of(std::size_t(i)), "a")}), {}).result == verdict::accepted); }
	const auto h = ix.reader();
	std::uint64_t gen = h.generation();
	auto same = [&] { return h.generation() == gen; };
	auto moved = [&] {
		const bool m = h.generation() == gen + 1;
		gen = h.generation();
		return m;
	};
	auto digest_matches = [&] { return h.acquire()->digest() == ix.digest(); };
	ix.refresh();
	CHECK(same());  // nothing changed (SR-D16)
	ix.commit();
	CHECK(same());
	// A delete of a flushed document buffers nothing and flushes nothing; the refresh publishes it.
	CHECK(ix.apply(tx({erase(key_of(2))}), {}).result == verdict::accepted);
	CHECK(ix.buffered_count() == 0);
	ix.refresh();
	CHECK(moved() && !h.acquire()->get(key_of(2)) && digest_matches());
	// So does a metadata-only transaction.
	CHECK(ix.apply(tx({set_meta("only", "meta")}), {}).result == verdict::accepted);
	ix.refresh();
	CHECK(moved() && h.acquire()->metadata().count("only") && digest_matches());
	// A rejected transaction changes nothing.
	CHECK(ix.apply(tx({replace(key_of(3), "never", 1u << 30)}), {}).result != verdict::accepted);
	ix.refresh();
	CHECK(same());
	// One at a position moves the applied position, so the point publishes it (SR-D16, CR1).
	CHECK(ix.apply(tx({replace(key_of(3), "never", 1u << 30)}), {7, 1}).result != verdict::accepted);
	ix.refresh();
	CHECK(moved() && h.published_position() == (files::position{7, 1}));
	// A replace pending: its old copy's bit is set in place, its new copy buffered. A merge
	// installed now, and heal(), keep the current view, which still reads the old value (SR-D1).
	for (int s = 0; s < 3; ++s) {
		CHECK(ix.apply(tx({replace(key_of(100 + std::size_t(s)), "b")}), {}).result == verdict::accepted);
		ix.refresh();
		CHECK(moved());
	}
	CHECK(ix.apply(tx({replace(key_of(5), "new")}), {}).result == verdict::accepted);
	const std::size_t before = ix.segments().size();
	ix.force_merge(1);
	CHECK(ix.segments().size() < before && ix.buffered_count() == 1);
	CHECK(same() && h.acquire()->get(key_of(5)) == std::optional<std::string>("a"));
	CHECK(ix.heal().empty());
	CHECK(same() && h.acquire()->get(key_of(5)) == std::optional<std::string>("a"));
	ix.refresh();
	CHECK(moved() && h.acquire()->get(key_of(5)) == std::optional<std::string>("new") && digest_matches());
	CHECK(h.acquire()->segments().size() == ix.segments().size());
	// A merge installed with nothing buffered publishes at once.
	CHECK(ix.apply(tx({replace(key_of(200), "c")}), {}).result == verdict::accepted);
	ix.refresh();
	CHECK(moved());
	ix.force_merge(1);
	CHECK(moved() && h.acquire()->segments().size() == 1 && digest_matches());
	// commit() publishes what it flushed; install() publishes the installed content.
	CHECK(ix.apply(tx({replace(key_of(6), "committed")}), {}).result == verdict::accepted);
	ix.commit();
	CHECK(moved() && h.acquire()->get(key_of(6)) == std::optional<std::string>("committed"));
	CHECK(source.apply(tx({replace("kinstalled", "yes")}), {}).result == verdict::accepted);
	const std::uint64_t cp = source.checkpoint();
	ix.install(sio, cp);
	CHECK(moved() && h.acquire()->get("kinstalled") == std::optional<std::string>("yes") && !h.acquire()->get(key_of(6)) && digest_matches());
	std::printf("publication points: ok\n");
}

// D6: a guard keeps its view through later publications; the owner frees a released view at its
// next scan; at most H views are kept; H guards out make the next acquire too_many_readers.
void retire_and_free() {
	reader_registry reg(2);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("retire"));
	CHECK(ix.apply(tx({replace(key_of(0), "g0")}), {}).result == verdict::accepted);
	const auto h = ix.reader();
	auto first = h.acquire();
	for (int i = 1; i <= 20; ++i) {
		CHECK(ix.apply(tx({replace(key_of(0), "g" + std::to_string(i))}), {}).result == verdict::accepted);
		ix.refresh();
		CHECK(index_test_access::retired(ix) == 1);  // only the guarded one is kept
	}
	CHECK(first->get(key_of(0)) == std::optional<std::string>("g0") && first->generation() == 1);
	auto second = h.acquire();
	CHECK(throws([&] { (void)h.acquire(); }));  // both records claimed: too_many_readers
	CHECK(ix.views_held().views == 2);
	first.release();
	CHECK(ix.views_held().views == 2);  // freed at the owner's next scan, not by the reader
	ix.reclaim();
	CHECK(ix.views_held().views == 1 && index_test_access::retired(ix) == 0);
	CHECK(second->get(key_of(0)) == std::optional<std::string>("g20"));
	std::printf("retire and free: ok\n");
}

// D7 and SR-D6: a guarded view's segment files are neither truncated nor removed; once it is
// released, reclaim() queues and frees them with no commit (D6's data-file pass); and commit's
// order frees the view it retires before retain(), so a merge's sources go in the same commit.
void retention_under_guard() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = with(reg);
	o.reclaim_step = 4096;  // every queued file truncated in steps: below a live mapping, faultfs aborts
	auto ix = kv_index::create(io, o, uuid_of("retain"));
	std::mt19937_64 rnd(7);
	std::string big(600, ' ');
	for (auto& c : big) { c = char('a' + rnd() % 26); }  // incompressible enough: segments pass a step
	for (int s = 0; s < 3; ++s) {
		for (int i = 0; i < 40; ++i) { CHECK(ix.apply(tx({replace(key_of(std::size_t(s * 40 + i)), big)}), {}).result == verdict::accepted); }
		ix.commit();
	}
	const auto h = ix.reader();
	std::vector<std::string> held;
	{
		const auto g = h.acquire();
		for (const auto& s : g->segments()) { held.push_back(index_detail::segment_name(s.id)); }
		CHECK(held.size() == 3);
		ix.force_merge(1);
		ix.commit();
		ix.commit();  // the old manifests go; only the guard holds the sources now
		ix.reclaim();
		for (const auto& n : held) { CHECK(files_named(fs, n) == 1 && fs.view(n).size() > o.reclaim_step); }
		CHECK(ix.views_held().views == 2 && ix.views_held().bytes > 0);
		CHECK(g->get(key_of(5)) == std::optional<std::string>(big));
	}
	ix.reclaim();  // frees the released view, then queues and frees its files
	for (const auto& n : held) { CHECK(files_named(fs, n) == 0); }
	CHECK(ix.views_held().views == 1 && ix.views_held().bytes == 0);
	// The commit order: a merge installed while a document is buffered cannot publish, so the
	// current view holds its sources at the merge's own retain(); commit() flushes, publishes,
	// frees that view, and only then retains, so the sources are queued in this commit.
	for (int s = 0; s < 2; ++s) {
		for (int i = 0; i < 40; ++i) { CHECK(ix.apply(tx({replace(key_of(std::size_t(200 + s * 40 + i)), big)}), {}).result == verdict::accepted); }
		ix.commit();
	}
	std::vector<std::string> sources;
	for (const auto& s : ix.segments()) { sources.push_back(index_detail::segment_name(s.id)); }
	CHECK(ix.apply(tx({replace(key_of(999), "buffered")}), {}).result == verdict::accepted);
	ix.force_merge(1);
	const std::size_t queued_before = ix.reclaim_pending();
	ix.commit();
	ix.reclaim();
	std::size_t gone = 0;
	for (const auto& n : sources) { gone += files_named(fs, n) == 0; }
	CHECK(gone == sources.size());
	std::printf("retention under a guard: ok (%zu queued before the commit)\n", queued_before);
}

// D11, Q2: close() never waits. With a guard out the index is closing: acquires throw
// index_closed, other calls index_fenced, the lock is kept, and the guard reads on. Past a
// deadline try_finish_close() names the record; after the release it finishes and frees the lock.
void close_with_guards() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("close"));
	for (int i = 0; i < 20; ++i) { CHECK(ix.apply(tx({replace(key_of(std::size_t(i)), "c")}), {}).result == verdict::accepted); }
	ix.commit();
	const auto h = ix.reader();
	auto g = h.acquire();
	ix.close();
	CHECK(ix.closing() && ix.views_held().views == 1);
	bool closed = false;
	try {
		(void)h.acquire();
	} catch (const index_closed&) { closed = true; }
	CHECK(closed);
	CHECK(throws([&] { (void)ix.apply(tx({replace(key_of(1), "x")}), {}); }) && throws([&] { ix.close(); }));
	CHECK(throws([&] { (void)kv_index::open(io, with(reg)); }));  // the directory lock is kept
	CHECK(g->get(key_of(7)) == std::optional<std::string>("c") && scanned(*g).size() == 20);
	CHECK(!ix.try_finish_close());
	std::size_t named = 0;
	try {
		(void)ix.try_finish_close(0);
	} catch (const readers_active& e) {
		for (const auto& c : e.claims) { named += c.p == g.get(); }
	}
	CHECK(named == 1);
	g.release();
	CHECK(ix.try_finish_close() && !ix.closing() && ix.views_held().views == 0);
	auto reopened = kv_index::open(io, with(reg));
	CHECK(reopened.get(key_of(7)) == std::optional<std::string>("c"));
	std::printf("close with guards: ok\n");
}

// SR-D2 and D11: destruction and move-assignment run close()'s first half; a guard out at
// destruction is reported and its view leaks, readable; a moved index keeps its handles.
void destroy_and_move() {
	reader_registry reg(4);
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::optional<kv_index::reader_handle> h;
		const std::uint64_t n = breaks([&] {
			auto ix = kv_index::create(io, with(reg), uuid_of("destroy"));
			CHECK(ix.apply(tx({replace(key_of(1), "d")}), {}).result == verdict::accepted);
			h = ix.reader();
		});
		CHECK(n == 0 && throws([&] { (void)h->acquire(); }));
	}
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::optional<kv_index::reader_handle> h;
		std::optional<kv_index::view_guard> g;
		const std::uint64_t n = breaks([&] {
			auto ix = kv_index::create(io, with(reg), uuid_of("leak"));
			CHECK(ix.apply(tx({replace(key_of(1), "leaked")}), {}).result == verdict::accepted);
			h = ix.reader();
			g = h->acquire();
		});
		CHECK(n == 1 && (*g)->get(key_of(1)) == std::optional<std::string>("leaked"));  // leaked, never freed
		// The leaked lock keeps a reopen from truncating the leaked files (CR3).
		CHECK(throws([&] { (void)kv_index::open(io, with(reg)); }));
		g.reset();
	}
	{
		faultfs::filesystem fs, fs2;
		faultfs::io io(fs), io2(fs2);
		auto ix = kv_index::create(io, with(reg), uuid_of("moved"));
		CHECK(ix.apply(tx({replace(key_of(1), "one")}), {}).result == verdict::accepted);
		const auto h = ix.reader();
		auto other = kv_index::create(io2, with(reg), uuid_of("target"));
		CHECK(other.apply(tx({replace(key_of(2), "two")}), {}).result == verdict::accepted);
		const auto ho = other.reader();
		const std::uint64_t n = breaks([&] { other = std::move(ix); });  // the target's views go first
		CHECK(n == 0 && throws([&] { (void)ho.acquire(); }));
		CHECK(other.apply(tx({replace(key_of(1), "uno")}), {}).result == verdict::accepted);
		other.refresh();
		CHECK(h.acquire()->get(key_of(1)) == std::optional<std::string>("uno"));  // the handle followed
	}
	std::printf("destroy and move: ok\n");
}

// D9 and SR-D20: publish_through(p) refreshes only when the current view is behind p, checked
// after the lease is taken, so a second call for the same position flushes nothing.
void publish_through_coalesces() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("through"));
	const auto h = ix.reader();
	CHECK(ix.apply(tx({replace(key_of(1), "a")}), {1, 1}).result == verdict::accepted);
	CHECK(ix.apply(tx({replace(key_of(2), "b")}), {2, 1}).result == verdict::accepted);
	CHECK(h.published_position() < (files::position{1, 1}));
	ix.publish_through({1, 1});
	const std::uint64_t g = h.generation();
	CHECK((h.published_position() == files::position{2, 1}) && ix.buffered_count() == 0);
	CHECK(ix.apply(tx({replace(key_of(3), "c")}), {3, 1}).result == verdict::accepted);
	ix.publish_through({1, 1});
	ix.publish_through({2, 1});  // already published: nothing flushes
	CHECK(h.generation() == g && ix.buffered_count() == 1);
	ix.publish_through({3, 1});
	CHECK(h.generation() == g + 1 && h.acquire()->get(key_of(3)) == std::optional<std::string>("c"));
	// A rejected transaction at a new position is published by one refresh and satisfies every
	// later call for it (CR1).
	CHECK(ix.apply(tx({replace(key_of(3), "never", 1u << 30)}), {4, 1}).result != verdict::accepted);
	ix.publish_through({4, 1});
	CHECK(h.generation() == g + 2 && h.published_position() == (files::position{4, 1}));
	ix.publish_through({4, 1});
	CHECK(h.generation() == g + 2);
	std::printf("publish_through coalesces: ok\n");
}

// SR-D15, between the owner's stores: once the pointer is stored, a reader that reads a position
// and then acquires gets a view at or past it; while the position is being stored, a reader gets
// none rather than a torn one.
void publication_order() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("order"));
	CHECK(ix.apply(tx({replace(key_of(1), "a")}), {1, 5}).result == verdict::accepted);
	const auto h = ix.reader();
	std::size_t checks = 0;
	index_test_access::probe(ix, [&](int step) {
		const auto p = h.try_published_position();
		if (step == 1) {
			CHECK(!p);  // mid-store: none, never (2, 5)
			if (p) { CHECK((*p == files::position{1, 5} || *p == files::position{2, 0})); }
		} else {
			CHECK(p.has_value());
			if (p) {
				const auto g = h.acquire();
				CHECK(g->position() >= *p);
			}
		}
		++checks;
	});
	CHECK(ix.apply(tx({replace(key_of(1), "b")}), {2, 0}).result == verdict::accepted);
	ix.refresh();
	CHECK(checks == 3 && (h.published_position() == files::position{2, 0}));
	index_test_access::probe(ix, {});
	std::printf("publication order: ok\n");
}

// SR-D19: the lease is the owner check. A thread that holds no lease and is not marked dropping
// a share is a break; so is a second thread taking a lease another holds. Debug builds only.
void lease_is_primary() {
	if constexpr (!detail::lease_checks) {
		std::printf("lease is primary: skipped (no lease checks in this build)\n");
		return;
	}
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("lease"));
	CHECK(ix.apply(tx({replace(key_of(1), "a")}), {}).result == verdict::accepted);
	ix.refresh();
	deleted_bits share;
	{
		const auto held = index_test_access::hold(ix);
		share = index_test_access::share(ix, 0);
		CHECK(index_test_access::lease(ix).held());
	}
	CHECK(!index_test_access::lease(ix).held());
	const std::uint64_t dropped = breaks([&] { std::thread([s = std::move(share)]() mutable { s = deleted_bits(); }).join(); });
	std::uint64_t second = 0;
	{
		const auto held = index_test_access::hold(ix);
		second = breaks([&] { std::thread([&] { const auto other = index_test_access::hold(ix); }).join(); });
	}
	const std::uint64_t nested = breaks([&] {
		const auto a = index_test_access::hold(ix);
		const auto b = index_test_access::hold(ix);  // one holder, nested
		ix.refresh();
	});
	CHECK(dropped == 1 && second == 1 && nested == 0);
	std::printf("lease is primary: unleased drop %llu, second holder %llu, nested %llu\n", (unsigned long long)dropped, (unsigned long long)second,
		(unsigned long long)nested);
}

// SR-D19's second check and SR-D20's handoff: a host's request run as a pool task (a marked
// thread) holds the lease for its writer calls, so the segments its merges drop there are owner
// work; the lease passes from one thread's calls to another's. Every build.
void lease_on_any_thread() {
	ThreadPool<> pool("svtest{}", 2);
	pool_executor<> ex(pool);
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	std::optional<kv_index> ix;
	const std::uint64_t n = breaks([&] {
		auto g = ex.open();
		g->post([&](const task::cancel_token&) {
			ix.emplace(kv_index::create(io, with(reg), uuid_of("pooled")));
			for (int s = 0; s < 4; ++s) {
				for (int i = 0; i < 20; ++i) { CHECK(ix->apply(tx({replace(key_of(std::size_t(i)), "s" + std::to_string(s))}), {}).result == verdict::accepted); }
				ix->refresh();
			}
			const auto h = ix->reader();
			ix->force_merge(1);  // inline merges: the sources drop here, on the pool's thread
			ix->commit();
		});
		g->wait();
		std::thread([&] {  // the next holder, on another thread
			CHECK(ix->get(key_of(3)) == std::optional<std::string>("s3"));
			CHECK(ix->apply(tx({erase(key_of(3))}), {}).result == verdict::accepted);
			ix->commit();
		}).join();
		ix.reset();
	});
	CHECK(n == 0);
	std::printf("lease on any thread: %llu breaks\n", (unsigned long long)n);
}

// T1 and T2(b) in small: one writer moving value between key pairs (the sum is invariant), with
// refreshes, commits, and background merges on a pool; readers on other threads check every view
// whole, see generations never decrease, and hand some guards to another reader to release. The
// index closes with guards out and finishes once they are released.
// D1 (heal row), D11: a heal() that replaces a segment object publishes the new object at its
// point, and an older view keeps the failing one and meets a typed error (CR2). Transient bit
// flips over every segment file; a read meets corrupt_segment, then heal() re-reads.
void heal_publishes() {
	kv_index::options keyed;
	keyed.key_prefix = "Q";
	faultfs::filesystem fs;
	fs.map_files(false);  // media faults apply to reads, not to mappings
	faultfs::io io(fs);
	{
		auto ix = kv_index::create(io, keyed, uuid_of("healview"));
		for (int s = 0; s < 3; ++s) {
			std::vector<operation> ops;
			for (int i = 0; i < 40; ++i) {
				const int k = s * 40 + i;
				ops.push_back(add("Q" + std::to_string(k), "doc" + std::to_string(k) + "alphabetagammadelta" + std::to_string(k * 7919)));
			}
			CHECK(ix.apply(tx(ops), {}).result == verdict::accepted);
			ix.refresh();
		}
		ix.commit();
	}
	std::uint64_t reference;
	{
		auto ix = kv_index::open(io, keyed);
		reference = ix.digest();
	}
	std::vector<std::string> names;
	for (const auto& n : fs.list()) {
		if (n.starts_with("seg-") && index_detail::parse_name(n)) { names.push_back(n); }
	}
	std::size_t healed = 0, advanced = 0, stale_threw = 0, new_bad = 0;
	for (const auto& name : names) {
		const std::uint64_t size = fs.view(name).size();
		const std::uint64_t stride = std::max<std::uint64_t>(1, size / 16);
		for (std::uint64_t at = 0; at < size; at += stride) {
			fs.clear_faults();
			fs.add_media_fault(name, faultfs::media::bit_flip, at, at + 1, 1);
			reader_registry reg(4);
			auto o = keyed;
			o.readers = &reg;
			kv_index ix = kv_index::open(io, o);
			const auto h = ix.reader();
			auto old = h.acquire();
			bool met = false;
			try {
				(void)ix.check();
				(void)ix.digest();
			} catch (const corrupt_segment&) { met = true; }
			if (!met) { continue; }
			const auto gen = h.generation();
			const auto before = ix.media_retries();
			CHECK(ix.heal().empty());
			if (ix.media_retries() == before) { continue; }  // nothing replaced
			++healed;
			if (h.generation() > gen) { ++advanced; }
			try {
				(void)old->digest();
			} catch (const corrupt_segment&) { ++stale_threw; }
			try {
				if (h.acquire()->digest() != reference) { ++new_bad; }
			} catch (const corrupt_segment&) { ++new_bad; }
		}
	}
	fs.clear_faults();
	CHECK(healed > 0 && advanced == healed && new_bad == 0);
	std::printf("heal publishes: %zu heals, %zu new views, %zu old views met a typed error, %zu bad\n", healed, advanced, stale_threw, new_bad);
}

// SR-D15, threaded: a reader that reads the published generation and position and then acquires
// gets a view at or past both. Only a weakly ordered CPU (arm64) can fail this when the owner's
// stores are weakened; on x86_64 it passes either way, and TSan does not model it.
void position_under_readers() {
	reader_registry reg(16);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("fastpath"));
	const auto h = ix.reader();
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> bad{0}, checks{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < 3; ++t) {
		ts.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed)) {
				const auto g0 = h.generation();
				const auto p = h.published_position();
				const auto g = h.acquire();
				if (g->generation() < g0 || g->position() < p) { bad.fetch_add(1); }
				checks.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	for (std::uint64_t i = 1; i <= 2000; ++i) {
		CHECK(ix.apply(tx({replace(key_of(i % 50), "v" + std::to_string(i))}), {1, i}).result == verdict::accepted);
		ix.refresh();
	}
	stop.store(true);
	for (auto& t : ts) { t.join(); }
	CHECK(bad.load() == 0 && checks.load() > 0);
	std::printf("position under readers: %llu checks, %llu bad\n", (unsigned long long)checks.load(), (unsigned long long)bad.load());
}

void views_under_readers(std::uint64_t seed, int readers) {
	ThreadPool<> pool("svmerge{}", 2);
	pool_executor<> ex(pool);
	reader_registry reg(64);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = with(reg);
	o.background = &ex;
	o.auto_merge = true;
	o.policy.segments_per_tier = 3;
	o.policy.floor_bytes = 1;
	o.reclaim_step = 4096;
	auto ix = kv_index::create(io, o, uuid_of("stress"));
	const std::size_t pairs = 64;
	const long total = long(pairs) * 200;
	for (std::size_t i = 0; i < 2 * pairs; ++i) { CHECK(ix.apply(tx({replace(key_of(i), "100")}), {}).result == verdict::accepted); }
	const auto h = ix.reader();
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> bad{0}, reads{0}, handed{0}, closed_seen{0};
	std::mutex mu;
	std::deque<kv_index::view_guard> pass;  // guards one reader acquired and another releases
	std::vector<std::thread> ts;
	for (int t = 0; t < readers; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 r(seed * 977 + std::uint64_t(t));
			std::uint64_t last = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				try {
					auto g = h.acquire();
					if (g->generation() < last) { bad.fetch_add(1); }
					last = g->generation();
					long sum = 0;
					std::size_t n = 0;
					g->scan("", [&](std::string_view, std::string_view v, std::uint64_t) {
						sum += std::stol(std::string(v));
						++n;
					});
					if (sum != total || n != 2 * pairs) { bad.fetch_add(1); }
					const std::string k = key_of(r() % (2 * pairs));
					if (!g->get(k)) { bad.fetch_add(1); }
					reads.fetch_add(1, std::memory_order_relaxed);
					if (r() % 8 == 0) {
						std::lock_guard l(mu);
						pass.push_back(std::move(g));
					} else if (r() % 4 == 0) {
						std::lock_guard l(mu);
						if (!pass.empty()) {
							pass.pop_front();  // released here, acquired on another thread
							handed.fetch_add(1, std::memory_order_relaxed);
						}
					}
				} catch (const index_closed&) {
					closed_seen.fetch_add(1, std::memory_order_relaxed);
					std::this_thread::yield();
				} catch (const too_many_readers&) {
					std::lock_guard l(mu);
					pass.clear();
				}
			}
		});
	}
	std::mt19937_64 r(seed);
	std::size_t refreshes = 0;
	std::size_t until_refresh = 1 + r() % 64;
	for (int step = 0; step < 3000; ++step) {
		const std::size_t i = r() % pairs;
		const std::string a = key_of(i), b = key_of(i + pairs);
		const long va = std::stol(*ix.get(a)), vb = std::stol(*ix.get(b));
		const long d = long(r() % 21) - 10;
		CHECK(ix.apply(tx({replace(a, std::to_string(va - d)), replace(b, std::to_string(vb + d))}), {}).result == verdict::accepted);
		if (--until_refresh == 0) {
			until_refresh = 1 + r() % 64;
			if (++refreshes % 10 == 0) {
				ix.commit();
			} else {
				ix.refresh();
			}
			if (refreshes % 25 == 0) { ix.force_merge(2); }
		}
	}
	ix.refresh();
	while (reads.load() < std::uint64_t(readers) * 50) { std::this_thread::yield(); }
	ix.close();
	const bool closing = ix.closing();
	while (closed_seen.load() < std::uint64_t(readers)) { std::this_thread::yield(); }
	stop.store(true);
	for (auto& t : ts) { t.join(); }
	{
		std::lock_guard l(mu);
		pass.clear();
	}
	CHECK(ix.try_finish_close() && ix.views_held().views == 0);
	std::printf("views under %d readers (seed %llu): %llu reads, %llu released on another thread, %zu refreshes, closing with guards: %s\n", readers,
		(unsigned long long)seed, (unsigned long long)reads.load(), (unsigned long long)handed.load(), refreshes, closing ? "yes" : "no");
	CHECK(bad.load() == 0 && reads.load() > 0);
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
	views_basic();
	publication_points();
	retire_and_free();
	retention_under_guard();
	close_with_guards();
	destroy_and_move();
	publish_through_coalesces();
	publication_order();
	heal_publishes();
	position_under_readers();
	lease_is_primary();
	lease_on_any_thread();
	views_under_readers(seed, 4);
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("shared view: ok\n");
	return 0;
}
