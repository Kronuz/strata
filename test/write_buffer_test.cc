// The write buffer's step 1, items 2 and 3 (docs/design/write-buffer.md D2, D3, D4, D6, D8, D10;
// WB-2, WB-9, WB-16, WB-19): lanes and records, the version map without removal, the position
// index, tombstones and prev, rejections, pending deletes carried through installs, freeze, the
// flush's install clearing superseded copies, and checkout and check-in under the write lease.
#include "kv_test.hh"
#include "strata/core/owner.hh"
#include "strata/core/pool_executor.hh"
#include "strata/core/write_buffer.hh"

#include <atomic>
#include <set>
#include <thread>

namespace strata {
struct index_test_access {
	static void freeze(kv_index& ix) {
		const lease_scope l{ix.lease_};
		ix.freeze();
	}
	// The oldest frozen buffer flushed and installed alone, as a flush on the pool would be.
	static void flush_oldest(kv_index& ix) {
		const lease_scope l{ix.lease_};
		const std::size_t first = ix.segs_.size();
		ix.flush_documents(*ix.frozen_.front());
		ix.install_flushed(first);
	}
	static std::size_t frozen(const kv_index& ix) { return ix.frozen_.size(); }
	static std::size_t records(const kv_index& ix) { return ix.wb_ ? ix.wb_->order.size() : 0; }
	static std::size_t pending(const kv_index& ix) { return ix.wb_ ? ix.wb_->pending.size() : 0; }
	static std::size_t key_entries(const kv_index& ix) { return ix.wb_ ? ix.wb_->keys.size() : 0; }
	static std::uint64_t lane_bytes(const kv_index& ix) { return ix.wb_ ? ix.wb_->lane.bytes() : 0; }
	static std::uint64_t buffered_bytes(const kv_index& ix) { return ix.buffered_bytes(); }
	static std::uint64_t segment_dead(const kv_index& ix, std::size_t i) { return ix.segs_[i]->dead; }
	static std::size_t segment_count(const kv_index& ix) { return ix.segs_.size(); }
	struct rec {
		bool tomb;
		std::uint64_t docid, version;
		std::uint32_t position;
		wbuf::ref_t prev;
		std::string value;
	};
	// The active buffer's records for a key, newest first, by following prev from its slot.
	static std::vector<rec> chain(const kv_index& ix, std::string_view key) {
		std::vector<rec> out;
		if (!ix.wb_) { return out; }
		const auto& b = *ix.wb_;
		for (wbuf::ref_t r = ix.find_key_in(b, key); r != wbuf::no_ref;) {
			const auto& x = b.lane.at(r);
			out.push_back({x.tombstone(), x.docid, x.version, x.position, x.prev, x.doc.stored});
			r = x.prev;
		}
		return out;
	}
	static const write_lease& lease(const kv_index& ix) { return ix.lease_; }
	static bool merges_done(const kv_index& ix) {
		for (const auto& j : ix.pending_) {
			if (!j->done.load()) { return false; }
		}
		return !ix.pending_.empty();
	}
};
}  // namespace strata

namespace {

kv_index::options opts() {
	kv_index::options o;
	o.key_prefix = "k";
	return o;
}
operation keyless(const std::string& body) {
	operation o;
	o.type = operation::add;
	o.doc = doc("", body);
	return o;
}
operation erase_docid(std::uint64_t d) {
	operation o;
	o.type = operation::delete_docid;
	o.docid = d;
	return o;
}
operation replace_docid(std::uint64_t d, const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::replace_docid;
	o.docid = d;
	o.doc = doc(key, body);
	return o;
}
bool ok(const verdict& v) { return v.result == verdict::accepted; }
template <class F>
std::uint64_t breaks(F&& f) {
	detail::abort_on_owner_break().store(false);
	const std::uint64_t before = detail::off_owner_changes().load();
	f();
	const std::uint64_t n = detail::off_owner_changes().load() - before;
	detail::abort_on_owner_break().store(true);
	return n;
}
// Every live document's stored value by key, through the index's own reads.
std::map<std::string, std::string> scanned(const kv_index& ix) {
	std::map<std::string, std::string> out;
	ix.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace(std::string(k), std::string(v)); });
	return out;
}

