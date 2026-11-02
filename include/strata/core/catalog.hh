// The catalog: the partition's non-cold indexes, their slots and pins (cold-tier D4, D5; CT-R13,
// CT-R15, CT-R29, CT-R30, CT-R31, CT-R34, CT-R35, CT-R41). Every call on a catalog index runs under
// a pin. A slot's 64-bit word `pins:24 | state:4 | freq:2 | gen:34` is the only place a pin, a state
// or a frequency is decided; every write to it is a compare-exchange or a fetch-op, so none loses a
// pin. A pin that meets a transition counts itself in the word, joins the slot's waiter list under
// the shard mutex and suspends; the transition's final compare-exchange, made under that mutex,
// hands every waiter its pin or removes it, and the waiters resume after the mutex is released, each
// posted to the scheduler it suspended on. A synchronous try_pin starts the open or the wake and
// answers would_wait, holding no pin.
//
// Built here: the table (64 shards of hash_slots over chunked slots), request and chore pins by key
// and by handle, opens single-flighted with failed opens backed off, the word's writers named in
// D4's table, and the wake (D5; CT-R23, CT-R28, CT-R33, CT-R40): a request pin that meets `owed`
// starts it, it runs promote() as a chore under chore_scope (a busy lease reposts through the host's
// timer, 1 ms doubling to 64 ms), the scope ends before the final compare-exchange that hands the
// waiters their pins, and a failed wake fails the request pins, keeps the error, backs off (10 ms
// doubling to 10 s) and counts toward the bound unless it was too_many_open. begin_bound_close() is
// the sweep's compare-exchange for a slot at the bound. Stubs behind the interface item 5 replaces:
// begin_demote() and begin_close() are the sweep's other compare-exchanges, with the chores a pass
// runs; no policy, sweep timer, reclaim or shutdown.
#pragma once

#include "strata/core/counted_io.hh"
#include "strata/core/executor.hh"
#include "strata/core/index.hh"
#include "strata/core/resource_budget.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace strata {

enum class pin_kind : unsigned { request, chore };

// A slot's state (D4). `owed` is warm with bodies owed (defer_io only).
enum class slot_state : unsigned { free = 0, opening, open, owed, waking, demoting, closing, failed };
inline constexpr unsigned slot_states = 8;

namespace catalog_detail {

// pins:24 | state:4 | freq:2 | gen:34, high to low.
struct word {
	static constexpr unsigned gen_bits = 34, freq_shift = 34, state_shift = 36, pin_shift = 40;
	static constexpr std::uint64_t gen_mask = (std::uint64_t(1) << gen_bits) - 1;
	static constexpr std::uint64_t one_pin = std::uint64_t(1) << pin_shift;
	static constexpr std::uint64_t one_freq = std::uint64_t(1) << freq_shift;
	static constexpr std::uint64_t max_pins = (std::uint64_t(1) << 24) - 1;

	static std::uint64_t pins(std::uint64_t w) noexcept { return w >> pin_shift; }
	static slot_state state(std::uint64_t w) noexcept { return static_cast<slot_state>((w >> state_shift) & 15); }
	static unsigned freq(std::uint64_t w) noexcept { return unsigned((w >> freq_shift) & 3); }
	static std::uint64_t gen(std::uint64_t w) noexcept { return w & gen_mask; }
	static std::uint64_t make(std::uint64_t pins, slot_state s, unsigned freq, std::uint64_t gen) noexcept {
		return pins << pin_shift | std::uint64_t(s) << state_shift | std::uint64_t(freq) << freq_shift | (gen & gen_mask);
	}
	// w with one more pin, and one more frequency if `use` (saturating at 3).
	static std::uint64_t add_pin(std::uint64_t w, bool use) noexcept { return w + one_pin + (use && freq(w) < 3 ? one_freq : 0); }
	static std::uint64_t with_state(std::uint64_t w, slot_state s) noexcept { return (w & ~(std::uint64_t(15) << state_shift)) | std::uint64_t(s) << state_shift; }
	static std::uint64_t with_pins(std::uint64_t w, std::uint64_t p) noexcept { return (w & ~(~std::uint64_t(0) << pin_shift)) | p << pin_shift; }
	static std::uint64_t next_gen(std::uint64_t w) noexcept { return (w & ~gen_mask) | ((w + 1) & gen_mask); }
};

// One suspended pin. `st`: 0 registered, 1 suspended, 2 delivered. A delivery before the suspension
// makes await_suspend decline, so a pin handed over early never suspends.
struct waiter {
	waiter* next = nullptr;
	std::atomic<int> st{0};
	std::coroutine_handle<> h;
	task::scheduler_ref sched;
	pin_kind kind = pin_kind::request;
	std::uint32_t idx = 0;
	std::uint64_t gen = 0;
	std::exception_ptr error;
};

inline std::uint64_t backoff_ns(unsigned failures) noexcept {  // 10 ms doubling to 10 s
	constexpr std::uint64_t base = 10'000'000, cap = 10'000'000'000;
	return std::min<std::uint64_t>(cap, base << std::min(failures ? failures - 1 : 0u, 10u));
}

}  // namespace catalog_detail

