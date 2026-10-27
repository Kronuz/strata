// The cold tier's step 1 (docs/design/cold-tier.md D2, D9, D11; K1, K2): a segment is a record plus
// a body mapped at most once, installed by compare-exchange; every owner read of a segment is an
// entry, map, output or body point with its failure rule. An IO wrapper over faultfs injects the
// failures (open, map, damaged maps) and counts maps; the races are made deterministic by holding
// every thread inside map() until all have arrived.
#include "kv_test.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace strata {
// Reaches the records no public call exposes.
struct index_test_access {
	static std::size_t records(const kv_index& ix) { return ix.segs_.size(); }
	static bool facts_known(const kv_index& ix, std::size_t i) { return ix.segs_[i]->facts_known; }
	static bool bodied(const kv_index& ix, std::size_t i) { return ix.segs_[i]->bodied(); }
	static bool failed(const kv_index& ix, std::size_t i) { return ix.segs_[i]->failed(); }
	static const void* body_of(const kv_index& ix, std::size_t i) { return ix.segs_[i]->bodied() ? &ix.segs_[i]->body() : nullptr; }
	static std::uint64_t id_of(const kv_index& ix, std::size_t i) { return ix.segs_[i]->ref.id; }
};
}  // namespace strata

namespace {

using std::chrono::seconds;

// What the wrapper does and counts.
struct hooks {
	std::atomic<int> fail_open{0};   // errno that open_existing of a segment file fails with; 0: none
	std::atomic<int> fail_map{0};    // errno that File::map fails with; 0: none
	std::atomic<std::uint64_t> maps{0};       // maps that succeeded
	std::atomic<std::int64_t> live_maps{0};   // mappings not yet destroyed
	std::atomic<std::uint64_t> opens{0};      // segment files opened
	std::mutex mu;
	std::condition_variable cv;
	int rendezvous = 0;           // map() waits for this many callers to be inside it (3 s at most)
	int inside = 0;
	std::string damage_name;      // maps of this file return one flipped bit...
	std::atomic<int> damage_left{0};  // ...this many times
	int hold_first = 0;           // 1: the next map() blocks after it mapped, until release()
	bool released = false;
	int held = 0;

