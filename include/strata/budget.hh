// The node-wide indexing memory budget (Phase 3 design D17, D20): one capacity shared by
// every index on the node, drawn on through move-only permits that settle (return their
// bytes) when they shrink or end. It has `ledger`'s shape (a capacity, permits, explicit
// settlement, a pressure signal) so switching to `ledger`'s permit core is mechanical.
// Nothing here runs on a thread of its own: every charge and release happens on the
// caller's thread (Goal 5).
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace strata {

class memory_budget {
public:
	explicit memory_budget(std::uint64_t capacity) : capacity_(capacity) {}
	memory_budget(const memory_budget&) = delete;
	memory_budget& operator=(const memory_budget&) = delete;

	// Bytes drawn by one holder; returns them when it shrinks or is destroyed.
	class permit {
	public:
		permit() = default;
		permit(permit&& o) noexcept : budget_(std::exchange(o.budget_, nullptr)), bytes_(std::exchange(o.bytes_, 0)) {}
		permit& operator=(permit&& o) noexcept {
			if (this != &o) {
				settle();
				budget_ = std::exchange(o.budget_, nullptr);
				bytes_ = std::exchange(o.bytes_, 0);
			}
			return *this;
		}
		~permit() { settle(); }

		// Draws n more bytes; false (nothing drawn) when that would pass the capacity.
		bool try_grow(std::uint64_t n) noexcept {
			if (!budget_ || !budget_->draw(n, false)) { return false; }
			bytes_ += n;
			return true;
		}
		// Draws n more bytes past the capacity if need be (memory already allocated, such as
		// a transaction that cannot be split); pressure() reports it.
		void grow(std::uint64_t n) noexcept {
			if (!budget_) { return; }
			budget_->draw(n, true);
			bytes_ += n;
		}
		// Returns n bytes (at most what is held).
		void shrink(std::uint64_t n) noexcept {
			n = n < bytes_ ? n : bytes_;
			if (budget_) { budget_->used_.fetch_sub(n, std::memory_order_relaxed); }
			bytes_ -= n;
		}
		void settle() noexcept { shrink(bytes_); }
		std::uint64_t bytes() const noexcept { return bytes_; }
		explicit operator bool() const noexcept { return budget_ != nullptr; }

	private:
		friend class memory_budget;
		explicit permit(memory_budget* b) : budget_(b) {}
		memory_budget* budget_ = nullptr;
		std::uint64_t bytes_ = 0;
	};

	// A permit holding nothing yet.
	permit open() { return permit(this); }

	std::uint64_t capacity() const noexcept { return capacity_; }
	std::uint64_t used() const noexcept { return used_.load(std::memory_order_relaxed); }
	// Whether holders should start flushing: within `reserve` bytes of the capacity (the
	// design starts flushing below the limit by the in-flight flush size).
	bool pressure(std::uint64_t reserve = 0) const noexcept { return used() + reserve >= capacity_; }

private:
	bool draw(std::uint64_t n, bool force) noexcept {
		std::uint64_t now = used_.load(std::memory_order_relaxed);
		for (;;) {
			if (!force && (n > capacity_ || now > capacity_ - n)) { return false; }
			if (used_.compare_exchange_weak(now, now + n, std::memory_order_relaxed)) { return true; }
		}
	}
	const std::uint64_t capacity_;
	std::atomic<std::uint64_t> used_{0};
};

}  // namespace strata
