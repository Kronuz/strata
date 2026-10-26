// Background merges (D11, D18) on the document layout, on the deterministic inline executor and on the threaded
// one (Kronuz/threadpool task groups, one and two threads): deletes and replaces racing a
// running merge carry over once, against an inline-merging twin's digest; seeded random
// schedules with a power loss while merges run; close, destruction, and install while
// merges run; the rate limit; merges that never wait for indexing-budget room; ENOSPC
// discarding a merge, EIO fencing, and a damaged source left for heal(); merge outputs
// published in id order; and the merge build's cancellation points.
#include "kv_test.hh"
#include "segment_edit.hh"
#include "strata/core/pool_executor.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>

using clock_type = std::chrono::steady_clock;

static const auto uuid = uuid_of("core-background");

static double seconds_since(clock_type::time_point t) { return std::chrono::duration<double>(clock_type::now() - t).count(); }

// The background executor of one run, inline (threads 0) or on a pool of `threads`, and
// the node's rate limiter on its clock.
struct background {
	std::unique_ptr<ThreadPool<>> pool;
	std::unique_ptr<executor> exec;
	inline_executor* inl = nullptr;
	rate_limiter rate;
	explicit background(std::size_t threads, std::uint64_t bytes_per_second = 0) : rate(bytes_per_second) {
		if (threads) {
			pool = std::make_unique<ThreadPool<>>("merge{}", threads);
			exec = std::make_unique<pool_executor<>>(*pool);
		} else {
			auto e = std::make_unique<inline_executor>();
			inl = e.get();
			exec = std::move(e);
		}
	}
	~background() { exec.reset(); }  // before the pool
	bool threaded() const { return inl == nullptr; }
	void run() {
		if (inl) { inl->run(); }
	}
	// Small tiers, so a handful of tiny segments asks for merges; `use`: merge here,
	// otherwise inline.
	kv_index::options options(bool use = true) {
		kv_index::options o;
		o.key_prefix = "Q";
		o.policy.segments_per_tier = 2;
		o.policy.floor_bytes = 1;
		if (use) {
			o.background = exec.get();
			o.rate = &rate;
		}
		return o;
	}
};
static const char* name_of(std::size_t threads) { return threads == 0 ? "inline" : threads == 1 ? "one-thread" : "two-thread"; }

// `segments` committed segments of `docs` documents each, every one with `bytes` random
// stored bytes (incompressible), keys from Q<first>.
static void fill(kv_index& ix, int segments, int docs, std::size_t bytes, std::uint64_t seed, int first = 0) {
	std::mt19937_64 r(seed);
	int n = first;
	for (int s = 0; s < segments; ++s) {
		for (int d = 0; d < docs; ++d) {
			operation o = replace("Q" + std::to_string(n++), "body");
			o.doc.stored.resize(bytes);
			for (auto& c : o.doc.stored) { c = char(r()); }
			CHECK(ix.apply(tx({o}), {}).result == verdict::accepted);
		}
		ix.refresh();
	}
	ix.commit();
}

// Segment files on disk, and those the manifest names.
static std::set<std::string> segment_files(const faultfs::filesystem& fs) {
	std::set<std::string> out;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::segment) { out.insert(n); }
	}
	return out;
}
static std::set<std::string> named_segments(const kv_index& ix) {
	std::set<std::string> named;
	for (const auto& s : ix.manifest().segments) { named.insert(index_detail::segment_name(s.id)); }
	return named;
}
static bool no_spills(const faultfs::filesystem& fs) {
	for (const auto& n : fs.list()) {
		if (n.ends_with(".spill")) { return false; }
	}
	return true;
}

// faultfs::io whose segment creations wait while it is held, counting the arrivals.
class gate_io final : public kronuz::journal::IO {
public:
	explicit gate_io(faultfs::io& inner) : inner_(&inner) {}
	void hold() {
		std::lock_guard l(mu_);
		held_ = true;
	}
	void release() {
		{
			std::lock_guard l(mu_);
			held_ = false;
		}
		cv_.notify_all();
	}
	void await_arrival() {
		std::unique_lock l(mu_);
		cv_.wait(l, [&] { return arrived_ > 0; });
	}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return inner_->acquire_owner(create); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_->acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return inner_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return inner_->directory_trust(); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override { return inner_->open_existing(name); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		if (name.starts_with("seg-") && name.ends_with(".sx")) {
			std::unique_lock l(mu_);
			if (held_) {
				++arrived_;
				cv_.notify_all();
				cv_.wait(l, [&] { return !held_; });
			}
		}
		return inner_->create_exclusive(name);
	}
	void replace(std::string_view a, std::string_view b) override { inner_->replace(a, b); }
	void remove(std::string_view name) override { inner_->remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_->scan_directory(); }
	void sync_directory() override { inner_->sync_directory(); }

private:
	faultfs::io* inner_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool held_ = false;
	int arrived_ = 0;
};

