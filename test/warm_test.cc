// The cold tier's step 1, item 5 (docs/design/cold-tier.md D3, D10, I5, I8; K5; CT-R21, CT-R22,
// CT-R25, CT-R26): demote() swaps every record for a twin with no body, promote() maps them again,
// and under defer_io apply answers verdict::map_owed in between. Each case names its K5 rule and
// the options it runs with; the mutants that each case must kill are run by .scratch/mutants.py.
// The device-call counter (faultfs::filesystem::calls()) is the oracle for "apply makes no I/O";
// the wrapper's live mappings are the oracle for "the index keeps no map".
#include "cold_hooks.hh"
#include "stepping_executor.hh"
#include "strata/core/owner.hh"
#include "strata/core/work_pool_executor.hh"

#include <algorithm>
#include <atomic>
#include <deque>
#include <random>
#include <set>

namespace strata {
// Reaches the records, the view and the flags no public call exposes.
struct index_test_access {
	static std::size_t records(const kv_index& ix) { return ix.segs_.size(); }
	static bool bodied(const kv_index& ix, std::size_t i) { return ix.segs_[i]->bodied(); }
	static bool failed(const kv_index& ix, std::size_t i) { return ix.segs_[i]->failed(); }
	static bool dirty(const kv_index& ix, std::size_t i) { return ix.segs_[i]->deletes_dirty; }
	static bool facts_known(const kv_index& ix, std::size_t i) { return ix.segs_[i]->facts_known; }
	static std::uint64_t id_of(const kv_index& ix, std::size_t i) { return ix.segs_[i]->ref.id; }
	static const void* rec(const kv_index& ix, std::size_t i) { return ix.segs_[i].get(); }
	static bool bit(const kv_index& ix) { return ix.map_asked_; }
	static std::uint64_t generation(const kv_index& ix) { return ix.pub_ ? ix.pub_->generation : 0; }
	static std::size_t retired(const kv_index& ix) { return ix.pub_ ? ix.pub_->retired.size() : 0; }
	static bool unmapped(const kv_index::segment_view& v) { return !v.rec->bodied() && !v.rec->failed(); }
};
}  // namespace strata