	void arm_rendezvous(int n) {
		std::lock_guard l(mu);
		rendezvous = n;
		inside = 0;
	}
	void arm_hold() {
		std::lock_guard l(mu);
		hold_first = 1;
		released = false;
		held = 0;
	}
	bool wait_held() {
		std::unique_lock l(mu);
		return cv.wait_for(l, seconds(3), [&] { return held > 0; });
	}
	void release() {
		std::lock_guard l(mu);
		released = true;
		cv.notify_all();
	}
};

class cmap final : public kronuz::journal::Mapping {
public:
	cmap(hooks& h, std::shared_ptr<const kronuz::journal::Mapping> inner, bool damage) : h_(&h), inner_(std::move(inner)) {
		if (damage) {
			copy_.assign(inner_->bytes());
			copy_[copy_.size() - 1] ^= 1;
			bytes_ = copy_;
		} else {
			bytes_ = inner_->bytes();
		}
		h_->live_maps.fetch_add(1);
	}
	~cmap() override { h_->live_maps.fetch_sub(1); }

private:
	hooks* h_;
	std::shared_ptr<const kronuz::journal::Mapping> inner_;
	std::string copy_;
};

class wfile final : public kronuz::journal::File {
public:
	wfile(std::unique_ptr<kronuz::journal::File> f, hooks& h, std::string name) : f_(std::move(f)), h_(&h), name_(std::move(name)) {}
	std::uint64_t size() override { return f_->size(); }
	std::size_t read_at(std::uint64_t o, std::span<char> b) override { return f_->read_at(o, b); }
	std::size_t write_at(std::uint64_t o, std::string_view b) override { return f_->write_at(o, b); }
	void truncate(std::uint64_t s) override { f_->truncate(s); }
	void sync() override { f_->sync(); }
	void sync_data() override { f_->sync_data(); }
	std::shared_ptr<const kronuz::journal::Mapping> map() override {
		if (const int e = h_->fail_map.load()) { throw std::system_error(e, std::generic_category(), "hook: map"); }
		auto inner = f_->map();
		if (!inner) { return nullptr; }
		bool damage = false;
		if (!h_->damage_name.empty() && name_ == h_->damage_name) {
			int left = h_->damage_left.load();
			while (left > 0 && !h_->damage_left.compare_exchange_weak(left, left - 1)) {}
			damage = left > 0;
		}
		h_->maps.fetch_add(1);
		auto m = std::make_shared<cmap>(*h_, std::move(inner), damage);
		std::unique_lock l(h_->mu);
		if (h_->hold_first) {  // this caller is held, mapped, until release()
			h_->hold_first = 0;
			++h_->held;
			h_->cv.notify_all();
			h_->cv.wait_for(l, seconds(3), [&] { return h_->released; });
		} else if (h_->rendezvous > 0) {
			++h_->inside;
			h_->cv.notify_all();
			h_->cv.wait_for(l, seconds(3), [&] { return h_->inside >= h_->rendezvous; });
		}
		return m;
	}

private:
	std::unique_ptr<kronuz::journal::File> f_;
	hooks* h_;
	std::string name_;
};

class wio final : public kronuz::journal::IO {
public:
	wio(faultfs::io& in, hooks& h) : in_(&in), h_(&h) {}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool c) override { return in_->acquire_owner(c); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return in_->acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return in_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return in_->directory_trust(); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view n) override {
		const std::string name(n);
		if (name.starts_with("seg-")) {
			h_->opens.fetch_add(1);
			if (const int e = h_->fail_open.load()) { throw std::system_error(e, std::generic_category(), "hook: open " + name); }
		}
		return std::make_unique<wfile>(in_->open_existing(n), *h_, name);
	}
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view n) override {
		return std::make_unique<wfile>(in_->create_exclusive(n), *h_, std::string(n));
	}
	void replace(std::string_view s, std::string_view d) override { in_->replace(s, d); }
	void remove(std::string_view n) override { in_->remove(n); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return in_->scan_directory(); }
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view n) override { return in_->entry_footprint(n); }
	void sync_directory() override { in_->sync_directory(); }

private:
	faultfs::io* in_;
	hooks* h_;
};

std::string key_of(std::size_t i) {
	char k[16];
	std::snprintf(k, sizeof k, "k%05zu", i);
	return k;
}

// A filesystem, its wrapper and a registry, shared by the cases.
struct fixture {
	faultfs::filesystem fs;
	faultfs::io inner{fs};
	hooks h;
	wio io{inner, h};
	reader_registry reg{64};
	fixture() { fs.map_files(true); }
	kv_index::options opts(bool eager, bool merges = false) {
		kv_index::options o;
		o.key_prefix = "k";
		o.eager_segments = eager;
		o.readers = &reg;
		o.auto_merge = merges;
		o.policy.segments_per_tier = 2;
		return o;
	}
};

// `n` segments of `per` documents each: key_of(0..), value "v" + number, one commit per segment.
void fill(kv_index& ix, std::size_t n, std::size_t per = 2) {
	std::size_t at = 0;
	for (std::size_t s = 0; s < n; ++s) {
		for (std::size_t i = 0; i < per; ++i, ++at) { CHECK(ix.apply(tx({add(key_of(at), "v" + std::to_string(at))}), {}).result == verdict::accepted); }
		ix.commit();
	}
}

enum class outcome { ok, corrupt, host, other };
template <class F>
outcome run(F&& f, int* host_code = nullptr) {
	try {
		f();
		return outcome::ok;
	} catch (const corrupt_segment&) {
		return outcome::corrupt;
	} catch (const corrupt_index&) {
		return outcome::other;
	} catch (const std::system_error& e) {
		if (host_code) { *host_code = e.code().value(); }
		return outcome::host;
	} catch (...) {
		return outcome::other;
	}
}

// --- K1: record and body ------------------------------------------------------------------

