// The process-wide budget of kernel resources a catalog's indexes hold: maps (Linux), descriptors
// and vnodes (macOS and FreeBSD) (cold-tier D6; CT-R8, CT-R12, CT-R27, CT-R37). It has byte_budget's
// shape: a capacity drawn on through move-only reservations that return what they hold when they
// end. One instance is shared by every catalog and its IO (counted_io.hh); a catalog and a slot
// keep a resource_counts of their own, which a reservation moves with it, for eviction.
//
// A reservation is made before the system call it covers and ends with the kernel object it
// stands for, so the budget's count is never below the kernel's. One that would pass a hard cap
// is refused with too_many_open, a host error that never reaches the kernel.
//
// Soft caps tell a sweep when to evict. The descriptor caps come from RLIMIT_NOFILE, which the
// constructor raises to the hard limit (bounded by the system's per-process maximum) and tick()
// re-reads. A counted call the kernel refuses with EMFILE, ENFILE or ENOMEM below the cap is
// external pressure: it lowers a separate `external` cap to the count reached, which tick()'s
// re-derivation keeps until the limit changes or the quiet period passes; it never goes below
// min_indexes, so the budget always leaves room for that many warm indexes.
//
// Lifetime: a reservation keeps raw pointers to the budget and to the resource_counts it was made
// with, and returns its count to them when it ends. The budget and every resource_counts therefore
// outlive every reservation made on them, including a mapping or file that a reader still holds
// after its catalog or slot is gone; ~resource_counts asserts that its counts are back to zero.
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>
#if !defined(__linux__)
#include <sys/sysctl.h>
#endif

namespace strata {

enum class resource : unsigned { maps = 0, descriptors = 1, vnodes = 2 };
inline constexpr std::size_t resource_kinds = 3;

// A reservation past a hard cap: the host's error under D9 (never marks a record, never fences).
// It carries EMFILE, so a caller that sorts errors by code files it with the kernel's.
struct too_many_open final : std::system_error {
	explicit too_many_open(const char* what) : std::system_error(EMFILE, std::generic_category(), what) {}
};

// The counts one holder keeps (a catalog, a slot): what its reservations hold now. It must outlive
// every reservation made with it (see the file comment); debug builds assert that they are all back.
struct resource_counts {
	std::atomic<std::uint64_t> n[resource_kinds] = {};
	resource_counts() = default;
	resource_counts(const resource_counts&) = delete;
	resource_counts& operator=(const resource_counts&) = delete;
	~resource_counts() {
		for (const auto& c : n) { assert(c.load(std::memory_order_relaxed) == 0 && "strata: a resource_counts ended while reservations made with it are held"); }
	}
	std::uint64_t get(resource r) const noexcept { return n[static_cast<unsigned>(r)].load(std::memory_order_relaxed); }
};

struct resource_limits {
	std::uint64_t soft = 0, hard = 0;
};

// What the kernel says; the constructor reads it through these, so a test can stand in for it.
struct resource_system {
	std::function<resource_limits()> get_limits;      // RLIMIT_NOFILE; RLIM_INFINITY is ~0
	std::function<bool(std::uint64_t)> set_soft;      // false when refused
	std::uint64_t max_files = ~std::uint64_t(0);      // the per-process ceiling of the soft limit
	bool count_maps = false;                          // Linux: vm.max_map_count less the maps at construction
	std::uint64_t max_maps = 0, process_maps = 0;
	bool count_vnodes = false;                        // elsewhere: kern.maxvnodes
	std::uint64_t max_vnodes = 0;

	static resource_system current() {
		resource_system s;
		s.get_limits = [] {
			rlimit rl{};
			::getrlimit(RLIMIT_NOFILE, &rl);
			auto v = [](rlim_t x) { return x == RLIM_INFINITY ? ~std::uint64_t(0) : static_cast<std::uint64_t>(x); };
			return resource_limits{v(rl.rlim_cur), v(rl.rlim_max)};
		};
		s.set_soft = [](std::uint64_t n) {
			rlimit rl{};
			if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) { return false; }
			rl.rlim_cur = static_cast<rlim_t>(n);
			return ::setrlimit(RLIMIT_NOFILE, &rl) == 0;
		};
#if defined(__linux__)
		s.max_files = read_number("/proc/sys/fs/nr_open").value_or(1048576);
		if (auto m = read_number("/proc/sys/vm/max_map_count")) {
			s.count_maps = true;
			s.max_maps = *m;
			std::ifstream maps("/proc/self/maps");
			std::string line;
			while (std::getline(maps, line)) { ++s.process_maps; }
		}
#else
		int v = 0;
		std::size_t len = sizeof v;
		if (::sysctlbyname("kern.maxfilesperproc", &v, &len, nullptr, 0) == 0 && v > 0) { s.max_files = static_cast<std::uint64_t>(v); }
		len = sizeof v;
		if (::sysctlbyname("kern.maxvnodes", &v, &len, nullptr, 0) == 0 && v > 0) {
			s.count_vnodes = true;
			s.max_vnodes = static_cast<std::uint64_t>(v);
		}
#endif
		return s;
	}

private:
	static std::optional<std::uint64_t> read_number(const char* path) {
		std::ifstream in(path);
		std::uint64_t v = 0;
		if (in >> v) { return v; }
		return std::nullopt;
	}
};

