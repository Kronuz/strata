// Fixtures for counted_io's tests (docs/design/cold-tier.md D6; S3): a model of the kernel's
// descriptor and map tables, an IO over faultfs that takes from it as PosixIO would (the directory
// descriptor when it is built, one per file, owner lock and scan, one map per mapping), and the
// failures the kernel can answer: a limit, a chosen errno, a random one.
#pragma once

#include "strata/core/counted_io.hh"
#include "strata/core/faultfs.hh"
#include "strata/core/resource_budget.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace {

using kronuz::journal::DirectoryCursor;
using kronuz::journal::File;
using kronuz::journal::IO;
using kronuz::journal::Mapping;
using kronuz::journal::OwnerLock;

[[noreturn]] void kernel_error(int e, const char* what) { throw std::system_error(e, std::generic_category(), what); }

// The kernel's tables, as far as the budget counts them.
struct kernel {
	std::atomic<std::int64_t> fds{0}, maps{0}, fd_peak{0}, map_peak{0};
	std::atomic<std::int64_t> fd_limit{std::int64_t(1) << 40}, map_limit{std::int64_t(1) << 40};
	std::atomic<std::int64_t> external_fds{0};  // descriptors another user of the limit holds
	std::atomic<std::uint64_t> fd_calls{0}, map_calls{0};
	std::atomic<std::uint64_t> refused{0};      // answers because of a limit (never chaos)
	std::atomic<std::uint64_t> chaos_hits{0};
	std::atomic<int> fail_fd{0}, fail_map{0};   // errno every call fails with while set
	std::atomic<unsigned> chaos{0};             // permille of calls that fail with a random errno
	std::atomic<bool> close_supported{true};
	std::function<void()> map_hook;             // runs inside a successful map call; set while quiescent

	struct fd_token {
		fd_token() = default;
		explicit fd_token(kernel* k) : k_(k) {}
		fd_token(fd_token&& o) noexcept : k_(std::exchange(o.k_, nullptr)) {}
		fd_token& operator=(fd_token&& o) noexcept {
			if (this != &o) {
				reset();
				k_ = std::exchange(o.k_, nullptr);
			}
			return *this;
		}
		~fd_token() { reset(); }
		void reset() noexcept {
			if (k_) { k_->fds.fetch_sub(1); }
			k_ = nullptr;
		}
		kernel* k_ = nullptr;
	};
	struct map_token {
		map_token() = default;
		explicit map_token(kernel* k) : k_(k) {}
		map_token(map_token&& o) noexcept : k_(std::exchange(o.k_, nullptr)) {}
		map_token& operator=(map_token&&) = delete;
		~map_token() {
			if (k_) { k_->maps.fetch_sub(1); }
		}
		kernel* k_ = nullptr;
	};

	void maybe_chaos() {
		const unsigned permille = chaos.load();
		if (!permille) { return; }
		thread_local std::mt19937 rng(static_cast<unsigned>(std::hash<std::thread::id>()(std::this_thread::get_id())));
		if (rng() % 1000 < permille) {
			static const int errors[] = {EMFILE, ENFILE, ENOMEM, EIO};
			++chaos_hits;
			kernel_error(errors[rng() % 4], "kernel: chaos");
		}
	}
	static void raise(std::atomic<std::int64_t>& peak, std::int64_t v) {
		std::int64_t p = peak.load();
		while (p < v && !peak.compare_exchange_weak(p, v)) {}
	}
	fd_token take_fd() {
		++fd_calls;
		if (const int e = fail_fd.load()) { kernel_error(e, "kernel: open"); }
		maybe_chaos();
		std::int64_t cur = fds.load();
		for (;;) {
			if (cur + external_fds.load() >= fd_limit.load()) {
				++refused;
				kernel_error(EMFILE, "kernel: descriptor table full");
			}
			if (fds.compare_exchange_weak(cur, cur + 1)) { break; }
		}
		raise(fd_peak, cur + 1);
		return fd_token(this);
	}
	map_token take_map() {
		++map_calls;
		if (const int e = fail_map.load()) { kernel_error(e, "kernel: map"); }
		maybe_chaos();
		std::int64_t cur = maps.load();
		for (;;) {
			if (cur >= map_limit.load()) {
				++refused;
				kernel_error(ENOMEM, "kernel: map count full");
			}
			if (maps.compare_exchange_weak(cur, cur + 1)) { break; }
		}
		raise(map_peak, cur + 1);
		if (map_hook) { map_hook(); }
		return map_token(this);
	}
};

class probe_mapping final : public Mapping {
public:
	probe_mapping(std::shared_ptr<const Mapping> inner, kernel::map_token t) : tok_(std::move(t)), inner_(std::move(inner)) { bytes_ = inner_->bytes(); }

private:
	kernel::map_token tok_;
	std::shared_ptr<const Mapping> inner_;
};

class probe_file final : public File {
public:
	probe_file(std::unique_ptr<File> in, kernel& k, kernel::fd_token t) : k_(&k), tok_(std::move(t)), in_(std::move(in)) {}
	std::uint64_t size() override { return in_->size(); }
	std::size_t read_at(std::uint64_t o, std::span<char> b) override { return in_->read_at(o, b); }
	std::size_t write_at(std::uint64_t o, std::string_view b) override { return in_->write_at(o, b); }
	void truncate(std::uint64_t s) override { in_->truncate(s); }
	void sync() override { in_->sync(); }
	void close() override {
		if (!k_->close_supported) { throw std::logic_error("explicit file close unsupported"); }
		tok_.reset();
	}
	std::shared_ptr<const Mapping> map() override {
		auto inner = in_->map();
		if (!inner) { return nullptr; }
		kernel::map_token t = k_->take_map();
		return std::make_shared<probe_mapping>(std::move(inner), std::move(t));
	}

private:
	kernel* k_;
	kernel::fd_token tok_;
	std::unique_ptr<File> in_;
};