// D4: slots store a reference with release, nothing is removed, and a restore in reverse order
// returns the table to its state before the stores.
void version_map_basics() {
	wbuf::version_map m;
	std::vector<std::uint32_t> vals;
	const auto eq = [&](std::uint32_t want) { return [&, want](wbuf::ref_t r) { return r == want; }; };
	m.reserve(1);
	for (std::uint32_t i = 0; i < 1000; ++i) {
		m.reserve(1);
		const auto p = m.locate(i * 7919u, eq(i));
		CHECK(wbuf::version_map::ref_of(p) == wbuf::no_ref);
		m.store(p, i * 7919u, i);
	}
	CHECK(m.size() == 1000);
	for (std::uint32_t i = 0; i < 1000; ++i) { CHECK(m.find(i * 7919u, eq(i)) == i); }
	// A transaction's stores, then their restore in reverse: the map is as before.
	m.reserve(4);
	std::vector<wbuf::version_map::place> undo;
	for (std::uint32_t i : {5u, 2000u, 2001u, 7u}) {
		const auto p = m.locate(i * 7919u, eq(i));
		undo.push_back(p);
		m.store(p, i * 7919u, i);
	}
	CHECK(m.size() == 1002);
	for (auto it = undo.rbegin(); it != undo.rend(); ++it) { m.restore(*it); }
	CHECK(m.size() == 1000 && m.find(2000u * 7919u, eq(2000)) == wbuf::no_ref && m.find(5u * 7919u, eq(5)) == 5);
	for (std::uint32_t i = 0; i < 1000; ++i) { CHECK(m.find(i * 7919u, eq(i)) == i); }
	std::printf("version map: ok\n");
}

// D3, WB-19: references stay valid across every chunk and directory block; a pin refuses appends
// and owns exactly the records' bytes.
void lane_refs() {
	struct rec {
		std::uint64_t a, b;
		std::array<char, 104> pad;
	};
	wbuf::lane<rec> l;
	std::vector<wbuf::ref_t> refs;
	const std::size_t n = 300000;  // about 35 MiB: past the 8 inline entries and two blocks
	for (std::size_t i = 0; i < n; ++i) { refs.push_back(l.append(rec{i, ~i, {}})); }
	bool all = true;
	for (std::size_t i = 0; i < n; ++i) { all = all && l.at(refs[i]).a == i && l.at(refs[i]).b == ~i; }
	CHECK(all);
	CHECK(wbuf::ref_chunk(refs.back()) >= 24);
	{
		auto pin = l.pin();
		CHECK(l.pinned() && pin.owns(&l.at(refs[0])) && pin.owns(&l.at(refs[n / 2])) && pin.owns(&l.at(refs.back())));
		const rec outside{};
		CHECK(!pin.owns(&outside));
		CHECK(throws([&] { l.append(rec{}); }));
	}
	CHECK(!l.pinned());
	wbuf::stable_array<std::uint32_t> a;
	for (std::uint32_t i = 0; i < 5000; ++i) { a.push_back(i); }
	const std::uint32_t* p = &a[100];
	for (std::uint32_t i = 5000; i < 20000; ++i) { a.push_back(i); }
	a.rewind(7000);
	CHECK(a.size() == 7000 && a[6999] == 6999 && p == &a[100] && *p == 100);
	std::printf("lane: %zu chunks' worth of records, refs stable\n", wbuf::ref_chunk(refs.back()) + 1);
}

