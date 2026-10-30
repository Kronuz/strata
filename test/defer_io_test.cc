// options::defer_io (docs/mechanisms/defer-io.md; phase7c design D6, review R5, C15, C16): apply()
// makes no device call. Each path where it did I/O is tested with faultfs's call counter (every
// IO and File call) against the same steps without defer_io, which must show the I/O the counter
// is there to see: the flush at the head of apply, storage admission's drain, and the chores a
// check-in left owed (merge publication, freed views and retention's manifest reads). An owed
// answer applies nothing; the host's chore (refresh, reclaim_for_space, check_in) does the I/O
// and the retry succeeds; and an owed answer never repeats when the chore cannot help.
#include "kv_test.hh"
#include "strata/core/owner.hh"
#include "strata/core/work_pool_executor.hh"

#include <algorithm>
#include <random>
#include <thread>

namespace strata {
struct index_test_access {
	static const write_lease& lease(const kv_index& ix) { return ix.lease_; }
	static std::size_t segment_count(const kv_index& ix) { return ix.segs_.size(); }
	static bool merges_done(const kv_index& ix) {
		for (const auto& j : ix.pending_) {
			if (!j->done.load()) { return false; }
		}
		return !ix.pending_.empty();
	}
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

std::string noise(std::size_t n, std::uint64_t seed) {
	std::mt19937_64 r(seed);
	std::string s(n, ' ');
	for (auto& c : s) { c = char('a' + r() % 26); }
	return s;
}

template <class P>
bool spin_until(P&& p) {
	for (int i = 0; i < 20'000'000; ++i) {
		if (p()) { return true; }
		std::this_thread::yield();
	}
	return false;
}

bool accepted(const verdict& v) { return v.result == verdict::accepted; }

// A program's device calls: how many IO and File calls `f` made.
template <class F>
std::uint64_t calls_in(const faultfs::filesystem& fs, F&& f) {
	const std::uint64_t before = fs.calls();
	f();
	return fs.calls() - before;
}

template <class F>
std::uint64_t breaks(F&& f) {
	detail::abort_on_owner_break().store(false);
	const std::uint64_t before = detail::off_owner_changes().load();
	f();
	const std::uint64_t n = detail::off_owner_changes().load() - before;
	detail::abort_on_owner_break().store(true);
	return n;
}

kv_index::options base(bool defer) {
	kv_index::options o;
	o.keyed = true;
	o.eager_segments = true;
	o.defer_io = defer;
	return o;
}

// The flush at the head of apply (index.hh apply, the buffer past its share). With defer_io the
// answer is flush_owed, nothing applied, no device call, and the position unmoved: the same
// transaction at the same position is accepted after refresh(). Without it apply refreshes inline.
void flush_owed(bool defer) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(defer);
	o.buffer_share = 4096;
	auto ix = kv_index::create(io, o, uuid_of("flush-owed"));
	const std::string body = noise(300, 1);
	std::uint64_t position = 0;
	bool inline_io = false, owed = false;
	for (int i = 0; i < 200 && !owed && !inline_io; ++i) {
		const auto t = tx({replace(key_of(std::size_t(i)), body)});
		const auto segments = ix.segments().size();
		const auto digest = ix.digest();
		verdict v;
		const std::uint64_t n = calls_in(fs, [&] { v = ix.apply(t, {1, position + 1}); });
		if (v.result == verdict::flush_owed) {
			CHECK(defer && n == 0);
			CHECK(ix.segments().size() == segments && ix.digest() == digest);  // nothing applied
			ix.refresh();                                                       // the host's chore
			CHECK(ix.segments().size() == segments + 1);
			v = ix.apply(t, {1, position + 1});  // the same position: not skipped
			CHECK(accepted(v));
			owed = true;
			break;
		}
		CHECK(accepted(v));
		++position;
		inline_io = n != 0;
	}
	CHECK(defer ? owed && !inline_io : inline_io && !owed);
	std::printf("flush %s: ok\n", defer ? "owed" : "inline (control)");
}

// C15: an owed flush ends. An oversize transaction (more operations than a segment holds, or a
// record lane it cannot fit) is invalid on an empty buffer, and after at most one flush_owed on a
// buffer that holds something; it never repeats once refresh() emptied the buffer.
void oversize_ends() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(true);
	o.buffered_docs_cap = 2;
	auto ix = kv_index::create(io, o, uuid_of("oversize"));
	const auto big = tx({replace("a", "1"), replace("b", "2"), replace("c", "3")});
	CHECK(ix.apply(big, {}).result == verdict::invalid);  // empty buffer: nothing to flush
	CHECK(accepted(ix.apply(tx({replace("x", "1")}), {})));
	int owed = 0;
	verdict v = ix.apply(big, {});
	while (v.result == verdict::flush_owed && owed < 5) {
		++owed;
		ix.refresh();
		v = ix.apply(big, {});
	}
	CHECK(owed == 1 && v.result == verdict::invalid);
	// The buffer's document count passes the cap with room in the lane: one owed round, then accepted.
	CHECK(accepted(ix.apply(tx({replace("y", "1"), replace("z", "1")}), {})));
	v = ix.apply(tx({replace("w", "1")}), {});
	CHECK(v.result == verdict::flush_owed);
	ix.refresh();
	CHECK(accepted(ix.apply(tx({replace("w", "1")}), {})));
	// A lane that cannot hold the transaction even when empty.
	kv_index::options l = base(true);
	l.lane_chunks_cap = 1;
	faultfs::filesystem fs2;
	faultfs::io io2(fs2);
	auto small = kv_index::create(io2, l, uuid_of("oversize-lane"));
	std::vector<operation> many;
	for (int i = 0; i < 100000; ++i) { many.push_back(replace(key_of(std::size_t(i)), "v")); }
	const auto huge = tx(many);
	v = small.apply(huge, {});
	int rounds = 0;
	while (v.result == verdict::flush_owed && rounds < 5) {
		++rounds;
		small.refresh();
		v = small.apply(huge, {});
	}
	CHECK(rounds == 0 && v.result == verdict::invalid);
	std::printf("oversize ends: ok\n");
}

