// Reclaim's deletes on the host's executor (ledger docs/status/reclaim-bg.md). With an executor
// the owner accounts for a queued file at once and posts its truncate, sync and unlink to a
// group of its own; the outcome comes back over a channel.
//   1. A publish posts the deletes and returns before they run (the files are on disk, counted,
//      none queued again by a commit); running the executor deletes them; wait_merges() runs
//      the merges only.
//   2. A failed truncate, sync or remove (faultfs) is requeued at the size left, loses no
//      bytes and counts none twice, and a later round deletes the file.
//   3. Admission's drain() waits for the deletes before it reports space.
//   4. close() and the destructor wait for the deletes; they never cancel them.
//   5. A crash mid-delete leaves an orphan that the next open reclaims.
//   6. A delete the executor drops without running is requeued.
//   7. The cap still bounds a publish. 8. Three workers, 800 faulted rounds (the channel's lock).
//   9. Admission with every delete already posted waits for them.
//   10. A file already gone (ENOENT) counts as deleted.
// The mapped run aborts on a truncation below a live mapping.
#include "kv_test.hh"

#include "strata/core/executor.hh"
#include "strata/core/pool_executor.hh"

#include <algorithm>
#include <memory>
#include <optional>
#include <random>
#include <set>

static const auto uuid = uuid_of("core-reclaim-bg");

// Runs posted work when the test says so. A group's wait() runs that group's items only, as a
// pool's task group does; run_all() runs everything queued.
class manual_executor final : public executor {
public:
	struct item {
		work fn;
		task::cancel_token stop;
		const void* owner;
	};
	std::unique_ptr<group> open() override { return std::make_unique<manual_group>(*this); }
	std::uint64_t now() override { return 0; }
	bool sleep_until(std::uint64_t, const task::cancel_token& stop) override { return !stop.requested(); }
	std::size_t queued() const { return items_.size(); }
	void run_all() { run_if([](const void*) { return true; }); }
	// Drops the queued items without running them, as an executor shutting down does.
	void drop_all() { items_.clear(); }

private:
	template <class Pred>
	void run_if(Pred pred) {
		for (std::size_t i = 0; i < items_.size();) {
			if (!pred(items_[i]->owner)) {
				++i;
				continue;
			}
			const auto it = items_[i];
			items_.erase(items_.begin() + std::ptrdiff_t(i));
			it->fn(it->stop);
		}
	}
	class manual_group final : public group {
	public:
		explicit manual_group(manual_executor& e) : e_(&e) {}
		~manual_group() override { cancel(); }
		void post(work w) override { e_->items_.push_back(std::make_shared<item>(item{std::move(w), source_.token(), this})); }
		void cancel() noexcept override {
			source_.request();
			std::erase_if(e_->items_, [&](const auto& i) { return i->owner == this; });
			source_ = task::cancel_source();
		}
		void wait() override {
			e_->run_if([&](const void* o) { return o == this; });
		}

	private:
		manual_executor* e_;
		task::cancel_source source_;
	};
	std::vector<std::shared_ptr<item>> items_;
};

namespace strata {
// Reads the accounting a public call does not show.
struct index_test_access {
	static std::uint64_t on_disk(const kv_index& ix) { return ix.on_disk_; }
	static std::uint64_t inflight(const kv_index& ix) { return ix.inflight_; }
	static std::uint64_t sum_files(const kv_index& ix) {
		std::uint64_t n = 0;
		for (const auto& [name, size] : ix.files_) { n += size; }
		return n;
	}
	static std::uint64_t sum_posted(const kv_index& ix) {
		std::uint64_t n = 0;
		for (const auto& [name, size] : ix.posted_) { n += size; }
		return n;
	}
	static bool drain(kv_index& ix) { return ix.drain(); }
};
}  // namespace strata
using peek = strata::index_test_access;

struct on_disk {
	std::size_t segments = 0;
	std::uint64_t bytes = 0;  // every strata file
};

static on_disk survey(const faultfs::filesystem& fs) {
	on_disk r;
	for (const auto& n : fs.list()) {
		auto p = index_detail::parse_name(n);
		if (!p) { continue; }
		r.bytes += fs.view(n).size();
		if (p->type == index_detail::parsed_name::segment) { ++r.segments; }
	}
	return r;
}

static std::vector<operation> round(int r, int keys) {
	std::vector<operation> ops;
	for (int i = 0; i < keys; ++i) { ops.push_back(replace("Q" + std::to_string(i), "round" + std::to_string(r) + std::string(300, char('a' + r % 26)))); }
	return ops;
}