// D3, D4, D6: every version is a record; the slot names the newest; a delete appends a
// tombstone the slot then names; prev links each record to the one its slot named before.
void tombstones_and_prev() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("tomb"));
	CHECK(ok(ix.apply(tx({replace("ka", "v1")}), {})));
	CHECK(ok(ix.apply(tx({replace("ka", "v2")}), {})));
	CHECK(ok(ix.apply(tx({erase("ka")}), {})));
	const auto c = index_test_access::chain(ix, "ka");
	CHECK(c.size() == 3 && c[0].tomb && !c[1].tomb && !c[2].tomb);
	CHECK(c.size() == 3 && c[1].value == "v2" && c[2].value == "v1" && c[1].version == 2 && c[2].version == 1 && c[0].docid == c[2].docid);
	CHECK(c.size() == 3 && c[0].position == 3 && c[1].position == 2 && c[2].position == 1 && c[2].prev == wbuf::no_ref);
	CHECK(index_test_access::key_entries(ix) == 1);  // the slot stays, naming the tombstone
	CHECK(!ix.get("ka") && !ix.find_key("ka") && scanned(ix).empty() && ix.buffered_count() == 2);
	CHECK(ok(ix.apply(tx({add("ka", "v3")}), {})));  // a re-add: a new document at version 1
	CHECK(ix.find_key("ka")->second == 1 && *ix.get("ka") == "v3");
	ix.refresh();
	CHECK(*ix.get("ka") == "v3" && contents(ix).size() == 1);
	(void)ix.check();
	std::printf("tombstones and prev: ok\n");
}

// D4: a rejected transaction restores the slots it changed and rewinds the position index; its
// records are never flushed.
void rejection_restores() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("reject"));
	CHECK(ok(ix.apply(tx({replace("ka", "v1")}), {})));
	const std::size_t records = index_test_access::records(ix), keys = index_test_access::key_entries(ix);
	const auto v = ix.apply(tx({replace("ka", "v2"), add("kb", "b"), erase("ka"), add("kb", "again")}), {});
	CHECK(v.result == verdict::version_conflict && v.op == 3);
	CHECK(index_test_access::records(ix) == records && index_test_access::key_entries(ix) == keys);
	CHECK(*ix.get("ka") == "v1" && !ix.get("kb") && ix.find_key("ka")->second == 1);
	CHECK(scanned(ix) == (std::map<std::string, std::string>{{"ka", "v1"}}));
	ix.refresh();
	CHECK(contents(ix).size() == 1 && contents(ix).begin()->second.second == "v1");
	// The docid map is built before the transaction changes anything (D3, D4): built during it,
	// it would take a slot for a record the rejection leaves unreferenced.
	CHECK(ok(ix.apply(tx({replace("kc", "c")}), {})));
	const std::uint64_t next = ix.find_key("kc")->first + 1;
	CHECK(ix.apply(tx({add("kq", "q"), erase_docid(9999), add("kc", "dup")}), {}).result == verdict::version_conflict);
	CHECK(!ix.has_docid(next) && !ix.get("kq"));
	(void)ix.check();
	std::printf("rejection restores: ok\n");
}

// CR1: a rejection cuts the lane back to the transaction's start, so its records and their
// document copies are freed at once, with or without an accepted record in the buffer.
void rejection_frees() {
	struct counted {
		int* live;
		explicit counted(int* l) : live(l) { ++*live; }
		~counted() { --*live; }
	};
	int live = 0;
	{
		wbuf::lane<counted> l;
		for (int i = 0; i < 10; ++i) { l.append(&live); }
		const auto m = l.tell();
		const std::uint64_t bytes = l.bytes();
		for (int i = 0; i < 1000; ++i) { l.append(&live); }  // past the first chunk
		l.truncate(m);
		CHECK(live == 10 && l.bytes() == bytes);
		const wbuf::ref_t r = l.append(&live);
		CHECK(live == 11 && l.at(r).live == &live);
	}
	CHECK(live == 0);
	const std::string big(64 * 1024, 'z');
	for (const bool in_segment : {false, true}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = kv_index::create(io, opts(), uuid_of("frees"));
		CHECK(ok(ix.apply(tx({replace("kexisting", "x")}), {})));
		if (in_segment) { ix.refresh(); }  // the buffer then holds no accepted record
		const auto reject = [&](int i) { return ix.apply(tx({replace("k" + std::to_string(i % 10), big), add("kexisting", "again")}), {}).result; };
		CHECK(reject(0) == verdict::version_conflict);
		const std::uint64_t lane = index_test_access::lane_bytes(ix);
		const std::size_t records = index_test_access::records(ix);
		const std::uint64_t buffered = index_test_access::buffered_bytes(ix);
		bool same = true;
		for (int i = 1; i < 2000; ++i) { same = reject(i) == verdict::version_conflict && same; }
		CHECK(same && index_test_access::lane_bytes(ix) == lane && index_test_access::records(ix) == records && index_test_access::buffered_bytes(ix) == buffered);
		CHECK(index_test_access::lane_bytes(ix) < 8192);
		ix.refresh();
		CHECK(contents(ix).size() == 1 && *ix.get("kexisting") == "x");
		(void)ix.check();
	}
	std::printf("rejection frees: ok\n");
}