// Rejected records in the buffer count as something to flush, and one refresh() clears them.
void rejected_buffer_ends() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(true);
	o.buffer_share = 1;  // any buffered byte is past it
	auto ix = kv_index::create(io, o, uuid_of("rejected"));
	CHECK(accepted(ix.apply(tx({replace("a", "1")}), {})));
	ix.refresh();
	// The version conflict leaves its records in the buffer until a flush drops them.
	const auto v = ix.apply(tx({replace("a", "2", 99)}), {});
	CHECK(v.result == verdict::version_conflict || v.result == verdict::flush_owed);
	int rounds = 0;
	verdict w = ix.apply(tx({replace("b", "1")}), {});
	while (w.result == verdict::flush_owed && rounds < 5) {
		++rounds;
		ix.refresh();
		w = ix.apply(tx({replace("b", "1")}), {});
	}
	CHECK(rounds <= 1 && w.result != verdict::flush_owed);
	std::printf("rejected buffer ends: ok\n");
}

// Storage admission (index.hh apply, the permit short): the obsolete files of a merge wait in the
// reclaim queue and hold the budget. Inline, apply drains them; with defer_io it answers drain_owed
// with no device call, reclaim_for_space() frees them, and the retry is accepted. Returns the
// device calls apply made while it answered drain_owed (or drained inline).
struct drain_world {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	byte_budget storage;
	kv_index::options o;
	std::optional<kv_index> ix;
	explicit drain_world(bool defer, std::uint64_t capacity) : storage(capacity), o(base(defer)) {
		o.storage = &storage;
		o.reclaim_slice = 0;  // a commit leaves the obsolete files queued
		o.reclaim_cap = 0;
		o.retained_predecessors = 0;
		ix.emplace(kv_index::create(io, o, uuid_of("drain")));
	}
	operation big(std::size_t i) const { return replace(key_of(i), noise(3000, i + 1)); }
	// Fills with documents, merges, and leaves the sources queued.
	void obsolete() {
		for (int s = 0; s < 4; ++s) {
			for (int i = 0; i < 4; ++i) { CHECK(accepted(ix->apply(tx({big(std::size_t(s * 4 + i))}), {}))); }
			ix->commit();
		}
		ix->force_merge(1);
		ix->commit();
		CHECK(ix->reclaim_pending() > 0);
	}
	// Applies until the answer is not accepted.
	verdict until_short(std::size_t from, std::uint64_t* calls = nullptr) {
		for (std::size_t i = from; i < from + 200; ++i) {
			verdict v;
			const std::uint64_t n = calls_in(fs, [&] { v = ix->apply(tx({big(i)}), {}); });
			if (calls) { *calls = n; }
			if (v.result != verdict::accepted) { return v; }
			next = i + 1;
		}
		return verdict{};
	}
	std::size_t next = 100;
};