struct resource_options {
	std::optional<resource_system> system;                  // default: resource_system::current()
	std::uint64_t descriptor_reserve = 0;                   // 0: the smaller of 1,024 and a quarter of the limit
	std::function<std::chrono::steady_clock::time_point()> clock;  // default: steady_clock
	std::chrono::steady_clock::duration external_quiet = std::chrono::seconds(60);
};

class resource_budget;

// What one counted call holds; returns it when it ends. Move-only.
class resource_reservation {
public:
	resource_reservation() = default;
	resource_reservation(resource_reservation&& o) noexcept { *this = std::move(o); }
	resource_reservation& operator=(resource_reservation&& o) noexcept {
		if (this != &o) {
			release();
			budget_ = std::exchange(o.budget_, nullptr);
			catalog_ = std::exchange(o.catalog_, nullptr);
			slot_ = std::exchange(o.slot_, nullptr);
			kind_ = o.kind_;
			n_ = std::exchange(o.n_, 0);
			before_ = o.before_;
		}
		return *this;
	}
	resource_reservation(const resource_reservation&) = delete;
	resource_reservation& operator=(const resource_reservation&) = delete;
	~resource_reservation() { release(); }

	inline void release() noexcept;
	explicit operator bool() const noexcept { return n_ != 0; }
	std::uint64_t held() const noexcept { return n_; }
	resource kind() const noexcept { return kind_; }
	// The budget's count just before this reservation was added.
	std::uint64_t before() const noexcept { return before_; }

private:
	friend class resource_budget;
	resource_budget* budget_ = nullptr;
	resource_counts *catalog_ = nullptr, *slot_ = nullptr;
	resource kind_ = resource::maps;
	std::uint64_t n_ = 0, before_ = 0;
};

class resource_budget {
public:
	static constexpr std::uint64_t unlimited = ~std::uint64_t(0);
	static constexpr std::uint64_t min_indexes = 64;  // a warm index holds one descriptor

	explicit resource_budget(resource_options options = {})
		: sys_(options.system ? std::move(*options.system) : resource_system::current()),
		  reserve_option_(options.descriptor_reserve),
		  clock_(options.clock ? std::move(options.clock) : [] { return std::chrono::steady_clock::now(); }),
		  quiet_(options.external_quiet) {
		for (auto& e : external_) { e.store(unlimited, std::memory_order_relaxed); }
		resource_limits lim = sys_.get_limits();
		const std::uint64_t want = std::min(lim.hard, sys_.max_files);
		if (lim.soft < want && sys_.set_soft && sys_.set_soft(want)) { lim = sys_.get_limits(); }
		derive_descriptors(lim);
		if (sys_.count_maps) {
			const std::uint64_t avail = sys_.max_maps > sys_.process_maps ? sys_.max_maps - sys_.process_maps : 0;
			set_caps(resource::maps, percent(avail, 50), percent(avail, 90));
		}
		if (sys_.count_vnodes) { set_caps(resource::vnodes, percent(sys_.max_vnodes, 40), percent(sys_.max_vnodes, 60)); }
		if (hard(resource::descriptors) < min_indexes) {
			throw std::system_error(EMFILE, std::generic_category(), "strata: the descriptor limit leaves room for fewer than 64 indexes");
		}
	}
	resource_budget(const resource_budget&) = delete;
	resource_budget& operator=(const resource_budget&) = delete;