// Open maps nothing when lazy and every segment when eager; a get maps newest first down to its hit.
void lazy_open() {
	fixture f;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("lazy-open"));
		fill(ix, 4);
	}
	f.h.maps = 0;
	{
		auto ix = kv_index::open(f.io, f.opts(true));
		CHECK(f.h.maps == 4 && f.h.live_maps == 4);
		CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0"));
		CHECK(f.h.maps == 4);
	}
	CHECK(f.h.live_maps == 0);
	f.h.maps = 0;
	auto ix = kv_index::open(f.io, f.opts(false));
	CHECK(f.h.maps == 0 && f.h.live_maps == 0);
	for (std::size_t i = 0; i < index_test_access::records(ix); ++i) { CHECK(!index_test_access::bodied(ix, i) && !index_test_access::facts_known(ix, i)); }
	CHECK(ix.get(key_of(7)) == std::optional<std::string>("v7"));  // in the newest segment
	CHECK(f.h.maps == 1);
	CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0"));  // down to the oldest
	CHECK(f.h.maps == 4 && f.h.live_maps == 4);
	CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0") && f.h.maps == 4);  // mapped once
	CHECK(!ix.get("k99999"));
	std::printf("lazy open: ok\n");
}

// N threads reach the same record's first map together: one body wins, the others destroy only
// their own and use the winner's, so they all hold one segment and one mapping stays.
void install_race() {
	fixture f;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("race"));
		fill(ix, 3);
	}
	auto ix = kv_index::open(f.io, f.opts(false));
	const auto reader = ix.reader();
	f.h.maps = 0;
	constexpr int N = 8;
	for (int round = 0; round < 3; ++round) {
		f.h.arm_rendezvous(N);
		std::vector<const void*> got(N, nullptr);
		std::vector<std::thread> ts;
		for (int t = 0; t < N; ++t) {
			ts.emplace_back([&, t] {
				const auto g = reader.acquire();
				const auto* seg = g->segments()[std::size_t(round)].seg();
				// The body is complete when seen: a read through it works.
				CHECK(seg->core().doc_count() == 2);
				got[std::size_t(t)] = seg;
			});
		}
		for (auto& t : ts) { t.join(); }
		f.h.arm_rendezvous(0);
		for (int t = 1; t < N; ++t) { CHECK(got[std::size_t(t)] && got[std::size_t(t)] == got[0]); }
		CHECK(f.h.maps == std::uint64_t(N) * std::uint64_t(round + 1));  // every thread mapped
		CHECK(f.h.live_maps == round + 1);                                // the losers' went with their bodies
	}
	// The same pointer for every later call, and across the owner's commit (a body is never cleared).
	const auto g = reader.acquire();
	const void* before = g->segments()[0].seg();
	CHECK(ix.apply(tx({add("knew", "x")}), {}).result == verdict::accepted);
	ix.commit();
	CHECK(g->segments()[0].seg() == before && reader.acquire()->segments()[0].seg() == before);
	CHECK(f.h.live_maps == 3 + 1);  // the three bodies and the commit's flush output
	std::printf("install race: ok\n");
}

// A good body that loses to the failed marker throws corrupt_segment and leaves nothing mapped;
// the others fail alike, one marker wins.
void marker_race() {
	fixture f;
	std::string name;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("marker"));
		fill(ix, 2);
		name = index_detail::segment_name(index_test_access::id_of(ix, 0));
	}
	auto ix = kv_index::open(f.io, f.opts(false));
	const auto reader = ix.reader();
	f.h.maps = 0;
	f.h.damage_name = name;
	// The first map is clean and held; two damaged maps (the second thread's two attempts) install
	// the marker; the held body then loses.
	f.h.arm_hold();
	f.h.damage_left = 0;
	outcome held_result = outcome::ok, other_result = outcome::ok;
	std::thread held([&] { held_result = run([&] { (void)reader.acquire()->segments()[0].seg(); }); });
	CHECK(f.h.wait_held());
	f.h.damage_left = 2;
	std::thread other([&] { other_result = run([&] { (void)reader.acquire()->segments()[0].seg(); }); });
	other.join();
	CHECK(other_result == outcome::corrupt && index_test_access::failed(ix, 0));
	f.h.release();
	held.join();
	CHECK(held_result == outcome::corrupt);
	CHECK(f.h.live_maps == 0);  // the held body went with its thread
	CHECK(ix.needs_heal());
	// Every later read fails at once, with no read of the file.
	const std::uint64_t opens = f.h.opens;
	CHECK(run([&] { (void)reader.acquire()->segments()[0].seg(); }) == outcome::corrupt && f.h.opens == opens);
	std::printf("marker race: ok\n");
}