// A request pin that meets `owed` during a failed wake's backoff fails at once with this, carrying the
// wake's last error (D5). It is a system_error so a handler for the wake's own failures catches it.
class wake_failed final : public std::system_error {
public:
	explicit wake_failed(const std::exception_ptr& cause) : std::system_error(code_of(cause), "strata: the wake failed"), cause_(cause) {}
	const std::exception_ptr& cause() const noexcept { return cause_; }

private:
	static std::error_code code_of(const std::exception_ptr& e) {
		try {
			std::rethrow_exception(e);
		} catch (const std::system_error& x) {
			return x.code();
		} catch (...) {
			return std::make_error_code(std::errc::io_error);
		}
	}
	std::exception_ptr cause_;
};

// A host's key resolves to a directory it owns: a second key on a held directory meets the directory
// lock's EWOULDBLOCK, which fails that open as a host error (CT-R15).
template <class Index, class H = std::monostate>
class catalog {
public:
	using index_type = Index;
	using reader_type = typename Index::reader_handle;

private:
	using word = catalog_detail::word;
	using waiter = catalog_detail::waiter;
	enum class outcome { granted, failed, waiting, would_wait };
	struct request {
		std::string_view key;
		pin_kind kind;
		bool use;
	};
	struct acquired {
		outcome out = outcome::would_wait;
		std::uint32_t idx = 0;
		std::uint64_t gen = 0;
		std::exception_ptr error;
	};
	static constexpr std::size_t shard_count = 64, chunk_slots = 256, max_chunks = 16384;
	static constexpr std::uint32_t none = ~std::uint32_t(0);

	struct slot {
		std::atomic<std::uint64_t> word{0};
		std::atomic<std::uint64_t> last_use{0};
		// Under the shard mutex.
		waiter *wait_head = nullptr, *wait_tail = nullptr;
		std::string key;
		std::uint32_t key_hash = 0;
		// Declared before the IO and the index, so it goes after them: their reservations return into it.
		resource_counts counts;
		// Written by a transition before its final compare-exchange; read after a pin's.
		std::unique_ptr<kronuz::journal::IO> io;
		std::optional<Index> index;
		reader_type reader;
		std::optional<H> payload;
		// A failed open's or a failed wake's count (the bound counts the wake's, not too_many_open), error and
		// retry time. `failures` and `probe_at` are read by the sweep without the mutex, the rest under it.
		std::atomic<unsigned> failures{0};
		unsigned streak = 0;  // consecutive failed wakes of any kind: the backoff's exponent
		std::exception_ptr error;
		std::uint64_t retry_at = 0;
		std::atomic<std::uint64_t> probe_at{0};  // the sweep's next try at the bound after a failed commit
		std::atomic<bool> commit_failed{false};
		std::uint32_t next_free = none;
	};
	struct shard {
		mutable std::mutex mu;
		index_detail::hash_slots table;
	};


public:

	// What the host supplies (Q2). `resolve` returns the key's IO (a counted_io over the budget in
	// io_counts) or throws; `open_index` opens it; `make_payload` makes H. Chores run on `exec`.
	struct host {
		executor* exec = nullptr;
		resource_budget* budget = nullptr;
		std::function<std::unique_ptr<kronuz::journal::IO>(std::string_view key, io_counts)> resolve;
		std::function<Index(kronuz::journal::IO&, const typename Index::options&)> open_index = [](kronuz::journal::IO& io, const typename Index::options& o) { return Index::open(io, o); };
		std::function<H(std::string_view key)> make_payload = [](std::string_view) { return H{}; };
		typename Index::options index_options;
		// Tests: called just before a compare-exchange of the word, with its site (never under a lock the
		// hook's action takes). A site that runs under the shard mutex passes locked = true.
		std::function<void(const char* site, bool locked)> before_cas;
		// Calls `fn` once, `delay_ns` from now, on any thread; `fn` only posts a chore. Empty: a chore that
		// sleeps on the executor (a worker waits; a host supplies a timer).
		std::function<void(std::uint64_t delay_ns, std::function<void()> fn)> timer;
		// The commit chore the bound close runs when something is uncommitted (D4); a failure ends the
		// close. Default: index.commit().
		std::function<void(H&, Index&)> commit = [](H&, Index& ix) { ix.commit(); };
		// Failed wakes after which the sweep closes the slot (D5; I5-N2); too_many_open does not count.
		unsigned wake_bound = 8;
	};

	// A slot reached by a handle: re-pin by compare-exchange while `gen` matches (CT-R30).
	struct handle {
		std::uint32_t slot = ~std::uint32_t(0);
		std::uint64_t gen = 0;
		explicit operator bool() const noexcept { return slot != ~std::uint32_t(0); }
	};

	struct stats_t {
		std::uint64_t misses = 0, opens = 0, open_failures = 0, failed_fast = 0, retries = 0, stale_handles = 0, handle_pins = 0, wakes_posted = 0, wake_reposts = 0;
		std::uint64_t wake_failures = 0, wake_overloads = 0, wake_gated = 0, bound_closes = 0, bound_commits = 0, commit_failures = 0;
		std::uint64_t waited[slot_states] = {};            // pins that met each state and waited
		std::uint64_t edges[slot_states][slot_states] = {};  // state changes made, from -> to
	};

