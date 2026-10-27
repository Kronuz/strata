// The flush's memory: stored records sized up front, borrowed from the buffer instead of
// copied, and the segment streamed to its file. The lifetime rule (the buffer is unchangeable
// while a writer borrows from it) is enforced by the buffer's type, not by convention; these
// tests break it every way the code can be asked to. The segment bytes of every flush shape
// stay what the copying flush wrote: pinned digests, taken on the commit before this change.
#include "kv_test.hh"
#include "strata/core/borrow.hh"
#include "strata/core/pool_executor.hh"
#include "strata/core/writer.hh"

#include <atomic>
#include <cstdio>
#include <memory>
#include <random>
#include <set>
#include <thread>

namespace strata {
// The index's friend: reaches the buffer no public call can mutate during a flush.
struct index_test_access {
	template <class Index>
	static void mutate(Index& ix, std::atomic<int>& refused, std::atomic<int>& allowed) {
		auto probe = [&](auto&& f) {
			try {
				f();
				++allowed;
			} catch (const std::logic_error&) {
				++refused;
			}
		};
		// The buffer a flush borrows from is its oldest frozen one (write-buffer D8).
		auto& b = *ix.frozen_.front();
		probe([&] { Index::record::emplace(b.lane, 1, 1, 1, wbuf::no_ref, 0, wbuf::no_ref, 0, typename Index::document{}); });
		probe([&] { Index::record::emplace(b.lane, 1, 1, 1, wbuf::no_ref, Index::record::tombstone_flag, wbuf::no_ref, 0, typename Index::document{}); });
		probe([&] { ix.clear_buffer(); });
		probe([&] { ix.install_flushed(ix.segs_.size()); });
		probe([&] { ix.close(); });
	}
	template <class Index>
	static bool buffer_pinned(const Index& ix) {
		bool pinned = false;
		ix.for_each_buffer([&](const auto& b) { pinned = pinned || b.lane.pinned(); });
		return pinned;
	}
};
}  // namespace strata

static const auto uuid = uuid_of("flush-memory");