// Readers' maps leave the facts to the owner: records stay unknown until a commit's prepare learns
// them, however many readers mapped, and the manifest then equals an eager index's.
void facts_by_owner() {
	fixture f;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("facts"));
		fill(ix, 4);
	}
	auto ix = kv_index::open(f.io, f.opts(false));
	const auto reader = ix.reader();
	{
		std::vector<std::thread> ts;
		for (int t = 0; t < 4; ++t) {
			ts.emplace_back([&] {
				const auto g = reader.acquire();
				for (const auto& v : g->segments()) { (void)v.seg(); }
			});
		}
		for (auto& t : ts) { t.join(); }
	}
	for (std::size_t i = 0; i < index_test_access::records(ix); ++i) { CHECK(index_test_access::bodied(ix, i) && !index_test_access::facts_known(ix, i)); }
	CHECK(ix.apply(tx({replace(key_of(0), "w")}), {}).result == verdict::accepted);
	ix.commit();
	for (std::size_t i = 0; i < index_test_access::records(ix); ++i) { CHECK(index_test_access::facts_known(ix, i)); }
	// The same operations on an eager index give the same manifest facts.
	fixture g;
	{
		auto ex = kv_index::create(g.io, g.opts(true), uuid_of("facts"));
		fill(ex, 4);
	}
	auto ex = kv_index::open(g.io, g.opts(true));
	CHECK(ex.apply(tx({replace(key_of(0), "w")}), {}).result == verdict::accepted);
	ex.commit();
	const auto& a = ix.manifest();
	const auto& b = ex.manifest();
	CHECK(a.mask == b.mask && a.min_version == b.min_version && a.max_version == b.max_version && a.total_length == b.total_length);
	CHECK(a.doc_count == b.doc_count && a.live_count == b.live_count && a.live_length == b.live_length && a.segments.size() == b.segments.size());
	for (std::size_t i = 0; i < a.segments.size() && i < b.segments.size(); ++i) {
		CHECK(a.segments[i].live_count == b.segments[i].live_count && a.segments[i].live_length == b.segments[i].live_length);
	}
	CHECK(ix.digest() == ex.digest());
	std::printf("facts by owner: ok\n");
}

// Readers map while the owner commits, replaces and merges: reads equal the model (under TSan this
// is the install's race detector).
void readers_and_owner() {
	fixture f;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("rw"));
		fill(ix, 5, 4);
	}
	auto ix = kv_index::open(f.io, f.opts(false, true));
	const auto reader = ix.reader();
	std::atomic<bool> stop{false};
	std::atomic<int> bad{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937 rng(unsigned(t) + 1);
			while (!stop.load()) {
				const auto g = reader.acquire();
				const std::size_t k = rng() % 20;
				const auto v = g->get(key_of(k));
				if (!v || v->empty() || (*v)[0] != 'v') { bad.fetch_add(1); }
				std::size_t n = 0;
				g->scan("", [&](std::string_view, std::string_view, std::uint64_t) { ++n; });
				if (n != 20) { bad.fetch_add(1); }
				for (const auto& s : g->segments()) { (void)s.seg(); }
			}
		});
	}
	for (int round = 0; round < 12; ++round) {
		for (std::size_t i = 0; i < 4; ++i) { CHECK(ix.apply(tx({replace(key_of((std::size_t(round) * 4 + i) % 20), "v-" + std::to_string(round))}), {}).result == verdict::accepted); }
		if (round % 3 == 2) {
			ix.force_merge(2);
			(void)ix.heal();
		}
		ix.commit();
	}
	stop = true;
	for (auto& t : ts) { t.join(); }
	CHECK(bad == 0);
	std::printf("readers and owner: ok\n");
}