namespace {

using strata::index_test_access;
using access = index_test_access;

kv_index::options opts_of(fixture& f, bool eager, bool defer, bool merges = false) {
	auto o = f.opts(eager, merges);
	o.defer_io = defer;
	return o;
}

files::position at(std::uint64_t n) { return {1, n}; }

template <class F>
std::uint64_t calls_in(const faultfs::filesystem& fs, F&& f) {
	const std::uint64_t before = fs.calls();
	f();
	return fs.calls() - before;
}

// What `demote()` must leave alone when it refuses.
struct snapshot {
	std::vector<const void*> recs;
	std::uint64_t generation = 0, digest = 0, view_digest = 0;
	bool owed = false, bit = false;
	std::size_t retired = 0;
	bool operator==(const snapshot&) const = default;
};
snapshot take(const kv_index& ix, const kv_index::reader_handle& r) {
	snapshot s;
	for (std::size_t i = 0; i < access::records(ix); ++i) { s.recs.push_back(access::rec(ix, i)); }
	s.generation = access::generation(ix);
	s.digest = ix.digest();
	s.view_digest = r ? r.acquire()->digest() : 0;
	s.owed = ix.promote_owed();
	s.bit = access::bit(ix);
	s.retired = access::retired(ix);
	return s;
}

// The host's turn for one transaction: each owed answer is answered by its chore and the same
// transaction is applied again. Counts the answers, and calls `between` before every retry (the
// adversary that demotes whenever it may).
struct turn_counts {
	int map = 0, flush = 0, drain = 0, retries = 0;
	std::string order;  // 'm', 'f', 'd' in the order the answers came
};
template <class Between>
verdict host_apply(kv_index& ix, const transaction& t, files::position p, turn_counts& c, Between&& between) {
	for (int guard = 0; guard < 50; ++guard) {
		const verdict v = ix.apply(t, p);
		switch (v.result) {
		case verdict::map_owed:
			++c.map;
			c.order += 'm';
			ix.promote();
			break;
		case verdict::flush_owed:
			++c.flush;
			c.order += 'f';
			ix.refresh();
			break;
		case verdict::drain_owed:
			++c.drain;
			c.order += 'd';
			(void)ix.reclaim_for_space();
			break;
		default:
			return v;
		}
		++c.retries;
		between();
	}
	CHECK(false);  // the turn did not end
	return {};
}
verdict host_apply(kv_index& ix, const transaction& t, files::position p, turn_counts& c) {
	return host_apply(ix, t, p, c, [] {});
}

// --- K5.1: warm's predicate ----------------------------------------------------------------

// A refusal changes nothing: the same records, view, digest, flag and bit. Three clauses, each
// reached alone. Options: eager, with and without defer_io.
void predicate() {
	for (const bool defer : {false, true}) {
		{  // a buffered record
			fixture f;
			auto ix = kv_index::create(f.io, opts_of(f, true, defer), uuid_of("pred-a"));
			const auto r = ix.reader();
			fill(ix, 2);
			CHECK(ix.apply(tx({add("zz", "1")}), {}).result == verdict::accepted);
			const auto before = take(ix, r);
			CHECK(!ix.demote());
			CHECK(take(ix, r) == before);
			if (defer) {  // the flag is not set before the predicate: an apply is accepted, with no device call
				verdict v;
				CHECK(calls_in(f.fs, [&] { v = ix.apply(tx({replace(key_of(1), "x")}), {}); }) == 0 && v.result == verdict::accepted);
			}
			ix.refresh();
			ix.commit();
			CHECK(ix.demote());  // control: it is only the buffered record
		}
		{  // a pending merge
			strata::testing::stepping_executor exec;
			fixture f;
			auto o = opts_of(f, true, defer, true);
			o.background = &exec;
			auto ix = kv_index::create(f.io, o, uuid_of("pred-b"));
			const auto r = ix.reader();
			fill(ix, 3);
			CHECK(ix.merges_pending() >= 1);
			const auto before = take(ix, r);
			CHECK(!ix.demote());
			CHECK(take(ix, r) == before);
			ix.wait_merges();
			CHECK(ix.merges_pending() == 0);
			ix.commit();  // the merge's output is unsynced until then
			ix.wait_merges();
			CHECK(ix.demote());
		}
		{  // an unsynced record: flushed, not committed
			fixture f;
			auto ix = kv_index::create(f.io, opts_of(f, true, defer), uuid_of("pred-c"));
			const auto r = ix.reader();
			fill(ix, 2);
			CHECK(ix.apply(tx({add("zz", "1")}), {}).result == verdict::accepted);
			ix.refresh();
			const auto before = take(ix, r);
			CHECK(!ix.demote());
			CHECK(take(ix, r) == before);
			ix.commit();
			CHECK(ix.demote());
		}
	}
	// A closed index is fenced: live() throws before the predicate (there is no "closing" clause).
	{
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("pred-d"));
		fill(ix, 2);
		CHECK(ix.demote());
		ix.close();
		bool fenced = false;
		try {
			(void)ix.demote();
		} catch (const index_fenced&) {
			fenced = true;
		}
		CHECK(fenced);
		fenced = false;
		try {
			ix.promote();
		} catch (const index_fenced&) {
			fenced = true;
		}
		CHECK(fenced);
	}
	std::printf("predicate: ok\n");
}

// --- K5.2: every record twinned ---------------------------------------------------------------

// Lazy segments, so that some records have no body when demote() starts. A reader that held the old
// view maps late, after the demotion and the registry scan: its body lands in a record the retired
// view holds, never in the index. Options: eager_segments off, no defer_io.
void twin_every_record() {
	fixture f;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("twin"));
		fill(ix, 3);
	}
	auto ix = kv_index::open(f.io, f.opts(false));
	const auto reader = ix.reader();
	auto g = reader.acquire();
	(void)g->segments()[2].seg();  // one record bodied, two not
	std::size_t bare = 0;
	for (std::size_t i = 0; i < access::records(ix); ++i) { bare += !access::bodied(ix, i); }
	CHECK(bare == 2 && access::bodied(ix, 2));
	CHECK(ix.demote());
	CHECK(f.h.live_maps == 1);  // the old view's record keeps its body while the guard holds it
	for (std::size_t i = 0; i < 2; ++i) { (void)g->segments()[i].seg(); }  // the late maps, through the retired view
	CHECK(f.h.live_maps == 3);
	for (std::size_t i = 0; i < access::records(ix); ++i) { CHECK(!access::bodied(ix, i)); }
	CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0"));  // the index still reads: it maps its own
	g = {};
	std::printf("twin every record: ok\n");
}

// --- K5.3: the twin's fields -----------------------------------------------------------------

