// The node's write-rate limit for background I/O (Phase 3 design D11, Scale): one token
// bucket in bytes per second, shared by every index's merges, and later by install
// transfers and reclamation. It is clocked by the executor it waits on, so under
// inline_executor it is deterministic; one limiter serves one executor's clock.
#pragma once

#include "strata/core/executor.hh"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace strata {

class rate_limiter {
public:
	explicit rate_limiter(std::uint64_t bytes_per_second) : rate_(bytes_per_second) {}
	rate_limiter(const rate_limiter&) = delete;
	rate_limiter& operator=(const rate_limiter&) = delete;

	// Admits `bytes` of writes at the rate across every caller, waiting on `exec` for its
	// turn; false, with its reservation refunded, once `stop` is requested.
	bool admit(executor& exec, std::uint64_t bytes, const task::cancel_token& stop) {
		if (stop.requested()) { return false; }
		if (rate_) {
			const std::uint64_t cost = std::uint64_t(double(bytes) * 1e9 / double(rate_));
			std::uint64_t until;
			{
				std::lock_guard l(mu_);
				next_ = std::max(next_, exec.now()) + cost;
				until = next_;
			}
			if (!exec.sleep_until(until, stop)) {
				std::lock_guard l(mu_);
				next_ -= std::min(next_, cost);
				return false;
			}
		}
		admitted_.fetch_add(bytes, std::memory_order_relaxed);
		return true;
	}
	// The bytes a caller writes per admit(): a tenth of a second's worth, 4 KiB to 1 MiB.
	std::size_t chunk() const noexcept { return rate_ ? std::size_t(std::clamp<std::uint64_t>(rate_ / 10, 4096, 1 << 20)) : std::size_t(1) << 20; }
	std::uint64_t bytes_per_second() const noexcept { return rate_; }
	std::uint64_t admitted() const noexcept { return admitted_.load(std::memory_order_relaxed); }

private:
	const std::uint64_t rate_;  // 0: unlimited
	std::mutex mu_;
	std::uint64_t next_ = 0;  // when the bucket has admitted every reservation so far
	std::atomic<std::uint64_t> admitted_{0};
};

}  // namespace strata