void drain_owed(bool defer) {
	// 16 incompressible documents are about 50 KB; the merge's copy and the queued sources fill it.
	drain_world w(defer, 150 * 1024);
	w.obsolete();
	if (defer) {
		std::uint64_t n = 99;
		const verdict v = w.until_short(100, &n);
		CHECK(v.result == verdict::drain_owed && n == 0);
		const std::size_t queued = w.ix->reclaim_pending();
		CHECK(queued > 0);
		CHECK(w.ix->reclaim_for_space());  // the host's chore
		CHECK(w.ix->reclaim_pending() == 0);
		CHECK(accepted(w.ix->apply(tx({w.big(w.next)}), {})));  // the retry
	} else {
		// Control: the same steps drain inside apply, and apply makes device calls.
		std::uint64_t drained_calls = 0;
		bool drained = false;
		for (std::size_t i = 100; i < 300 && !drained; ++i) {
			const std::size_t queued = w.ix->reclaim_pending();
			std::uint64_t n = 0;
			verdict v;
			n = calls_in(w.fs, [&] { v = w.ix->apply(tx({w.big(i)}), {}); });
			CHECK(accepted(v) || v.result == verdict::no_space);
			if (queued && w.ix->reclaim_pending() == 0) {
				drained = true;
				drained_calls = n;
			}
		}
		CHECK(drained && drained_calls > 0);
	}
	std::printf("drain %s: ok\n", defer ? "owed" : "inline (control)");
}

// C15 for drains: a full disk with nothing to reclaim answers no_space at once; a drain that frees
// nothing (the removal fails) is owed once and the retry answers no_space; the next apply starts a
// new round.
void drain_ends() {
	{
		drain_world w(true, 150 * 1024);
		for (int s = 0; s < 8; ++s) {
			for (int i = 0; i < 4; ++i) { (void)w.ix->apply(tx({w.big(std::size_t(s * 4 + i))}), {}); }
			w.ix->commit();
		}
		w.ix->reclaim();
		CHECK(w.ix->reclaim_pending() == 0);
		const verdict v = w.until_short(100);
		CHECK(v.result == verdict::no_space);  // a full disk: never owed
	}
	{
		drain_world w(true, 150 * 1024);
		w.obsolete();
		w.fs.add_fault({faultfs::op::remove, 1, EIO, false});  // the first removal fails; the queue keeps its file
		const verdict v = w.until_short(100);
		CHECK(v.result == verdict::drain_owed);
		(void)w.ix->reclaim_for_space();
		CHECK(w.ix->reclaim_pending() > 0 || w.ix->deletes_pending() > 0);  // the chore could not clear it
		const verdict again = w.ix->apply(tx({w.big(w.next)}), {});
		CHECK(again.result != verdict::drain_owed);  // the retry ends the round
		w.fs.clear_faults();
		const verdict next = w.until_short(w.next + 1);
		CHECK(next.result == verdict::drain_owed);  // a new round
		CHECK(w.ix->reclaim_for_space());
		CHECK(accepted(w.ix->apply(tx({w.big(w.next)}), {})));
	}
	std::printf("drain ends: ok\n");
}