static kv_index::options opts(executor* ex, byte_budget* storage = nullptr) {
	kv_index::options o;
	o.key_prefix = "Q";
	o.auto_merge = true;
	o.reclaim_step = 256;  // a segment is truncated over several steps before it is unlinked
	o.reclaim_slice = 0;
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	o.background = ex;
	o.storage = storage;
	return o;
}

// Runs the deletes and collects them, until nothing is queued or posted.
static void settle(kv_index& ix, manual_executor& ex) {
	for (int i = 0; i < 3; ++i) {
		ex.run_all();
		ix.reclaim();
	}
}

// Applies a round, schedules the merge, and runs the merges only: their publish posts the deletes.
static void merge_round(kv_index& ix, int r) {
	ix.apply(tx(round(r, 50)), {});
	ix.refresh();
	ix.wait_merges();
}

// The accounting holds the files exactly: the sum of the files on record is on_disk_, and with
// nothing posted the bytes on disk are on_disk_.
static void balanced(const kv_index& ix, const faultfs::filesystem& fs, const char* where) {
	if (peek::sum_files(ix) != peek::on_disk(ix)) {
		std::fprintf(stderr, "%s: files on record %llu, on_disk %llu\n", where, (unsigned long long)peek::sum_files(ix), (unsigned long long)peek::on_disk(ix));
		CHECK(false);
	}
	CHECK(peek::sum_posted(ix) == peek::inflight(ix));
	if (!ix.deletes_pending() && survey(fs).bytes != peek::on_disk(ix)) {
		std::fprintf(stderr, "%s: bytes on disk %llu, on_disk %llu\n", where, (unsigned long long)survey(fs).bytes, (unsigned long long)peek::on_disk(ix));
		CHECK(false);
	}
}

// 1.
static void posted() {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(1ull << 40);
	auto ix = kv_index::create(io, opts(&ex, &storage), uuid);
	std::size_t posted_seen = 0, kept_dead = 0;
	for (int r = 0; r < 30; ++r) {
		merge_round(ix, r);
		const auto d = survey(fs);
		posted_seen += ix.deletes_pending();
		// The publish returned with the deletes queued: their files are still on disk, counted by
		// the permit, and no commit or publish queues them a second time.
		if (ix.deletes_pending()) {
			CHECK(d.segments > ix.segments().size());
			++kept_dead;
		}
		CHECK(storage.used() >= d.bytes);
		CHECK(ix.reclaim_pending() == 0);
		balanced(ix, fs, "posted");
		ix.commit();  // retain() sees the posted files neither on record nor queued
		ix.reclaim();  // posts what the commit queued, beside the deletes in flight
		balanced(ix, fs, "posted, committed");
	}
	CHECK(posted_seen > 0 && kept_dead > 0);
	settle(ix, ex);
	CHECK(ix.deletes_pending() == 0);
	CHECK(ix.reclaim_pending() == 0);
	balanced(ix, fs, "posted, run");
	CHECK(survey(fs).segments == ix.segments().size());
	CHECK(storage.used() >= survey(fs).bytes);
	for (int k = 0; k < 50; k += 7) { CHECK(ix.get("Q" + std::to_string(k)).has_value()); }
	// Run every round: nothing waits.
	for (int r = 30; r < 60; ++r) {
		merge_round(ix, r);
		ix.commit();  // the new manifest stops naming the merged-away sources
		settle(ix, ex);
		CHECK(survey(fs).segments == ix.segments().size());
		balanced(ix, fs, "run each round");
	}
	std::printf("reclaim_bg: posted: %zu deletes seen queued, %zu publishes left dead files on disk\n", posted_seen, kept_dead);
	ix.close();
}

// 2.
static void failed(faultfs::op kind, const char* what) {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(1ull << 40);
	auto ix = kv_index::create(io, opts(&ex, &storage), uuid);
	for (int r = 0; r < 6; ++r) { merge_round(ix, r); }
	const std::size_t queued = ix.deletes_pending();
	CHECK(queued >= 2);
	fs.add_fault({kind, 1, EIO});
	ex.run_all();
	ix.reclaim();  // collects, then posts the requeued file again
	CHECK(ix.deletes_pending() == 1);
	// The failed file is back on record (queued or posted again), its bytes counted once.
	balanced(ix, fs, what);
	CHECK(storage.used() >= survey(fs).bytes);
	CHECK(survey(fs).segments == ix.segments().size() + 1);
	ex.run_all();
	ix.reclaim();
	CHECK(ix.deletes_pending() == 0);
	CHECK(ix.reclaim_pending() == 0);
	CHECK(survey(fs).segments == ix.segments().size());
	balanced(ix, fs, what);
	for (int k = 0; k < 50; k += 7) { CHECK(ix.get("Q" + std::to_string(k)).has_value()); }
	std::printf("reclaim_bg: failed %s: requeued, then deleted (%zu posted)\n", what, queued);
	ix.close();
}