// Deletes and replaces while merges run carry over to the outputs exactly once: no
// document resurrected or lost, the digest the inline twin's. On the pool, a merge has
// read its sources and waits to create its output while they arrive.
static void carry_over(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs, tfs;
	faultfs::io inner(fs), tio(tfs);
	gate_io gate(inner);
	kronuz::journal::IO& io = bg.threaded() ? static_cast<kronuz::journal::IO&>(gate) : static_cast<kronuz::journal::IO&>(inner);
	auto ix = kv_index::create(io, bg.options(), uuid);
	auto twin = kv_index::create(tio, bg.options(false), uuid);
	auto both = [&](const transaction& t) {
		const auto a = ix.apply(t, {}), b = twin.apply(t, {});
		CHECK(a.result == b.result && a.docids == b.docids && a.versions == b.versions);
	};
	for (int s = 0; s < 6; ++s) {
		for (int i = 0; i < 10; ++i) { both(tx({replace("Q" + std::to_string(s * 10 + i), "v0")})); }
		ix.refresh();
		twin.refresh();
	}
	both(tx({erase("Q0"), erase("Q11")}));  // deleted before the merges read them
	std::set<std::uint64_t> before;
	for (const auto& v : ix.segments()) { before.insert(v.id); }
	if (bg.threaded()) { gate.hold(); }
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	if (bg.threaded()) { gate.await_arrival(); }
	for (int s = 0; s < 6; ++s) {
		both(tx({erase("Q" + std::to_string(s * 10 + 2)), replace("Q" + std::to_string(s * 10 + 3), "v1")}));
		both(tx({replace("Q" + std::to_string(s * 10 + 4), "v1"), replace("Q" + std::to_string(s * 10 + 4), "v2")}));
	}
	both(tx({erase("Q11"), replace("Q0", "back")}));  // already gone: a no-op, then a new docid
	CHECK(ix.digest() == twin.digest());
	if (bg.threaded()) { gate.release(); }
	ix.wait_merges();
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 6);
	CHECK(ix.digest() == twin.digest() && contents(ix) == contents(twin));
	bool carried = false;
	for (const auto& v : ix.segments()) { carried = carried || (!before.count(v.id) && v.deleted); }
	CHECK(carried);  // an output was born with the deletes made while it ran
	(void)ix.check();
	ix.commit();
	twin.commit();
	const auto digest = ix.digest();
	ix.close();
	ix = kv_index::open(io, bg.options());
	CHECK(ix.digest() == digest && contents(ix) == contents(twin));
	(void)ix.check();
}

// A merge takes its output's id when scheduled; a flush that publishes while it runs takes
// a higher one. The output goes in id order, so the next commit's manifest lists the
// segments in increasing id order and reopens.
static void id_order(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, bg.options(), uuid);
	fill(ix, 6, 4, 300, 11);
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	CHECK(ix.apply(tx({replace("Q500", "flushed while merging")}), {}).result == verdict::accepted);
	ix.refresh();  // the flush's segment id is above the pending merges' output ids
	ix.wait_merges();
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 7);
	std::uint64_t last = 0;
	for (const auto& v : ix.segments()) {
		CHECK(v.id > last);
		last = v.id;
	}
	ix.commit();
	const auto digest = ix.digest();
	ix.close();
	ix = kv_index::open(io, bg.options());
	CHECK(ix.digest() == digest && contents(ix).size() == 25);
	last = 0;
	for (const auto& s : ix.manifest().segments) {
		CHECK(s.id > last);
		last = s.id;
	}
}