void twin_fields() {
	{  // deletes_dirty and the bits: an uncommitted delete survives a demotion, a commit and a reopen
		fixture f;
		const auto o = f.opts(true);
		std::size_t victim = 0;
		{
			auto ix = kv_index::create(f.io, o, uuid_of("fields-a"));
			const auto reader = ix.reader();
			fill(ix, 3);
			CHECK(ix.apply(tx({erase(key_of(0))}), {}).result == verdict::accepted);
			ix.refresh();
			for (std::size_t i = 0; i < access::records(ix); ++i) {
				if (access::dirty(ix, i)) { victim = i; }
			}
			CHECK(access::dirty(ix, victim));
			CHECK(ix.get(key_of(0)) == std::nullopt);
			CHECK(ix.demote());
			CHECK(access::dirty(ix, victim));
			CHECK(ix.get(key_of(0)) == std::nullopt);               // the bits came across
			CHECK(reader.acquire()->get(key_of(0)) == std::nullopt);  // and so did the new view's
			CHECK(ix.get(key_of(1)) == std::optional<std::string>("v1"));
			ix.commit();  // the twin writes the live-docs file
		}
		auto ix = kv_index::open(f.io, o);
		CHECK(ix.get(key_of(0)) == std::nullopt && ix.get(key_of(1)) == std::optional<std::string>("v1"));
	}
	{  // the marker: a record that failed its checks stays marked
		fixture f;
		std::string name;
		{
			auto ix = kv_index::create(f.io, f.opts(true), uuid_of("fields-b"));
			fill(ix, 2);
			name = index_detail::segment_name(access::id_of(ix, 0));
		}
		auto ix = kv_index::open(f.io, f.opts(false));
		const auto reader = ix.reader();
		f.h.damage_name = name;
		f.h.damage_left = 2;
		CHECK(run([&] { (void)reader.acquire()->segments()[0].seg(); }) == outcome::corrupt);
		CHECK(access::failed(ix, 0) && ix.needs_heal());
		CHECK(ix.demote());
		CHECK(access::failed(ix, 0) && ix.needs_heal());
		CHECK(run([&] { (void)ix.get(key_of(0)); }) == outcome::corrupt);  // still marked, no second read of the file
	}
	{  // the facts: a record a reader mapped, whose facts the owner never learned, then a commit
		fixture f;
		{
			auto ix = kv_index::create(f.io, f.opts(true), uuid_of("fields-c"));
			fill(ix, 2);
		}
		auto ix = kv_index::open(f.io, f.opts(false));
		const auto reader = ix.reader();
		(void)reader.acquire()->segments()[0].seg();
		CHECK(!access::facts_known(ix, 0));  // a reader's map leaves the facts in the body
		CHECK(ix.demote());
		CHECK(access::facts_known(ix, 0) && !access::bodied(ix, 0));  // the twin has them
		CHECK(ix.apply(tx({add("zz", "z")}), {}).result == verdict::accepted);
		ix.commit();
		CHECK(!ix.needs_heal() && ix.get("zz") == std::optional<std::string>("z"));
	}
	std::printf("twin fields: ok\n");
}

// --- K5.4: forced publication and freeing ------------------------------------------------------

// The index keeps no map once the last guard is gone, counted over every mapping alive (the
// retired views' included). Options: eager, no defer_io.
void publication_and_free() {
	{  // no guard held
		fixture f;
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("free-a"));
		const auto reader = ix.reader();
		fill(ix, 3);
		CHECK(f.h.live_maps == 3);
		const auto gen = access::generation(ix);
		CHECK(ix.demote());
		CHECK(access::generation(ix) == gen + 1);  // publication forced, though the state was unchanged
		CHECK(f.h.live_maps == 0 && access::retired(ix) == 0);
	}
	{  // a guard across demote(): its view keeps the originals until it is released, then a scan frees them
		fixture f;
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("free-b"));
		const auto reader = ix.reader();
		fill(ix, 3);
		auto g = reader.acquire();
		CHECK(ix.demote());
		CHECK(f.h.live_maps == 3 && access::retired(ix) == 1);
		const auto held = ix.views_held();
		CHECK(held.views == 2 && held.bytes == 0);  // the twins name the same files: nothing only the views hold
		std::thread([g = std::move(g)]() mutable { g = {}; }).join();  // released on another thread
		CHECK(f.h.live_maps == 3);                                      // nothing frees it until the owner scans
		ix.reclaim();
		CHECK(f.h.live_maps == 0 && access::retired(ix) == 0);
	}
	{  // no reader at all: the originals go at the swap
		fixture f;
		auto o = f.opts(true);
		o.readers = nullptr;
		auto ix = kv_index::create(f.io, o, uuid_of("free-c"));
		fill(ix, 3);
		CHECK(f.h.live_maps == 3);
		CHECK(ix.demote());
		CHECK(f.h.live_maps == 0);
		CHECK(ix.get(key_of(5)) == std::optional<std::string>("v5") && f.h.live_maps == 1);  // a read maps one
	}
	std::printf("publication and free: ok\n");
}

// --- K5.5: invisible to reads --------------------------------------------------------------------

