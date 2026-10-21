// Which threads may change an index's owner-only state (shared-reader D3, D4, SR-D5). Live-docs
// shares and segment lifetimes change on the owner thread alone; a thread that never owns an
// index (an executor's worker while it runs work, a host's reader thread) is marked, so a
// debug build asserts that none of that state changes there, and every build counts it.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

namespace strata {
namespace detail {

inline bool& off_owner_flag() noexcept {
	thread_local bool off = false;
	return off;
}

// Owner-only state changed on a marked thread, since the process started.
inline std::atomic<std::uint64_t>& off_owner_changes() noexcept {
	static std::atomic<std::uint64_t> n{0};
	return n;
}

// Called where owner-only state changes: counts a change on a marked thread, and asserts.
inline void owner_only() noexcept {
	if (off_owner_flag()) {
		off_owner_changes().fetch_add(1, std::memory_order_relaxed);
		assert(false && "strata: owner-only state changed off the owner thread");
	}
}

}  // namespace detail

// Marks the calling thread as one that never owns an index while the scope lives.
class off_owner_scope {
public:
	off_owner_scope() noexcept : prev_(std::exchange(detail::off_owner_flag(), true)) {}
	~off_owner_scope() { detail::off_owner_flag() = prev_; }
	off_owner_scope(const off_owner_scope&) = delete;
	off_owner_scope& operator=(const off_owner_scope&) = delete;

private:
	bool prev_;
};

}  // namespace strata