struct probe_owner final : OwnerLock {
	probe_owner(std::unique_ptr<OwnerLock> l, kernel::fd_token t) : tok(std::move(t)), in(std::move(l)) {}
	kernel::fd_token tok;
	std::unique_ptr<OwnerLock> in;
};

struct probe_cursor final : DirectoryCursor {
	probe_cursor(std::unique_ptr<DirectoryCursor> c, kernel::fd_token t) : tok(std::move(t)), in(std::move(c)) {}
	std::optional<std::string> next() override { return in->next(); }
	void close() override { tok.reset(); }
	kernel::fd_token tok;
	std::unique_ptr<DirectoryCursor> in;
};

// What PosixIO is to the kernel: built with a descriptor on its directory, one descriptor per
// open file, owner lock and scan, one map per mapping.
class probe_io final : public IO {
public:
	static std::unique_ptr<IO> make(kernel& k, strata::faultfs::io& in) {
		kernel::fd_token t = k.take_fd();
		return std::unique_ptr<IO>(new probe_io(in, k, std::move(t)));
	}
	std::unique_ptr<OwnerLock> acquire_owner(bool create) override {
		auto t = k_->take_fd();
		auto l = in_->acquire_owner(create);
		return std::make_unique<probe_owner>(std::move(l), std::move(t));
	}
	std::unique_ptr<OwnerLock> acquire_directory_owner() override { return in_->acquire_directory_owner(); }
	std::unique_ptr<OwnerLock> acquire_directory_reader() override { return in_->acquire_directory_reader(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return in_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return in_->directory_trust(); }
	std::unique_ptr<File> open_existing(std::string_view n) override {
		auto t = k_->take_fd();
		auto f = in_->open_existing(n);
		return std::make_unique<probe_file>(std::move(f), *k_, std::move(t));
	}
	std::unique_ptr<File> create_exclusive(std::string_view n) override {
		auto t = k_->take_fd();
		auto f = in_->create_exclusive(n);
		return std::make_unique<probe_file>(std::move(f), *k_, std::move(t));
	}
	std::unique_ptr<File> open_existing_read_only(std::string_view n) override { return in_->open_existing_read_only(n); }
	void open_existing_read_only_into(std::string_view n, File& f) override { in_->open_existing_read_only_into(n, f); }
	std::unique_ptr<File> open_reclaim_candidate_read_only(std::string_view n) override { return in_->open_reclaim_candidate_read_only(n); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, File& f) override { return in_->open_reclaim_candidate_read_only_into(n, f); }
	void replace(std::string_view s, std::string_view d) override { in_->replace(s, d); }
	void remove(std::string_view n) override { in_->remove(n); }
	std::unique_ptr<DirectoryCursor> scan_directory() override {
		auto t = k_->take_fd();
		auto c = in_->scan_directory();
		return std::make_unique<probe_cursor>(std::move(c), std::move(t));
	}
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view n) override { return in_->entry_footprint(n); }
	void sync_directory() override { in_->sync_directory(); }

private:
	probe_io(strata::faultfs::io& in, kernel& k, kernel::fd_token t) : in_(&in), k_(&k), tok_(std::move(t)) {}
	strata::faultfs::io* in_;
	kernel* k_;
	kernel::fd_token tok_;
};

// A system with fixed limits: `fds` is both soft and hard, so nothing is raised.
strata::resource_system test_system(std::uint64_t fds, std::uint64_t max_maps, std::uint64_t max_vnodes) {
	strata::resource_system s;
	s.get_limits = [fds] { return strata::resource_limits{fds, fds}; };
	s.set_soft = [](std::uint64_t) { return false; };
	s.count_maps = true;
	s.max_maps = max_maps;
	s.count_vnodes = true;
	s.max_vnodes = max_vnodes;
	return s;
}

// Fails the n-th allocation after it is armed, on the arming thread only; the unhurt
// threads allocate as usual.
thread_local int alloc_countdown = 0;
struct alloc_arm {
	explicit alloc_arm(int n) { alloc_countdown = n; }
	~alloc_arm() { alloc_countdown = 0; }
};

}  // namespace

#if defined(__SANITIZE_THREAD__)
#define STRATA_NO_ALLOC_HOOK 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STRATA_NO_ALLOC_HOOK 1
#endif
#endif

// ThreadSanitizer's runtime defines the allocation operators itself, so a build under it has no
// injection; every other build replaces the whole family, so a sanitizer sees matched pairs.
#ifndef STRATA_NO_ALLOC_HOOK
namespace {
void* hook_alloc(std::size_t n, std::size_t align) {
	if (alloc_countdown > 0 && --alloc_countdown == 0) { return nullptr; }
	if (align <= alignof(std::max_align_t)) { return std::malloc(n ? n : 1); }
	void* p = nullptr;
	return ::posix_memalign(&p, align, n ? n : 1) == 0 ? p : nullptr;
}
void* hook_new(std::size_t n, std::size_t align) {
	if (void* p = hook_alloc(n, align)) { return p; }
	throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t n) { return hook_new(n, 0); }
void* operator new[](std::size_t n) { return hook_new(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) { return hook_new(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return hook_new(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return hook_alloc(n, 0); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return hook_alloc(n, 0); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return hook_alloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return hook_alloc(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
#endif