// D6, D8: a replace or delete of a segment's copy is pending until the freeze, shadowed meanwhile;
// the freeze applies it to the segment's bits.
void pending_until_freeze() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("pending"));
	CHECK(ok(ix.apply(tx({replace("ka", "v1"), replace("kb", "b"), keyless("loose")}), {})));
	const std::uint64_t loose = 3;
	ix.refresh();
	CHECK(ok(ix.apply(tx({replace("ka", "v2"), erase("kb"), erase_docid(loose)}), {})));
	CHECK(index_test_access::pending(ix) == 3 && index_test_access::segment_dead(ix, 0) == 0);
	CHECK(*ix.get("ka") == "v2" && !ix.get("kb") && !ix.has_docid(loose) && ix.find_key("ka")->second == 2);
	CHECK(scanned(ix) == (std::map<std::string, std::string>{{"ka", "v2"}}));
	CHECK(ix.apply(tx({erase_docid(loose)}), {}).docids == std::vector<std::uint64_t>{0});  // already gone
	(void)ix.check();  // the digest applies them: one live copy per key and docid
	ix.refresh();
	CHECK(index_test_access::segment_dead(ix, 0) == 3 && contents(ix).size() == 1);
	(void)ix.check();
	std::printf("pending until freeze: ok\n");
}

// WB-2: a merge's install carries the pending deletes naming its sources to its output.
void merge_carries_pending() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("merge"));
	CHECK(ok(ix.apply(tx({replace("ka", "a"), replace("kb", "b")}), {})));
	ix.refresh();
	CHECK(ok(ix.apply(tx({replace("kc", "c")}), {})));
	ix.refresh();
	CHECK(ok(ix.apply(tx({erase("ka"), replace("kc", "c2")}), {})));  // pending on both segments
	CHECK(index_test_access::pending(ix) == 2);
	ix.force_merge(1);
	CHECK(index_test_access::segment_count(ix) == 1 && index_test_access::pending(ix) == 2);
	CHECK(!ix.get("ka") && *ix.get("kc") == "c2");
	(void)ix.check();
	ix.refresh();
	CHECK(!ix.get("ka") && *ix.get("kc") == "c2" && contents(ix).size() == 2);
	(void)ix.check();
	std::printf("merge carries pending: ok\n");
}

// WB-16: a frozen buffer keeps no live bits; a newer record or tombstone shadows its copies, and
// its flush's install clears in the new segment every copy a newer buffer holds.
void flush_install_clears() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("install"));
	CHECK(ok(ix.apply(tx({replace("ka", "a1"), replace("kb", "b"), replace("kx", "x")}), {})));
	index_test_access::freeze(ix);
	CHECK(index_test_access::frozen(ix) == 1 && index_test_access::records(ix) == 0);
	CHECK(*ix.get("ka") == "a1" && ix.find_key("kb")->second == 1);  // read through the frozen buffer
	CHECK(ok(ix.apply(tx({replace("ka", "a2"), erase("kx")}), {})));
	CHECK(index_test_access::pending(ix) == 0);  // a frozen copy takes no pending delete
	CHECK(*ix.get("ka") == "a2" && !ix.get("kx") && ix.find_key("ka")->second == 2);
	CHECK(scanned(ix) == (std::map<std::string, std::string>{{"ka", "a2"}, {"kb", "b"}}));
	(void)ix.check();
	index_test_access::flush_oldest(ix);
	CHECK(index_test_access::frozen(ix) == 0 && index_test_access::segment_dead(ix, 0) == 2);
	CHECK(*ix.get("ka") == "a2" && !ix.get("kx"));
	(void)ix.check();
	ix.refresh();
	CHECK(*ix.get("ka") == "a2" && !ix.get("kx") && contents(ix).size() == 2);
	CHECK(scanned(ix) == (std::map<std::string, std::string>{{"ka", "a2"}, {"kb", "b"}}));
	(void)ix.check();
	std::printf("flush install clears: ok\n");
}