	// Adds n to the budget's count (and the holders'), or throws too_many_open when that would
	// pass the hard cap. The count rises before the system call it covers.
	resource_reservation reserve(resource r, std::uint64_t n = 1, resource_counts* catalog = nullptr, resource_counts* slot = nullptr) {
		const unsigned k = static_cast<unsigned>(r);
		std::uint64_t cur = used_[k].load(std::memory_order_relaxed);
		for (;;) {
			const std::uint64_t h = hard_[k].load(std::memory_order_relaxed);
			if (n > h || cur > h - n) { throw too_many_open("strata: a resource cap is reached"); }
			if (used_[k].compare_exchange_weak(cur, cur + n, std::memory_order_relaxed)) { break; }
		}
		resource_reservation res;
		res.budget_ = this;
		res.catalog_ = catalog;
		res.slot_ = slot;
		res.kind_ = r;
		res.n_ = n;
		res.before_ = cur;
		if (catalog) { catalog->n[k].fetch_add(n, std::memory_order_relaxed); }
		if (slot) { slot->n[k].fetch_add(n, std::memory_order_relaxed); }
		const std::uint64_t s = soft(r);
		if (cur <= s && cur + n > s) { post(); }
		return res;
	}

	// The kernel refused the call `res` covered. EMFILE, ENFILE and ENOMEM below the cap are
	// external pressure: the soft cap drops to the count reached (not below min_indexes), and every
	// listener hears it.
	void note_failure(const resource_reservation& res, int error) noexcept {
		if (!res || (error != EMFILE && error != ENFILE && error != ENOMEM)) { return; }
		const unsigned k = static_cast<unsigned>(res.kind());
		// The stamp precedes the lowering, so a pass that sees the lowered cap sees a time not before it.
		external_at_[k].store(clock_().time_since_epoch().count(), std::memory_order_relaxed);
		const std::uint64_t target = std::max(res.before(), min_indexes);
		std::uint64_t e = external_[k].load(std::memory_order_relaxed);
		while (target < e && !external_[k].compare_exchange_weak(e, target, std::memory_order_release, std::memory_order_relaxed)) {}
		external_hits_.fetch_add(1, std::memory_order_relaxed);
		post();
	}

	// One sweep pass: re-reads the limit and re-derives the descriptor caps. A changed limit
	// clears the descriptor `external` cap; an `external` cap with no failure for the quiet
	// period rises by an eighth of its gap to the derived cap.
	void tick() {
		{
			std::lock_guard lock(mu_);
			const resource_limits lim = sys_.get_limits();
			if (lim.soft != limits_.soft || lim.hard != limits_.hard) {
				external_[static_cast<unsigned>(resource::descriptors)].store(unlimited, std::memory_order_relaxed);
			}
			derive_descriptors(lim);
			for (unsigned k = 0; k < resource_kinds; ++k) {
				std::uint64_t e = external_[k].load(std::memory_order_acquire);
				if (e == unlimited) { continue; }
				const std::chrono::steady_clock::time_point at{std::chrono::steady_clock::duration(external_at_[k].load(std::memory_order_relaxed))};
				if (clock_() - at < quiet_) { continue; }
				const std::uint64_t d = derived_soft_[k].load(std::memory_order_relaxed);
				const std::uint64_t up = e >= d ? 0 : std::max<std::uint64_t>(1, (d - e) / 8);
				// A failure that lowered the cap since the load wins: the raise is dropped.
				external_[k].compare_exchange_strong(e, e + up >= d ? unlimited : e + up, std::memory_order_acq_rel, std::memory_order_relaxed);
			}
		}
		if (over_soft()) { post(); }
	}

	// A sweep calls this: whether some resource is over its soft cap.
	bool over_soft() const noexcept {
		for (unsigned k = 0; k < resource_kinds; ++k) {
			if (used(static_cast<resource>(k)) > soft(static_cast<resource>(k))) { return true; }
		}
		return false;
	}
	std::uint64_t excess(resource r) const noexcept {
		const std::uint64_t u = used(r), s = soft(r);
		return u > s ? u - s : 0;
	}