// An apply during which another thread's check-in meets the lease held: the check-in leaves its
// chores owed, as a host's concurrent request does. The view's publication probe runs it mid-apply.
// A transaction of many new keys, after one that built the key table, grows it, which publishes a
// view in the apply (a replaced table is stamped with the current view's generation).
transaction growing_tx(const char* prefix) {
	std::vector<operation> ops;
	for (int i = 0; i < 3000; ++i) { ops.push_back(replace(prefix + key_of(std::size_t(i)), "v")); }
	return tx(std::move(ops));
}

verdict apply_with_check_in(kv_index& ix, const transaction& t) {
	CHECK(accepted(ix.apply(tx({replace("warm", "1")}), {})));
	bool fired = false;
	index_test_access::probe(ix, [&](int step) {
		if (step != 0 || fired) { return; }
		fired = true;
		std::thread([&] { auto c = ix.check_out(); }).join();
	});
	const verdict v = ix.apply(t, {});
	index_test_access::probe(ix, nullptr);
	CHECK(fired);
	return v;
}

// R5.1, merge publication: a background merge finished on the pool waits to publish. Inline, the
// apply that meets the owed chores publishes it; with defer_io the apply leaves it, makes no device
// call, and chores_owed() stays set until the host's check_in() publishes it.
void merge_publication_owed(bool defer) {
	WorkPool::Options po;
	po.threads = 2;
	po.name_format = "dio{:02}";
	WorkPool pool(po);
	work_pool_executor ex(pool);
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(defer);
	o.background = &ex;
	o.readers = &reg;
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	auto ix = kv_index::create(io, o, uuid_of("merge-owed"));
	for (int s = 0; s < 4; ++s) {
		for (int i = 0; i < 8; ++i) { CHECK(accepted(ix.apply(tx({replace(key_of(std::size_t(s * 8 + i)), "v" + std::to_string(s))}), {}))); }
		ix.refresh();
	}
	const auto h = ix.reader();
	const std::size_t before = index_test_access::segment_count(ix);
	ix.maybe_merge();
	CHECK(spin_until([&] { return index_test_access::merges_done(ix); }));
	CHECK(index_test_access::segment_count(ix) == before);  // finished, not published
	verdict v;
	std::uint64_t n = 0;
	const std::uint64_t off = breaks([&] { n = calls_in(fs, [&] { v = apply_with_check_in(ix, growing_tx("z")); }); });
	CHECK(off == 0 && accepted(v));
	if (defer) {
		CHECK(n == 0);
		CHECK(ix.chores_owed() && index_test_access::segment_count(ix) == before);
		CHECK(ix.merges_pending() > 0);
		ix.check_in();  // the host's chore
		CHECK(!ix.chores_owed() && index_test_access::segment_count(ix) < before && ix.merges_pending() == 0);
	} else {
		CHECK(!ix.chores_owed() && index_test_access::segment_count(ix) < before);
	}
	ix.commit();
	(void)ix.check();
	std::printf("merge publication %s: ok\n", defer ? "owed" : "inline (control)");
}

