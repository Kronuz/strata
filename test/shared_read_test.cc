// The shared reader's step 1, items 2 and 4 (docs/design/shared-reader.md D3, D4, D8; SR-D5,
// SR-D10): copy-on-write live-docs bits, alone and under reader threads reading frozen shares
// while the owner deletes, rolls back, flushes, and merges; the owner letting go of a merge's
// sources itself; and a snapshot's get and scan, the index's own segment code, against the
// index's reads and a std::map model at every publishable point.
#include "kv_test.hh"

#include "strata/core/owner.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

namespace strata {
// Frozen shares of the index's segments, as a view will hold them: the segment objects, their
// bits shared (D3), and segment_views over those. Built and dropped on the owner thread.
struct index_test_access {
	struct frozen {
		std::vector<std::shared_ptr<kv_index::open_segment>> keep;
		std::vector<deleted_bits> bits;
		std::vector<kv_index::segment_view> views;
		std::uint64_t ones = 0;  // bits set across the shares, to check they never change
	};
	static std::unique_ptr<frozen> freeze(const kv_index& ix) {
		auto f = std::make_unique<frozen>();
		f->bits.reserve(ix.segs_.size());
		for (const auto& s : ix.segs_) {
			f->keep.push_back(s);
			f->bits.push_back(s->deleted.share());
			for (auto w : f->bits.back()) { f->ones += std::uint64_t(std::popcount(w)); }
			f->views.push_back({s->ref.id, &s->seg, s->dead ? &f->bits.back() : nullptr, s->dead});
		}
		return f;
	}
	static std::size_t shared_count(const kv_index& ix) {
		return std::size_t(std::count_if(ix.segs_.begin(), ix.segs_.end(), [](const auto& s) { return s->deleted.shared(); }));
	}
};
}  // namespace strata

namespace {

std::string key_of(std::size_t i) {
	char k[16];
	std::snprintf(k, sizeof k, "k%05zu", i);
	return k;
}

using model_t = std::map<std::string, std::string>;

// Every (key, value) a scan from "" visits.
template <class Reader>
model_t scanned(const Reader& r) {
	model_t out;
	r.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace(std::string(k), std::string(v)); });
	return out;
}

void bits_alone() {
	deleted_bits none;
	CHECK(none.empty() && none.size() == 0 && !none.shared() && none.data() == nullptr && none.mutable_words() == nullptr);
	CHECK(none.share().empty());
	deleted_bits a(3);
	CHECK(a.size() == 3 && !a.shared() && a[0] == 0 && a[2] == 0);
	a.set(5);
	a.set(130);
	CHECK(a.test(5) && a.test(130) && !a.test(6));
	{
		deleted_bits b = a.share();
		CHECK(a.shared() && b.shared() && b.data() == a.data());
		a.set(64);  // a write to shared bits clones first
		CHECK(!a.shared() && !b.shared() && b.data() != a.data());
		CHECK(a.test(64) && !b.test(64) && b.test(5) && b.test(130));
		deleted_bits c = b.share();
		c.reset(5);  // the rollback path clones too
		CHECK(!c.test(5) && b.test(5));
	}
	deleted_bits d = a.clone();
	CHECK(!d.shared() && d.data() != a.data() && d.test(64));
	deleted_bits e = std::move(d);
	CHECK(d.empty() && e.test(130));
	static_assert(sizeof(deleted_bits) == sizeof(void*), "one pointer in open_segment; the count and size share the words' allocation");
	std::printf("bits alone: ok\n");
}