static std::string value_of(std::mt19937_64& r, std::size_t size) {
	std::string v(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { v[i] = i < size / 2 ? char('a' + r() % 26) : char('a' + i % 7); }
	return v;
}

static std::uint64_t fnv(std::uint64_t h, std::string_view s) {
	for (unsigned char c : s) { h = (h ^ c) * 0x100000001b3ull; }
	return h;
}

// --- the gate and its leases --------------------------------------------------------------

static void gate() {
	borrowable_vector<int> v;
	v.push_back(1);
	v.push_back(2);
	CHECK(!v.pinned());
	{
		auto lease = v.pin();
		CHECK(v.pinned() && lease);
		int outside = 0;
		CHECK(lease.owns(&v[0]) && lease.owns(&v[1]) && !lease.owns(&outside));
		CHECK(throws([&] { v.push_back(3); }));
		CHECK(throws([&] { v.push_back(int(4)); }));
		CHECK(throws([&] { v.pop_back(); }));
		CHECK(throws([&] { v.clear(); }));
		CHECK(throws([&] { (void)v.at_mut(0); }));
		CHECK(throws([&] { (void)v.back_mut(); }));
		CHECK(v.size() == 2 && v[0] == 1 && v[1] == 2);  // nothing changed
		auto moved = std::move(lease);  // the pin follows the lease
		CHECK(!lease && moved && v.pinned());
		auto second = v.pin();
		moved = {};
		CHECK(v.pinned());  // two pins: it holds until both end
	}
	CHECK(!v.pinned());
	v.push_back(3);
	v.at_mut(0) = 7;
	v.pop_back();
	CHECK(v.size() == 2 && v[0] == 7);
	v.clear();
	CHECK(v.empty());
}

// --- the stored builder: sized up front, borrowing ---------------------------------------

using doc_t = kv_index::document;

static format::stored_codec codec_of(unsigned i) {
	return i == 0 ? format::stored_codec{format::codec::raw, 0} : i == 1 ? format::lz4_codec : format::zstd_codec(3);
}

// Documents of every record size that matters: none, inside a string's own buffer, a few
// bytes, a block's worth, more than a block.
static std::vector<doc_t> records(std::uint64_t seed, std::size_t n) {
	std::mt19937_64 r(seed);
	std::vector<doc_t> out;
	for (std::size_t i = 0; i < n; ++i) {
		doc_t d;
		char k[16];
		std::snprintf(k, sizeof k, "Q%05u", unsigned(i * 2));
		d.key = k;
		static constexpr std::size_t sizes[] = {0, 1, 7, 15, 16, 23, 40, 300, 4096, 9000};
		d.stored = value_of(r, i % 5 == 0 ? sizes[r() % std::size(sizes)] : r() % 200);
		out.push_back(std::move(d));
	}
	return out;
}

static void borrowed_equals_copied() {
	for (unsigned codec = 0; codec < 3; ++codec) {
		for (const std::size_t block : {std::size_t(512), std::size_t(4) << 10}) {
			const auto docs = records(codec * 7 + block, 250);
			const std::array<std::uint8_t, 16> id{1, 2, 3};
			auto build = [&](bool borrow, bool reserve) {
				borrowable_vector<doc_t> held;
				for (const auto& d : docs) { held.push_back(d); }
				basic_segment_writer<document_layout> w;
				w.key_prefix = "Q";
				w.stored_block = block;
				w.stored_codec = codec_of(codec);
				// Not a lease on a vector the writer was not given: the pin and the parts agree.
				auto lease = held.pin();
				if (borrow) { w.borrow_stored(lease); }
				std::size_t bytes = 0;
				for (const auto& d : docs) { bytes += d.stored.size(); }
				if (reserve) { w.reserve(docs.size(), bytes); }
				for (std::size_t i = 0; i < held.size(); ++i) { w.add({i + 1, 1, held[i].key}, held[i], true); }
				return w.flush(id);
			};
			const std::string copied = build(false, false);
			CHECK(build(true, true) == copied);
			CHECK(build(true, false) == copied);
			CHECK(build(false, true) == copied);
		}
	}
}

static void borrow_refuses_what_it_cannot_hold() {
	borrowable_vector<doc_t> held;
	held.push_back(records(1, 1)[0]);
	doc_t outside = records(2, 1)[0];
	// A part outside the leased container: the lease cannot vouch for it.
	{
		basic_segment_writer<document_layout> w;
		w.key_prefix = "Q";
		auto lease = held.pin();
		w.borrow_stored(lease);
		CHECK(!lease);  // the builder took it
		bool refused = false;
		try {
			w.add({1, 1, outside.key}, outside, false);
		} catch (const std::logic_error&) {
			refused = true;
		}
		CHECK(refused);
	}
	// A copied record in a builder that borrows would break the order of views and copies.
	{
		stored_builder b;
		auto lease = held.pin();
		b.borrow(lease);
		CHECK(throws([&] { b.add(std::string_view("copied")); }));
	}
	// Borrowing after copying began would leave records on both sides.
	{
		stored_builder b;
		b.add(std::string_view("copied"));
		auto lease = held.pin();
		CHECK(throws([&] { b.borrow(lease); }));
	}
	CHECK(!held.pinned());  // every lease ended with its builder or scope
}

// Sizing up front: once told the totals, adding the records allocates nothing more.
static void reserve_allocates_once() {
	const auto docs = records(3, 400);
	std::size_t bytes = 0;
	for (const auto& d : docs) { bytes += d.stored.size(); }
	for (const bool borrow : {false, true}) {
		for (const bool reserve : {false, true}) {
			borrowable_vector<doc_t> held;
			for (const auto& d : docs) { held.push_back(d); }
			flush_charge charge;
			stored_builder b;
			b.configure({&charge, nullptr, "Q", format::bit::stored, std::size_t(4) << 10, format::zstd_codec(3)});
			auto lease = held.pin();
			if (borrow) { b.borrow(lease); }
			if (reserve) { b.reserve(held.size(), bytes); }
			std::uint64_t after_first = 0;
			for (std::size_t i = 0; i < held.size(); ++i) {
				const stored_plugin::part& p = held[i];
				if (borrow) {
					b.add(ordinal_t(i), doc_fields{i + 1, 1, held[i].key}, p);
				} else {
					b.add(std::string_view(p.stored));
				}
				if (i == 0) { after_first = charge.held(); }
			}
			// Borrowing holds a pointer's worth per record, not the records.
			if (borrow) {
				CHECK(charge.held() < bytes / 4);
			} else {
				CHECK(charge.held() >= bytes);
			}
			if (reserve) {
				CHECK(charge.held() == after_first);  // nothing grew after the first record
			} else {
				CHECK(charge.held() > after_first);  // without the hint it grows by doubling
			}
		}
	}
}

// --- the index: bytes, the lifetime rule, the failure paths -------------------------------

// The workload: keyed documents put in a shuffled order and replaced, keyless ones, records of
// every size; two refreshes make two flushes.
static void fill(kv_index& ix, std::uint64_t seed) {
	std::mt19937_64 r(seed);
	std::vector<int> ids(700);
	for (int i = 0; i < 700; ++i) { ids[i] = i; }
	for (std::size_t i = ids.size() - 1; i > 0; --i) { std::swap(ids[i], ids[r() % (i + 1)]); }
	static constexpr std::size_t sizes[] = {0, 1, 7, 15, 16, 40, 120, 300, 9000};
	for (int s = 0; s < 2; ++s) {
		for (int j = s * 350; j < (s + 1) * 350; ++j) {
			char k[16];
			std::snprintf(k, sizeof k, "Q%05d", ids[j]);
			const std::size_t size = j % 9 == 0 ? sizes[r() % std::size(sizes)] : 40 + r() % 300;
			CHECK(ix.apply(tx({replace(k, value_of(r, size))}), {}).result == verdict::accepted);
			if (j % 11 == 0) { CHECK(ix.apply(tx({replace(k, value_of(r, 30))}), {}).result == verdict::accepted); }  // a dead copy in the buffer
			if (j % 50 == 0) {
				operation keyless;
				keyless.type = operation::add;
				keyless.doc = doc("", value_of(r, 64));
				CHECK(ix.apply(tx({keyless}), {}).result == verdict::accepted);
			}
		}
		ix.refresh();
	}
	ix.commit();
}

static std::map<std::string, std::string> segment_files(const faultfs::filesystem& fs) {
	std::map<std::string, std::string> out;
	for (const auto& n : fs.list()) {
		if (n.starts_with("seg-") && n.ends_with(".sx")) { out[n] = std::string(fs.view(n)); }
	}
	return out;
}

static kv_index::options options(std::size_t threads) {
	kv_index::options o;
	o.key_prefix = "Q";
	o.identity_seed = 77;
	o.flush_threads = threads;
	o.flush_min_docs = 40;
	return o;
}

// The digest of every segment file one flush shape writes, name and bytes.
static std::uint64_t flushed_digest(std::size_t threads, std::size_t* files = nullptr) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, options(threads), uuid);
	fill(ix, 5);
	std::uint64_t h = 0xcbf29ce484222325ull;
	const auto segs = segment_files(fs);
	for (const auto& [name, bytes] : segs) { h = fnv(fnv(h, name), bytes); }
	if (files) { *files = segs.size(); }
	return h;
}