// --- K2: the map points' failure rules ------------------------------------------------------------

// A host error from a reader's map reaches the caller unchanged: nothing is marked or counted, the
// index is not fenced, and the read works once the host recovers.
void host_error_in_reads() {
	fixture f;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("host"));
		fill(ix, 3);
	}
	auto ix = kv_index::open(f.io, f.opts(false));
	for (const int e : {EMFILE, ENFILE, ENOMEM}) {
		f.h.fail_open = e;
		int code = 0;
		CHECK(run([&] { (void)ix.get(key_of(0)); }, &code) == outcome::host && code == e);
		f.h.fail_open = 0;
		f.h.fail_map = e;
		code = 0;
		CHECK(run([&] { (void)ix.get(key_of(0)); }, &code) == outcome::host && code == e);
		f.h.fail_map = 0;
		CHECK(!ix.needs_heal() && ix.media_retries() == 0);
		for (std::size_t i = 0; i < index_test_access::records(ix); ++i) { CHECK(!index_test_access::failed(ix, i)); }
	}
	CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0"));
	CHECK(ix.apply(tx({add("knew", "x")}), {}).result == verdict::accepted);
	ix.commit();
	std::printf("host errors in reads: ok\n");
}

// A damaged segment's first map is corrupt_segment; the record is marked, others read on, and heal()
// reports a segment whose file is damaged for good.
void damaged_first_map() {
	fixture f;
	std::string name;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("damage"));
		fill(ix, 3);
		name = index_detail::segment_name(index_test_access::id_of(ix, 0));
	}
	auto ix = kv_index::open(f.io, f.opts(false));
	f.h.damage_name = name;
	f.h.damage_left = 1000000;
	CHECK(run([&] { (void)ix.get(key_of(0)); }) == outcome::corrupt);
	CHECK(ix.needs_heal() && index_test_access::failed(ix, 0));
	CHECK(ix.get(key_of(5)) == std::optional<std::string>("v5"));  // the others read on
	const auto bad = ix.heal();
	CHECK(bad.size() == 1 && bad[0] == index_test_access::id_of(ix, 0));
	CHECK(ix.needs_heal());
	std::printf("damaged first map: ok\n");
}

// Commit's prepare: a host error leaves the index as refresh() leaves it (nothing durable, nothing
// fenced); a marked record throws corrupt_segment each time until heal() replaces it from a file
// that reads clean now, and a commit after that succeeds with every document (CT-R19).
void commit_prepare() {
	fixture f;
	std::string name;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("prepare"));
		fill(ix, 4);
		name = index_detail::segment_name(index_test_access::id_of(ix, 0));
	}
	f.h.maps = 0;
	auto ix = kv_index::open(f.io, f.opts(false));
	const std::uint64_t generation = ix.manifest().generation;
	// Keyless documents look nothing up, so the prepare is the first to map the old segments.
	const auto write = [&](const char* body) { return ix.apply(tx({add("", body)}), {}).result == verdict::accepted; };
	// A host error in the prepare.
	CHECK(write("a") && f.h.maps == 0);
	f.h.fail_open = EMFILE;
	int code = 0;
	CHECK(run([&] { ix.commit(); }, &code) == outcome::host && code == EMFILE);
	f.h.fail_open = 0;
	CHECK(ix.manifest().generation == generation && ix.buffered_count() == 0);  // flushed, nothing durable
	CHECK(ix.segments().size() == 5 && !ix.needs_heal() && ix.media_retries() == 0);
	CHECK(write("b"));  // not fenced
	// A transient fault on the oldest segment: both attempts damaged, then the file reads clean.
	f.h.damage_name = name;
	f.h.damage_left = 2;
	CHECK(run([&] { ix.commit(); }) == outcome::corrupt);
	CHECK(ix.manifest().generation == generation && ix.needs_heal());
	CHECK(run([&] { ix.commit(); }) == outcome::corrupt);  // until heal() replaces it
	CHECK(write("c"));
	CHECK(ix.heal().empty() && !ix.needs_heal());
	ix.commit();
	CHECK(ix.manifest().generation == generation + 1);
	const auto digest = ix.digest();
	const auto docs = contents(ix).size();
	CHECK(docs == 8 + 3);
	ix.close();
	auto again = kv_index::open(f.io, f.opts(true));
	CHECK(again.digest() == digest && contents(again).size() == docs);
	std::printf("commit prepare: ok\n");
}