// Item 4: a snapshot of segments() reads what the index reads, at every publishable point (a
// refresh with nothing applied after it), and the index reads what the model holds throughout.
void snapshot_differential(std::uint64_t seed) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "k";
	auto ix = kv_index::create(io, o, uuid_of("snapdiff"));
	std::mt19937_64 r(seed);
	model_t model;
	const std::size_t keys = 600;
	std::size_t publishable_checks = 0, gets = 0;
	auto check_point = [&] {
		const auto segs = ix.segments();
		const kv_index::snapshot snap(segs);
		CHECK(scanned(snap) == model && scanned(ix) == model);
		for (std::size_t i = 0; i < keys; i += 1 + r() % 7) {
			const std::string k = key_of(i);
			const auto want = model.count(k) ? std::optional<std::string>(model[k]) : std::nullopt;
			CHECK(snap.get(k) == want && ix.get(k) == want);
			++gets;
		}
		for (int t = 0; t < 8; ++t) {  // bounded and limited scans agree pairwise
			std::string from = key_of(r() % keys), to = key_of(r() % keys);
			const std::size_t n = r() % 20;
			std::vector<std::pair<std::string, std::string>> a, b;
			snap.scan(from, to, n, [&](std::string_view k, std::string_view v, std::uint64_t) { a.emplace_back(k, v); });
			ix.scan(from, to, n, [&](std::string_view k, std::string_view v, std::uint64_t) { b.emplace_back(k, v); });
			CHECK(a == b);
			a.clear();
			snap.scan(from, n, [&](std::string_view k, std::string_view v, std::uint64_t) {
				a.emplace_back(k, v);
				return a.size() < 5;
			});
			CHECK(a.size() <= std::min<std::size_t>(5, n));
		}
		++publishable_checks;
	};
	for (int step = 0; step < 400; ++step) {
		const int what = int(r() % 100);
		if (what < 70) {
			std::vector<operation> ops;
			model_t next = model;
			const int n = 1 + int(r() % 4);
			for (int j = 0; j < n; ++j) {
				const std::string k = key_of(r() % keys);
				if (r() % 3 == 0) {
					ops.push_back(erase(k));
					next.erase(k);
				} else {
					const std::string v = std::string(1 + r() % 40, char('a' + r() % 26)) + std::to_string(step);
					ops.push_back(replace(k, v));
					next[k] = v;
				}
			}
			const bool reject = r() % 8 == 0;
			if (reject) { ops.push_back(replace(key_of(r() % keys), "never", 1u << 30)); }
			const auto v = ix.apply(tx(ops), {});
			CHECK((v.result == verdict::accepted) == !reject);
			if (!reject) { model = std::move(next); }
			if (r() % 5 == 0) {  // the owner's reads include the buffer
				const std::string k = key_of(r() % keys);
				CHECK(ix.get(k) == (model.count(k) ? std::optional<std::string>(model[k]) : std::nullopt));
			}
		} else if (what < 85) {
			ix.refresh();
			check_point();
		} else if (what < 92) {
			ix.refresh();
			ix.force_merge(1 + r() % 3);
			check_point();
		} else {
			ix.commit();
			check_point();
		}
	}
	std::printf("snapshot differential (seed %llu): %zu publishable points, %zu gets, %zu segments at the end\n", (unsigned long long)seed,
		publishable_checks, gets, ix.segments().size());
}