namespace strata {
// Breaks an invariant no public call can: two segments out of id order.
struct index_test_access {
	static void swap_segments(kv_index& ix) { std::swap(ix.segs_[0], ix.segs_[1]); }
	// Replaces segment `at`'s in-memory bytes (its file untouched); returns its id.
	static std::uint64_t bytes_of(const kv_index& ix, std::size_t at, std::string& out) {
		out = std::string(ix.segs_[at]->bytes());
		return ix.segs_[at]->ref.id;
	}
	static void replace_bytes(kv_index& ix, std::size_t at, std::string bytes) {
		const lease_scope held{ix.lease_};  // the old segment drops here
		auto ref = ix.segs_[at]->ref;
		ix.segs_[at] = std::make_shared<kv_index::open_segment>(ref, std::move(bytes));
	}
};
}  // namespace strata

// Segments out of id order fence the commit before it writes anything: no new manifest,
// no new file, and the index stays fenced.
static void order_check_fences() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	background bg(0);
	auto ix = kv_index::create(io, bg.options(false), uuid);
	fill(ix, 2, 2, 100, 12);
	const auto files = fs.list();
	index_test_access::swap_segments(ix);
	bool fenced = false;
	try {
		ix.commit();
	} catch (const index_fenced&) {
		fenced = true;
	}
	CHECK(fenced && fs.list() == files);
	CHECK(throws([&] { ix.commit(); }));
	CHECK(fs.list() == files);
}

// A source that opens and passes its checksums but is inconsistent (SR4): two keys naming
// one ordinal, or a document's key dropped while its version stays. The merge abandons, the
// index reports the segment through needs_heal() and keeps serving; heal() clears it.
static void inconsistent_source(std::size_t threads, bool dropped_key) {
	background bg(threads);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, bg.options(), uuid);
	fill(ix, 6, 4, 300, 13);  // segment 0 holds Q0..Q3 at ordinals 0..3
	std::string bytes;
	index_test_access::bytes_of(ix, 0, bytes);
	std::vector<keys::entry> es;
	const std::string ks[] = {"Q0", "Q1", "Q2", "Q3"};
	for (std::uint32_t i = 0; i < 4; ++i) {
		if (dropped_key && i == 2) { continue; }
		es.push_back({ks[i].data(), std::uint32_t(ks[i].size()), dropped_key ? i : (i == 2 ? 1 : i)});
	}
	const keys::sections k = keys::encode(es);
	index_test_access::replace_bytes(ix, 0, test::reseal(bytes, {{format::section::keys, k.keys}, {format::section::key_index, k.index}}));
	ix.force_merge(1);  // the inconsistent one is left out; the rest merge
	CHECK(ix.needs_heal() && ix.segments().size() == 2);
	CHECK(ix.apply(tx({replace("Q900", "after")}), {}).result == verdict::accepted);  // not fenced
	ix.refresh();
	// The damage is in memory only: heal() reads the file back whole in its place.
	CHECK(ix.heal().empty() && !ix.needs_heal());
	ix.force_merge(1);
	CHECK(ix.segments().size() == 1 && !ix.needs_heal());
}

// Two well-formed segments sharing a docid break the merge's own contract, not a source:
// the index still fences.
static void shared_docid_fences(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, bg.options(), uuid);
	fill(ix, 6, 4, 300, 14);
	std::string bytes;
	index_test_access::bytes_of(ix, 0, bytes);
	index_test_access::replace_bytes(ix, 1, bytes);
	bool fenced = false;
	try {
		ix.force_merge(1);
	} catch (const index_fenced&) {
		fenced = true;
	}
	CHECK(fenced && !ix.needs_heal());
}

// One index merging in the background and its twin merging inline, fed the same
// transactions: their verdicts, digests, and contents must agree at every point.
struct twins {
	faultfs::filesystem fs, tfs;
	faultfs::io io{fs}, tio{tfs};
	kv_index::options bo, to;
	kv_index ix, twin;
	explicit twins(background& bg)
		: bo(with_auto(bg.options())), to(with_auto(bg.options(false))), ix(kv_index::create(io, bo, uuid)), twin(kv_index::create(tio, to, uuid)) {}
	static kv_index::options with_auto(kv_index::options o) {
		o.auto_merge = true;
		return o;
	}
	void apply(const transaction& t) {
		const auto a = ix.apply(t, {}), b = twin.apply(t, {});
		CHECK(a.result == b.result && a.docids == b.docids && a.versions == b.versions);
	}
	bool same() const { return ix.digest() == twin.digest() && contents(ix) == contents(twin); }
};

