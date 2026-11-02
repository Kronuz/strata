// Hazard records (shared-reader D5, D6; Michael, IEEE TPDS 15(6), 2004): how a reader on any
// thread protects what an owner publishes through one atomic pointer, and how the owner learns
// what it may free. A registry is a list of chunks of records, each record one word on its own
// cache line, free while null. A reader claims one by a compare-exchange from null to the pointer
// it loaded, starting at a per-thread hint and loading each record before it tries one (SR-D17),
// then validates against the pointer; one store of null from any thread releases it. The owner
// stores a new pointer, retires the old, and scans the records: a retired pointer no record names
// is free.
//
// The list grows (revision 4, D5): empty until the first reader, then a chunk of `chunk_records`
// records appended whenever every record is claimed, up to the registry's capacity. A chunk is
// built whole, its first record already naming the appender's pointer, and linked by one
// compare-exchange on the last link; it is never moved, unlinked, or freed while the registry
// lives, so a record's address is stable and a scan that loads the links reaches every record.
// The link is stored and loaded sequentially consistent, which both publishes the chunk's
// contents and puts the appender's record write before the owner's scan in the total order.
//
// Correctness is the store-load pair on both sides under one total order: the reader writes its
// record then loads the pointer, the owner stores the pointer then loads the records, all
// sequentially consistent, so a reader that validated the old pointer wrote its record before
// the owner's scan read it. No fence is used, since ThreadSanitizer does not model them; the
// order is checked natively by test/hazard_litmus_test.cc, which also proves a weaker order fails.
//
// One registry per index by default (D4); a host may share one among indexes, whose lease holders
// then scan it at once from several threads. The scan's protection is its loads alone and is safe
// for any number of scanners; its diagnostics (the claim-time table, the high-water mark) have
// one writer at a time, the scanner that takes their lock without waiting (SR-D18). A record is
// written by its guard's release, so the registry outlives its guards, or, when it is destroyed
// with a record claimed, its chunks are kept (detail::keep_forever) and never freed.
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
namespace detail {

// What an owner gives up so that no reader faults: kept reachable, never freed.
inline void keep_forever(std::shared_ptr<const void> p) {
	static std::mutex mu;
	static auto* kept = new std::vector<std::shared_ptr<const void>>;
	const std::lock_guard l(mu);
	kept->push_back(std::move(p));
}

}  // namespace detail

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
	static constexpr std::memory_order link = std::memory_order_seq_cst;      // a chunk's link: the appender's store, every walk's loads
	static constexpr std::size_t chunk_records = 4;                           // records per chunk
};

// A record's own line: Apple's M-series lines are 128 bytes, 64 elsewhere.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t hazard_line = 128;
#else
inline constexpr std::size_t hazard_line = 64;
#endif

// A registry with no capacity limit: it grows with the readers.
inline constexpr std::size_t unbounded_records = ~std::size_t(0);

template <class Orders = hazard_orders>
class basic_reader_registry {
	static constexpr std::size_t K = Orders::chunk_records;
	static_assert(K >= 1);

	struct alignas(hazard_line) record {
		std::atomic<const void*> p{nullptr};
	};
	// Records [base, base + K) of the registry, and the link to the next chunk.
	struct alignas(hazard_line) chunk {
		explicit chunk(std::size_t b) noexcept : base(b) {}
		record r[K];
		std::atomic<chunk*> next{nullptr};
		const std::size_t base;
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

	// At most `records` records (at least one), allocated in chunks as readers need them: no
	// memory but this object until the first acquire.
	explicit basic_reader_registry(std::size_t records = unbounded_records) : cap_(records ? records : 1), id_(next_id()) {}
	basic_reader_registry(const basic_reader_registry&) = delete;
	basic_reader_registry& operator=(const basic_reader_registry&) = delete;
	// A record still claimed has a guard that will write it: its chunks are kept, never freed.
	~basic_reader_registry() {
		chunk* head = head_.load(std::memory_order_relaxed);
		bool held = false;
		for (chunk* c = head; c; c = c->next.load(std::memory_order_relaxed)) {
			for (const record& r : c->r) { held = held || r.p.load(std::memory_order_relaxed); }
		}
		for (chunk* c = head; c;) {
			chunk* n = c->next.load(std::memory_order_relaxed);
			if (held) {
				detail::keep_forever(std::shared_ptr<const void>(static_cast<const void*>(c), [](const void*) {}));
			} else {
				delete c;
			}
			c = n;
		}
	}

	// The most records the registry holds at once.
	std::size_t capacity() const noexcept { return cap_; }
	// The records allocated so far: a multiple of the chunk size, at most capacity() rounded up.
	std::size_t allocated() const noexcept {
		std::size_t n = 0;
		for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) { n += K; }
		return n;
	}
	static constexpr std::size_t chunk_records() noexcept { return K; }
	static constexpr std::size_t chunk_bytes() noexcept { return sizeof(chunk); }

	// Any thread: a guard on the pointer `current` holds, or an empty one when it holds null.
	// Throws too_many_readers when every record is claimed and the registry is at its capacity.
	// Never blocks: it loops only while it probes for a free record, appends a chunk another
	// reader appended first, or when the owner published between its load and validation.
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