// Position, digest and contents are the same before demote(), after it and after promote(), with an
// uncommitted delete standing in a twin. Options: eager, defer_io (so promote() has work).
void invisible_to_reads() {
	fixture f;
	auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("invisible"));
	const auto reader = ix.reader();
	fill(ix, 3);
	CHECK(ix.apply(tx({erase(key_of(2))}), at(10)).result == verdict::accepted);
	ix.refresh();
	struct state {
		std::uint64_t digest, view_digest;
		files::position pos;
		contents_t all;
	};
	const auto read = [&] {
		const auto g = reader.acquire();
		return state{ix.digest(), g->digest(), g->position(), contents(ix)};
	};
	const auto same = [](const state& a, const state& b) { return a.digest == b.digest && a.view_digest == b.view_digest && a.pos == b.pos && a.all == b.all; };
	const auto before = read();
	CHECK(before.all.size() == 5);
	CHECK(ix.demote());
	CHECK(same(before, read()));
	ix.promote();
	CHECK(same(before, read()));
	CHECK(ix.get(key_of(2)) == std::nullopt && ix.get(key_of(3)) == std::optional<std::string>("v3"));
	std::printf("invisible to reads: ok\n");
}

// --- K5.6: map_owed ------------------------------------------------------------------------------

// Options: eager (forced by defer_io), defer_io on; and the control with defer_io off.
void owed_answers() {
	for (const bool by_docid : {false, true}) {
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("owed"));
		std::uint64_t docid = 0;
		{
			const auto v = ix.apply(tx({add("d1", "x")}), at(1));
			CHECK(v.result == verdict::accepted);
			docid = v.docids.at(0);
		}
		ix.commit();
		fill(ix, 2);
		transaction txn;
		if (by_docid) {
			operation o;
			o.type = operation::delete_docid;
			o.docid = docid;
			txn = tx({o});
		} else {
			txn = tx({replace(key_of(1), "new")});
		}
		CHECK(ix.demote() && ix.promote_owed());
		const auto digest = ix.digest();
		const auto contents_before = contents(ix);
		verdict v;
		CHECK(calls_in(f.fs, [&] { v = ix.apply(txn, at(2)); }) == 0 && v.result == verdict::map_owed);
		CHECK(ix.digest() == digest && contents(ix) == contents_before);  // nothing applied
		CHECK(ix.promote_owed() && access::bit(ix));
		ix.promote();
		CHECK(calls_in(f.fs, [&] { v = ix.apply(txn, at(2)); }) == 0 && v.result == verdict::accepted);  // the same position: accepted, not skipped
		CHECK(ix.digest() != digest);
	}
	{  // control: without defer_io demote() sets no flag, and the read inside apply maps
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, false), uuid_of("owed-off"));
		fill(ix, 3);
		CHECK(ix.demote() && !ix.promote_owed());
		verdict v;
		const auto n = calls_in(f.fs, [&] { v = ix.apply(tx({replace(key_of(1), "new")}), at(1)); });
		CHECK(v.result == verdict::accepted && n > 0 && f.h.live_maps >= 1);
		CHECK(!access::bit(ix));
	}
	std::printf("owed answers: ok\n");
}

// --- K5.7: the answer ends ----------------------------------------------------------------------