// Item 2 under readers: each round freezes shares of every segment's bits, starts reader threads
// that get and scan the frozen snapshot and recount its bits, and has the owner delete flushed
// documents (cloning shared bits), reject transactions after deleting (the rollback clears the
// clone's bit), flush, merge, and commit. The frozen state never changes, and readers see the
// model as of the freeze.
void bits_under_readers(std::uint64_t seed, int readers) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "k";
	auto ix = kv_index::create(io, o, uuid_of("cowread"));
	std::mt19937_64 r(seed);
	model_t model;
	const std::size_t keys = 800;
	for (std::size_t i = 0; i < keys; ++i) {
		model[key_of(i)] = "v" + std::to_string(i);
		CHECK(ix.apply(tx({replace(key_of(i), model[key_of(i)])}), {}).result == verdict::accepted);
		if (i % 200 == 199) { ix.refresh(); }
	}
	std::uint64_t reads = 0, clones_seen = 0;
	for (int round = 0; round < 12; ++round) {
		ix.refresh();
		CHECK(scanned(ix) == model);
		auto frozen = index_test_access::freeze(ix);
		const model_t then = model;
		const kv_index::snapshot snap(frozen->views);
		std::atomic<bool> stop{false};
		std::atomic<std::uint64_t> bad{0}, n{0};
		std::vector<std::thread> ts;
		for (int t = 0; t < readers; ++t) {
			ts.emplace_back([&, t] {
				const off_owner_scope reader;  // owner-only state changing here asserts
				std::mt19937_64 rr(seed * 131 + std::uint64_t(t));
				while (!stop.load(std::memory_order_relaxed)) {
					const std::string k = key_of(rr() % keys);
					const auto got = snap.get(k);
					const auto it = then.find(k);
					if (got != (it == then.end() ? std::nullopt : std::optional<std::string>(it->second))) { bad.fetch_add(1); }
					if (rr() % 16 == 0) {
						std::size_t seen = 0;
						auto at = then.lower_bound(k);
						snap.scan(k, 10, [&](std::string_view kk, std::string_view vv, std::uint64_t) {
							if (at == then.end() || at->first != kk || at->second != vv) { bad.fetch_add(1); }
							if (at != then.end()) { ++at; }
							++seen;
						});
						if (seen != std::min<std::size_t>(10, std::size_t(std::distance(then.lower_bound(k), then.end())))) { bad.fetch_add(1); }
						std::uint64_t ones = 0;
						for (const auto& b : frozen->bits) {
							for (auto w : b) { ones += std::uint64_t(std::popcount(w)); }
						}
						if (ones != frozen->ones) { bad.fetch_add(1); }
					}
					n.fetch_add(1, std::memory_order_relaxed);
				}
			});
		}
		for (int step = 0; step < 300; ++step) {
			const std::string k = key_of(r() % keys);
			const int what = int(r() % 10);
			if (what < 4) {  // a delete of a flushed copy: shared bits clone
				const std::size_t shared = index_test_access::shared_count(ix);
				CHECK(ix.apply(tx({erase(k)}), {}).result == verdict::accepted);
				model.erase(k);
				clones_seen += shared - index_test_access::shared_count(ix);
			} else if (what < 7) {  // a delete, then a conflict: the rollback clears the clone's bit
				const std::string other = key_of(r() % keys);
				CHECK(ix.apply(tx({erase(k), replace(other, "never", 1u << 30)}), {}).result == verdict::version_conflict);
			} else if (what < 9) {
				model[k] = "r" + std::to_string(round) + "." + std::to_string(step);
				CHECK(ix.apply(tx({replace(k, model[k])}), {}).result == verdict::accepted);
			} else if (r() % 3 == 0) {
				ix.refresh();
				ix.force_merge(2);  // the sources stay alive in the frozen shares
			} else {
				ix.commit();
			}
		}
		stop.store(true);
		for (auto& t : ts) { t.join(); }
		reads += n.load();
		CHECK(bad.load() == 0);
		std::uint64_t ones = 0;
		for (const auto& b : frozen->bits) {
			for (auto w : b) { ones += std::uint64_t(std::popcount(w)); }
		}
		CHECK(ones == frozen->ones);
		frozen.reset();  // the owner drops the shares and any segment only they held
	}
	ix.refresh();
	CHECK(scanned(ix) == model);
	std::printf("bits under %d readers (seed %llu): %llu reads, %llu deletes that cloned shared bits\n", readers, (unsigned long long)seed,
		(unsigned long long)reads, (unsigned long long)clones_seen);
	CHECK(reads > 0 && clones_seen > 0);
}