// Random schedules, seeded, with two indexes sharing the executor: transactions,
// refreshes and commits merging after each flush, forced merges, waits, and (inline) the
// executor running at seeded points. Then a power loss while merges run recovers the
// last commit.
static void random_schedules(std::size_t threads, std::uint64_t seed) {
	background bg(threads);
	twins a(bg), b(bg);
	twins* pair[] = {&a, &b};
	std::mt19937_64 r(seed * 7919 + threads);
	for (int step = 0; step < 300; ++step) {
		auto& p = *pair[r() % 2];
		const std::string key = "Q" + std::to_string(r() % 60), other = "Q" + std::to_string(r() % 60);
		const std::string body = "b" + std::to_string(r() % 7);
		switch (r() % 17) {
		case 0: case 1: case 2: case 3: case 4: case 5: case 6: p.apply(tx({replace(key, body)})); break;
		case 7: case 8: case 9: p.apply(tx({erase(key)})); break;
		case 10: p.apply(tx({replace(key, body), erase(other)})); break;
		case 11: case 12:
			p.ix.refresh();
			p.twin.refresh();
			break;
		case 13:
			p.ix.commit();
			p.twin.commit();
			break;
		case 14:
			if (r() % 2) {
				p.ix.force_merge_deletes();
				p.twin.force_merge_deletes();
			} else {
				p.ix.force_merge(2);
				p.twin.force_merge(2);
			}
			break;
		case 15: p.ix.wait_merges(); break;
		case 16: bg.run(); break;
		}
		if (step % 8 == 0) { CHECK(p.same()); }
	}
	for (auto* p : pair) {
		p->ix.wait_merges();
		CHECK(p->ix.merges_pending() == 0 && p->same());
		(void)p->ix.check();
		p->ix.commit();
		p->twin.commit();
		CHECK(p->same());
	}
	const auto durable = a.ix.digest();
	for (int i = 0; i < 40; ++i) {
		a.ix.apply(tx({replace("Q" + std::to_string(r() % 60), "late")}), {});
		if (i % 4 == 3) { a.ix.refresh(); }
		if (i % 8 == 5) { bg.run(); }
	}
	a.fs.crash(seed);
	a.ix.close();
	a.ix = kv_index::open(a.io, a.bo);
	CHECK(a.ix.digest() == durable);
	(void)a.ix.check();
}

// Close and destruction while merges run or wait: queued merges are dropped and running
// ones stopped, promptly, and reopening finds the last commit and no merge's file.
static void close_while_merging(std::size_t threads) {
	background bg(threads, 8 << 10);  // each merge here takes seconds at this rate
	faultfs::filesystem fa, fb, fc;
	faultfs::io ia(fa), ib(fb), ic(fc);
	const auto o = bg.options();
	auto a = kv_index::create(ia, o, uuid);
	auto b = kv_index::create(ib, o, uuid);
	fill(a, 6, 4, 3000, 1);
	fill(b, 6, 4, 3000, 2);
	const auto da = a.digest(), db = b.digest();
	{
		auto c = kv_index::create(ic, o, uuid);
		fill(c, 6, 4, 3000, 3);
		c.maybe_merge();
	}  // destroyed with its merges queued or running
	a.maybe_merge();
	b.maybe_merge();
	CHECK(a.merges_pending() >= 1 && b.merges_pending() >= 1);
	if (bg.threaded()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
	const auto t0 = clock_type::now();
	b.close();
	a.close();
	CHECK(seconds_since(t0) < 2.0);
	a = kv_index::open(ia, o);
	b = kv_index::open(ib, o);
	auto c = kv_index::open(ic, o);
	CHECK(a.digest() == da && b.digest() == db && contents(c).size() == 24);
	CHECK(segment_files(fa) == named_segments(a) && segment_files(fb) == named_segments(b) && segment_files(fc) == named_segments(c));
	CHECK(no_spills(fa) && no_spills(fb) && no_spills(fc));
	(void)a.check();
	(void)b.check();
	(void)c.check();
}

// The rate limit: a merge writing N bytes under L bytes per second takes at least N/L, on
// the wall clock with threads and on the inline executor's clock without.
static void rate_limit(std::size_t threads) {
	const std::uint64_t rate = 64 << 10;
	background bg(threads, rate);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, bg.options(), uuid);
	fill(ix, 4, 4, 3000, 4);
	const auto digest = ix.digest();
	const auto t0 = clock_type::now();
	const std::uint64_t v0 = bg.exec->now();
	ix.force_merge(1);
	const double took = bg.threaded() ? seconds_since(t0) : double(bg.exec->now() - v0) / 1e9;
	const double n = double(bg.rate.admitted());
	CHECK(ix.segments().size() == 1 && ix.digest() == digest);
	CHECK(n > 40000 && took >= (bg.threaded() ? 0.9 : 0.999) * n / double(rate));
	if (took < 0.9 * n / double(rate)) { std::fprintf(stderr, "  rate (%s): %.0f bytes in %.3f s\n", name_of(threads), n, took); }
}