// 3. A queued 31,000-byte segment on a budget and a disk with no room for the next record: the
// admission's drain frees it durably, waiting for the posted deletes, so the record is accepted.
static void drain_waits() {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(1ull << 40);
	kv_index::options o = opts(&ex, &storage);
	o.auto_merge = false;
	o.retained_predecessors = 0;
	std::mt19937_64 r(35);
	auto record = [&](const std::string& key, std::size_t bytes) {
		operation op = add(key, "x");
		op.doc.stored.resize(bytes);
		for (auto& c : op.doc.stored) { c = char(r()); }
		return op;
	};
	std::optional<byte_budget> tight_budget;
	{
		auto ix = kv_index::create(io, o, uuid);
		ix.apply(tx({record("Qold", 31000)}), {});
		ix.commit();
		ix.apply(tx({erase("Qold")}), {});
		ix.commit();
		CHECK(ix.reclaim_pending() == 1);
		tight_budget.emplace(storage.used() + 2000);
		ix.close();
	}
	byte_budget& tight = *tight_budget;
	o.storage = &tight;
	auto ix = kv_index::open(io, o);
	CHECK(ix.reclaim_pending() == 1);
	fs.set_capacity(fs.used() + (tight.capacity() - tight.used()));
	CHECK(ix.apply(tx({record("Qnew", 10000)}), {}).result == verdict::accepted);
	CHECK(ix.deletes_pending() == 0);  // drain waited and collected
	CHECK(ix.reclaim_pending() == 0);
	CHECK(!throws([&] { ix.commit(); }));
	// drain() itself: nothing to free reports no space; queued deletes are waited for.
	CHECK(!peek::drain(ix));
	ix.apply(tx({erase("Qnew")}), {});
	ix.commit();
	CHECK(ix.reclaim_pending() == 1);
	const std::size_t before = survey(fs).segments;
	CHECK(peek::drain(ix));
	CHECK(survey(fs).segments == before - 1);
	CHECK(ex.queued() == 0);
	balanced(ix, fs, "drain");
	std::printf("reclaim_bg: drain waited for the deletes\n");
	ix.close();
}

// 4.
static void close_waits() {
	for (int how = 0; how < 2; ++how) {
		manual_executor ex;
		faultfs::filesystem fs;
		faultfs::io io(fs);
		std::size_t live = 0;
		{
			auto ix = kv_index::create(io, opts(&ex), uuid);
			for (int r = 0; r < 12; ++r) { merge_round(ix, r); }
			ix.commit();
			CHECK(ix.deletes_pending() > 0);
			CHECK(ex.queued() > 0);
			live = ix.segments().size();
			CHECK(survey(fs).segments > live);
			if (how == 0) { ix.close(); }
			// how 1: the destructor ends the index.
		}
		CHECK(ex.queued() == 0);  // none was cancelled: all ran
		CHECK(survey(fs).segments == live);
	}
	std::printf("reclaim_bg: close and the destructor waited for the deletes\n");
}

// 5. Power fails at each mutation of the deletes: the reopen finds the orphan and reclaims it.
static void crash_mid_delete() {
	std::size_t orphans = 0, points = 0;
	for (std::uint64_t at = 1; at <= 900; at += 9, ++points) {
		manual_executor ex;
		faultfs::filesystem fs;
		faultfs::io io(fs);
		{
			auto ix = kv_index::create(io, opts(&ex), uuid);
			for (int r = 0; r < 8; ++r) { merge_round(ix, r); }
			ix.commit();
			CHECK(ix.deletes_pending() > 0);
			fs.power_off_after(at);
			ex.run_all();
			ix.close();
		}
		fs.crash(at);
		std::size_t cut = 0;
		for (const auto& n : fs.list()) {
			auto p = index_detail::parse_name(n);
			if (p && p->type == index_detail::parsed_name::segment) {
				try {
					(void)format::decode_footer(fs.view(n));
				} catch (const std::exception&) {
					++cut;
				}
			}
		}
		orphans += cut;
		auto ix = kv_index::open(io, opts(&ex));
		ix.reclaim();
		ex.run_all();
		ix.reclaim();
		CHECK(survey(fs).segments == ix.segments().size());
		balanced(ix, fs, "crash");
		for (int k = 0; k < 50; k += 7) { CHECK(ix.get("Q" + std::to_string(k)).has_value()); }
		ix.close();
	}
	CHECK(orphans > 0);
	std::printf("reclaim_bg: crash mid-delete: %zu points, %zu truncated orphans reclaimed\n", points, orphans);
}