// An IO whose files call `hook` before each write: the flush is mid-run when it fires.
class hook_io final : public kronuz::journal::IO {
public:
	hook_io(faultfs::io& inner, std::function<void()> hook) : inner_(inner), hook_(std::move(hook)) {}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return inner_.acquire_owner(create); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_.acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return inner_.directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return inner_.directory_trust(); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override { return std::make_unique<file>(*this, inner_.open_existing(name)); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		if (on_create) { on_create(name); }
		return std::make_unique<file>(*this, inner_.create_exclusive(name));
	}
	void replace(std::string_view a, std::string_view b) override { inner_.replace(a, b); }
	void remove(std::string_view name) override { inner_.remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_.scan_directory(); }
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view name) override { return inner_.entry_footprint(name); }
	void sync_directory() override { inner_.sync_directory(); }
	std::atomic<int> armed{0};
	std::function<void(std::string_view)> on_create;  // before each file is created, on the creating thread

private:
	class file final : public kronuz::journal::File {
	public:
		file(hook_io& io, std::unique_ptr<kronuz::journal::File> f) : io_(io), f_(std::move(f)) {}
		std::uint64_t size() override { return f_->size(); }
		std::size_t read_at(std::uint64_t at, std::span<char> b) override { return f_->read_at(at, b); }
		std::size_t write_at(std::uint64_t at, std::string_view b) override {
			if (io_.armed.load() && io_.hook_) { io_.hook_(); }
			return f_->write_at(at, b);
		}
		void truncate(std::uint64_t n) override { f_->truncate(n); }
		void sync() override { f_->sync(); }
		std::shared_ptr<const kronuz::journal::Mapping> map() override { return f_->map(); }

	private:
		hook_io& io_;
		std::unique_ptr<kronuz::journal::File> f_;
	};
	faultfs::io& inner_;
	std::function<void()> hook_;
};