// The executor of one run, counting the merges' sleeps.
class counting_executor final : public executor {
public:
	explicit counting_executor(executor& inner) : inner_(&inner) {}
	std::unique_ptr<group> open() override { return inner_->open(); }
	std::uint64_t now() override { return inner_->now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		sleeps_.fetch_add(1);
		return inner_->sleep_until(t, stop);
	}
	bool checkpoint(const task::cancel_token& stop) override { return inner_->checkpoint(stop); }
	std::uint64_t sleeps() const { return sleeps_.load(); }

private:
	executor* inner_;
	std::atomic<std::uint64_t> sleeps_{0};
};

// Other holders of the indexing budget hold all of it: a merge still runs at once, drawing
// past the capacity, while flushes go on (they draw past it too). It waits for none of them.
static void memory_held(std::size_t threads) {
	background bg(threads);
	counting_executor counted(*bg.exec);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget memory(4 << 20);
	auto o = bg.options();
	o.background = &counted;
	o.budget = &memory;
	auto ix = kv_index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 9);
	auto hog = memory.open();
	CHECK(hog.try_grow(memory.capacity() - memory.used()));
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	ix.apply(tx({replace("Q1000", "flushed while the merge runs")}), {});
	ix.refresh();
	ix.wait_merges();
	CHECK(counted.sleeps() == 0);
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 7 && contents(ix).count(25));
	CHECK(memory.peak() > memory.capacity());
	hog.settle();
	CHECK(contents(ix).size() == 25);
	CHECK(memory.used() < (1 << 20));  // the merges' charges settled at publication
}

// Merges in flight hold more than the whole budget, so it is under pressure at every apply:
// the buffer flushes once it holds a meaningful share (buffer_share / divisor), not on every
// small apply. Divisor 0 flushes on any pressure, one segment per apply; the default does not,
// yet still flushes under pressure well before the buffer reaches its share.
static std::size_t pressure_segments(std::optional<std::uint64_t> divisor, std::uint64_t share, int applies, std::size_t body) {
	background bg(0);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget memory(4 << 20);
	auto o = bg.options();
	o.budget = &memory;
	o.buffer_share = share;
	if (divisor) { o.pressure_flush_divisor = *divisor; }
	auto ix = kv_index::create(io, o, uuid);
	auto held = memory.open();
	held.grow(memory.capacity() + (2 << 20));  // the merges' working memory, past the capacity
	CHECK(memory.pressure());
	std::mt19937_64 r(41);
	for (int n = 0; n < applies; ++n) {
		operation op = replace("Q" + std::to_string(n), "body");
		op.doc.stored.resize(body);
		for (auto& c : op.doc.stored) { c = char(r()); }
		CHECK(ix.apply(tx({op}), {}).result == verdict::accepted);
	}
	const std::size_t segments = ix.segments().size();
	ix.refresh();
	held.settle();
	CHECK(contents(ix).size() == std::size_t(applies));
	return segments;
}

static void pressure_flush_bounded() {
	constexpr int applies = 2000;
	constexpr std::uint64_t share = 256 << 10;
	constexpr std::size_t body = 200;
	const auto every = pressure_segments(0, share, applies, body);
	CHECK(every >= applies / 2);
	const auto gated = pressure_segments(std::nullopt, share, applies, body);
	// About applies * body / (share / 4) flushes: a few dozen, not a thousand.
	CHECK(gated >= 1 && gated <= 2 * (std::size_t(applies) * body / (share / 4) + 1));
	std::fprintf(stderr, "  pressure_flush: %zu segments gated, %zu ungated\n", gated, every);
}