// 6.
static void dropped() {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(1ull << 40);
	auto ix = kv_index::create(io, opts(&ex, &storage), uuid);
	for (int r = 0; r < 6; ++r) { merge_round(ix, r); }
	const std::size_t queued = ix.deletes_pending();
	CHECK(queued >= 2);
	ex.drop_all();
	CHECK(peek::drain(ix));  // waits, finds that nothing ran, and requeues every file
	CHECK(ix.reclaim_pending() == queued);
	balanced(ix, fs, "dropped");
	ex.run_all();
	ix.reclaim();
	CHECK(ix.reclaim_pending() == 0);
	ex.run_all();
	ix.reclaim();
	CHECK(survey(fs).segments == ix.segments().size());
	std::printf("reclaim_bg: %zu dropped deletes requeued and run\n", queued);
	ix.close();
}

// 7. The quota still bounds a publish: it posts files until the cap is spent, and the debt left
// is posted by later commits.
static void quota() {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto o = opts(&ex);
	o.auto_merge = false;
	o.reclaim_cap = 2048;
	o.reclaim_slice = 2048;
	auto ix = kv_index::create(io, o, uuid);
	for (int s = 0; s < 8; ++s) {
		std::vector<operation> ops;
		for (int i = 0; i < 60; ++i) { ops.push_back(add("Q" + std::to_string(s * 100 + i), std::string(300, char('a' + s)) + std::to_string(i))); }
		ix.apply(tx(ops), {});
		ix.refresh();
	}
	std::uint64_t largest = 0;
	for (const auto& v : ix.segments()) { largest = std::max<std::uint64_t>(largest, fs.view(index_detail::segment_name(v.id)).size()); }
	ix.maybe_merge();
	ix.wait_merges();  // merges and publishes, posting what the cap allows
	CHECK(ix.deletes_pending() >= 1 && ix.deletes_pending() < 8);
	CHECK(peek::inflight(ix) >= o.reclaim_cap && peek::inflight(ix) <= o.reclaim_cap + largest);
	CHECK(ix.reclaim_pending() > 0);  // the debt remains
	std::size_t commits = 0;
	while (ix.reclaim_pending() && commits < 50) {
		ix.commit();
		++commits;
	}
	CHECK(commits > 1);
	settle(ix, ex);
	CHECK(survey(fs).segments == ix.segments().size());
	std::printf("reclaim_bg: quota: the publish posted up to the cap, %zu commits posted the rest\n", commits);
	ix.close();
}

// 8. On a thread pool the deletes run beside the owner: three workers, 800 rounds of merges, commits
// and 400 injected faults (before and after the effect), a reopen whenever one fences. Workers report
// failures at the same time, so the channel's lock is exercised (the TSan run flags it without one).
static void threaded() {
	ThreadPool<> pool("del{}", 3);
	pool_executor<> ex(pool);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(1ull << 40);
	std::mt19937_64 rng(7);
	auto ix = kv_index::create(io, opts(&ex, &storage), uuid);
	const faultfs::op kinds[] = {faultfs::op::truncate, faultfs::op::sync, faultfs::op::remove};
	for (int i = 0; i < 400; ++i) { fs.add_fault({kinds[rng() % 3], 1 + rng() % 4000, rng() % 2 ? EIO : ENOSPC, bool(rng() % 2)}); }
	for (int r = 0; r < 800; ++r) {
		try {
			ix.apply(tx(round(r, 30)), {});
			ix.refresh();
			if (r % 2 == 0) { ix.wait_merges(); }
			if (r % 3 == 0) { ix.commit(); }
			if (r % 11 == 0) { ix.reclaim(); }
			if (r % 37 == 0) { ix.wait_deletes(); }
		} catch (const index_fenced&) {
			try {
				ix.close();
			} catch (...) {
			}
			for (int a = 0; a < 20; ++a) {
				try {
					ix = kv_index::open(io, opts(&ex, &storage));
					break;
				} catch (const std::exception&) {
				}
			}
			continue;
		} catch (const std::exception&) {
		}
		CHECK(peek::sum_files(ix) == peek::on_disk(ix));
		CHECK(peek::sum_posted(ix) == peek::inflight(ix));
		for (int k = 0; k < 30; k += 13) { CHECK(ix.get("Q" + std::to_string(k)).has_value()); }
	}
	fs.clear_faults();
	for (int i = 0; i < 6; ++i) {
		ix.wait_merges();
		ix.commit();
		ix.reclaim();
		ix.wait_deletes();
	}
	CHECK(ix.deletes_pending() == 0);
	CHECK(ix.reclaim_pending() == 0);
	CHECK(peek::inflight(ix) == 0);
	CHECK(peek::on_disk(ix) == survey(fs).bytes);
	CHECK(survey(fs).segments == ix.segments().size());
	const auto used = storage.used();
	ix.close();
	byte_budget storage2(1ull << 40);
	auto again = kv_index::open(io, opts(&ex, &storage2));
	again.wait_deletes();
	CHECK(used <= storage2.used() + 4096 + 64 * 1024);  // the permit is never far above a fresh count
	CHECK(storage2.used() <= used + 64 * 1024);
	std::printf("reclaim_bg: threaded: 800 faulted rounds on 3 workers settled\n");
	again.close();
}