// WB-16, D6: keyless documents in a frozen buffer, deleted or replaced by docid, are shadowed
// through the active buffer's docid map, and the frozen buffer's install drops them.
void keyless_docid_shadowing() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("keyless"));
	const auto v = ix.apply(tx({keyless("one"), keyless("two"), replace("ka", "a")}), {});
	CHECK(ok(v) && v.docids.size() == 3);
	const std::uint64_t d1 = v.docids[0], d2 = v.docids[1], da = v.docids[2];
	index_test_access::freeze(ix);
	CHECK(ix.has_docid(d1) && ix.has_docid(d2));
	CHECK(ok(ix.apply(tx({erase_docid(d1), replace_docid(d2, "", "two-b")}), {})));
	CHECK(!ix.has_docid(d1) && ix.has_docid(d2));
	CHECK(ix.apply(tx({erase_docid(d1)}), {}).docids == std::vector<std::uint64_t>{0});
	// The keyed document by docid: replace it under a new key; its old key dies with it.
	CHECK(ok(ix.apply(tx({replace_docid(da, "kz", "z")}), {})));
	CHECK(!ix.get("ka") && *ix.get("kz") == "z");
	(void)ix.check();
	index_test_access::flush_oldest(ix);
	CHECK(index_test_access::segment_dead(ix, 0) == 3);
	CHECK(!ix.has_docid(d1) && ix.has_docid(d2) && !ix.get("ka"));
	(void)ix.check();
	ix.refresh();
	const auto c = contents(ix);
	CHECK(c.size() == 2 && !c.count(d1) && c.count(d2) && c.at(d2).second == "two-b" && c.at(da).second == "z");
	(void)ix.check();
	std::printf("keyless docid shadowing: ok\n");
}

// D8: writes after a freeze land in a new buffer; a scan merges the frozen and active buffers
// newest first, a tombstone hiding the frozen copy; a failed flush keeps its frozen buffer.
void freeze_and_scan() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("scan"));
	for (int i = 0; i < 6000; ++i) { CHECK(ok(ix.apply(tx({replace("k" + std::to_string(10000 + i), "f")}), {}))); }
	index_test_access::freeze(ix);
	CHECK(ok(ix.apply(tx({erase("k10003"), replace("k10004", "a"), replace("k99999", "new")}), {})));
	const auto got = scanned(ix);
	CHECK(got.size() == 6000 && !got.count("k10003") && got.at("k10004") == "a" && got.at("k99999") == "new");
	std::size_t calls = 0;
	ix.scan("k10002", 3, [&](std::string_view k, std::string_view v, std::uint64_t ver) {
		CHECK((calls == 0 && k == "k10002" && v == "f") || (calls == 1 && k == "k10004" && v == "a" && ver == 2) || (calls == 2 && k == "k10005"));
		++calls;
	});
	CHECK(calls == 3);
	ix.refresh();
	CHECK(scanned(ix) == got && index_test_access::frozen(ix) == 0);
	(void)ix.check();
	std::printf("freeze and scan: ok\n");
}