// Every mutator of the buffer refuses while the flush's writers borrow from it: the probes run
// from inside the flush, on the threads that write the runs, with background merges queued
// on a pool.
static void lifetime_rule_in_the_flush() {
	for (const std::size_t threads : {std::size_t(1), std::size_t(3)}) {
		ThreadPool<> pool("merge{}", 2);
		pool_executor<> exec(pool);
		faultfs::filesystem fs;
		faultfs::io inner(fs);
		kv_index* target = nullptr;
		std::atomic<int> refused{0}, allowed{0}, fired{0}, pinned_seen{0};
		hook_io io(inner, [&] {
			if (fired.fetch_add(1) == 0) {  // once, on whichever thread writes first
				index_test_access::mutate(*target, refused, allowed);
				pinned_seen += index_test_access::buffer_pinned(*target) ? 1 : 0;
			}
		});
		auto o = options(threads);
		o.background = &exec;
		o.policy.segments_per_tier = 2;
		o.policy.floor_bytes = 1;
		auto ix = kv_index::create(io, o, uuid);
		target = &ix;
		std::mt19937_64 r(threads);
		// Committed segments for the pool to merge while the last flush runs.
		for (int s = 0; s < 3; ++s) {
			for (int d = 0; d < 60; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(1000 * s + d), value_of(r, 200))}), {}).result == verdict::accepted); }
			ix.refresh();
		}
		for (int d = 0; d < 400; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(5000 + d), value_of(r, 100 + d))}), {}).result == verdict::accepted); }
		const auto before = contents(ix);
		io.armed = 1;
		ix.commit();
		io.armed = 0;
		CHECK(fired.load() > 0);
		CHECK(refused.load() == 5 && allowed.load() == 0);  // all five mutators refused
		CHECK(pinned_seen.load() == 1);
		CHECK(!index_test_access::buffer_pinned(ix));  // and the pin ended with the flush
		CHECK(contents(ix).size() >= before.size());
		for (int d = 0; d < 10; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(9000 + d), "after")}), {}).result == verdict::accepted); }  // the buffer takes writes again
		ix.commit();
		ix.wait_merges();
		for (int d = 0; d < 400; d += 37) {
			CHECK(ix.find_key("Q" + std::to_string(5000 + d)));
		}
	}
}

