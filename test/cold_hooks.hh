// The cold tier's test fixtures (docs/design/cold-tier.md): an IO wrapper over faultfs that
// injects open, map and damaged-map failures, counts maps and live mappings, and holds threads
// inside map() so that races are deterministic; a fixture with a registry; fill().
#pragma once

#include "kv_test.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using std::chrono::seconds;

// What the wrapper does and counts.
struct hooks {
	std::atomic<int> fail_open{0};   // errno that open_existing of a segment file fails with; 0: none
	std::atomic<int> fail_map{0};    // errno that File::map fails with; 0: none
	std::atomic<int> fail_after{-1}; // >= 0: the map() after that many more maps starts a burst of two failures
	std::atomic<int> fail_burst{0};  // map() calls left that fail with fail_errno
	std::atomic<int> fail_errno{ENOMEM};
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
		{
			int a = h_->fail_after.load();
			while (a >= 0 && !h_->fail_after.compare_exchange_weak(a, a - 1)) {}
			if (a == 0) { h_->fail_burst.store(2); }  // read_checked retries once, so one failure would be absorbed
			int b = h_->fail_burst.load();
			while (b > 0 && !h_->fail_burst.compare_exchange_weak(b, b - 1)) {}
			if (b > 0) { throw std::system_error(h_->fail_errno.load(), std::generic_category(), "hook: map burst"); }
		}
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
		o.keyed = true;
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

}  // namespace