// R5.1, retention: a freed view makes retain() read a kept manifest it could not read before (a
// media fault at open left it uncached). Inline, the apply that meets the owed chores reads it;
// with defer_io the apply reads nothing and retention stays blocked until check_in().
void retention_read_owed(bool defer) {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(defer);
	o.readers = &reg;
	o.retained_predecessors = 1;
	{
		auto first = kv_index::create(io, o, uuid_of("retain-owed"));
		for (int s = 0; s < 3; ++s) {
			CHECK(accepted(first.apply(tx({replace(key_of(std::size_t(s)), "v")}), {})));
			first.commit();
		}
		first.close();
	}
	// The newest manifest is the current one; its predecessor fails its first read.
	std::uint64_t newest = 0;
	for (const auto& name : fs.list()) {
		if (const auto p = index_detail::parse_name(name); p && p->type == index_detail::parsed_name::manifest) { newest = std::max(newest, p->a); }
	}
	CHECK(newest > 1);
	fs.add_media_fault(index_detail::manifest_name(newest - 1), faultfs::media::read_error, 0, ~std::uint64_t(0), 1);
	auto ix = kv_index::open(io, o);
	CHECK(ix.retention_blocked() == newest - 1);
	const auto h = ix.reader();
	{
		const auto g = h.acquire();
		CHECK(accepted(ix.apply(tx({replace("a", "1")}), {})));
		ix.refresh();  // a new view; the guard keeps the old one retired
		CHECK(index_test_access::retired(ix) > 0);
	}
	verdict v;
	std::uint64_t n = 0;
	const std::uint64_t off = breaks([&] { n = calls_in(fs, [&] { v = apply_with_check_in(ix, growing_tx("b")); }); });
	CHECK(off == 0 && accepted(v));
	if (defer) {
		CHECK(n == 0 && ix.chores_owed() && ix.retention_blocked());
		const std::uint64_t m = calls_in(fs, [&] { ix.check_in(); });
		CHECK(m > 0 && !ix.chores_owed() && !ix.retention_blocked());
	} else {
		CHECK(n > 0 && !ix.chores_owed() && !ix.retention_blocked());
	}
	std::printf("retention read %s: ok\n", defer ? "owed" : "inline (control)");
}

// C16: in host mode (no check_out, so no chores), reclaim() frees the files a released view held
// with no further write; the idle task runs it.
void reclaim_after_release() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(true);
	o.readers = &reg;
	o.reclaim_step = 4096;
	auto ix = kv_index::create(io, o, uuid_of("c16"));
	const std::string big = noise(600, 7);
	for (int s = 0; s < 3; ++s) {
		for (int i = 0; i < 40; ++i) { CHECK(accepted(ix.apply(tx({replace(key_of(std::size_t(s * 40 + i)), big)}), {}))); }
		ix.commit();
	}
	const auto h = ix.reader();
	std::vector<std::string> held;
	{
		const auto g = h.acquire();
		for (const auto& sg : g->segments()) { held.push_back(index_detail::segment_name(sg.id)); }
		ix.force_merge(1);
		ix.commit();
		ix.commit();
		ix.reclaim();
		const auto names = fs.list();
		for (const auto& name : held) { CHECK(std::count(names.begin(), names.end(), name) == 1); }
	}
	CHECK(!ix.chores_owed());                                      // nobody checked in
	CHECK(ix.reclaim() > 0);                                       // the idle task's chore
	const auto names = fs.list();
	for (const auto& name : held) { CHECK(std::count(names.begin(), names.end(), name) == 0); }
	std::printf("reclaim after a release: ok\n");
}