// A merge whose inputs are larger than the whole indexing budget never sleeps for room
// that no flush can make: it runs at once, charging what it builds past the capacity, and
// the budget is whole again once it is published.
static void budget_below_inputs(std::size_t threads) {
	background bg(threads);
	counting_executor counted(*bg.exec);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget memory(16 << 10);
	auto o = bg.options();
	o.background = &counted;
	o.budget = &memory;
	auto ix = kv_index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 11);
	const auto digest = ix.digest();
	const auto good = contents(ix);
	std::uint64_t input = 0;
	for (const auto& s : ix.manifest().segments) { input += s.size; }
	CHECK(input > memory.capacity());
	const auto t0 = clock_type::now();
	const std::uint64_t v0 = bg.exec->now();
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	ix.wait_merges();
	const double took = bg.threaded() ? seconds_since(t0) : double(bg.exec->now() - v0) / 1e9;
	CHECK(counted.sleeps() == 0 && took < 1.0);
	CHECK(ix.merges_pending() == 0 && ix.segments().size() < 6 && ix.digest() == digest && contents(ix) == good);
	CHECK(memory.peak() > memory.capacity());  // the merge drew past it
	CHECK(memory.used() < (8 << 10));          // and returned it at publication
	if (counted.sleeps() != 0 || took >= 1.0) { std::fprintf(stderr, "  budget_below_inputs (%s): %llu sleeps, %.3f s\n", name_of(threads), (unsigned long long)counted.sleeps(), took); }
}

// ENOSPC while a background merge writes discards it, its partial files removed and the
// index as it was; force_merge reports it; once space returns the next merge succeeds.
// EIO fences, and reopening recovers the last commit.
static void space_and_faults(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	const auto o = bg.options();
	auto ix = kv_index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 5);
	const auto digest = ix.digest();
	const auto good = contents(ix);
	const auto files = segment_files(fs);
	fs.set_capacity(fs.used() + 100);
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	ix.wait_merges();
	CHECK(ix.merges_pending() == 0 && ix.segments().size() == 6 && ix.digest() == digest && contents(ix) == good);
	CHECK(segment_files(fs) == files && no_spills(fs));
	bool space = false;
	try {
		ix.force_merge(1);
	} catch (const std::system_error& e) {
		space = e.code().value() == ENOSPC;
	}
	CHECK(space && ix.segments().size() == 6 && ix.digest() == digest && segment_files(fs) == files);
	fs.set_capacity(~std::uint64_t(0));
	ix.maybe_merge();
	ix.wait_merges();
	CHECK(ix.segments().size() < 6 && ix.digest() == digest && contents(ix) == good);
	ix.commit();
	fill(ix, 3, 4, 3000, 6, 100);
	const auto committed = ix.digest();
	fs.add_fault({faultfs::op::create, 1, EIO, false});  // the merge's first create
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	CHECK(throws([&] { ix.wait_merges(); }));
	CHECK(throws([&] { ix.commit(); }));  // fenced
	fs.clear_faults();
	ix.close();
	ix = kv_index::open(io, o);
	CHECK(ix.digest() == committed && contents(ix).size() == 36);
}

// A merge reading a damaged source marks it for heal() instead of failing the index; the
// policy leaves it out until heal() reads it back whole.
static void damaged_source(std::size_t threads) {
	background bg(threads);
	faultfs::filesystem fs;
	fs.map_files(false);  // media faults act on reads
	faultfs::io io(fs);
	const auto o = bg.options();
	auto ix = kv_index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 10);
	const auto digest = ix.digest();
	const std::uint64_t victim = ix.manifest().segments.front().id;
	ix.close();
	const std::string name = index_detail::segment_name(victim);
	{
		const kv_index::segment seg(fs.view(name));
		for (const auto& e : seg.core().sections()) {
			if (e.kind == format::section::stored) { fs.add_media_fault(name, faultfs::media::bit_flip, e.offset + e.length / 2, e.offset + e.length / 2 + 1, 0); }
		}
	}
	ix = kv_index::open(io, o);
	ix.force_merge(1);  // the damaged one is left out; the rest merge
	CHECK(ix.needs_heal() && ix.segments().size() == 2);
	CHECK(ix.heal() == std::vector<std::uint64_t>{victim});  // its file reads damaged too
	fs.clear_faults();
	CHECK(ix.heal().empty() && !ix.needs_heal());
	ix.force_merge(1);
	CHECK(ix.segments().size() == 1 && ix.digest() == digest);
}