	// An owner, after publish: calls named(p) for each record's pointer, in every chunk linked
	// when the walk reaches it. Then, when no other scan is stamping, it reads the records again to
	// stamp `now` in the claim-time table for each record naming a pointer the last stamping scan
	// did not see it name, and raises the high-water mark; when one is, it leaves both to that
	// scan and the next. Never waits. The protection pass takes no lock, so nothing but the
	// protocol's orders sits between the owner's publish and its loads (the litmus tests exactly
	// those).
	template <class F>
	void scan(std::uint64_t now, F&& named) {
		for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {
			for (const record& r : c->r) {
				if (const void* p = r.p.load(Orders::scan)) { named(p); }
			}
		}
		const std::unique_lock stamp(stamp_mu_, std::try_to_lock);
		if (!stamp) { return; }
		std::size_t claimed = 0;
		for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {
			if (seen_.size() < c->base + K) { seen_.resize(c->base + K); }
			for (std::size_t i = 0; i < K; ++i) {
				const void* p = c->r[i].p.load(std::memory_order_relaxed);
				seen& s = seen_[c->base + i];
				if (p != s.p) { s = {p, now}; }
				claimed += p != nullptr;
			}
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
		for (std::size_t i = 0; i < seen_.size(); ++i) {
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
	// For tests: record i, appending empty chunks until the one holding it exists.
	record& slot(std::size_t i) {
		for (;;) {
			chunk* tail = nullptr;
			for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {
				if (i < c->base + K) { return c->r[i - c->base]; }
				tail = c;
			}
			auto fresh = std::make_unique<chunk>(tail ? tail->base + K : 0);
			if (append(tail, fresh.get())) { fresh.release(); }
		}
	}

private:
	static std::uint64_t next_id() noexcept {
		static std::atomic<std::uint64_t> n{1};
		return n.fetch_add(1, std::memory_order_relaxed);
	}
	// Where this thread starts looking in this registry: its last claim here (the record, which
	// stays valid while this registry lives, and its index), or a spread seed. Registry ids are
	// never reused, so a hint left by a destroyed registry names nothing here.
	struct hint_t {
		std::uint64_t id = 0;
		std::size_t at = 0;
		record* rec = nullptr;
	};
	hint_t& hint() const noexcept {
		static std::atomic<std::size_t> seed{0};
		thread_local const std::size_t mine = seed.fetch_add(1, std::memory_order_relaxed);
		thread_local hint_t h;
		if (h.id != id_) { h = {id_, mine, nullptr}; }
		return h;
	}

	// Links `fresh` after `tail` (the head when null) if no one has: the chunk is published, whole.
	bool append(chunk* tail, chunk* fresh) noexcept {
		chunk* expected = nullptr;
		return (tail ? tail->next : head_).compare_exchange_strong(expected, fresh, Orders::link, std::memory_order_relaxed);
	}

	// Takes record r for v if it is free: load, then compare-exchange only a null one (SR-D17).
	static bool take(record& r, const void* v) noexcept {
		const void* expected = nullptr;
		return !r.p.load(std::memory_order_relaxed) && r.p.compare_exchange_strong(expected, v, Orders::claim, std::memory_order_relaxed);
	}

	// Claims a free record in [lo, hi) for v, or returns null; sets `at` to its index.
	record* take_in(std::size_t lo, std::size_t hi, const void* v, std::size_t& at) noexcept {
		for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {
			if (c->base + K <= lo) { continue; }
			if (c->base >= hi) { break; }
			for (std::size_t i = std::max(lo, c->base) - c->base; i < K && c->base + i < hi; ++i) {
				if (take(c->r[i], v)) {
					at = c->base + i;
					return &c->r[i];
				}
			}
		}
		return nullptr;
	}

	// Claims a free record for v: the hinted one, then every other that exists, then one in a new
	// chunk, until the capacity is reached.
	record* claim(const void* v) {
		hint_t& hint_ = hint();
		if (hint_.rec && take(*hint_.rec, v)) { return hint_.rec; }
		std::size_t at = 0;
		for (;;) {
			std::size_t total = 0;
			chunk* tail = nullptr;
			for (chunk* c = head_.load(Orders::link); c; c = c->next.load(Orders::link)) {
				total += K;
				tail = c;
			}
			const std::size_t n = std::min(total, cap_);
			if (n) {
				const std::size_t start = hint_.at % n;
				record* r = take_in(start, n, v, at);
				if (!r) { r = take_in(0, start, v, at); }
				if (r) {
					hint_.at = at;
					hint_.rec = r;
					return r;
				}
			}
			if (total >= cap_) { throw too_many_readers("strata: every hazard record is claimed"); }
			auto fresh = std::make_unique<chunk>(total);
			fresh->r[0].p.store(v, std::memory_order_relaxed);  // published by the link
			if (append(tail, fresh.get())) {
				hint_.at = total;
				hint_.rec = &fresh->r[0];
				return &fresh.release()->r[0];
			}
		}
	}

	struct seen {
		const void* p = nullptr;
		std::uint64_t since = 0;
	};
	std::atomic<chunk*> head_{nullptr};
	const std::size_t cap_;
	const std::uint64_t id_;
	mutable std::mutex stamp_mu_;  // the stamping scan's: seen_ and high_water_'s writer
	std::vector<seen> seen_;       // what each record named at the last stamping scan, and since when
	std::atomic<std::size_t> high_water_{0};
};

using reader_registry = basic_reader_registry<>;

// The owner's retired pointers, freed once no record names them (D6). After reclaim() at most one remains
// per record that names one.
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
	template <class F>
	void for_each(F&& f) const {
		for (const auto& p : list_) { f(*p); }
	}
	// Gives up every retired pointer unfreed (detail::keep_forever): for an owner that must never
	// free what a reader may hold.
	void leak() {
		for (auto& p : list_) { detail::keep_forever(std::shared_ptr<const T>(std::move(p))); }
		list_.clear();
	}

private:
	std::vector<std::unique_ptr<const T>> list_;
	std::vector<const void*> named_;  // scratch, kept between scans
};

}  // namespace strata