// A flush's and a merge's output map that fails for a reason of the host keeps the bytes and does
// not fence (the architect's output_map_probe is the oracle).
void output_maps() {
	for (const int e : {ENOMEM, EMFILE}) {
		{
			fixture f;
			auto ix = kv_index::create(f.io, f.opts(true), uuid_of("out-flush"));
			CHECK(ix.apply(tx({add("k1", "a")}), {}).result == verdict::accepted);
			f.h.fail_map = e;
			CHECK(run([&] { ix.refresh(); }) == outcome::ok);
			f.h.fail_map = 0;
			CHECK(ix.get("k1") == std::optional<std::string>("a"));
			CHECK(ix.apply(tx({add("k9", "z")}), {}).result == verdict::accepted);
			ix.commit();
			const auto digest = ix.digest();
			ix.close();
			auto again = kv_index::open(f.io, f.opts(true));
			CHECK(again.digest() == digest && again.get("k1") == std::optional<std::string>("a"));
		}
		{
			fixture f;
			auto ix = kv_index::create(f.io, f.opts(true), uuid_of("out-merge"));
			fill(ix, 3);
			f.h.fail_map = e;
			f.h.fail_open = e;  // the read-back goes through the file the merge holds: no new descriptor
			CHECK(run([&] { ix.force_merge(1); }) == outcome::ok);
			f.h.fail_map = 0;
			f.h.fail_open = 0;
			CHECK(ix.segments().size() == 1 && ix.get(key_of(0)) == std::optional<std::string>("v0"));
			CHECK(ix.apply(tx({add("k9", "z")}), {}).result == verdict::accepted);
			ix.commit();
			const auto digest = ix.digest();
			ix.close();
			auto again = kv_index::open(f.io, f.opts(true));
			CHECK(again.digest() == digest && again.segments().size() == 2);
		}
	}
	std::printf("output maps: ok\n");
}

// Scheduling: a host error mapping a source drops the round (maybe_merge swallows and counts it, the
// next round merges; merge() and force_merge() rethrow); a source that fails its checks is marked
// and left out of the merge.
void scheduling() {
	fixture f;
	std::string name;
	{
		auto ix = kv_index::create(f.io, f.opts(true), uuid_of("sched"));
		fill(ix, 4);
		name = index_detail::segment_name(index_test_access::id_of(ix, 1));
	}
	// Keyless documents look nothing up, so scheduling is the first to map the old segments.
	auto ix = kv_index::open(f.io, f.opts(false, true));
	const auto write = [&](kv_index& x, const char* body) { return x.apply(tx({add("", body)}), {}).result == verdict::accepted; };
	CHECK(ix.merges_skipped() == 0 && ix.segments().size() == 4);
	f.h.fail_open = EMFILE;
	CHECK(write(ix, "a"));
	CHECK(run([&] { ix.refresh(); }) == outcome::ok);  // the round is dropped, refresh completes
	CHECK(ix.merges_skipped() == 1 && ix.segments().size() == 5 && !ix.needs_heal());
	int code = 0;
	CHECK(run([&] { ix.merge({index_test_access::id_of(ix, 0), index_test_access::id_of(ix, 1)}); }, &code) == outcome::host && code == EMFILE);
	CHECK(run([&] { ix.force_merge(1); }, &code) == outcome::host && code == EMFILE);
	CHECK(ix.merges_skipped() == 1);  // only maybe_merge counts
	f.h.fail_open = 0;
	CHECK(write(ix, "b"));
	ix.refresh();  // the next round merges
	CHECK(ix.merges_skipped() == 1 && ix.segments().size() < 6);
	ix.commit();
	CHECK(ix.get(key_of(0)) == std::optional<std::string>("v0") && contents(ix).size() == 8 + 2);
	// A source whose checks fail is marked and left out; the rest merge.
	fixture g;
	{
		auto ex = kv_index::create(g.io, g.opts(true), uuid_of("sched2"));
		fill(ex, 4);
		name = index_detail::segment_name(index_test_access::id_of(ex, 1));
	}
	auto iy = kv_index::open(g.io, g.opts(false, true));
	g.h.damage_name = name;
	g.h.damage_left = 1000000;
	CHECK(write(iy, "a"));
	CHECK(run([&] { iy.refresh(); }) == outcome::ok);
	CHECK(iy.needs_heal() && iy.merges_skipped() == 0);
	CHECK(iy.segments().size() < 5);  // the others merged
	CHECK(write(iy, "b"));            // not fenced
	std::printf("scheduling: ok\n");
}