// The default (defer_io off) is today's apply, byte for byte: a workload through the flush at the
// head of apply, storage admission's drain and positioned applies leaves the same verdicts, the
// same count of mutating calls and the same file bytes as the tree before defer_io (captured there).
void default_path_unchanged() {
	const auto fnv = [](std::uint64_t h, std::string_view b) {
		for (unsigned char c : b) {
			h ^= c;
			h *= 1099511628211ull;
		}
		return h;
	};
	std::string verdicts;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(150 * 1024);
	kv_index::options o = base(false);
	o.identity_seed = 7;
	o.buffer_share = 6000;
	o.storage = &storage;
	o.reclaim_slice = 0;
	o.reclaim_cap = 0;
	auto ix = kv_index::create(io, o, uuid_of("identity"));
	std::uint64_t pos = 0;
	const auto note = [&](const verdict& v) { verdicts += char('0' + int(v.result)); };
	for (int s = 0; s < 6; ++s) {
		for (int i = 0; i < 10; ++i) { note(ix.apply(tx({replace("k" + std::to_string(s * 10 + i), noise(900, std::uint64_t(s * 10 + i)))}), {1, ++pos})); }
		ix.commit();
		if (s == 3) {
			ix.force_merge(1);
			ix.commit();
		}
	}
	for (int i = 0; i < 40; ++i) { note(ix.apply(tx({replace("m" + std::to_string(i), noise(3000, std::uint64_t(100 + i)))}), {1, ++pos})); }
	note(ix.apply(tx({replace("k1", "x")}), {1, 3}));  // below the applied position
	note(ix.apply(tx({replace("k1", "y", 99)}), {1, ++pos}));
	ix.commit();
	std::uint64_t h = 1469598103934665603ull;
	for (const auto& n : fs.list()) {
		h = fnv(h, n);
		h = fnv(h, fs.view(n));
	}
	CHECK(verdicts == "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000555555555555541");
	CHECK(fs.mutations() == 163);
	CHECK(h == 0xcb9aafd89bf3cd0full);
	CHECK(ix.digest() == 0x6cc0ff118bf776d3ull);
	std::printf("default path unchanged: ok\n");
}

// The executor binding's waits (threadpool L2 re-check): a wait on a worker serves sibling items on
// its own stack. A host's flush chore (force_merge, commit) waits on the same index's merge chores
// through work_pool_executor (a group's work never waits on its own group: the chore waits on the
// index's child group); the merge needs the worker slot the chore holds, so the wait must run it
// inline, on a pool of one worker with no reserve and on two with one reserved, at the default width
// of one and at two. Nothing here holds a lock across the wait: the lease is the chore's own token.
void chore_waits_on_merge_chore(unsigned threads, unsigned reserve, unsigned width) {
	WorkPool::Options po;
	po.threads = threads;
	po.reserve = reserve;
	po.name_format = "dio{:02}";
	WorkPool pool(po);
	work_pool_executor ex(pool, width);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o = base(true);
	o.background = &ex;
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	auto ix = kv_index::create(io, o, uuid_of("chore-wait"));
	for (int s = 0; s < 4; ++s) {
		for (int i = 0; i < 8; ++i) { CHECK(accepted(ix.apply(tx({replace(key_of(std::size_t(s * 8 + i)), "v" + std::to_string(s))}), {}))); }
		ix.refresh();
	}
	const auto before = contents(ix);
	ix.maybe_merge();  // merge chores posted to the index's group
	WorkGroup host(pool);
	std::atomic<bool> done{false}, ok{false};
	host.enqueue(
		[&] {
			try {
				ix.force_merge(1);  // waits for the merge chores
				ix.commit();
				ok = true;
			} catch (...) {
			}
			done = true;
		},
		WorkClass::flush);
	if (!spin_until([&] { return done.load(); })) {
		std::fprintf(stderr, "chore_waits_on_merge_chore(%u, %u, width %u): the wait deadlocked\n", threads, reserve, width);
		std::_Exit(1);
	}
	host.wait();
	CHECK(ok && ix.segments().size() == 1 && contents(ix) == before);
	std::printf("chore waits on a merge chore (%u threads, %u reserved, width %u): ok\n", threads, reserve, width);
}

}  // namespace

int main() {
	default_path_unchanged();
	flush_owed(true);
	flush_owed(false);
	oversize_ends();
	rejected_buffer_ends();
	drain_owed(true);
	drain_owed(false);
	drain_ends();
	merge_publication_owed(true);
	merge_publication_owed(false);
	retention_read_owed(true);
	retention_read_owed(false);
	reclaim_after_release();
	for (unsigned width : {1u, 2u}) {
		chore_waits_on_merge_chore(1, 0, width);
		chore_waits_on_merge_chore(2, 1, width);
		chore_waits_on_merge_chore(4, 1, width);
	}
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); }
	return failures ? 1 : 0;
}