void answer_ends() {
	{  // a completed promote() clears the flag; the answer stops
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("ends-a"));
		fill(ix, 3);
		CHECK(ix.demote() && ix.promote_owed());
		CHECK(ix.apply(tx({replace(key_of(1), "a")}), at(1)).result == verdict::map_owed);
		ix.promote();
		CHECK(!ix.promote_owed());
		CHECK(ix.apply(tx({replace(key_of(1), "a")}), at(1)).result == verdict::accepted);
		CHECK(ix.apply(tx({replace(key_of(2), "b")}), at(2)).result == verdict::accepted);
	}
	{  // CT-R22: a demote() between promote() and the retry is refused; the turn ends with an accepted apply
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("ends-b"));
		fill(ix, 3);
		CHECK(ix.demote());
		CHECK(ix.apply(tx({replace(key_of(1), "a")}), at(1)).result == verdict::map_owed && access::bit(ix));
		ix.promote();
		const auto recs = access::rec(ix, 0);
		CHECK(!ix.demote() && access::rec(ix, 0) == recs && !ix.promote_owed());
		CHECK(ix.apply(tx({replace(key_of(1), "a")}), at(1)).result == verdict::accepted);
		CHECK(!access::bit(ix));
		ix.commit();
		CHECK(ix.demote());  // the turn is over: warm again
	}
	{  // a host error inside promote(): rethrown unchanged, unfenced, the flag set, the bodies kept
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("ends-c"));
		fill(ix, 3);
		CHECK(ix.demote());
		f.h.fail_errno = ENOMEM;
		f.h.fail_after = 1;  // the second map starts a burst of two failures; read_checked retries once
		int code = 0;
		CHECK(run([&] { ix.promote(); }, &code) == outcome::host && code == ENOMEM);
		CHECK(ix.promote_owed() && access::bodied(ix, 0) && !access::bodied(ix, 1));
		verdict v;
		CHECK(calls_in(f.fs, [&] { v = ix.apply(tx({replace(key_of(1), "a")}), at(1)); }) == 0 && v.result == verdict::map_owed);
		ix.promote();  // the burst is spent
		CHECK(!ix.promote_owed());
		for (std::size_t i = 0; i < access::records(ix); ++i) { CHECK(access::bodied(ix, i)); }
		CHECK(ix.apply(tx({replace(key_of(1), "a")}), at(1)).result == verdict::accepted);
	}
	{  // a record whose first map fails its checks takes the marker; promote() completes; apply meets it
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("ends-d"));
		fill(ix, 3);
		const std::string name = index_detail::segment_name(access::id_of(ix, 0));
		CHECK(ix.demote());
		f.h.damage_name = name;
		f.h.damage_left = 2;
		ix.promote();
		CHECK(!ix.promote_owed() && access::failed(ix, 0) && ix.needs_heal());
		CHECK(access::bodied(ix, 1) && access::bodied(ix, 2));
		const auto applied = ix.manifest().applied;
		const auto n = f.fs.calls();
		CHECK(run([&] { (void)ix.apply(tx({replace(key_of(0), "a")}), at(1)); }) == outcome::corrupt);  // rolled back
		CHECK(f.fs.calls() == n && ix.manifest().applied == applied && ix.buffered_count() == 0);
		CHECK(ix.apply(tx({replace(key_of(4), "a")}), at(1)).result == verdict::accepted);  // later positions go on
	}
	{  // a twin that carries the marker from before demote(): promote() completes and clears the flag
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("ends-e"));
		const auto reader = ix.reader();
		fill(ix, 3);
		const std::string name = index_detail::segment_name(access::id_of(ix, 0));
		CHECK(ix.demote());
		f.h.damage_name = name;
		f.h.damage_left = 2;
		CHECK(run([&] { (void)reader.acquire()->segments()[0].seg(); }) == outcome::corrupt);
		CHECK(access::failed(ix, 0));
		CHECK(ix.demote() && access::failed(ix, 0) && ix.promote_owed());  // the twin carries it
		CHECK(run([&] { ix.promote(); }) == outcome::ok);
		CHECK(!ix.promote_owed() && access::failed(ix, 0) && access::bodied(ix, 1));
	}
	{  // promote() with the flag clear does nothing and makes no device call, though records have no body
		fixture f;
		auto ix = kv_index::create(f.io, opts_of(f, true, false), uuid_of("ends-f"));
		fill(ix, 3);
		CHECK(ix.demote() && !ix.promote_owed());
		CHECK(calls_in(f.fs, [&] { ix.promote(); }) == 0);
		CHECK(!access::bodied(ix, 0));
		fixture g;
		auto jx = kv_index::create(g.io, opts_of(g, true, true), uuid_of("ends-g"));
		fill(jx, 2);
		CHECK(calls_in(g.fs, [&] { jx.promote(); }) == 0);  // nothing demoted
		CHECK(jx.demote());
		jx.promote();
		CHECK(calls_in(g.fs, [&] { jx.promote(); }) == 0);  // already promoted
	}
	std::printf("answer ends: ok\n");
}

// --- K5.10: the turn's bound ---------------------------------------------------------------------

// A rejected transaction keeps its bytes in a buffer that readers may hold, so a flush is owed with
// nothing buffered that demote() counts: the adversary can demote between the chore and the retry.
// With the bit, at most one of each owed answer and three retries; the orders map-then-flush and
// flush-then-map are both run.
void turn_bound() {
	for (const bool demote_first : {true, false}) {
		fixture f;
		auto o = opts_of(f, true, true);
		o.buffer_share = 1;  // any buffered byte is past it
		auto ix = kv_index::create(f.io, o, uuid_of("bound"));
		const auto reader = ix.reader();
		for (std::size_t s = 0; s < 2; ++s) {  // fill(), with the owed flushes answered
			for (std::size_t i = 0; i < 2; ++i) {
				turn_counts c;
				CHECK(host_apply(ix, tx({add(key_of(s * 2 + i), "v" + std::to_string(s * 2 + i))}), {}, c).result == verdict::accepted);
			}
			ix.commit();
		}
		// version_conflict: rejected, its bytes stay in the buffer (a reader exists)
		CHECK(ix.apply(tx({replace(key_of(0), "pushed"), replace(key_of(1), "x", 12345)}), at(1)).result == verdict::version_conflict);
		if (demote_first) { CHECK(ix.demote()); }
		turn_counts c;
		int demotes = 0;
		const auto adversary = [&] { demotes += ix.demote(); };
		const verdict v = host_apply(ix, tx({replace(key_of(1), "ok")}), at(2), c, adversary);
		CHECK(v.result == verdict::accepted);
		CHECK(c.map <= 1 && c.flush <= 1 && c.drain <= 1 && c.retries <= 3);
		CHECK(c.flush == 1);  // the flush was owed, and ran
		if (demote_first) {
			CHECK(c.map == 1 && c.retries == 2 && demotes == 0);  // map, flush: the bit kept the adversary out
		} else {
			CHECK(c.map == 1 && c.retries == 2 && demotes == 1);  // flush, then the demote it slipped in, then map
		}
		CHECK(!access::bit(ix));  // the turn is over
	}
	std::printf("turn bound: ok\n");
}

