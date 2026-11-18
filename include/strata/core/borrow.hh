// Borrowing from a container without copying (flush memory). A writer that keeps views into a
// container's elements until it finishes holds a lease on the container; while any lease lives,
// every mutator of `borrowable_vector` throws, so the views cannot dangle or see a change. The
// rule is the type's, not a convention: the container is the only owner of its elements, its
// mutators all go through the gate, and a lease is how a borrower proves what it borrows from.
// The gate holds the index to its one-writer contract (a second thread's mutation is refused
// after a lease, not synchronized with it): it is a tripwire for that contract, not a lock.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace strata {

class borrow_gate {
public:
	// A pin on the gate over the address range [lo, hi): move-only, released when it ends.
	class lease {
	public:
		lease() = default;
		lease(lease&& o) noexcept : gate_(std::exchange(o.gate_, nullptr)), lo_(o.lo_), hi_(o.hi_), spans_(std::move(o.spans_)) {}
		lease& operator=(lease&& o) noexcept {
			if (this != &o) {
				release();
				gate_ = std::exchange(o.gate_, nullptr);
				lo_ = o.lo_;
				hi_ = o.hi_;
				spans_ = std::move(o.spans_);
			}
			return *this;
		}
		~lease() { release(); }
		explicit operator bool() const noexcept { return gate_ != nullptr; }
		// Whether `p` lies in the leased range: what a borrower checks before it keeps a view.
		bool owns(const void* p) const noexcept {
			if (!gate_) { return false; }
			if (!spans_) { return !std::less<const void*>{}(p, lo_) && std::less<const void*>{}(p, hi_); }
			// The span starting at or before p, by start address.
			const auto it = std::upper_bound(spans_->begin(), spans_->end(), p, [](const void* q, const span& s) { return std::less<const void*>{}(q, s.first); });
			return it != spans_->begin() && std::less<const void*>{}(p, std::prev(it)->second);
		}
		using span = std::pair<const void*, const void*>;
		using spans = std::vector<span>;

	private:
		friend class borrow_gate;
		lease(borrow_gate* g, const void* lo, const void* hi) noexcept : gate_(g), lo_(lo), hi_(hi) {}
		lease(borrow_gate* g, std::shared_ptr<const spans> s) noexcept : gate_(g), spans_(std::move(s)) {}
		void release() noexcept {
			if (gate_) { gate_->pins_.fetch_sub(1, std::memory_order_release); }
			gate_ = nullptr;
		}
		borrow_gate* gate_ = nullptr;
		const void* lo_ = nullptr;
		const void* hi_ = nullptr;
		std::shared_ptr<const spans> spans_;  // several ranges, sorted by start, when not one
	};

	borrow_gate() = default;
	// A gate moves with its container, and only while no lease points at it: a pinned move
	// would leave a lease on the old address, so it ends the process, as a dangling borrow would.
	borrow_gate(borrow_gate&& o) noexcept { o.require_idle(); }
	borrow_gate& operator=(borrow_gate&& o) noexcept {
		require_idle();
		o.require_idle();
		return *this;
	}

	lease pin(const void* lo, const void* hi) noexcept {
		pins_.fetch_add(1, std::memory_order_acq_rel);
		return lease(this, lo, hi);
	}
	// A pin over several disjoint ranges, sorted by start address (a chunked container's chunks).
	lease pin(std::shared_ptr<const lease::spans> spans) noexcept {
		pins_.fetch_add(1, std::memory_order_acq_rel);
		return lease(this, std::move(spans));
	}
	bool pinned() const noexcept { return pins_.load(std::memory_order_acquire) != 0; }
	// Every mutator of the guarded container calls this first.
	void require_unpinned() const {
		if (pinned()) { throw std::logic_error("strata: a buffer changed while a writer borrows from it"); }
	}

private:
	void require_idle() const noexcept {
		if (pinned()) { std::terminate(); }
	}
	std::atomic<std::uint32_t> pins_{0};
};

// A vector whose elements can be leased: reads are free, and each mutation (also a non-const
// access, which can move or change an element) requires that no lease is held.
template <class T>
class borrowable_vector {
public:
	using value_type = T;
	using const_iterator = typename std::vector<T>::const_iterator;

	borrowable_vector() = default;
	borrowable_vector(borrowable_vector&&) = default;
	borrowable_vector& operator=(borrowable_vector&&) = default;

	bool empty() const noexcept { return v_.empty(); }
	std::size_t size() const noexcept { return v_.size(); }
	const_iterator begin() const noexcept { return v_.begin(); }
	const_iterator end() const noexcept { return v_.end(); }
	const T& operator[](std::size_t i) const { return v_[i]; }
	const T& back() const { return v_.back(); }

	T& at_mut(std::size_t i) {
		gate_.require_unpinned();
		return v_[i];
	}
	T& back_mut() {
		gate_.require_unpinned();
		return v_.back();
	}
	void push_back(T&& x) {
		gate_.require_unpinned();
		v_.push_back(std::move(x));
	}
	void push_back(const T& x) {
		gate_.require_unpinned();
		v_.push_back(x);
	}
	void pop_back() {
		gate_.require_unpinned();
		v_.pop_back();
	}
	void clear() {
		gate_.require_unpinned();
		v_.clear();
	}

	// Throws while a lease is held: for callers that change the container in several steps.
	void require_unpinned() const { gate_.require_unpinned(); }

	// A lease on the elements as they are now; the container is immutable until it ends.
	borrow_gate::lease pin() noexcept { return gate_.pin(v_.data(), v_.data() + v_.size()); }
	bool pinned() const noexcept { return gate_.pinned(); }

private:
	std::vector<T> v_;
	borrow_gate gate_;
};

}  // namespace strata
