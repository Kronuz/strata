// T3's single-index arm (docs/design/shared-reader.md T3), as far as step 1 builds it: the cost of
// a hazard acquire and release (hazard.hh) per operation at 1 to N threads on one published
// pointer, a hot-cache get on a snapshot of one index's segments (the shared read path, D8) with
// and without the guard, the same get with each guard released on another thread, and the
// comparison arm, a shared_ptr loaded atomically (Lucene's acquire shape, rejected alternative 1).
// Views and publication come in step 1's item 3; here the pointer is published once.
//   acquire [threads...] [seconds per arm]   default: 1 2 4 8, 1 s
#include "strata/core/faultfs.hh"
#include "strata/core/hazard.hh"
#include "strata/core/index.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace strata;
using kv_index = basic_index<document_layout>;

namespace {

struct shared_snapshot {
	std::vector<kv_index::segment_view> segs;
	kv_index::snapshot snap() const { return kv_index::snapshot(segs); }
};

std::string key_of(std::size_t i) {
	char k[16];
	std::snprintf(k, sizeof k, "Q%07zu", i);
	return k;
}

using clock_type = std::chrono::steady_clock;

std::atomic<std::uint64_t> sink{0};  // keeps the bodies' results alive, added once per thread

// Runs body(thread, i) on `threads` threads for `secs`; body returns how many operations it did.
// Returns ns per operation per thread.
template <class Body>
double per_op(int threads, double secs, Body body) {
	std::atomic<bool> go{false}, stop{false};
	std::vector<std::uint64_t> ops(std::size_t(threads) * 16, 0);  // padded counters
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			while (!go.load(std::memory_order_acquire)) {}
			std::uint64_t n = 0, i = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				for (int k = 0; k < 64; ++k) { n += body(t, i++); }
			}
			ops[std::size_t(t) * 16] = n;
			sink.fetch_add(i, std::memory_order_relaxed);
		});
	}
	const auto t0 = clock_type::now();
	go.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::duration<double>(secs));
	stop.store(true);
	for (auto& t : ts) { t.join(); }
	const double ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	std::uint64_t total = 0;
	for (int t = 0; t < threads; ++t) { total += ops[std::size_t(t) * 16]; }
	return ns * threads / double(total);
}

}  // namespace

int main(int argc, char** argv) {
	std::vector<int> counts;
	double secs = 1.0;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]).find('.') != std::string::npos) {
			secs = std::atof(argv[i]);
		} else {
			counts.push_back(std::atoi(argv[i]));
		}
	}
	if (counts.empty()) { counts = {1, 2, 4, 8}; }

	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "Q";
	auto ix = kv_index::create(io, o, std::array<char, 16>{'s', 'r', '-', 'a', 'c', 'q'});
	const std::size_t n = 100000;
	std::mt19937_64 r(1);
	for (std::size_t s = 0; s < 3; ++s) {
		for (std::size_t i = s; i < n; i += 3) {
			kv_index::operation op;
			op.type = kv_index::operation::replace_key;
			op.key = key_of(i);
			op.doc.key = op.key;
			op.doc.stored = std::string(100, char('a' + r() % 26));
			kv_index::transaction t;
			t.ops.push_back(std::move(op));
			ix.apply(t, {});
		}
		ix.refresh();
	}
	auto held = std::make_unique<shared_snapshot>();
	{
		const auto list = ix.segments();  // after a refresh nothing is pending: the views name the segments' own bits
		held->segs.assign(list.begin(), list.end());
	}
	std::atomic<const shared_snapshot*> current{held.get()};
	std::vector<std::string> keys;
	for (std::size_t i = 0; i < 4096; ++i) { keys.push_back(key_of(r() % n)); }
	for (const auto& k : keys) { (void)held->snap().get(k, [](std::string_view) {}); }  // warm every region

	const int maxt = *std::max_element(counts.begin(), counts.end());
	reader_registry reg(std::size_t(maxt) * 4 + 16);
	auto sp = std::make_shared<const shared_snapshot>(*held);

	std::printf("threads  acquire+release  get  acquire+get+release  shared_ptr load+drop  (ns per op per thread; segments %zu, H %zu)\n", held->segs.size(),
		reg.capacity());
	for (int t : counts) {
		const double acq = per_op(t, secs, [&](int, std::uint64_t) {
			auto g = reg.acquire(current);
			return std::uint64_t(g ? 1 : 0);
		});
		const double get = per_op(t, secs, [&](int th, std::uint64_t i) {
			std::size_t len = 0;
			current.load(std::memory_order_acquire)->snap().get(keys[(i * 7 + std::uint64_t(th) * 977) & 4095], [&](std::string_view v) { len = v.size(); });
			return std::uint64_t(len ? 1 : 0);
		});
		const double both = per_op(t, secs, [&](int th, std::uint64_t i) {
			auto g = reg.acquire(current);
			std::size_t len = 0;
			g->snap().get(keys[(i * 7 + std::uint64_t(th) * 977) & 4095], [&](std::string_view v) { len = v.size(); });
			return std::uint64_t(len ? 1 : 0);
		});
		const double shared = per_op(t, secs, [&](int, std::uint64_t) {
			auto p = std::atomic_load_explicit(&sp, std::memory_order_acquire);
			return std::uint64_t(p ? 1 : 0);
		});
		std::printf("%7d  %15.1f  %4.1f  %19.1f  %20.1f\n", t, acq, get, both, shared);
	}
	// Cross-thread release: each thread acquires, and the next thread releases its guard.
	std::printf("threads  acquire here, release on the next thread (ns per acquire+release per thread)\n");
	for (int t : counts) {
		if (t < 2) { continue; }
		struct alignas(64) slot {
			std::atomic<bool> full{false};
			reader_registry::guard<shared_snapshot> g;
		};
		std::vector<slot> slots(static_cast<std::size_t>(t));
		const double cross = per_op(t, secs, [&](int th, std::uint64_t) {  // counts acquires
			slot& mine = slots[std::size_t(th)];
			slot& theirs = slots[std::size_t((th + t - 1) % t)];
			if (theirs.full.load(std::memory_order_acquire)) {  // the previous thread's guard: release it here
				theirs.g.release();
				theirs.full.store(false, std::memory_order_release);
			}
			if (!mine.full.load(std::memory_order_acquire)) {
				mine.g = reg.acquire(current);
				mine.full.store(true, std::memory_order_release);
				return std::uint64_t(1);
			}
			return std::uint64_t(0);
		});
		for (auto& s : slots) { s.g.release(); }
		// The control: the same handoff of a full slot to the next thread, which only clears it;
		// the acquiring thread releases its own guard before it hands the slot over.
		const double same = per_op(t, secs, [&](int th, std::uint64_t) {
			slot& mine = slots[std::size_t(th)];
			slot& theirs = slots[std::size_t((th + t - 1) % t)];
			if (theirs.full.load(std::memory_order_acquire)) { theirs.full.store(false, std::memory_order_release); }
			if (!mine.full.load(std::memory_order_acquire)) {
				mine.g = reg.acquire(current);
				mine.g.release();
				mine.full.store(true, std::memory_order_release);
				return std::uint64_t(1);
			}
			return std::uint64_t(0);
		});
		std::printf("%7d  %.1f  (control, the same handoff with each guard released where it was acquired: %.1f)\n", t, cross, same);
	}
	std::printf("(sink %llu)\n", (unsigned long long)sink.load());
	return 0;
}