// The third answer: storage admission owes a drain while the reclaim queue holds the budget. Every
// transaction is applied after a demote() and with the adversary between the retries; a turn that
// sees the map first and the drain after it must not see a second map.
std::string noise(std::size_t n, std::uint64_t seed) {
	std::mt19937_64 r(seed);
	std::string s(n, ' ');
	for (auto& c : s) { c = char('a' + r() % 26); }
	return s;
}
void turn_bound_drain() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(150 * 1024);
	kv_index::options o;
	o.keyed = true;
	o.defer_io = true;
	o.storage = &storage;
	o.reclaim_slice = 0;  // a commit leaves the obsolete files queued
	o.reclaim_cap = 0;
	o.retained_predecessors = 0;
	auto ix = kv_index::create(io, o, uuid_of("bound-drain"));
	const auto big = [](std::size_t i) { return replace(key_of(i), noise(3000, i + 1)); };
	for (int s = 0; s < 4; ++s) {
		for (int i = 0; i < 4; ++i) { CHECK(ix.apply(tx({big(std::size_t(s * 4 + i))}), {}).result == verdict::accepted); }
		ix.commit();
	}
	ix.force_merge(1);
	ix.commit();
	CHECK(ix.reclaim_pending() > 0);
	std::set<std::string> orders;
	for (std::size_t i = 100; i < 300; ++i) {
		(void)ix.demote();
		turn_counts c;
		int demotes = 0;
		const verdict v = host_apply(ix, tx({big(i)}), {}, c, [&] { demotes += ix.demote(); });
		CHECK(c.map <= 1 && c.flush <= 1 && c.drain <= 1 && c.retries <= 3);
		orders.insert(c.order);
		if (v.result != verdict::accepted) { break; }
		ix.commit();
	}
	CHECK(orders.count("md") == 1);  // a turn that began warm and ran into the drain
	CHECK(!access::bit(ix));
	std::printf("turn bound with a drain: ok\n");
}

// --- K5.11: reads after promote() ------------------------------------------------------------------

void reads_after_promote() {
	fixture f;
	auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("reads"));
	const auto reader = ix.reader();
	fill(ix, 3);
	CHECK(ix.demote());
	const auto base = ix.unleased_maps();
	CHECK(reader.acquire()->get(key_of(5)) == std::optional<std::string>("v5"));  // a lease-less read of a demoted index maps
	CHECK(ix.unleased_maps() == base + 1 && ix.promote_owed());
	ix.promote();
	const auto maps = ix.unleased_maps();
	std::size_t seen = 0;
	const auto n = calls_in(f.fs, [&] {
		CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0"));
		CHECK(reader.acquire()->get(key_of(3)) == std::optional<std::string>("v3"));
		seen = reader.acquire()->scan(std::string_view(), [](std::string_view, std::string_view, std::uint64_t) {});
		CHECK(!ix.get("nope"));  // a miss reads every segment
	});
	CHECK(n == 0 && ix.unleased_maps() == maps && seen == 6);
	std::printf("reads after promote: ok\n");
}

// --- K5.9: the sanitizer run ---------------------------------------------------------------------

// Readers hold guards across demote() and promote() and release them on another thread, an index
// idle after posting deletes, writers apply, refresh, commit, merge on a work pool; under defer_io
// the host answers map_owed with promote(). The counters show that the interleavings happened.
struct interleavings {
	std::uint64_t late = 0, losses = 0, raced_promote = 0, demoted = 0, promoted = 0;
};