// Install while merges run (D11's carry-over at install): the merges end unpublished,
// the index holds exactly the checkpoint, and no merge's file stays behind.
static void install_while_merging(std::size_t threads) {
	background bg(threads, 8 << 10);
	faultfs::filesystem fs, sfs;
	faultfs::io io(fs), sio(sfs);
	const auto o = bg.options();
	auto src = kv_index::create(sio, bg.options(false), uuid);
	fill(src, 2, 5, 100, 7, 1000);
	const auto generation = src.checkpoint();
	const auto want = src.digest();
	const auto want_contents = contents(src);
	auto ix = kv_index::create(io, o, uuid);
	fill(ix, 6, 4, 3000, 8);
	const auto files = segment_files(fs);
	ix.maybe_merge();
	CHECK(ix.merges_pending() >= 1);
	if (bg.threaded()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
	ix.install(sio, generation);
	CHECK(ix.merges_pending() == 0);
	ix.wait_merges();
	CHECK(ix.digest() == want && contents(ix) == want_contents && ix.segments().size() == 2);
	ix.commit();
	ix.reclaim();
	const auto named = named_segments(ix);
	for (const auto& n : segment_files(fs)) { CHECK(files.count(n) || named.count(n)); }
	CHECK(no_spills(fs));
	ix.close();
	ix = kv_index::open(io, o);
	CHECK(ix.digest() == want && contents(ix) == want_contents);
}

// The merge build stops at its next document or stored frame once cancelled.
static void build_cancellation() {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "Q";
	auto ix = kv_index::create(io, o, uuid);
	std::mt19937_64 r(3);
	for (int s = 0; s < 2; ++s) {
		for (int d = 0; d < 200; ++d) {
			operation op = replace("Q" + std::to_string(s * 200 + d), "");
			op.doc.stored.resize(2000);
			for (auto& c : op.doc.stored) { c = char(r()); }  // incompressible: the stored frames dominate the segment
			CHECK(ix.apply(tx({op}), {}).result == verdict::accepted);
		}
		ix.refresh();
	}
	std::vector<basic_merge_source<document_layout>> sources;
	for (const auto& v : ix.segments()) { sources.push_back({v.seg, nullptr}); }
	const std::array<std::uint8_t, 16> header{};
	std::string full;
	{
		string_output out(full);
		string_spill spill;
		(void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), header, out, spill, {.key_prefix = "Q"});
	}
	{
		task::cancel_source source;
		const auto token = source.token();
		source.request();
		std::string bytes;
		string_output out(bytes);
		string_spill spill;
		bool cancelled = false;
		try {
			(void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), header, out, spill, {.cancel = &token, .key_prefix = "Q"});
		} catch (const task::cancelled&) {
			cancelled = true;
		}
		CHECK(cancelled && bytes.empty());
	}
	{
		// Cancelled at the output's first write (the header): the first stored frame stops it.
		struct cancelling final : output {
			task::cancel_source& source;
			std::string bytes;
			explicit cancelling(task::cancel_source& s) : source(s) {}
			using output::write;
			void write(const char* data, std::size_t size) override {
				bytes.append(data, size);
				source.request();
			}
		};
		task::cancel_source source;
		const auto token = source.token();
		cancelling out(source);
		string_spill spill;
		bool cancelled = false;
		try {
			(void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), header, out, spill, {.cancel = &token, .key_prefix = "Q"});
		} catch (const task::cancelled&) {
			cancelled = true;
		}
		CHECK(cancelled && !out.bytes.empty() && out.bytes.size() < full.size() / 2);
	}
}

int main() {
	build_cancellation();
	order_check_fences();
	pressure_flush_bounded();
	for (std::size_t threads : {0, 1, 2}) {
		const int before = failures;
		carry_over(threads);
		id_order(threads);
		for (std::uint64_t seed = 1; seed <= 6; ++seed) { random_schedules(threads, seed); }
		close_while_merging(threads);
		rate_limit(threads);
		memory_held(threads);
		budget_below_inputs(threads);
		space_and_faults(threads);
		damaged_source(threads);
		inconsistent_source(threads, false);
		inconsistent_source(threads, true);
		shared_docid_fences(threads);
		install_while_merging(threads);
		if (failures != before) { std::fprintf(stderr, "  (failures above: the %s executor)\n", name_of(threads)); }
	}
	std::printf("background_merge: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