// D2: checkout never waits; check-in runs the chores owed (finished merges, retired views) under
// the lease when it is free, and leaves them to the holder when a writer holds it.
void checkout_and_check_in() {
	ThreadPool<> pool("wbtest{}", 2);
	pool_executor<> ex(pool);
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto o = opts();
	o.background = &ex;
	o.readers = &reg;
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	auto ix = kv_index::create(io, o, uuid_of("checkin"));
	for (int s = 0; s < 4; ++s) {
		CHECK(ok(ix.apply(tx({replace("k" + std::to_string(s), "v")}), {})));
		ix.refresh();
	}
	const std::size_t before = index_test_access::segment_count(ix);
	ix.maybe_merge();
	// Wait for the merges to finish on the pool without publishing them.
	for (int spin = 0; spin < 20000 && !index_test_access::merges_done(ix); ++spin) { std::this_thread::yield(); }
	CHECK(index_test_access::segment_count(ix) == before);
	std::uint64_t n = breaks([&] {
		std::thread([&] {
			const auto c = ix.check_out();  // on another thread, while no writer runs
		}).join();
	});
	CHECK(n == 0 && index_test_access::segment_count(ix) < before);
	// A check-in while a writer holds the lease leaves the chores owed, with no break.
	n = breaks([&] {
		const lease_scope held{index_test_access::lease(ix)};
		std::thread([&] { auto c = ix.check_out(); }).join();
		CHECK(ix.chores_owed());
	});
	CHECK(n == 0);
	CHECK(ok(ix.apply(tx({replace("k9", "v")}), {})));  // the next writer call runs them
	CHECK(!ix.chores_owed());
	ix.commit();
	(void)ix.check();
	std::printf("checkout and check-in: %zu segments to %zu\n", before, index_test_access::segment_count(ix));
}

// CR2: a check-in whose chores fail (a merge that failed with EIO) fences the index instead of
// throwing from ~checkout; the next writer call reports the fence.
void checkin_fences() {
	ThreadPool<> pool("wbfence{}", 2);
	pool_executor<> ex(pool);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto o = opts();
	o.background = &ex;
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	auto ix = kv_index::create(io, o, uuid_of("fences"));
	for (int s = 0; s < 4; ++s) {
		CHECK(ok(ix.apply(tx({replace("k" + std::to_string(s), "v")}), {})));
		ix.refresh();
	}
	fs.add_fault({faultfs::op::create, 1, EIO, false});  // the merge's first create
	ix.maybe_merge();
	for (int spin = 0; spin < 200000 && !index_test_access::merges_done(ix); ++spin) { std::this_thread::yield(); }
	CHECK(index_test_access::merges_done(ix));
	{
		auto c = ix.check_out();
	}  // publishes the failed merge: fences, and survives
	bool fenced = false;
	try {
		ix.apply(tx({replace("ka", "v")}), {});
	} catch (const index_fenced&) {
		fenced = true;
	}
	CHECK(fenced);
	std::printf("check-in fences: ok\n");
}

// CR4: a check-in that meets a writer's lease leaves its chores owed; the writer re-reads the
// flag after its handoff, so none stays owed with the lease free, whatever the call (here apply,
// refresh and the short wait_merges) and however the check-in's timing falls against the handoff.
void owed_chores_run() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("owed"));
	const int trials = 40000;
	const auto span = [](int t) { return t % 4 < 2 ? 3000u : 200u; };  // short calls, short delays
	std::atomic<int> round{0}, ready{0}, done{0};
	std::thread other([&] {
		for (int t = 1; t <= trials; ++t) {
			while (round.load(std::memory_order_acquire) < t) {}
			ready.fetch_add(1);
			while (ready.load() < 2 * t) {}
			for (volatile unsigned i = 0; i < (t * 2654435761u >> 7) % span(t); ++i) {}
			{ auto c = ix.check_out(); }
			done.fetch_add(1);
		}
	});
	int stuck = 0;
	for (int t = 1; t <= trials; ++t) {
		round.store(t, std::memory_order_release);
		ready.fetch_add(1);
		while (ready.load() < 2 * t) {}
		if (t % 4 == 0) {
			ix.refresh();
		} else if (t % 4 == 1) {
			ix.apply(tx({replace("k" + std::to_string(t % 5), "v")}), {});
		} else {
			ix.wait_merges();
		}
		while (done.load() < t) {}
		if (ix.chores_owed()) {
			++stuck;
			ix.apply(tx({replace("kz", "v")}), {});
		}
	}
	other.join();
	CHECK(stuck == 0);
	std::printf("owed chores run: %d of %d trials left them owed\n", stuck, trials);
}