// The deterministic overlaps, counted on top of the random run's.
void forced_overlaps(interleavings& out) {
	fixture f;
	auto ix = kv_index::create(f.io, opts_of(f, true, true), uuid_of("overlap"));
	const auto reader = ix.reader();
	fill(ix, 3);
	constexpr int N = 4;
	CHECK(ix.demote());  // twice: the first view the guards hold is over records with no body
	std::vector<kv_index::view_guard> guards;
	for (int i = 0; i < N; ++i) { guards.push_back(reader.acquire()); }
	CHECK(ix.demote());
	out.demoted += 2;
	const auto lost0 = ix.map_losses();
	// N threads map the same record of the retired view together: one body wins.
	f.h.arm_rendezvous(N);
	std::vector<std::thread> ts;
	std::atomic<int> late{0};
	for (int t = 0; t < N; ++t) {
		ts.emplace_back([&, t] {
			if (reader.generation() > guards[std::size_t(t)]->generation() && access::unmapped(guards[std::size_t(t)]->segments()[0])) { ++late; }
			(void)guards[std::size_t(t)]->segments()[0].seg();
		});
	}
	for (auto& t : ts) { t.join(); }
	f.h.arm_rendezvous(0);
	out.late += std::uint64_t(late.load());
	out.losses += ix.map_losses() - lost0;
	// promote() racing a reader's map of the same record: the reader is held inside map() with its body.
	const auto lost1 = ix.map_losses();
	f.h.arm_hold();
	const void* theirs = nullptr;
	std::thread reader_thread([&] { theirs = reader.acquire()->segments()[2].seg(); });
	CHECK(f.h.wait_held());
	ix.promote();
	f.h.release();
	reader_thread.join();
	++out.promoted;
	CHECK(access::unmapped(reader.acquire()->segments()[0]) == false);
	out.raced_promote += ix.map_losses() - lost1;
	CHECK(theirs && !ix.promote_owed());
	guards.clear();
	ix.reclaim();
	CHECK(f.h.live_maps == 3 && !ix.promote_owed());
}

void stress(bool defer, interleavings& total) {
	WorkPool::Options po;
	po.threads = 3;
	po.name_format = "warm{:02}";
	WorkPool pool(po);
	work_pool_executor ex(pool);
	fixture f;
	auto o = opts_of(f, false, defer, true);
	o.background = &ex;
	o.policy.floor_bytes = 1;
	auto ix = kv_index::create(f.io, o, uuid_of("stress"));
	const auto reader = ix.reader();
	constexpr std::size_t base_keys = 40;
	fill(ix, 4, base_keys / 4);
	CHECK(ix.demote());
	std::atomic<bool> stop{false};
	std::atomic<int> bad{0};
	std::atomic<std::uint64_t> late{0};
	std::mutex qm;
	std::deque<kv_index::view_guard> parked;
	std::vector<std::thread> ts;
	for (int t = 0; t < 3; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(std::uint64_t(t) + 7);
			while (!stop.load()) {
				auto g = reader.acquire();
				if (rng() % 2) {
					for (int spin = 0; spin < 200 && reader.generation() == g->generation(); ++spin) { std::this_thread::yield(); }
				}
				if (reader.generation() > g->generation()) {
					for (const auto& s : g->segments()) {
						if (access::unmapped(s)) {
							++late;
							break;
						}
					}
				}
				const auto i = std::size_t(rng() % base_keys);
				if (g->get(key_of(i)) != std::optional<std::string>("v" + std::to_string(i))) { ++bad; }
				if (rng() % 4 == 0) {
					std::size_t n = 0;
					g->scan(std::string_view(), [&](std::string_view, std::string_view, std::uint64_t) { ++n; });
					if (n < base_keys) { ++bad; }
				}
				if (rng() % 3 == 0) {
					std::lock_guard l(qm);
					parked.push_back(std::move(g));
				}
			}
		});
	}
	ts.emplace_back([&] {  // releases parked guards: on a thread that is neither a reader nor the owner
		while (!stop.load()) {
			std::this_thread::yield();
			std::lock_guard l(qm);
			if (parked.size() > 4) { parked.pop_front(); }
		}
		std::lock_guard l(qm);
		parked.clear();
	});
	std::uint64_t pos = 100, demoted = 0, promoted = 0;
	for (int r = 0; r < 120; ++r) {
		turn_counts c;
		const verdict v = host_apply(ix, tx({replace(key_of(base_keys + std::size_t(r % 25)), "w" + std::to_string(r))}), at(++pos), c);
		CHECK(v.result == verdict::accepted);
		promoted += std::uint64_t(c.map);
		if (r % 7 == 0) { ix.reclaim(); }
		if (r % 2 == 0) {  // quiesce: nothing buffered, nothing unsynced, no merge pending
			ix.commit();
			ix.wait_merges();
			ix.commit();
			ix.wait_merges();
			demoted += ix.demote();
		}
	}
	stop = true;
	for (auto& t : ts) { t.join(); }
	ix.wait_merges();
	CHECK(bad == 0);
	for (std::size_t i = 0; i < base_keys; ++i) { CHECK(ix.get(key_of(i)) == std::optional<std::string>("v" + std::to_string(i))); }
	ix.commit();
	total.late += late.load();
	total.losses += ix.map_losses();
	total.demoted += demoted;
	total.promoted += promoted;
}