	class pin {
	public:
		pin() = default;
		pin(pin&& o) noexcept : c_(std::exchange(o.c_, nullptr)), s_(o.s_), idx_(o.idx_), gen_(o.gen_) {}
		pin& operator=(pin&& o) noexcept {
			if (this != &o) {
				release();
				c_ = std::exchange(o.c_, nullptr), s_ = o.s_, idx_ = o.idx_, gen_ = o.gen_;
			}
			return *this;
		}
		pin(const pin&) = delete;
		pin& operator=(const pin&) = delete;
		~pin() { release(); }

		// Valid for the pin's life only; a host keeps neither past it (I7).
		Index& index() const noexcept { return *s_->index; }
		H& payload() const noexcept { return *s_->payload; }
		const reader_type& reader() const noexcept { return s_->reader; }
		handle where() const noexcept { return {idx_, gen_}; }
		explicit operator bool() const noexcept { return c_ != nullptr; }
		void release() noexcept {
			if (c_) { std::exchange(c_, nullptr)->unpin(*s_); }
		}

	private:
		friend class catalog;
		pin(catalog* c, std::uint32_t idx, std::uint64_t gen) noexcept : c_(c), s_(&c->at(idx)), idx_(idx), gen_(gen) {}
		catalog* c_ = nullptr;
		slot* s_ = nullptr;
		std::uint32_t idx_ = 0;
		std::uint64_t gen_ = 0;
	};

	enum class try_status { pinned, would_wait, failed };
	struct try_result {
		try_status status = try_status::would_wait;
		pin p;
		std::exception_ptr error;
	};

	// co_await catalog.pin(...): the pin, or the open's or the wake's error.
	class pin_awaiter {
	public:
		bool await_ready() {
			const acquired a = c_->acquire_impl(req_, hint_, &w_);
			r_ = a.out;
			if (r_ == outcome::waiting) { return false; }
			w_.idx = a.idx, w_.gen = a.gen, w_.error = a.error;
			return true;
		}
		bool await_suspend(std::coroutine_handle<> h) noexcept {
			w_.h = h;
			int expect = 0;
			return w_.st.compare_exchange_strong(expect, 1, std::memory_order_acq_rel, std::memory_order_acquire);
		}
		pin await_resume() {
			if (w_.error) { std::rethrow_exception(w_.error); }
			return pin(c_, w_.idx, w_.gen);
		}

	private:
		friend class catalog;
		pin_awaiter(catalog& c, std::string_view key, pin_kind kind, bool use, handle hint) : c_(&c), req_{key, kind, use}, hint_(hint) {}
		catalog* c_;
		request req_;
		handle hint_;
		catalog_detail::waiter w_;
		outcome r_ = outcome::waiting;
	};

	explicit catalog(host h) : h_(std::move(h)), group_(h_.exec->open()) {
		for (auto& c : chunks_) { c.store(nullptr, std::memory_order_relaxed); }
	}
	catalog(const catalog&) = delete;
	catalog& operator=(const catalog&) = delete;
	// Every pin released and every chore finished (item 5's shutdown() will say so).
	~catalog() {
		for (;;) {  // a timer's callback posts before it is counted done, and a chore may arm another
			for (unsigned n; (n = timers_.load(std::memory_order_acquire)) != 0;) { timers_.wait(n, std::memory_order_acquire); }
			group_->wait();
			if (timers_.load(std::memory_order_acquire) == 0) { break; }
		}
		for (auto& c : chunks_) { delete[] c.load(std::memory_order_relaxed); }
	}

	// A pin by key. `use`: one per request, by key (CT-R31); a re-pin within the request is not.
	[[nodiscard]] pin_awaiter pin_request(std::string_view key, bool use = true) { return pin_awaiter(*this, key, pin_kind::request, use, {}); }
	[[nodiscard]] pin_awaiter pin_chore(std::string_view key, bool use = false) { return pin_awaiter(*this, key, pin_kind::chore, use, {}); }
	// A re-pin by handle: one compare-exchange while the slot is open and `gen` matches, else by key.
	[[nodiscard]] pin_awaiter repin(handle hint, std::string_view key, pin_kind kind = pin_kind::request, bool use = false) { return pin_awaiter(*this, key, kind, use, hint); }
	// Starts the open or the wake as a pin would and answers would_wait, holding no pin (CT-R35).
	try_result try_pin(std::string_view key, pin_kind kind = pin_kind::request, bool use = true) {
		const acquired a = acquire_impl({key, kind, use}, {}, nullptr);
		try_result r;
		switch (a.out) {
		case outcome::granted: r.status = try_status::pinned, r.p = pin(this, a.idx, a.gen); break;
		case outcome::failed: r.status = try_status::failed, r.error = a.error; break;
		default: r.status = try_status::would_wait; break;
		}
		return r;
	}

	// --- the sweep's compare-exchanges (item 5 replaces the chores; the word's rows are final) ------