// What the writer holds while it streams: records borrowed, not copied. The records compress to
// almost nothing, so a copy is the only thing that would hold their bytes at the first write.
// The budget is shared by the runs: when one run writes first, the others may be inside their
// key filters' build scratch (about 1 MiB a run, whatever the records' size). The records are
// sized so a quarter of them clears that scratch twice over, and the bound stays a quarter.
static void writer_holds_no_copy() {
	for (const std::size_t threads : {std::size_t(1), std::size_t(3)}) {
		byte_budget budget(1ull << 30);
		faultfs::filesystem fs;
		faultfs::io inner(fs);
		std::atomic<std::uint64_t> at_write{0};
		std::atomic<int> fired{0};
		hook_io io(inner, [&] {
			if (fired.fetch_add(1) == 0) { at_write = budget.used(); }
		});
		auto o = options(threads);
		o.budget = &budget;
		o.buffer_share = 1ull << 30;
		auto ix = kv_index::create(io, o, uuid);
		const std::size_t docs = 600, size = 64 << 10;
		// Every run's scratch at once, and the sort's charge.
		const std::uint64_t scratch = flush_charge::quantum + threads * (keys::filter_build_bytes(docs / threads) + flush_charge::quantum);
		CHECK(docs * size / 4 >= 2 * scratch);
		for (std::size_t d = 0; d < docs; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(d), std::string(size, char('a' + d % 7)))}), {}).result == verdict::accepted); }
		const std::uint64_t before = budget.used();
		io.armed = 1;
		ix.commit();
		io.armed = 0;
		CHECK(fired.load() > 0);
		CHECK(at_write.load() >= before);
		CHECK(at_write.load() - before < docs * size / 4);  // a copy of the records alone is docs * size
	}
}

// The flush's draws on the indexing budget end with it: streaming holds no output to settle.
static void budget_returns() {
	for (const std::size_t threads : {std::size_t(1), std::size_t(3)}) {
		byte_budget budget(64 << 20);
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto o = options(threads);
		o.budget = &budget;
		auto ix = kv_index::create(io, o, uuid);
		fill(ix, 9);
		CHECK(ix.digest() != 0);
		ix.close();
		CHECK(budget.used() == 0);
		CHECK(budget.peak() > 0);
	}
}

// The owner's lease starts before the flush collects its pointers into the buffer: when the
// first file is created, a mutation is already refused.
static void owner_pins_before_the_files() {
	for (const std::size_t threads : {std::size_t(1), std::size_t(3)}) {
		faultfs::filesystem fs;
		faultfs::io inner(fs);
		hook_io io(inner, nullptr);
		auto ix = kv_index::create(io, options(threads), uuid);
		std::mt19937_64 r(threads);
		for (int d = 0; d < 300; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(d), value_of(r, 50))}), {}).result == verdict::accepted); }
		std::atomic<int> created{0}, pinned{0};
		io.on_create = [&](std::string_view name) {
			if (!name.starts_with("seg-")) { return; }  // the commit's other files come after the flush
			++created;
			pinned += index_test_access::buffer_pinned(ix) ? 1 : 0;
		};
		ix.commit();
		CHECK(created.load() >= 1 && pinned.load() == created.load());
		CHECK(!index_test_access::buffer_pinned(ix));
	}
}