	std::uint64_t used(resource r) const noexcept { return used_[static_cast<unsigned>(r)].load(std::memory_order_relaxed); }
	std::uint64_t hard(resource r) const noexcept { return hard_[static_cast<unsigned>(r)].load(std::memory_order_relaxed); }
	// The soft cap in force: the smaller of the derived one and the external one.
	std::uint64_t soft(resource r) const noexcept {
		const unsigned k = static_cast<unsigned>(r);
		return std::min(derived_soft_[k].load(std::memory_order_relaxed), external_[k].load(std::memory_order_relaxed));
	}
	std::uint64_t derived_soft(resource r) const noexcept { return derived_soft_[static_cast<unsigned>(r)].load(std::memory_order_relaxed); }
	// The external cap, or unlimited.
	std::uint64_t external(resource r) const noexcept { return external_[static_cast<unsigned>(r)].load(std::memory_order_relaxed); }
	std::uint64_t external_failures() const noexcept { return external_hits_.load(std::memory_order_relaxed); }
	std::uint64_t descriptor_reserve() const noexcept { return reserve_.load(std::memory_order_relaxed); }
	// The RLIMIT_NOFILE the descriptor caps came from, after the raise.
	resource_limits limits() const {
		std::lock_guard lock(mu_);
		return limits_;
	}

	// A function called, on the thread that crossed a soft cap, when the pressure changes: a
	// catalog posts a pass to its sweep. It must not call back into add_listener or remove_listener.
	std::uint64_t add_listener(std::function<void()> fn) {
		std::lock_guard lock(listeners_mu_);
		const std::uint64_t id = ++next_listener_;
		listeners_.emplace_back(id, std::make_shared<std::function<void()>>(std::move(fn)));
		return id;
	}
	void remove_listener(std::uint64_t id) {
		std::lock_guard lock(listeners_mu_);
		std::erase_if(listeners_, [&](const auto& l) { return l.first == id; });
	}

private:
	friend class resource_reservation;

	void set_caps(resource r, std::uint64_t soft_cap, std::uint64_t hard_cap) noexcept {
		derived_soft_[static_cast<unsigned>(r)].store(soft_cap, std::memory_order_relaxed);
		hard_[static_cast<unsigned>(r)].store(hard_cap, std::memory_order_relaxed);
	}
	// p percent of x, exact and without overflow for any x (RLIM_INFINITY is ~0).
	static constexpr std::uint64_t percent(std::uint64_t x, std::uint64_t p) noexcept { return x / 100 * p + x % 100 * p / 100; }
	// 75% and 95% of the limit less the reserve.
	void derive_descriptors(const resource_limits& lim) {
		limits_ = lim;
		const std::uint64_t reserve = reserve_option_ ? reserve_option_ : std::min<std::uint64_t>(1024, lim.soft / 4);
		reserve_.store(reserve, std::memory_order_relaxed);
		const std::uint64_t avail = lim.soft > reserve ? lim.soft - reserve : 0;
		set_caps(resource::descriptors, percent(avail, 75), percent(avail, 95));
	}
	// Tells every listener; one that throws, or an allocation that fails, misses this post (the
	// next crossing or pass makes it again).
	void post() noexcept {
		try {
			std::vector<std::shared_ptr<std::function<void()>>> fns;
			{
				std::lock_guard lock(listeners_mu_);
				for (const auto& l : listeners_) { fns.push_back(l.second); }
			}
			for (const auto& f : fns) {
				try { (*f)(); } catch (...) {}
			}
		} catch (...) {}
	}
	void give_back(unsigned k, std::uint64_t n) noexcept { used_[k].fetch_sub(n, std::memory_order_relaxed); }

	resource_system sys_;
	const std::uint64_t reserve_option_;
	std::function<std::chrono::steady_clock::time_point()> clock_;
	const std::chrono::steady_clock::duration quiet_;
	mutable std::mutex mu_;
	resource_limits limits_;
	std::atomic<std::uint64_t> reserve_{0};
	std::atomic<std::uint64_t> used_[resource_kinds] = {};
	std::atomic<std::uint64_t> hard_[resource_kinds] = {unlimited, unlimited, unlimited};
	std::atomic<std::uint64_t> derived_soft_[resource_kinds] = {unlimited, unlimited, unlimited};
	std::atomic<std::uint64_t> external_[resource_kinds];
	std::atomic<std::int64_t> external_at_[resource_kinds] = {};
	std::atomic<std::uint64_t> external_hits_{0};
	std::mutex listeners_mu_;
	std::vector<std::pair<std::uint64_t, std::shared_ptr<std::function<void()>>>> listeners_;
	std::uint64_t next_listener_ = 0;
};

inline void resource_reservation::release() noexcept {
	if (!n_) { return; }
	const unsigned k = static_cast<unsigned>(kind_);
	if (catalog_) { catalog_->n[k].fetch_sub(n_, std::memory_order_relaxed); }
	if (slot_) { slot_->n[k].fetch_sub(n_, std::memory_order_relaxed); }
	budget_->give_back(k, n_);
	n_ = 0;
}

}  // namespace strata