// D2: a writer that finds a check-in's chores running waits for them instead of reporting a
// second holder; two writers at once are still reported (SR-D20).
void writer_waits_for_chores() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(), uuid_of("waits"));
	std::atomic<bool> in{false}, go{false};
	const std::uint64_t n = breaks([&] {
		std::thread chores([&] {
			const chore_scope c{index_test_access::lease(ix)};
			CHECK(bool(c));
			in = true;
			while (!go) { std::this_thread::yield(); }
		});
		while (!in) { std::this_thread::yield(); }
		std::thread release([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			go = true;
		});
		CHECK(ok(ix.apply(tx({replace("ka", "v")}), {})));  // waits for the chore scope
		CHECK(go.load());
		chores.join();
		release.join();
	});
	CHECK(n == 0);
	if constexpr (detail::lease_checks) {
		// CR3: a lease-less read while another thread's check-in runs chores is reported.
		in = false;
		go = false;
		const std::uint64_t reads = breaks([&] {
			std::thread chores([&] {
				const chore_scope c{index_test_access::lease(ix)};
				in = true;
				while (!go) { std::this_thread::yield(); }
			});
			while (!in) { std::this_thread::yield(); }
			(void)ix.get("ka");
			(void)ix.find_key("ka");
			go = true;
			chores.join();
			(void)ix.get("ka");  // no chores running: nothing reported
		});
		CHECK(reads == 2);
		const std::uint64_t second = breaks([&] {
			const lease_scope held{index_test_access::lease(ix)};
			std::thread([&] { const lease_scope other{index_test_access::lease(ix)}; }).join();
		});
		CHECK(second == 1);
	}
	std::printf("writer waits for chores: ok\n");
}

// WB-9's order at one writer with a reader on another thread: a slot a reader loads names a
// complete record. The map is reserved first, so no growth frees a table under the reader.
void slot_after_record() {
	struct rec {
		std::uint64_t key, check;
	};
	wbuf::lane<rec> l;
	wbuf::version_map m;
	const std::uint32_t n = 200000;
	m.reserve(n);
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> torn{0}, seen{0};
	std::thread reader([&] {
		std::uint32_t k = 0;
		while (!done.load(std::memory_order_acquire)) {
			const wbuf::ref_t r = m.find(k * 2654435761u, [](wbuf::ref_t) { return true; });
			if (r != wbuf::no_ref) {
				const rec& x = l.at(r);
				if (x.check != ~x.key) { ++torn; }
				++seen;
			}
			k = (k + 1) % n;
		}
	});
	for (std::uint32_t k = 0; k < n; ++k) {
		const auto p = m.locate(k * 2654435761u, [&](wbuf::ref_t r) { return l.at(r).key == k; });
		const wbuf::ref_t r = l.append(rec{k, ~std::uint64_t(k)});
		m.store(p, k * 2654435761u, r);
	}
	done.store(true, std::memory_order_release);
	reader.join();
	CHECK(torn.load() == 0);
	std::printf("slot after record: %llu reads, %llu torn\n", (unsigned long long)seen.load(), (unsigned long long)torn.load());
}

}  // namespace

int main() {
	version_map_basics();
	lane_refs();
	tombstones_and_prev();
	rejection_restores();
	rejection_frees();
	pending_until_freeze();
	merge_carries_pending();
	flush_install_clears();
	keyless_docid_shadowing();
	freeze_and_scan();
	checkout_and_check_in();
	checkin_fences();
	owed_chores_run();
	writer_waits_for_chores();
	slot_after_record();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("write_buffer_test: ok\n");
	return 0;
}
