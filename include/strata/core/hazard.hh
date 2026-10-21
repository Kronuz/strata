// Hazard records (shared-reader D5, D6; Michael, IEEE TPDS 15(6), 2004): how a reader on any
// thread protects what an owner publishes through one atomic pointer, and how the owner learns
// what it may free. A registry holds H records, each one word on its own cache line, free while
// null. A reader claims one by a compare-exchange from null to the pointer it loaded, starting
// at a per-thread hint and loading each record before it tries one (SR-D17), then validates
// against the pointer; one store of null from any thread releases it. The owner stores a new
// pointer, retires the old, and scans the records: a retired pointer no record names is free.
//
// Correctness is the store-load pair on both sides under one total order: the reader writes its
// record then loads the pointer, the owner stores the pointer then loads the records, all
// sequentially consistent, so a reader that validated the old pointer wrote its record before
// the owner's scan read it. No fence is used, since ThreadSanitizer does not model them; the
// order is checked natively by test/hazard_litmus_test.cc, which also proves a weaker order fails.
//
// One registry per pool partition (D4, SR-D8): the partition's indexes share it, so their lease
// holders may scan it at once from several threads. The scan's protection is its loads alone and
// is safe for any number of scanners; its diagnostics (the claim-time table, the high-water mark)
// have one writer at a time, the scanner that takes their lock without waiting (SR-D18).
// The registry outlives its guards and the indexes that scan it.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace strata {

// Every record is claimed: the overload error a host sheds (ARCHITECTURE, Concurrency), never a wait.
struct too_many_readers : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// The protocol's memory orders. Tests substitute weaker ones to show the litmus catches them.
struct hazard_orders {
	static constexpr std::memory_order claim = std::memory_order_seq_cst;     // a reader's record write: claim and retry
	static constexpr std::memory_order validate = std::memory_order_seq_cst;  // a reader's loads of the pointer
	static constexpr std::memory_order publish = std::memory_order_seq_cst;   // the owner's store of the pointer
	static constexpr std::memory_order scan = std::memory_order_seq_cst;      // the owner's loads of the records
};

// A record's own line: Apple's M-series lines are 128 bytes, 64 elsewhere.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t hazard_line = 128;
#else
inline constexpr std::size_t hazard_line = 64;
#endif

template <class Orders = hazard_orders>
class basic_reader_registry {
	struct alignas(hazard_line) record {
		std::atomic<const void*> p{nullptr};
	};

public:
	// What a reader holds: a claimed record naming `get()`. Movable, released by the holder on any
	// thread, at destruction or release(). Empty when the pointer was null.
	template <class T>
	class guard {
	public:
		guard() noexcept = default;
		guard(guard&& o) noexcept : r_(std::exchange(o.r_, nullptr)), v_(std::exchange(o.v_, nullptr)) {}
		guard& operator=(guard&& o) noexcept {
			if (this != &o) {
				release();
				r_ = std::exchange(o.r_, nullptr);
				v_ = std::exchange(o.v_, nullptr);
			}
			return *this;
		}
		guard(const guard&) = delete;
		guard& operator=(const guard&) = delete;
		~guard() { release(); }

		const T* get() const noexcept { return v_; }
		const T* operator->() const noexcept { return v_; }
		const T& operator*() const noexcept { return *v_; }
		explicit operator bool() const noexcept { return v_ != nullptr; }
		void release() noexcept {
			if (r_) { r_->p.store(nullptr, std::memory_order_release); }
			r_ = nullptr;
			v_ = nullptr;
		}

	private:
		friend class basic_reader_registry;
		guard(record* r, const T* v) noexcept : r_(r), v_(v) {}
		record* r_ = nullptr;
		const T* v_ = nullptr;
	};

	explicit basic_reader_registry(std::size_t records) : recs_(new record[records ? records : 1]), h_(records ? records : 1), seen_(h_) {}
	basic_reader_registry(const basic_reader_registry&) = delete;
	basic_reader_registry& operator=(const basic_reader_registry&) = delete;

	std::size_t capacity() const noexcept { return h_; }

	// Any thread: a guard on the pointer `current` holds, or an empty one when it holds null.
	// Throws too_many_readers when every record is claimed. Never blocks: it loops only while
	// it probes for a free record, or when the owner published between its load and validation.
	template <class T>
	guard<T> acquire(const std::atomic<const T*>& current) {
		const T* v = current.load(Orders::validate);
		if (!v) { return {}; }
		record* r = claim(v);
		v = settle(*r, v, current);
		if (!v) {
			r->p.store(nullptr, std::memory_order_release);
			return {};
		}
		return {r, v};
	}

	// The owner: makes `next` the pointer readers acquire. The caller retires the old one.
	template <class T>
	static void publish(std::atomic<const T*>& current, const T* next) noexcept {
		current.store(next, Orders::publish);
	}