void sanitizer_run() {
	interleavings total;
	forced_overlaps(total);
	CHECK(total.late >= 1 && total.losses >= 1 && total.raced_promote >= 1);
	const interleavings forced = total;
	stress(false, total);
	stress(true, total);
	CHECK(total.demoted > forced.demoted && total.promoted > 0);
	std::printf("sanitizer run: late maps %llu, install losses %llu, promote raced a reader's map %llu, demotions %llu, owed promotes %llu\n",
		(unsigned long long)total.late, (unsigned long long)total.losses, (unsigned long long)total.raced_promote, (unsigned long long)total.demoted,
		(unsigned long long)total.promoted);
}

// --- crash points ------------------------------------------------------------------------------

// A workload that demotes and promotes between acknowledged commits, cut by a power loss after every
// mutation. demote() and promote() write nothing, so the cuts fall in the commits and flushes around
// them (a commit with a twin standing for an uncommitted delete, a flush after a promote). After the
// crash and the reopen: the contents are the last acknowledged commit's, or the state the commit in
// flight was making; no record is without a body; and the index applies without an owed answer.
struct workload_result {
	contents_t acked;     // after the last commit that returned
	contents_t inflight;  // before the commit that was running when power went
	bool in_commit = false;
};
void workload(kv_index& ix, faultfs::filesystem& fs, bool defer, workload_result& r) {
	std::uint64_t pos = 0;
	const auto go = [&](const transaction& t) {
		turn_counts c;
		const verdict v = host_apply(ix, t, at(++pos), c);
		CHECK(v.result == verdict::accepted);
	};
	const auto commit = [&] {
		r.inflight = contents(ix);
		r.in_commit = true;
		ix.commit();
		r.acked = r.inflight;
		r.in_commit = false;
	};
	const auto demote_checked = [&] {
		const auto m = fs.mutations(), n = fs.calls();
		if (ix.demote()) { CHECK(fs.mutations() == m && fs.calls() == n); }  // demotion makes no device call
	};
	const auto promote_checked = [&] {
		const auto m = fs.mutations();
		ix.promote();
		CHECK(fs.mutations() == m);  // promotion writes nothing
	};
	go(tx({add("a", "1"), add("b", "2"), add("c", "3")}));
	ix.refresh();
	commit();
	demote_checked();
	go(tx({add("d", "4"), erase("a")}));  // answers map_owed under defer_io
	ix.refresh();
	demote_checked();                     // twins standing for an uncommitted delete
	commit();
	if (defer) { promote_checked(); }
	go(tx({replace("b", "22"), add("e", "5")}));
	ix.refresh();
	commit();
	demote_checked();
	go(tx({erase("c")}));
	ix.refresh();
	demote_checked();
	promote_checked();
	commit();
	go(tx({add("f", "6")}));
	ix.refresh();
	ix.force_merge(1);
	commit();
}

void crash_points() {
	for (const bool defer : {false, true}) {
		kv_index::options o;
		o.keyed = true;
		o.defer_io = defer;
		std::uint64_t points = 0;
		{
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = kv_index::create(io, o, uuid_of("crash"));
			workload_result r;
			const auto from = fs.mutations();
			workload(ix, fs, defer, r);
			points = fs.mutations() - from;
		}
		CHECK(points > 40);
		for (std::uint64_t n = 0; n <= points; ++n) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			workload_result r;
			{
				auto ix = kv_index::create(io, o, uuid_of("crash"));
				fs.power_off_after(n);
				try {
					workload(ix, fs, defer, r);
				} catch (const std::exception&) {
				}
			}
			fs.crash(n);
			auto ix = kv_index::open(io, o);
			const auto now = contents(ix);
			CHECK(now == r.acked || (r.in_commit && now == r.inflight));
			for (std::size_t i = 0; i < access::records(ix); ++i) { CHECK(access::bodied(ix, i)); }
			CHECK(!ix.promote_owed() && !ix.needs_heal());
			verdict v;
			CHECK(calls_in(fs, [&] { v = ix.apply(tx({add("zz", "9")}), {}); }) == 0 && v.result == verdict::accepted);
			if (failures) {
				std::fprintf(stderr, "  power lost after mutation %llu of %llu (defer_io %d)\n", (unsigned long long)n, (unsigned long long)points, int(defer));
				break;
			}
		}
		std::printf("crash points (defer_io %d): %llu\n", int(defer), (unsigned long long)points);
	}
}

}  // namespace

int main() {
	predicate();
	twin_every_record();
	twin_fields();
	publication_and_free();
	invisible_to_reads();
	owed_answers();
	answer_ends();
	turn_bound();
	turn_bound_drain();
	reads_after_promote();
	sanitizer_run();
	crash_points();
	if (failures) {
		std::printf("warm: FAILED (%d)\n", failures);
		return 1;
	}
	std::printf("warm: ok\n");
	return 0;
}
