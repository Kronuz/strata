// The cold tier's step 2, item 1 (docs/design/cold-tier.md D3, D10; S1, S2; CT-R4, I5-N3, CT-R38):
// the unleased-map count is per index, and close_ready() is cold's predicate. Each case names its
// criterion and the options it runs with; the mutants each must kill are run by
// .scratch/mutants.py (docs/status/cold-tier-step2.md). The wrapper's map counter is the oracle for
// "a map happened"; the storage budget and the files on disk are the oracle for close_ready().
#include "cold_hooks.hh"
#include "strata/core/executor.hh"
#include "stepping_executor.hh"
#include "strata/core/owner.hh"

#include <thread>

namespace strata {
// Reaches the lease and the accounting no public call exposes.
struct index_test_access {
	static const write_lease& prereq_lease(const kv_index& ix) { return ix.lease_; }
	static std::uint64_t prereq_debt(const kv_index& ix) { return ix.debt_; }
};
}  // namespace strata

namespace {

using strata::index_test_access;
using access = index_test_access;

kv_index::options opts_of(fixture& f, bool eager, bool defer) {
	auto o = f.opts(eager, false);
	o.defer_io = defer;
	return o;
}

// --- S1: the count is per index -------------------------------------------------------------

// Index B's records map inside index A's lease scope and chore scope: counted on B. B's own
// lease, B's promote(): not counted. A reader's map on another thread: counted. Options: B lazy
// (eager_segments off) without defer_io for the first three; defer_io for promote(). Runs in
// builds with lease checks and without (STRATA_LEASE_CHECKS=0), where held() is always true.
void unleased_per_index() {
	fixture fa, fb;
	auto a = kv_index::create(fa.io, opts_of(fa, true, false), uuid_of("pre-a"));
	fill(a, 1);
	auto b = kv_index::create(fb.io, opts_of(fb, false, false), uuid_of("pre-b"));
	const auto rb = b.reader();
	fill(b, 6);
	CHECK(b.demote());  // twins: no record has a body
	const auto m0 = fb.h.maps.load();
	CHECK(b.unleased_maps() == 0);

	{  // A's lease held: a map of B's record counts on B
		const strata::lease_scope hold{access::prereq_lease(a)};
		CHECK(b.get(key_of(10)) == std::optional<std::string>("v10"));
	}
	CHECK(b.unleased_maps() == 1 && fb.h.maps.load() == m0 + 1);
	{  // A's chore
		const strata::chore_scope chore{access::prereq_lease(a)};
		CHECK(bool(chore));
		CHECK(b.get(key_of(8)) == std::optional<std::string>("v8"));
	}
	CHECK(b.unleased_maps() == 2 && fb.h.maps.load() == m0 + 2);
	{  // B's own lease: the map is the owner's, not counted
		const strata::lease_scope own{access::prereq_lease(b)};
		CHECK(b.get(key_of(6)) == std::optional<std::string>("v6"));
	}
	CHECK(b.unleased_maps() == 2 && fb.h.maps.load() == m0 + 3);
	{  // B's own chore
		const strata::chore_scope own{access::prereq_lease(b)};
		CHECK(bool(own));
		CHECK(b.get(key_of(4)) == std::optional<std::string>("v4"));
	}
	CHECK(b.unleased_maps() == 2 && fb.h.maps.load() == m0 + 4);
	CHECK(b.demote());
	const auto maps = fb.h.maps.load();
	std::thread([&] {  // a reader's map, on a thread with no lease
		CHECK(rb.acquire()->get(key_of(10)) == std::optional<std::string>("v10"));
	}).join();
	CHECK(b.unleased_maps() == 3 && fb.h.maps.load() == maps + 1);
	std::printf("unleased per index: ok\n");
}

// A map in promote() does not count, the lease being the index's own. Options: eager, defer_io.
void promote_not_counted() {
	fixture f;
	auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("pre-p"));
	fill(ix, 4);
	CHECK(ix.demote() && ix.promote_owed());
	const auto maps = f.h.maps.load();
	ix.promote();
	CHECK(f.h.maps.load() == maps + 4);  // every record mapped, none counted
	CHECK(ix.unleased_maps() == 0);
	std::printf("promote not counted: ok\n");
}