// close() inside a flush is refused before it resets anything: the index keeps its durable
// documents and later commits keep them. With three runs it is called from a run's own thread,
// which holds no lease: the refusal comes first, so that is no ownership break either.
static void close_inside_a_flush() {
	for (const std::size_t threads : {std::size_t(1), std::size_t(3)}) {
		faultfs::filesystem fs;
		faultfs::io inner(fs);
		kv_index* target = nullptr;
		std::atomic<int> fired{0}, refused{0};
		const auto owner = std::this_thread::get_id();
		hook_io io(inner, [&] {
			if (threads > 1 && std::this_thread::get_id() == owner) { return; }
			if (fired.fetch_add(1) == 0) {
				try {
					target->close();
				} catch (const std::logic_error&) {
					++refused;
				}
			}
		});
		auto ix = kv_index::create(io, options(threads), uuid);
		target = &ix;
		std::mt19937_64 r(threads);
		for (int d = 0; d < 300; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(d), value_of(r, 50))}), {}).result == verdict::accepted); }
		ix.commit();  // 300 documents durable
		CHECK(contents(ix).size() == 300);
		for (int d = 0; d < 300; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(10000 + d), value_of(r, 50))}), {}).result == verdict::accepted); }
		io.armed = 1;
		ix.commit();
		io.armed = 0;
		CHECK(refused.load() == 1);
		CHECK(ix.apply(tx({replace("Qpost", "p")}), {}).result == verdict::accepted);
		ix.commit();
		CHECK(contents(ix).size() == 601);
		CHECK(ix.find_key("Q0") && ix.find_key("Q10000") && ix.find_key("Qpost"));
	}
}

// A run that fails mid-stream leaves no file and no segment; the retry writes one copy.
static void failure_paths() {
	for (const std::size_t threads : {std::size_t(1), std::size_t(3)}) {
		for (const int error : {ENOSPC, EIO}) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			auto ix = kv_index::create(io, options(threads), uuid);
			std::mt19937_64 r(11);
			for (int d = 0; d < 300; ++d) { CHECK(ix.apply(tx({replace("Q" + std::to_string(d), value_of(r, 1500))}), {}).result == verdict::accepted); }
			const auto files_before = segment_files(fs).size();
			if (error == ENOSPC) {
				fs.set_capacity(fs.used() + 120000);  // room for part of the runs, not all
			} else {
				fs.add_fault({faultfs::op::write, threads, EIO});  // the last run's write, after the others
			}
			bool failed = false;
			try {
				ix.commit();
			} catch (const std::system_error& e) {
				failed = e.code().value() == error;
			} catch (const index_fenced&) {
				failed = error == EIO;
			}
			CHECK(failed);
			CHECK(segment_files(fs).size() == files_before);  // every partial file removed
			CHECK(ix.segments().empty());                      // none published
			fs.clear_faults();
			fs.set_capacity(~std::uint64_t(0));
			if (error == ENOSPC) {
				ix.commit();
				std::size_t live = 0;
				for (const auto& s : ix.segments()) { for (ordinal_t o = 0; o < s.seg()->core().doc_count(); ++o) { live += s.live(o); } }
				CHECK(live == 300 && contents(ix).size() == 300);
			}
		}
	}
}

int main(int argc, char** argv) {
	if (argc > 1 && std::string_view(argv[1]) == "--print") {
		for (const std::size_t t : {std::size_t(1), std::size_t(3)}) {
			std::size_t files = 0;
			const auto h = flushed_digest(t, &files);
			std::printf("threads %zu: %zu files, digest 0x%016llxull\n", t, files, (unsigned long long)h);
		}
		return 0;
	}
	gate();
	borrowed_equals_copied();
	borrow_refuses_what_it_cannot_hold();
	reserve_allocates_once();
	lifetime_rule_in_the_flush();
	writer_holds_no_copy();
	owner_pins_before_the_files();
	close_inside_a_flush();
	budget_returns();
	failure_paths();
	// The segment bytes each flush shape writes, as the commit before this change wrote them.
	{
		std::size_t files = 0;
		CHECK(flushed_digest(1, &files) == 0x9ce03837c27a5408ull && files == 2);
		CHECK(flushed_digest(3, &files) == 0x07bef1f86cb043c8ull && files == 6);
	}
	if (failures) {
		std::fprintf(stderr, "flush_memory: %d failures\n", failures);
		return 1;
	}
	std::puts("flush_memory: ok");
	return 0;
}