// Runs each work on one worker thread, marked off the owner as a pool's are, then keeps it,
// as a pool may keep a task's closure after the task set `done`, until release() destroys the
// kept work there.
class holding_executor final : public executor {
public:
	holding_executor() : t_([this] { loop(); }) {}
	~holding_executor() override {
		{
			std::lock_guard l(mu_);
			quit_ = true;
		}
		cv_.notify_all();
		t_.join();
	}
	std::unique_ptr<group> open() override { return std::make_unique<holding_group>(*this); }
	std::uint64_t now() override { return std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }
	bool sleep_until(std::uint64_t, const task::cancel_token& stop) override { return !stop.requested(); }
	void await_ran(std::size_t n) {
		std::unique_lock l(mu_);
		cv_.wait(l, [&] { return ran_ >= n; });
	}
	void release() {
		std::unique_lock l(mu_);
		release_ = true;
		cv_.notify_all();
		cv_.wait(l, [&] { return !release_; });
	}

private:
	struct item {
		work w;
		task::cancel_token stop;
	};
	class holding_group final : public group {
	public:
		explicit holding_group(holding_executor& e) : e_(&e) {}
		~holding_group() override { cancel(); }
		void post(work w) override {
			std::lock_guard l(e_->mu_);
			e_->queue_.push_back({std::move(w), source_.token()});
			e_->cv_.notify_all();
		}
		void cancel() noexcept override {
			source_.request();
			std::deque<item> dropped;
			std::unique_lock l(e_->mu_);
			dropped.swap(e_->queue_);
			e_->cv_.wait(l, [&] { return !e_->running_; });
			l.unlock();
			dropped.clear();  // on the caller, as a pool drops queued work
			source_ = task::cancel_source();
		}
		void wait() override {
			std::unique_lock l(e_->mu_);
			e_->cv_.wait(l, [&] { return e_->queue_.empty() && !e_->running_; });
		}

	private:
		holding_executor* e_;
		task::cancel_source source_;
	};
	void loop() {
		const off_owner_scope worker;
		std::unique_lock l(mu_);
		for (;;) {
			cv_.wait(l, [&] { return quit_ || release_ || !queue_.empty(); });
			if (release_ || (quit_ && queue_.empty())) {
				std::deque<item> k;
				k.swap(kept_);
				l.unlock();
				k.clear();  // here, on the worker
				l.lock();
				if (quit_ && !release_) { return; }
				release_ = false;
				cv_.notify_all();
				continue;
			}
			item it = std::move(queue_.front());
			queue_.pop_front();
			running_ = true;
			l.unlock();
			try {
				it.w(it.stop);
			} catch (...) {
			}
			l.lock();
			running_ = false;
			kept_.push_back(std::move(it));
			++ran_;
			cv_.notify_all();
		}
	}
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<item> queue_, kept_;
	std::size_t ran_ = 0;
	bool running_ = false, release_ = false, quit_ = false;
	std::thread t_;
};

// SR-D5: a merge's job outlives `done` in the executor; the segments it replaced must not drop
// there. Installed at a refresh, ended by close(), and ended by destruction, each with the
// worker still holding the finished job until after the owner let it go.
void merge_sources_on_owner() {
	const std::uint64_t before = detail::off_owner_changes().load();
	for (int path = 0; path < 3; ++path) {
		holding_executor ex;
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.key_prefix = "k";
		o.background = &ex;
		o.auto_merge = true;
		o.policy.segments_per_tier = 2;
		o.policy.floor_bytes = 1;
		{
			auto ix = kv_index::create(io, o, uuid_of("mergesrc"));
			for (int s = 0; s < 12 && !ix.merges_pending(); ++s) {
				for (int i = 0; i < 50; ++i) { CHECK(ix.apply(tx({replace(key_of(std::size_t(s * 50 + i)), "v")}), {}).result == verdict::accepted); }
				ix.refresh();  // once the tiers are passed, a merge goes to the executor, not waited for
			}
			CHECK(ix.merges_pending() == 1);
			if (ix.merges_pending() != 1) { return; }
			ex.await_ran(1);
			if (path == 0) {
				const std::size_t segments = ix.segments().size();
				ix.wait_merges();  // publishes it: its sources leave segs_
				CHECK(ix.segments().size() < segments && ix.merges_pending() == 0);
				ex.release();  // the worker drops the job; the owner already let go of its sources
				CHECK(ix.get(key_of(7)) == std::optional<std::string>("v"));
			} else if (path == 1) {
				ix.close();
				ex.release();
			}
		}
		ex.release();
	}
	const std::uint64_t after = detail::off_owner_changes().load();
	std::printf("merge sources on the owner: %llu segments or shares dropped off the owner\n", (unsigned long long)(after - before));
	CHECK(after == before);
}

}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
	bits_alone();
	for (std::uint64_t s = seed; s < seed + 3; ++s) { snapshot_differential(s); }
	bits_under_readers(seed, 4);
	merge_sources_on_owner();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::printf("shared read: ok\n");
	return 0;
}