// install(): a lazy target keeps an unmapped record per copy, with the incoming body's facts and its
// deletes, and no map; an eager one maps the copies it owns (cold-tier D9).
void install_records() {
	for (const bool eager : {false, true}) {
		fixture src, dst;
		auto S = kv_index::create(src.io, src.opts(true), uuid_of("install-src"));
		fill(S, 3);
		CHECK(S.apply(tx({erase(key_of(1))}), {}).result == verdict::accepted);
		S.commit();
		const std::uint64_t cp = S.checkpoint();
		auto T = kv_index::create(dst.io, dst.opts(eager), uuid_of("install-dst"));
		dst.h.maps = 0;
		T.install(src.io, cp);
		if (eager) {
			CHECK(dst.h.maps == 3);
		} else {
			CHECK(dst.h.maps == 0);
			CHECK(index_test_access::records(T) == 3);
			for (std::size_t i = 0; i < 3; ++i) { CHECK(!index_test_access::bodied(T, i) && index_test_access::facts_known(T, i)); }
		}
		CHECK(T.digest() == S.digest() && !T.get(key_of(1)) && T.get(key_of(4)) == std::optional<std::string>("v4"));
		T.commit();
		const auto& a = T.manifest();
		const auto& b = S.manifest();
		CHECK(a.mask == b.mask && a.total_length == b.total_length && a.live_count == b.live_count && a.live_length == b.live_length);
	}
	std::printf("install records: ok\n");
}

// D11: open checks every manifest-named segment's name and size against the scan, lazily or not.
void open_checks() {
	for (const bool eager : {true, false}) {
		for (const int what : {0, 1}) {
			fixture f;
			std::string name;
			{
				auto ix = kv_index::create(f.io, f.opts(true), uuid_of("open"));
				fill(ix, 3);
				name = index_detail::segment_name(index_test_access::id_of(ix, 1));
			}
			if (what == 0) {
				f.inner.remove(name);
			} else {
				auto file = f.inner.open_existing(name);
				file->truncate(file->size() - 1);
			}
			const outcome got = run([&] { (void)kv_index::open(f.io, f.opts(eager)); });
			CHECK(got == outcome::other);  // corrupt_index
			std::string msg;
			try {
				(void)kv_index::open(f.io, f.opts(eager));
			} catch (const corrupt_index& e) {
				msg = e.what();
			}
			CHECK(msg.find(what == 0 ? "missing file" : "file size differs") != std::string::npos);
		}
	}
	std::printf("open checks: ok\n");
}

}  // namespace

int main() {
	lazy_open();
	install_race();
	marker_race();
	facts_by_owner();
	readers_and_owner();
	host_error_in_reads();
	damaged_first_map();
	commit_prepare();
	output_maps();
	scheduling();
	install_records();
	open_checks();
	if (failures) {
		std::printf("cold_segment: FAILED (%d)\n", failures);
		return 1;
	}
	std::printf("cold_segment: ok\n");
	return 0;
}