// 9. Admission with every delete already posted (nothing queued, bytes in flight): drain() waits
// for them instead of answering "nothing to free".
static void drain_posted() {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget storage(1ull << 40);
	kv_index::options o = opts(&ex, &storage);
	o.auto_merge = false;
	o.retained_predecessors = 0;
	std::mt19937_64 r(35);
	auto record = [&](const std::string& key, std::size_t bytes) {
		operation op = add(key, "x");
		op.doc.stored.resize(bytes);
		for (auto& c : op.doc.stored) { c = char(r()); }
		return op;
	};
	std::optional<byte_budget> tight_budget;
	{
		auto ix = kv_index::create(io, o, uuid);
		ix.apply(tx({record("Qold", 31000)}), {});
		ix.commit();
		ix.apply(tx({erase("Qold")}), {});
		ix.commit();
		tight_budget.emplace(storage.used() + 2000);
		ix.close();
	}
	byte_budget& tight = *tight_budget;
	o.storage = &tight;
	auto ix = kv_index::open(io, o);  // queues the orphan
	ix.reclaim();  // posts it; the executor has not run
	CHECK(ix.reclaim_pending() == 0);
	CHECK(ix.deletes_pending() == 1);
	fs.set_capacity(fs.used() + (tight.capacity() - tight.used()));
	CHECK(ix.apply(tx({record("Qnew", 10000)}), {}).result == verdict::accepted);
	std::printf("reclaim_bg: admission waited for deletes already posted\n");
	ix.close();
}

// 10. A file that is already gone (ENOENT at the open) counts as deleted, not as a failure to
// retry for ever.
static void enoent_is_done() {
	manual_executor ex;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, opts(&ex), uuid);
	for (int r = 0; r < 6; ++r) { merge_round(ix, r); }
	CHECK(ix.deletes_pending() >= 2);
	std::set<std::string> live;
	for (const auto& v : ix.segments()) { live.insert(index_detail::segment_name(v.id)); }
	std::size_t gone = 0;
	for (const auto& n : fs.list()) {
		auto p = index_detail::parse_name(n);
		if (p && p->type == index_detail::parsed_name::segment && !live.count(n) && !gone) {
			io.remove(n);  // under a posted delete, which finds it missing
			++gone;
		}
	}
	CHECK(gone == 1);
	const std::size_t queued = ix.reclaim_pending();
	ex.run_all();
	ix.wait_deletes();
	CHECK(ix.deletes_pending() == 0);
	CHECK(ix.reclaim_pending() == queued);  // not requeued
	settle(ix, ex);
	CHECK(ix.reclaim_pending() == 0);
	CHECK(survey(fs).segments == ix.segments().size());
	balanced(ix, fs, "enoent");
	std::printf("reclaim_bg: a file already gone counted as deleted\n");
	ix.close();
}

int main() {
	posted();
	failed(faultfs::op::truncate, "truncate");
	failed(faultfs::op::sync, "sync");
	failed(faultfs::op::remove, "remove");
	drain_waits();
	close_waits();
	crash_mid_delete();
	dropped();
	quota();
	threaded();
	drain_posted();
	enoent_is_done();
	std::printf("reclaim_bg: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