	// `{0, open}` to `{0, demoting}` and a demotion chore. False when a pin or a transition holds it.
	bool begin_demote(std::uint32_t idx) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_acquire);
		for (;;) {
			if (word::pins(w) != 0 || word::state(w) != slot_state::open) { return false; }
			hook("sweep.demote", false);
			if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::demoting), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(slot_state::open, slot_state::demoting);
		post([this, idx] { run_demote(idx); });
		return true;
	}
	// `{0, open/owed}` to `{0, closing}` and a close chore; the chore ends at the prior state unless the
	// slot's maps count is zero and close_ready() (the sweep reads fields only after its compare-exchange).
	bool begin_close(std::uint32_t idx) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_acquire);
		slot_state prior;
		for (;;) {
			prior = word::state(w);
			if (word::pins(w) != 0 || (prior != slot_state::open && prior != slot_state::owed)) { return false; }
			hook("sweep.close", false);
			if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(prior, slot_state::closing);
		post([this, idx, prior] { run_close(idx, prior); });
		return true;
	}
	// `{0, owed}` to `{0, closing}` for a slot whose wake failed `wake_bound` times (D5; the sweep's call,
	// after the probe time of a failed commit), and a close chore that commits first if anything is
	// uncommitted. False when a pin or a transition holds it or the count is below the bound. The chore
	// ends at `owed` when the commit fails (the probe time is set, failing() counts it) or the slot is
	// not ready after it; the sweep reads the count only as a hint, the chore checks it after the exchange.
	bool begin_bound_close(std::uint32_t idx) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_acquire);
		for (;;) {
			if (word::pins(w) != 0 || word::state(w) != slot_state::owed) { return false; }
			if (s.failures.load(std::memory_order_relaxed) < h_.wake_bound || exec_now() < s.probe_at.load(std::memory_order_relaxed)) { return false; }
			hook("sweep.bound", false);
			if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(slot_state::owed, slot_state::closing);
		post([this, idx] { run_close(idx, slot_state::owed, true); });
		return true;
	}
	// The policy's decrement (D6): the frequency down by one, no lower than zero.
	void decay(std::uint32_t idx) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_relaxed);
		while (word::freq(w) > 0) {
			hook("policy.decay", false);
			if (s.word.compare_exchange_weak(w, w - word::one_freq, std::memory_order_acq_rel, std::memory_order_relaxed)) { return; }
		}
	}

	// --- observation (tests, the sweep, T4) ---------------------------------------------------

	std::uint32_t slots_made() const noexcept {
		std::lock_guard l(free_mu_);
		return next_new_;
	}
	std::uint64_t word_of(std::uint32_t idx) const noexcept { return at(idx).word.load(std::memory_order_acquire); }
	static std::uint64_t pins_of(std::uint64_t w) noexcept { return word::pins(w); }
	static slot_state state_of(std::uint64_t w) noexcept { return word::state(w); }
	static unsigned freq_of(std::uint64_t w) noexcept { return word::freq(w); }
	static std::uint64_t gen_of(std::uint64_t w) noexcept { return word::gen(w); }
	std::uint64_t last_use_of(std::uint32_t idx) const noexcept { return at(idx).last_use.load(std::memory_order_relaxed); }
	std::size_t waiters_of(std::uint32_t idx) const {  // under the shard mutex
		const slot& s = at(idx);
		std::lock_guard l(shard_of(s.key_hash).mu);
		std::size_t n = 0;
		for (const auto* w = s.wait_head; w; w = w->next) { ++n; }
		return n;
	}
	std::string key_at(std::uint32_t idx) const {  // under the shard mutex; empty for a free slot
		const slot& s = at(idx);
		std::lock_guard l(shard_of(s.key_hash).mu);
		return s.key;
	}
	std::size_t table_size() const {
		std::size_t n = 0;
		for (const auto& sh : shards_) {
			std::lock_guard l(sh.mu);
			n += sh.table.size();
		}
		return n;
	}
	stats_t stats() const noexcept {
		stats_t o;
		o.misses = misses_.load(), o.opens = opens_.load(), o.open_failures = open_failures_.load(), o.failed_fast = failed_fast_.load();
		o.retries = retries_.load(), o.stale_handles = stale_.load(), o.handle_pins = handle_pins_.load(), o.wakes_posted = wakes_.load(), o.wake_reposts = reposts_.load();
		o.wake_failures = wake_failures_.load(), o.wake_overloads = wake_overloads_.load(), o.wake_gated = wake_gated_.load();
		o.bound_closes = bound_closes_.load(), o.bound_commits = bound_commits_.load(), o.commit_failures = commit_failures_.load();
		for (unsigned i = 0; i < slot_states; ++i) {
			o.waited[i] = waited_[i].load();
			for (unsigned j = 0; j < slot_states; ++j) { o.edges[i][j] = edges_[i][j].load(); }
		}
		return o;
	}
	const resource_counts& counts() const noexcept { return counts_; }
	// Slots whose bound close failed to commit and have not closed or committed since (D5).
	std::size_t failing() const noexcept { return failing_.load(std::memory_order_relaxed); }
	unsigned wake_failures_of(std::uint32_t idx) const noexcept { return at(idx).failures.load(std::memory_order_relaxed); }
	std::uint64_t probe_at_of(std::uint32_t idx) const noexcept { return at(idx).probe_at.load(std::memory_order_relaxed); }
	static constexpr std::uint64_t repost_first_ns = 1'000'000, repost_cap_ns = 64'000'000;  // D5 (estimates)
	static constexpr std::uint64_t commit_probe_ns = 10'000'000'000ull;      // D5
	// A slot's index, for a test that holds the slot in a state; null unless built.
	Index* index_of(std::uint32_t idx) noexcept { return at(idx).index ? &*at(idx).index : nullptr; }
	// Lets the posted chores of a finished test run to their end.
	void wait_chores() { group_->wait(); }