	// An owner, after publish: calls named(p) for each record's pointer. Then, when no other scan
	// is stamping, it reads the records again to stamp `now` in the claim-time table for each
	// record naming a pointer the last stamping scan did not see it name, and raises the
	// high-water mark; when one is, it leaves both to that scan and the next. Never waits. The
	// protection pass takes no lock, so nothing but the protocol's orders sits between the
	// owner's publish and its loads (the litmus tests exactly those).
	template <class F>
	void scan(std::uint64_t now, F&& named) {
		for (std::size_t i = 0; i < h_; ++i) {
			if (const void* p = recs_[i].p.load(Orders::scan)) { named(p); }
		}
		const std::unique_lock stamp(stamp_mu_, std::try_to_lock);
		if (!stamp) { return; }
		std::size_t claimed = 0;
		for (std::size_t i = 0; i < h_; ++i) {
			const void* p = recs_[i].p.load(std::memory_order_relaxed);
			if (p != seen_[i].p) { seen_[i] = {p, now}; }
			claimed += p != nullptr;
		}
		if (claimed > high_water_.load(std::memory_order_relaxed)) { high_water_.store(claimed, std::memory_order_relaxed); }
	}

	// Diagnostics (D6, SR-D7), exact to the last stamping scan. claims() waits for a scan stamping
	// at that moment, at most one pass over the records.
	struct claim_time {
		std::size_t record;
		const void* p;
		std::uint64_t since;  // the `now` of the first scan that saw the record naming p
	};
	std::vector<claim_time> claims() const {
		std::lock_guard stamp(stamp_mu_);
		std::vector<claim_time> out;
		for (std::size_t i = 0; i < h_; ++i) {
			if (seen_[i].p) { out.push_back({i, seen_[i].p, seen_[i].since}); }
		}
		return out;
	}
	std::size_t high_water() const noexcept { return high_water_.load(std::memory_order_relaxed); }

	// The protocol's steps, for the litmus test: the reader's record write and validation loop.
	// Stores the newer pointer into the record it already owns until the pointer stops changing;
	// returns the validated pointer, or null when the owner stored null.
	template <class T>
	static const T* settle(record& r, const T* v, const std::atomic<const T*>& current) noexcept {
		for (;;) {
			const T* w = current.load(Orders::validate);
			if (w == v || !w) { return w; }
			r.p.store(w, Orders::claim);  // still ours: it never held null
			v = w;
		}
	}
	record& slot(std::size_t i) noexcept { return recs_[i]; }

private:
	static std::size_t& hint() noexcept {
		static std::atomic<std::size_t> next{0};
		thread_local std::size_t h = next.fetch_add(1, std::memory_order_relaxed);
		return h;
	}

	// Claims a free record for v: loads first and compare-exchanges only a null one, so a probe
	// writes no line another reader holds (SR-D17).
	record* claim(const void* v) {
		std::size_t& hint_ = hint();
		std::size_t i = hint_ % h_;
		for (std::size_t probed = 0; probed < h_; ++probed) {
			record& r = recs_[i];
			const void* expected = nullptr;
			if (!r.p.load(std::memory_order_relaxed) && r.p.compare_exchange_strong(expected, v, Orders::claim, std::memory_order_relaxed)) {
				hint_ = i;
				return &r;
			}
			if (++i == h_) { i = 0; }
		}
		throw too_many_readers("strata: every hazard record is claimed");
	}

	struct seen {
		const void* p = nullptr;
		std::uint64_t since = 0;
	};
	std::unique_ptr<record[]> recs_;
	std::size_t h_;
	mutable std::mutex stamp_mu_;  // the stamping scan's: seen_ and high_water_'s writer
	std::vector<seen> seen_;       // what each record named at the last stamping scan, and since when
	std::atomic<std::size_t> high_water_{0};
};

using reader_registry = basic_reader_registry<>;

// The owner's retired pointers, freed once no record names them (D6). After reclaim() at most
// the registry's capacity remain, one per record that names one.
template <class T, class Orders = hazard_orders>
class basic_retired {
public:
	void retire(std::unique_ptr<const T> p) { list_.push_back(std::move(p)); }
	// Scans the registry and frees every retired pointer no record names; returns how many remain.
	std::size_t reclaim(basic_reader_registry<Orders>& reg, std::uint64_t now) {
		if (list_.empty()) { return 0; }
		named_.clear();
		reg.scan(now, [&](const void* p) { named_.push_back(p); });
		std::sort(named_.begin(), named_.end());
		std::erase_if(list_, [&](const std::unique_ptr<const T>& p) { return !std::binary_search(named_.begin(), named_.end(), static_cast<const void*>(p.get())); });
		return list_.size();
	}
	std::size_t size() const noexcept { return list_.size(); }
	bool empty() const noexcept { return list_.empty(); }

private:
	std::vector<std::unique_ptr<const T>> list_;
	std::vector<const void*> named_;  // scratch, kept between scans
};

}  // namespace strata