// Every attempt of a lease-less thread is counted: with 8 readers on distinct records the count
// is the wrapper's maps. A moved index keeps its word. Options: lazy, no defer_io.
void unleased_threads_and_moves() {
	fixture f;
	auto ix = kv_index::create(f.io, opts_of(f, false, false), uuid_of("pre-t"));
	const auto r = ix.reader();
	fill(ix, 8);
	CHECK(ix.demote());
	const auto base = ix.unleased_maps(), m0 = f.h.maps.load();
	std::vector<std::thread> ts;
	for (std::size_t i = 0; i < 8; ++i) {
		ts.emplace_back([&, i] { (void)r.acquire()->get(key_of(2 * i)); });
	}
	for (auto& t : ts) { t.join(); }
	CHECK(f.h.maps.load() > m0 && ix.unleased_maps() - base == f.h.maps.load() - m0);

	fixture fa, fb;
	auto a = kv_index::create(fa.io, opts_of(fa, true, false), uuid_of("pre-ma"));
	fill(a, 1);
	auto b = kv_index::create(fb.io, opts_of(fb, false, false), uuid_of("pre-mb"));
	fill(b, 3);
	auto moved = std::move(b);  // the word is in the records' context: it moves with the index
	CHECK(moved.demote());
	const auto mm = fb.h.maps.load();
	{
		const strata::lease_scope hold{access::prereq_lease(a)};
		(void)moved.get(key_of(5));  // the newest of three segments
	}
	CHECK(moved.unleased_maps() == 1);
	{
		const strata::lease_scope own{access::prereq_lease(moved)};
		(void)moved.get(key_of(3));
	}
	CHECK(moved.unleased_maps() == 1 && fb.h.maps.load() == mm + 2);
	std::printf("unleased threads and moves: ok\n");
}

// --- S2: close_ready() ----------------------------------------------------------------------

std::uint64_t disk_bytes(const faultfs::filesystem& fs) {
	std::uint64_t n = 0;
	for (const auto& name : fs.list()) { n += fs.view(name).size(); }
	return n;
}
std::size_t segment_files(const faultfs::filesystem& fs) {
	std::size_t n = 0;
	for (const auto& name : fs.list()) { n += name.starts_with("seg-"); }
	return n;
}

// An index with `n` segments merged into one: the sources queue for reclaim (reclaim_slice 0
// keeps commit from draining them).
struct queued_index {
	fixture f;
	byte_budget storage{1ull << 40};
	strata::inline_executor ex;
	std::optional<kv_index> ix;
	explicit queued_index(bool posted, std::size_t n = 6) {
		auto o = f.opts(true, false);
		o.storage = &storage;
		o.reclaim_slice = 0;
		if (posted) { o.background = &ex; }
		ix.emplace(kv_index::create(f.io, o, uuid_of("pre-q")));
		fill(*ix, n);
		ix->force_merge(1);
		ex.run();
		ix->commit();
		ex.run();
	}
};

// The queue clause, the posted-delete clause, and the accounting: with files queued close_ready()
// is false; reclaim() empties the queue, and with an executor the posted deletes keep it false
// until they ran and a call collected them; then true, and close() leaves the budget at the
// bytes the open indexes hold (none) and no obsolete file on disk. Options: eager; deletes inline
// and posted to an executor.
void queue_clause() {
	for (const bool posted : {false, true}) {
		queued_index q(posted);
		kv_index& ix = *q.ix;
		const std::size_t queued = ix.reclaim_pending();
		CHECK(queued == 6 && segment_files(q.f.fs) == 7);
		CHECK(!ix.close_ready());
		CHECK(q.storage.used() > disk_bytes(q.f.fs));  // the permit counts every file, the queued ones too

		(void)ix.reclaim();
		CHECK(ix.reclaim_pending() == 0);
		if (posted) {  // posted, not yet run: the outcome is not collected
			CHECK(ix.deletes_pending() == queued);
			CHECK(!ix.close_ready());
			q.ex.run();
			CHECK(ix.deletes_pending() == queued);  // run, not yet collected
			CHECK(ix.close_ready());                // close_ready() collects them, without waiting
			CHECK(ix.deletes_pending() == 0);
		} else {
			CHECK(ix.close_ready());
		}
		CHECK(access::prereq_debt(ix) == 0);
		CHECK(segment_files(q.f.fs) == 1);
		ix.close();
		CHECK(q.storage.used() == 0);
		CHECK(segment_files(q.f.fs) == 1);  // nothing the budget forgot is left on disk
	}
	std::printf("queue clause: ok\n");
}

// What the clause protects: closing with the queue full releases the permit, and the queued
// files stay on disk outside every budget.
void queue_clause_control() {
	queued_index q(false);
	CHECK(!q.ix->close_ready());
	q.ix->close();
	CHECK(q.storage.used() == 0 && segment_files(q.f.fs) == 7);
	std::printf("queue clause control: ok\n");
}