private:
	friend class pin;
	friend class pin_awaiter;

	slot& at(std::uint32_t i) const noexcept { return chunks_[i / chunk_slots].load(std::memory_order_acquire)[i % chunk_slots]; }
	shard& shard_of(std::uint32_t h) const noexcept { return const_cast<shard&>(shards_[h & (shard_count - 1)]); }
	void hook(const char* site, bool locked) const {
		if (h_.before_cas) { h_.before_cas(site, locked); }
	}
	void note(slot_state from, slot_state to) noexcept { edges_[unsigned(from)][unsigned(to)].fetch_add(1, std::memory_order_relaxed); }
	void post(std::function<void()> f) {
		group_->post([f = std::move(f)](const task::cancel_token&) { f(); });
	}

	std::uint32_t alloc_slot() {
		std::lock_guard l(free_mu_);
		if (free_head_ != none) {
			const std::uint32_t i = free_head_;
			free_head_ = at(i).next_free;
			return i;
		}
		const std::uint32_t i = next_new_;
		if (i / chunk_slots >= max_chunks) { throw too_many_open("strata: the catalog has no slot left"); }
		if (i % chunk_slots == 0) { chunks_[i / chunk_slots].store(new slot[chunk_slots], std::memory_order_release); }
		++next_new_;
		return i;
	}
	void free_slot(std::uint32_t i) {
		std::lock_guard l(free_mu_);
		at(i).next_free = free_head_;
		free_head_ = i;
	}

	void append(slot& s, waiter* w) {
		w->next = nullptr;
		(s.wait_tail ? s.wait_tail->next : s.wait_head) = w;
		s.wait_tail = w;
	}
	waiter* take(slot& s) noexcept {
		waiter* h = std::exchange(s.wait_head, nullptr);
		s.wait_tail = nullptr;
		return h;
	}

	// Registers the request as a waiter on the slot it counted itself on (under the shard mutex).
	void join(slot& s, waiter* w, const request& r, std::uint32_t idx, slot_state met) {
		w->kind = r.kind;
		w->idx = idx;
		w->sched = task::current();
		append(s, w);
		waited_[unsigned(met)].fetch_add(1, std::memory_order_relaxed);
	}

	// Delivers a taken list outside the mutex: a pin (error null) or the error. The next link is read
	// first, as a delivered waiter may be resumed and gone.
	static void deliver(waiter* list, std::uint64_t gen, const std::exception_ptr& error) noexcept {
		for (waiter* w = list; w;) {
			waiter* next = w->next;
			w->gen = gen;
			w->error = error;
			if (w->st.exchange(2, std::memory_order_acq_rel) == 1) {
				const std::coroutine_handle<> h = w->h;
				const task::scheduler_ref sched = w->sched;
				if (sched) {
					sched.post(h);
				} else {
					h.resume();
				}
			}
			w = next;
		}
	}

	acquired acquire_impl(const request& r, handle hint, waiter* w) {
		acquired a;
		if (hint && try_fast(hint, r, a)) { return a; }
		const std::uint32_t h = index_detail::key_hash(r.key);
		shard& sh = shard_of(h);
		const bool sync = w == nullptr;
		bool post_open = false, post_wake = false;
		std::uint32_t idx = none;
		{
			std::unique_lock lock(sh.mu);
			const auto eq = [&](std::uint32_t v) { return at(v).key == r.key; };
			if (const std::uint32_t* f = sh.table.find(h, eq)) {
				idx = *f;
				slot& s = at(idx);
				std::uint64_t cur = s.word.load(std::memory_order_acquire);
				for (;;) {
					const slot_state st = word::state(cur);
					assert(word::pins(cur) < word::max_pins);
					std::uint64_t next = 0;
					outcome out = outcome::granted;
					bool joins = false;
					switch (st) {
					case slot_state::open:
						next = word::add_pin(cur, r.use);
						break;
					case slot_state::owed:
						if (r.kind == pin_kind::chore) {
							next = word::add_pin(cur, r.use);
							break;
						}
						if (s.streak != 0 && exec_now() < s.retry_at) {  // a failed wake's backoff: no wake, no pin
							failed_fast_.fetch_add(1, std::memory_order_relaxed);
							wake_gated_.fetch_add(1, std::memory_order_relaxed);
							return {outcome::failed, idx, 0, s.error};
						}
						next = word::with_state(sync ? cur : word::add_pin(cur, r.use), slot_state::waking);
						out = sync ? outcome::would_wait : outcome::waiting;
						joins = !sync;
						post_wake = true;
						break;
					case slot_state::waking:
						if (r.kind == pin_kind::chore && s.index && s.index->lease().held_here()) {
							next = word::add_pin(cur, r.use);
							break;
						}
						[[fallthrough]];
					case slot_state::opening:
					case slot_state::demoting:
					case slot_state::closing:
						if (sync) {
							waited_[unsigned(st)].fetch_add(1, std::memory_order_relaxed);
							return {outcome::would_wait, idx, 0, nullptr};
						}
						next = word::add_pin(cur, r.use);
						out = outcome::waiting;
						joins = true;
						break;
					case slot_state::failed:
						if (exec_now() < s.retry_at) {
							failed_fast_.fetch_add(1, std::memory_order_relaxed);
							return {outcome::failed, idx, 0, s.error};
						}
						next = word::next_gen(word::with_state(sync ? cur : word::add_pin(cur, r.use), slot_state::opening));
						out = sync ? outcome::would_wait : outcome::waiting;
						joins = !sync;
						post_open = true;
						retries_.fetch_add(1, std::memory_order_relaxed);
						break;
					case slot_state::free:
						assert(false && "a slot in the table is not free");
						return {outcome::failed, idx, 0, nullptr};
					}
					hook("pin.key", true);
					if (!s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
						post_open = post_wake = false;
						continue;
					}
					if (st != word::state(next)) { note(st, word::state(next)); }
					a.out = out;
					a.idx = idx;
					a.gen = word::gen(next);
					if (joins) { join(s, w, r, idx, st); }
					break;
				}
			} else {
				idx = alloc_slot();
				slot& s = at(idx);
				try {
					s.key = std::string(r.key);
					s.key_hash = h;
					sh.table.insert(h, idx);
				} catch (...) {
					s.key.clear();
					free_slot(idx);
					throw;
				}
				std::uint64_t cur = s.word.load(std::memory_order_acquire);
				for (;;) {  // free to opening; the miss's own pin is not a use
					assert(word::state(cur) == slot_state::free && word::pins(cur) == 0);
					const std::uint64_t next = word::next_gen(word::make(sync ? 0 : 1, slot_state::opening, 0, word::gen(cur)));
					hook("pin.miss", true);
					if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
						a.gen = word::gen(next);
						break;
					}
				}
				note(slot_state::free, slot_state::opening);
				a.idx = idx;
				a.out = sync ? outcome::would_wait : outcome::waiting;
				if (!sync) { join(s, w, r, idx, slot_state::opening); }
				misses_.fetch_add(1, std::memory_order_relaxed);
				post_open = true;
			}
		}
		if (post_open) { post([this, idx] { run_open(idx); }); }
		if (post_wake) { post_wake_chore(idx); }
		return a;
	}

	// The by-handle fast path: one compare-exchange while open and `gen` matches (no mutex).
	bool try_fast(handle hint, const request& r, acquired& a) {
		slot& s = at(hint.slot);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		for (;;) {
			if (word::gen(cur) != hint.gen) {
				stale_.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (word::state(cur) != slot_state::open) { return false; }
			hook("pin.handle", false);
			if (s.word.compare_exchange_weak(cur, word::add_pin(cur, r.use), std::memory_order_acq_rel, std::memory_order_acquire)) {
				handle_pins_.fetch_add(1, std::memory_order_relaxed);
				a = {outcome::granted, hint.slot, hint.gen, nullptr};
				return true;
			}
		}
	}

	void unpin(slot& s) noexcept {
		s.last_use.store(exec_now(), std::memory_order_relaxed);
		hook("unpin", false);
		[[maybe_unused]] const std::uint64_t before = s.word.fetch_sub(word::one_pin, std::memory_order_release);
		assert(word::pins(before) > 0);
	}
	std::uint64_t exec_now() const { return h_.exec->now(); }

	// --- transitions ---------------------------------------------------------------------------

	void run_open(std::uint32_t idx) {
		slot& s = at(idx);
		std::exception_ptr err;
		try {
			s.io = h_.resolve(s.key, io_counts{h_.budget, &counts_, &s.counts});
			s.index.emplace(h_.open_index(*s.io, h_.index_options));
			s.reader = s.index->reader();
			s.payload.emplace(h_.make_payload(s.key));
		} catch (...) {
			err = std::current_exception();
			s.payload.reset();
			s.reader = {};
			s.index.reset();
			s.io.reset();
		}
		finish_open(idx, err);
	}

	// Open ends: `{p, opening}` to `{p, open}` handing every waiter its pin, or `{0, failed}`, `gen` + 1,
	// with the waiters failed and their pins removed in the same compare-exchange (CT-R29, CT-R34).
	void finish_open(std::uint32_t idx, const std::exception_ptr& err) {
		slot& s = at(idx);
		waiter* list;
		std::uint64_t gen;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == slot_state::opening);
			if (err) {
				++s.failures;
				s.error = err;
				s.retry_at = exec_now() + catalog_detail::backoff_ns(s.failures);
			} else {
				s.failures = 0;
				s.streak = 0;
				s.error = nullptr;
				s.retry_at = 0;
				s.probe_at.store(0, std::memory_order_relaxed);
				clear_failing(s);
			}
			for (;;) {
				const std::uint64_t next = err ? word::next_gen(word::with_pins(word::with_state(cur, slot_state::failed), 0)) : word::with_state(cur, slot_state::open);
				hook("open.end", true);
				if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
					gen = word::gen(next);
					break;
				}
			}
			note(slot_state::opening, err ? slot_state::failed : slot_state::open);
			(err ? open_failures_ : opens_).fetch_add(1, std::memory_order_relaxed);
			list = take(s);
		}
		hook("open.unlocked", false);
		deliver(list, gen, err);
	}

	// A state the transition ends at, handing the waiters their pins, or `waking` when the end is
	// `owed` and a request waiter needs the wake (it waits for the wake's end, as does a chore waiter).
	void end_at(std::uint32_t idx, slot_state from, slot_state target) {
		slot& s = at(idx);
		waiter* list = nullptr;
		std::uint64_t gen;
		bool wake = false;
		slot_state to = target;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == from);
			bool request_waiter = false;
			for (const waiter* w = s.wait_head; w; w = w->next) { request_waiter |= w->kind == pin_kind::request; }
			wake = target == slot_state::owed && request_waiter;
			to = wake ? slot_state::waking : target;
			for (;;) {
				hook("transition.end", true);
				if (s.word.compare_exchange_weak(cur, word::with_state(cur, to), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
			}
			gen = word::gen(cur);
			note(from, to);
			if (!wake) { list = take(s); }
		}
		if (wake) {
			post_wake_chore(idx);
		} else {
			deliver(list, gen, nullptr);
		}
	}

	void post_wake_chore(std::uint32_t idx) {
		wakes_.fetch_add(1, std::memory_order_relaxed);
		post([this, idx] { run_wake(idx); });
	}

	// Runs `fn` as a chore after `delay_ns`: through the host's timer, which only posts it, or, without
	// one, a chore that sleeps on the executor. A timer is counted until its callback has posted.
	void after(std::uint64_t delay_ns, std::function<void()> fn) {
		if (h_.timer) {
			timers_.fetch_add(1, std::memory_order_acq_rel);
			h_.timer(delay_ns, [this, fn = std::move(fn)]() mutable {
				post(std::move(fn));
				if (timers_.fetch_sub(1, std::memory_order_acq_rel) == 1) { timers_.notify_all(); }
			});
			return;
		}
		group_->post([this, delay_ns, fn = std::move(fn)](const task::cancel_token& stop) {
			if (h_.exec->sleep_until(exec_now() + delay_ns, stop)) { fn(); }
		});
	}

	// The wake (D5): promote() under chore_scope, never waiting. A busy lease (a chore pin's caller holds
	// it) reposts through the timer, 1 ms doubling to 64 ms, counted and outside the failure bound. The
	// scope ends before the final compare-exchange, so a resumed request acquires from free (CT-R40).
	void run_wake(std::uint32_t idx, std::uint64_t delay = repost_first_ns) {
		slot& s = at(idx);
		std::exception_ptr err;
		{
			const chore_scope chore{s.index->lease()};
			if (!chore) {
				reposts_.fetch_add(1, std::memory_order_relaxed);
				after(delay, [this, idx, next = std::min(delay * 2, repost_cap_ns)] { run_wake(idx, next); });
				return;
			}
			try {
				s.index->promote();
			} catch (...) {
				err = std::current_exception();
			}
		}
		finish_wake(idx, err);
	}

	static bool overload(const std::exception_ptr& e) {  // the budget's hard cap: a refusal, not a fault
		try {
			std::rethrow_exception(e);
		} catch (const too_many_open&) {
			return true;
		} catch (...) {
			return false;
		}
	}

	// Wake ends: `{p, waking}` to `{p, open}` with every waiter handed its pin; failed, `{p - w, owed}`
	// with the `w` request waiters failed and the chore waiters handed theirs (CT-R29, CT-R40). The
	// failure's fields are written before the exchange, under the mutex, so the next request pin that
	// meets `owed` sees the backoff; a success resets the count and the backoff.
	void finish_wake(std::uint32_t idx, const std::exception_ptr& err) {
		slot& s = at(idx);
		waiter *list, *chores = nullptr;
		std::uint64_t gen;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == slot_state::waking);
			list = take(s);
			std::uint64_t w = 0;
			if (err) {  // split the list: request waiters fail, chore waiters keep their pins
				waiter *req = nullptr, *tail = nullptr;
				for (waiter* x = list; x;) {
					waiter* next = x->next;
					if (x->kind == pin_kind::request) {
						++w;
						x->next = req, req = x;
					} else {
						x->next = nullptr;
						(tail ? tail->next : chores) = x;
						tail = x;
					}
					x = next;
				}
				list = req;
				if (overload(err)) {
					wake_overloads_.fetch_add(1, std::memory_order_relaxed);
				} else {
					s.failures.store(s.failures.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
					wake_failures_.fetch_add(1, std::memory_order_relaxed);
				}
				++s.streak;
				s.error = std::make_exception_ptr(wake_failed(err));
				s.retry_at = exec_now() + catalog_detail::backoff_ns(s.streak);
			} else {
				s.failures.store(0, std::memory_order_relaxed);
				s.streak = 0;
				s.error = nullptr;
				s.retry_at = 0;
			}
			for (;;) {
				hook("wake.end", true);
				const std::uint64_t next = err ? word::with_pins(word::with_state(cur, slot_state::owed), word::pins(cur) - w) : word::with_state(cur, slot_state::open);
				if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
					gen = word::gen(next);
					break;
				}
			}
			note(slot_state::waking, err ? slot_state::owed : slot_state::open);
		}
		hook("wake.unlocked", false);
		deliver(list, gen, err);
		if (chores) { deliver(chores, gen, nullptr); }
	}

	void run_demote(std::uint32_t idx) {
		slot& s = at(idx);
		bool owed = false;
		{
			const chore_scope chore{s.index->lease()};
			if (chore) {
				try {
					owed = s.index->demote() && s.index->promote_owed();
				} catch (...) {
				}
			}
		}
		end_at(idx, slot_state::demoting, owed ? slot_state::owed : slot_state::open);
	}

	void clear_failing(slot& s) noexcept {
		if (s.commit_failed.exchange(false, std::memory_order_relaxed)) { failing_.fetch_sub(1, std::memory_order_relaxed); }
	}

	// The close chore. At the wake's bound (`bound`) it first commits what is uncommitted, with the host's
	// commit chore, and closes only after that; a failed commit ends at `owed` with a probe time.
	void run_close(std::uint32_t idx, slot_state prior, bool bound = false) {
		slot& s = at(idx);
		bool ready = false, commit_failed = false;
		{
			const chore_scope chore{s.index->lease()};
			if (chore) {
				try {
					// The exchange can follow a reset and a demotion, so the bound is checked again.
					if (!bound || s.failures.load(std::memory_order_relaxed) >= h_.wake_bound) {
						ready = s.index->close_ready() && s.counts.get(resource::maps) == 0;
						if (!ready && bound && s.index->uncommitted()) {
							bound_commits_.fetch_add(1, std::memory_order_relaxed);
							try {
								h_.commit(*s.payload, *s.index);
							} catch (...) {
								commit_failed = true;
							}
							if (!commit_failed) {  // the commit's own maps go with a demotion's publication
								(void)s.index->demote();
								ready = s.index->close_ready() && s.counts.get(resource::maps) == 0;
							}
						}
					}
				} catch (...) {
				}
			}
		}
		if (commit_failed) {
			commit_failures_.fetch_add(1, std::memory_order_relaxed);
			s.probe_at.store(exec_now() + commit_probe_ns, std::memory_order_relaxed);
			if (!s.commit_failed.exchange(true, std::memory_order_relaxed)) { failing_.fetch_add(1, std::memory_order_relaxed); }
		}
		if (!ready) {
			end_at(idx, slot_state::closing, prior);
			return;
		}
		s.index->close();
		if (s.index->closing()) { return; }  // a view kept past its pin: item 5 retries
		if (bound) { bound_closes_.fetch_add(1, std::memory_order_relaxed); }
		clear_failing(s);
		s.payload.reset();
		s.reader = {};
		s.index.reset();
		s.io.reset();
		finish_close(idx);
	}

	// Close ends: `{0, closing}` to free, `gen` + 1, erased under the mutex; with pinners, `{p, opening}`,
	// `gen` + 1: reopened in place (CT-R30).
	void finish_close(std::uint32_t idx) {
		slot& s = at(idx);
		const std::uint32_t h = s.key_hash;
		bool reopen = false;
		{
			std::lock_guard lock(shard_of(h).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == slot_state::closing);
			s.failures.store(0, std::memory_order_relaxed);
			s.streak = 0;
			s.error = nullptr;
			s.retry_at = 0;
			s.probe_at.store(0, std::memory_order_relaxed);
			for (;;) {
				hook("close.end", true);
				reopen = word::pins(cur) != 0;
				const std::uint64_t next = reopen ? word::next_gen(word::with_state(cur, slot_state::opening)) : word::next_gen(word::make(0, slot_state::free, 0, word::gen(cur)));
				if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
			}
			note(slot_state::closing, reopen ? slot_state::opening : slot_state::free);
			if (!reopen) {
				const auto eq = [&](std::uint32_t v) { return v == idx; };
				shard_of(h).table.erase(h, eq);
				s.key.clear();
			}
		}
		if (reopen) {
			post([this, idx] { run_open(idx); });
		} else {
			free_slot(idx);
		}
	}

	host h_;
	std::unique_ptr<executor::group> group_;
	resource_counts counts_;
	std::array<shard, shard_count> shards_;
	std::array<std::atomic<slot*>, max_chunks> chunks_;
	mutable std::mutex free_mu_;
	std::uint32_t free_head_ = none, next_new_ = 0;
	std::atomic<std::uint64_t> misses_{0}, opens_{0}, open_failures_{0}, failed_fast_{0}, retries_{0}, stale_{0}, handle_pins_{0}, wakes_{0}, reposts_{0};
	std::atomic<std::uint64_t> wake_failures_{0}, wake_overloads_{0}, wake_gated_{0}, bound_closes_{0}, bound_commits_{0}, commit_failures_{0};
	std::atomic<unsigned> timers_{0};  // host timers armed and not yet fired
	std::atomic<std::size_t> failing_{0};
	std::atomic<std::uint64_t> waited_[slot_states] = {};
	std::atomic<std::uint64_t> edges_[slot_states][slot_states] = {};
};

}  // namespace strata
