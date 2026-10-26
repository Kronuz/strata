// Who may change an index's owner-only state (shared-reader D3, D4; SR-D5, SR-D19, SR-D20).
// The owner is the write lease's holder, on whatever thread holds it. The lease is index-level:
// every owner duty (a writer call, publication, the registry scan and frees, close, destruction)
// runs inside a lease_scope, which debug builds check against the lease's holder token, set on
// acquire and cleared on handoff. Step 1 ships the token only: the host serializes writer calls
// as it does today (SR-D20), and a second holder while one holds it is a host bug, reported here.
//
// The leaf changes (live-docs shares, segment drops) call owner_only(): debug builds require a
// lease held on this thread. Every build also keeps a second check: background work (a pool's
// worker, marked by off_owner_scope) holds no lease, so owner state changing there is counted.
// Background work that takes a lease, a host's request run on the pool, is owner work.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

#if !defined(STRATA_LEASE_CHECKS)
#if defined(NDEBUG)
#define STRATA_LEASE_CHECKS 0
#else
#define STRATA_LEASE_CHECKS 1
#endif
#endif

namespace strata {
namespace detail {

inline constexpr bool lease_checks = STRATA_LEASE_CHECKS != 0;

struct owner_thread_state {
	std::uint32_t leases = 0;  // lease scopes this thread is inside
	bool background = false;   // inside background work (off_owner_scope)
};
inline owner_thread_state& owner_thread() noexcept {
	thread_local owner_thread_state s;
	return s;
}
inline bool off_owner_flag() noexcept { return owner_thread().background; }

// Owner-only state changed off the lease, and other ownership breaks, since the process started.
inline std::atomic<std::uint64_t>& off_owner_changes() noexcept {
	static std::atomic<std::uint64_t> n{0};
	return n;
}
// Tests that provoke a break clear this to count it instead of aborting.
inline std::atomic<bool>& abort_on_owner_break() noexcept {
	static std::atomic<bool> on{true};
	return on;
}
inline void owner_break([[maybe_unused]] const char* what) noexcept {
	off_owner_changes().fetch_add(1, std::memory_order_relaxed);
	assert(!abort_on_owner_break().load(std::memory_order_relaxed) && what);
}

// Called where owner-only state changes.
inline void owner_only() noexcept {
	const owner_thread_state& t = owner_thread();
	if (t.leases == 0 && (lease_checks || t.background)) { owner_break("strata: owner-only state changed outside the write lease"); }
}

// This thread's holder token: nonzero, unique per thread.
inline std::uint64_t lease_token() noexcept {
	static std::atomic<std::uint64_t> next{1};
	thread_local const std::uint64_t token = next.fetch_add(1, std::memory_order_relaxed);
	return token;
}

}  // namespace detail

// An index's write lease (D4): the holder token in debug builds, nothing in others. A moved
// lease starts free, as the moving holder still holds the old one until its scope ends.
class write_lease {
public:
	write_lease() noexcept = default;
	write_lease(write_lease&& o) noexcept {
		if constexpr (detail::lease_checks) {
			const std::uint64_t h = o.holder_.load(std::memory_order_acquire);
			if (h && h != detail::lease_token()) { detail::owner_break("strata: an index moved while another thread holds its lease"); }
		}
		(void)o;
	}
	write_lease& operator=(write_lease&&) = delete;
	write_lease(const write_lease&) = delete;

	// Whether this thread holds it (always true without lease checks).
	bool held() const noexcept {
		if constexpr (detail::lease_checks) { return holder_.load(std::memory_order_acquire) == detail::lease_token(); }
		return true;
	}

private:
	friend class lease_scope;
#if STRATA_LEASE_CHECKS
	mutable std::atomic<std::uint64_t> holder_{0};
#endif
};

// Holds a lease for an owner duty. Nested scopes on the same lease and thread are one hold; the
// outermost acquires (an acquire compare-exchange from free) and hands off (a release store of free).
class lease_scope {
public:
	explicit lease_scope(const write_lease& l) noexcept : l_(&l) {
		if constexpr (detail::lease_checks) {
			const std::uint64_t me = detail::lease_token();
			std::uint64_t h = 0;
			if (l.holder_.compare_exchange_strong(h, me, std::memory_order_acquire, std::memory_order_acquire)) {
				outer_ = true;
			} else if (h != me) {
				detail::owner_break("strata: a writer call while another thread holds the index's write lease");
			}
		}
		++detail::owner_thread().leases;
	}
	~lease_scope() {
		--detail::owner_thread().leases;
		if constexpr (detail::lease_checks) {
			if (outer_) { l_->holder_.store(0, std::memory_order_release); }
		}
	}
	lease_scope(const lease_scope&) = delete;
	lease_scope& operator=(const lease_scope&) = delete;

private:
	[[maybe_unused]] const write_lease* l_;
	[[maybe_unused]] bool outer_ = false;
};

// Marks the calling thread as background work, which holds no lease, while the scope lives.
class off_owner_scope {
public:
	off_owner_scope() noexcept : prev_(std::exchange(detail::owner_thread().background, true)) {}
	~off_owner_scope() { detail::owner_thread().background = prev_; }
	off_owner_scope(const off_owner_scope&) = delete;
	off_owner_scope& operator=(const off_owner_scope&) = delete;

private:
	bool prev_;
};

}  // namespace strata