// The warm clauses, each alone, then nothing uncommitted: close_ready() is false until the write
// commits; after the commit the index closes and a reopen has it. Options: eager, no defer_io.
void uncommitted_clause() {
	fixture f;
	byte_budget storage(1ull << 40);
	auto o = f.opts(true, false);
	o.storage = &storage;
	auto ix = kv_index::create(f.io, o, uuid_of("pre-u"));
	fill(ix, 2);
	CHECK(ix.close_ready());
	CHECK(ix.apply(tx({add("zz", "1")}), {}).result == verdict::accepted);
	CHECK(!ix.close_ready());  // buffered
	ix.refresh();
	CHECK(!ix.close_ready());  // flushed to a segment, not committed
	ix.commit();
	CHECK(ix.close_ready());
	CHECK(ix.close_ready());  // a query changes nothing
	ix.close();
	CHECK(storage.used() == 0);
	auto again = kv_index::open(f.io, o);
	CHECK(again.get("zz") == std::optional<std::string>("1"));
	std::printf("uncommitted clause: ok\n");
}

// The same write lost: closing without the commit (what a missing clause would allow) leaves no
// "zz" after a reopen. Documents the oracle of the case above.
void uncommitted_control() {
	fixture f;
	auto o = f.opts(true, false);
	auto ix = kv_index::create(f.io, o, uuid_of("pre-v"));
	fill(ix, 2);
	CHECK(ix.apply(tx({add("zz", "1")}), {}).result == verdict::accepted);
	ix.refresh();
	ix.close();
	auto again = kv_index::open(f.io, o);
	CHECK(!again.get("zz"));
	std::printf("uncommitted control: ok\n");
}

// Warm's clauses, each reached with nothing applied since the commit: an outstanding map_owed
// turn, a pending merge, a merge output awaiting its sync; a closed index throws. Options: eager;
// defer_io for the turn; merges on a stepping executor.
void warm_clauses() {
	{  // a buffered record is the first uncommitted case above; here the map_owed turn
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("pre-w"));
		fill(ix, 3);
		CHECK(ix.demote() && ix.promote_owed());
		CHECK(ix.apply(tx({replace(key_of(1), "x")}), {}).result == verdict::map_owed);
		CHECK(!ix.close_ready());  // the turn's bit is set
		ix.promote();
		CHECK(ix.apply(tx({replace(key_of(1), "x")}), {}).result == verdict::accepted);
		ix.refresh();
		ix.commit();
		CHECK(ix.close_ready());
	}
	{  // a pending merge, then its unsynced output: nothing applied since the last commit
		strata::testing::stepping_executor exec;
		fixture f;
		auto o = f.opts(true, true);
		o.background = &exec;
		auto ix = kv_index::create(f.io, o, uuid_of("pre-y"));
		fill(ix, 3);
		CHECK(ix.merges_pending() >= 1);
		CHECK(!ix.close_ready());
		ix.wait_merges();
		CHECK(ix.merges_pending() == 0);
		CHECK(!ix.close_ready());  // the merge's output awaits the commit's sync
		ix.commit();
		ix.wait_merges();
		(void)ix.reclaim();
		ix.wait_deletes();
		CHECK(ix.close_ready());
	}
	{  // a closed index
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, false), uuid_of("pre-x"));
		fill(ix, 1);
		ix.close();
		CHECK(throws([&] { (void)ix.close_ready(); }));
	}
	std::printf("warm clauses: ok\n");
}

// close_ready() changes nothing: the snapshot of a refusal is equal before and after.
void refusal_changes_nothing() {
	queued_index q(true);
	kv_index& ix = *q.ix;
	const auto pending = ix.reclaim_pending();
	const auto digest = ix.digest();
	CHECK(!ix.close_ready() && !ix.close_ready());
	CHECK(ix.reclaim_pending() == pending && ix.digest() == digest && ix.deletes_pending() == 0);
	std::printf("refusal changes nothing: ok\n");
}

}  // namespace

int main() {
	unleased_per_index();
	promote_not_counted();
	unleased_threads_and_moves();
	queue_clause();
	queue_clause_control();
	uncommitted_clause();
	uncommitted_control();
	warm_clauses();
	refusal_changes_nothing();
	if (failures) {
		std::fprintf(stderr, "index_prereq: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("index_prereq: ok\n");
	return 0;
}
