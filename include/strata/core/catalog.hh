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
// and by handle, opens single-flighted with failed opens backed off, and the word's writers named
// in D4's table. Stubs behind the interface the later items replace:
//   - the wake (item 4): run_wake() promotes under a chore_scope and calls finish_wake(); it reposts
//     at once when the lease is busy and keeps no failure bound or backoff;
//   - the sweep (item 5): begin_demote() and begin_close() are the sweep's compare-exchanges, with
//     the chores a pass runs; no timer, reclaim, commit or shutdown.
//
// The closing policy is S3-FIFO (D6, CT-R10): policy_pass() demotes hot slots over `soft_hot` and,
// over `soft_open`, closes by the policy's order. A slot enters the small FIFO, or the main one when
// its key's hash is in the ghost; the small FIFO's head moves to the main FIFO if a use set its
// frequency, else it closes and its hash enters the ghost; the main FIFO's head is decremented and
// reinserted while its frequency is set, else closed. The queues, the ghost and the slots' links
// are guarded by one mutex held for O(1) steps, taken on a miss and by the pass, never on a hit;
// a hit's only cost is the frequency its compare-exchange sets.
#pragma once

#include "strata/core/counted_io.hh"
#include "strata/core/executor.hh"
#include "strata/core/ghost_set.hh"
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
#include <utility>
#include <variant>
#include <vector>

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
	static std::uint64_t add_pin(std::uint64_t w, bool use) noexcept { return add_use(w, use) + one_pin; }
	// w with one more frequency if `use` (saturating at 3), no pin: a synchronous pin that starts a transition.
	static std::uint64_t add_use(std::uint64_t w, bool use) noexcept { return w + (use && freq(w) < 3 ? one_freq : 0); }
	static constexpr std::uint64_t freq_mask = std::uint64_t(3) << freq_shift;
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
		unsigned failures = 0;
		std::exception_ptr error;
		std::uint64_t retry_at = 0;
		std::uint32_t next_free = none;
		// The policy's queue (under policy_mu_): 0 none, 1 small, 2 main, and the links.
		std::uint32_t q_prev = none, q_next = none;
		std::uint8_t q = 0;
		std::atomic<bool> to_ghost{false};  // the pass chose this close from the small FIFO: its hash enters the ghost when it ends
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
		// The policy's caps (D6): the open slots, and the maps the hot ones hold. 0: no cap.
		std::uint64_t soft_open = 0, soft_hot = 0;
		// Tests: called just before a compare-exchange of the word, with its site (never under a lock the
		// hook's action takes). A site that runs under the shard mutex passes locked = true.
		std::function<void(const char* site, bool locked)> before_cas;
	};

	// A slot reached by a handle: re-pin by compare-exchange while `gen` matches (CT-R30).
	struct handle {
		std::uint32_t slot = ~std::uint32_t(0);
		std::uint64_t gen = 0;
		explicit operator bool() const noexcept { return slot != ~std::uint32_t(0); }
	};

	struct stats_t {
		std::uint64_t misses = 0, opens = 0, open_failures = 0, failed_fast = 0, retries = 0, stale_handles = 0, handle_pins = 0, wakes_posted = 0, wake_reposts = 0;
		// The policy: passes, small heads moved to main, main heads decremented, closes and demotions begun, candidates
		// put back (pinned, in a transition, or used since), ghost hits and additions.
		std::uint64_t policy_passes = 0, promoted = 0, decayed = 0, evictions = 0, evict_demotes = 0, busy = 0, ghost_hits = 0, ghost_adds = 0;
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
		if (h_.soft_hot) { soft_hot_.store(h_.soft_hot, std::memory_order_relaxed); }
		if (h_.soft_open) {
			soft_open_ = h_.soft_open;  // the small FIFO is a tenth of it; the ghost is the main FIFO's size
			small_cap_ = soft_open_ / 10;
			ghost_ = ghost_set(static_cast<std::size_t>(std::min<std::uint64_t>(soft_open_ - small_cap_, std::uint64_t(1) << 22)));
		}
	}
	catalog(const catalog&) = delete;
	catalog& operator=(const catalog&) = delete;
	// Every pin released and every chore finished (item 5's shutdown() will say so).
	~catalog() {
		group_->wait();
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
		const std::uint64_t maps = s.counts.get(resource::maps);
		pending_maps_.fetch_add(maps, std::memory_order_relaxed);
		post([this, idx, maps] { run_demote(idx, maps); });
		return true;
	}
	// `{0, open/owed}` to `{0, closing}` and a close chore; the chore ends at the prior state unless the
	// slot's maps count is zero and close_ready() (the sweep reads fields only after its compare-exchange).
	// `unused`: also only while the frequency is clear, decided by the same compare-exchange (the policy's).
	bool begin_close(std::uint32_t idx, bool unused = false) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_acquire);
		slot_state prior;
		for (;;) {
			prior = word::state(w);
			if (word::pins(w) != 0 || (prior != slot_state::open && prior != slot_state::owed) || (unused && word::freq(w) != 0)) { return false; }
			hook("sweep.close", false);
			if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(prior, slot_state::closing);
		post([this, idx, prior] { run_close(idx, prior); });
		return true;
	}
	// The policy's reset of a promoted slot's frequency (D6): a fetch-op on the frequency only.
	void clear_freq(std::uint32_t idx) {
		hook("policy.clear", false);
		at(idx).word.fetch_and(~word::freq_mask, std::memory_order_acq_rel);
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

	// --- the closing policy: S3-FIFO and the hot cap (D6; CT-R10, CT-R31, CT-R41) ----------------

	// What one pass may do: queue candidates examined (a move, a decrement or a requeue counts), closes
	// and demotions begun, and slots the demotion scan visits.
	struct policy_limits {
		std::uint64_t visits = 256, transitions = 16, scan = 256;
		static policy_limits lifted() noexcept { return {~std::uint64_t(0), ~std::uint64_t(0), 256}; }
	};
	struct policy_result {
		bool skipped = false;  // another pass was running
		std::uint64_t visited = 0, promoted = 0, decayed = 0, closes = 0, demotes = 0, busy = 0, scanned = 0;
	};

	// One pass: hot slots over `soft_hot` demote (the oldest last-use among the slots the scan visits),
	// then, over `soft_open`, the small and main FIFOs' heads are taken in S3-FIFO order. At most one
	// pass runs; a second caller returns at once. A pass starts transitions and returns; the chores
	// finish on the host's executor.
	policy_result policy_pass(const policy_limits& lim = {}) {
		policy_result r;
		if (policy_running_.exchange(true, std::memory_order_acquire)) {
			r.skipped = true;
			return r;
		}
		policy_passes_.fetch_add(1, std::memory_order_relaxed);
		demote_hot(lim, r);
		evict(lim, r, [this] { return queued_over_soft_open(); }, [](std::uint32_t) {});
		policy_running_.store(false, std::memory_order_release);
		return r;
	}

	struct queue_sizes_t {
		std::uint64_t small = 0, main = 0, ghost = 0;
	};
	queue_sizes_t queue_sizes() const {
		std::lock_guard l(policy_mu_);
		return {qsize_[1], qsize_[2], ghost_.size()};
	}
	// The queue a slot is in: 0 none, 1 small, 2 main.
	unsigned queue_of(std::uint32_t idx) const {
		std::lock_guard l(policy_mu_);
		return at(idx).q;
	}
	std::uint64_t small_cap() const noexcept { return small_cap_; }
	std::uint64_t pending_maps() const noexcept { return pending_maps_.load(std::memory_order_relaxed); }

	// --- observation (tests, the sweep, T4) ---------------------------------------------------

	std::uint64_t misses() const noexcept { return misses_.load(std::memory_order_relaxed); }
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
		o.policy_passes = policy_passes_.load(), o.promoted = promoted_.load(), o.decayed = decayed_.load(), o.evictions = evictions_.load();
		o.evict_demotes = evict_demotes_.load(), o.busy = busy_.load(), o.ghost_hits = ghost_hits_.load(), o.ghost_adds = ghost_adds_.load();
		for (unsigned i = 0; i < slot_states; ++i) {
			o.waited[i] = waited_[i].load();
			for (unsigned j = 0; j < slot_states; ++j) { o.edges[i][j] = edges_[i][j].load(); }
		}
		return o;
	}
	const resource_counts& counts() const noexcept { return counts_; }
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
						next = word::with_state(sync ? word::add_use(cur, r.use) : word::add_pin(cur, r.use), slot_state::waking);
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
						next = word::next_gen(word::with_state(sync ? word::add_use(cur, r.use) : word::add_pin(cur, r.use), slot_state::opening));
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


	// --- the policy's queues (under policy_mu_) ---------------------------------------------------

	static std::uint64_t key_hash64(std::string_view key) noexcept { return ::wordwise::hash(key.data(), key.size()); }
	void push_tail_locked(std::uint32_t idx, unsigned q) {
		slot& s = at(idx);
		assert(s.q == 0 && (q == 1 || q == 2));
		s.q = static_cast<std::uint8_t>(q), s.q_next = none, s.q_prev = qtail_[q];
		(qtail_[q] != none ? at(qtail_[q]).q_next : qhead_[q]) = idx;
		qtail_[q] = idx;
		++qsize_[q];
	}
	void unlink_locked(std::uint32_t idx) {
		slot& s = at(idx);
		const unsigned q = s.q;
		assert(q == 1 || q == 2);
		(s.q_prev != none ? at(s.q_prev).q_next : qhead_[q]) = s.q_next;
		(s.q_next != none ? at(s.q_next).q_prev : qtail_[q]) = s.q_prev;
		s.q = 0, s.q_prev = s.q_next = none;
		--qsize_[q];
	}
	// An open slot enters the small FIFO, or the main one when its hash is in the ghost (before its open ends).
	void policy_enqueue(std::uint32_t idx) {
		slot& s = at(idx);
		const std::uint64_t h = key_hash64(s.key);
		std::lock_guard l(policy_mu_);
		if (s.q != 0) { return; }
		const bool seen = ghost_.take(h);
		if (seen) { ghost_hits_.fetch_add(1, std::memory_order_relaxed); }
		push_tail_locked(idx, seen ? 2 : 1);
	}
	// A slot leaves the queue it is in (a close ends).
	void policy_unlink(std::uint32_t idx) {
		std::lock_guard l(policy_mu_);
		if (at(idx).q != 0) { unlink_locked(idx); }
	}
	// The ghost remembers the hash of a key the pass closed from the small FIFO, its frequency clear.
	void policy_ghost(std::uint64_t h) {
		std::lock_guard l(policy_mu_);
		ghost_.add(h);
		ghost_adds_.fetch_add(1, std::memory_order_relaxed);
	}
	// Puts a slot back at the tail of `q` unless it is in a queue or its `gen` moved on (closed or reused).
	void policy_requeue(std::uint32_t idx, std::uint64_t gen, unsigned q) {
		std::lock_guard l(policy_mu_);
		if (at(idx).q == 0 && word::gen(at(idx).word.load(std::memory_order_acquire)) == gen) { push_tail_locked(idx, q); }
	}

	bool queued_over_soft_open() const {
		std::lock_guard l(policy_mu_);
		return qsize_[1] + qsize_[2] > soft_open_;
	}
	// The next candidate: the small FIFO's head while it is over its share, else the main FIFO's, taken out
	// of its queue; false when both are empty. `round`: the queued slots, for the caller's bound.
	bool pick(std::uint32_t& idx, std::uint64_t& gen, unsigned& q, std::uint64_t& round) {
		std::lock_guard l(policy_mu_);
		round = qsize_[1] + qsize_[2];
		if (round == 0) { return false; }
		q = (qsize_[1] > small_cap_ || qsize_[2] == 0) ? 1 : 2;
		idx = qhead_[q];
		unlink_locked(idx);
		gen = word::gen(at(idx).word.load(std::memory_order_acquire));
		return true;
	}

	// Closes slots in S3-FIFO order while `over()` says more are wanted: the cap on the open slots, the descriptors'
	// or the vnodes' soft cap (item 5's pass supplies it). `before_close(idx)` runs just before a close is begun
	// (its relief is read there: a chore may release the slot's counts at once).
	template <class Over, class BeforeClose>
	void evict(const policy_limits& lim, policy_result& r, Over&& over, BeforeClose&& before_close) {
		std::uint64_t transitions = r.demotes, skipped = 0;
		for (std::uint32_t idx; r.visited < lim.visits && transitions < lim.transitions && over();) {
			std::uint64_t gen, round;
			unsigned q;
			if (!pick(idx, gen, q, round)) { break; }
			++r.visited;
			slot& s = at(idx);
			const std::uint64_t w = s.word.load(std::memory_order_acquire);
			if (word::freq(w) != 0) {  // used since it entered: small moves to main with the frequency cleared, main decrements
				if (q == 1) {
					clear_freq(idx);
					promoted_.fetch_add(1, std::memory_order_relaxed), ++r.promoted;
				} else {
					decay(idx);
					decayed_.fetch_add(1, std::memory_order_relaxed), ++r.decayed;
				}
				policy_requeue(idx, gen, 2);
				skipped = 0;
				continue;
			}
			const slot_state st = word::state(w);
			if (st == slot_state::open && word::pins(w) == 0 && s.counts.get(resource::maps) != 0) {  // hot: demote first
				if (begin_demote(idx)) {
					++transitions, ++r.demotes;
					evict_demotes_.fetch_add(1, std::memory_order_relaxed);
				}
				policy_requeue(idx, gen, q);
			} else {
				if (st == slot_state::open || st == slot_state::owed) {
					before_close(idx);
					if (q == 1) { s.to_ghost.store(true, std::memory_order_relaxed); }
					if (begin_close(idx, true)) {
						++transitions, ++r.closes;
						evictions_.fetch_add(1, std::memory_order_relaxed);
						skipped = 0;
						continue;
					}
					s.to_ghost.store(false, std::memory_order_relaxed);
				}
				busy_.fetch_add(1, std::memory_order_relaxed), ++r.busy;  // pinned, in a transition, or used since: put back
				policy_requeue(idx, gen, q);
			}
			if (++skipped > round) { break; }  // a full round with nothing to take
		}
	}

	// Hot slots over `soft_hot` (the maps they hold, less the demotions in flight) demote, the oldest last-use
	// first among the slots a scan of `lim.scan` visits from where the last one ended.
	void demote_hot(const policy_limits& lim, policy_result& r) {
		const std::uint64_t cap = soft_hot_.load(std::memory_order_relaxed);
		if (cap == ~std::uint64_t(0)) { return; }
		const std::uint64_t held = counts_.get(resource::maps), pending = pending_maps_.load(std::memory_order_relaxed);
		std::uint64_t need = held > pending + cap ? held - pending - cap : 0;
		const std::uint32_t made = slots_made();
		if (!need || !made) { return; }
		cands_.clear();
		const std::uint64_t n = std::min<std::uint64_t>(lim.scan, made);
		for (std::uint64_t i = 0; i < n; ++i) {
			const std::uint32_t idx = static_cast<std::uint32_t>((scan_at_ + i) % made);
			const std::uint64_t w = at(idx).word.load(std::memory_order_acquire);
			if (word::state(w) == slot_state::open && word::pins(w) == 0 && at(idx).counts.get(resource::maps) != 0) { cands_.push_back({at(idx).last_use.load(std::memory_order_relaxed), idx}); }
		}
		scan_at_ = static_cast<std::uint32_t>((scan_at_ + n) % made);
		r.scanned += n;
		std::sort(cands_.begin(), cands_.end());
		for (const auto& c : cands_) {
			if (!need || r.demotes >= lim.transitions) { break; }
			const std::uint64_t maps = at(c.second).counts.get(resource::maps);
			if (begin_demote(c.second)) {
				++r.demotes;
				need = need > maps ? need - maps : 0;
			}
		}
	}

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
		if (!err) { policy_enqueue(idx); }
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
				s.error = nullptr;
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

	// The wake's stub (item 4): promote() under a chore_scope; a busy lease reposts at once.
	void run_wake(std::uint32_t idx) {
		slot& s = at(idx);
		std::exception_ptr err;
		{
			const chore_scope chore{s.index->lease()};
			if (!chore) {
				reposts_.fetch_add(1, std::memory_order_relaxed);
				post([this, idx] { run_wake(idx); });
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

	// Wake ends: `{p, waking}` to `{p, open}` with every waiter handed its pin; failed, `{p - w, owed}`
	// with the `w` request waiters failed and the chore waiters handed theirs (CT-R29, CT-R40).
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
		deliver(list, gen, err);
		if (chores) { deliver(chores, gen, nullptr); }
	}

	void run_demote(std::uint32_t idx, std::uint64_t maps) {
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
		pending_maps_.fetch_sub(maps, std::memory_order_relaxed);
		end_at(idx, slot_state::demoting, owed ? slot_state::owed : slot_state::open);
	}

	void run_close(std::uint32_t idx, slot_state prior) {
		slot& s = at(idx);
		bool ready = false;
		{
			const chore_scope chore{s.index->lease()};
			if (chore) {
				try {
					ready = s.index->close_ready() && s.counts.get(resource::maps) == 0;
				} catch (...) {
				}
			}
		}
		if (!ready) {
			s.to_ghost.store(false, std::memory_order_relaxed);
			policy_requeue(idx, word::gen(s.word.load(std::memory_order_acquire)), 2);  // a refused close stays in the policy
			end_at(idx, slot_state::closing, prior);
			return;
		}
		s.index->close();
		if (s.index->closing()) { return; }  // a view kept past its pin: item 5 retries
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
		policy_unlink(idx);
		const bool ghost = s.to_ghost.exchange(false, std::memory_order_relaxed);
		std::uint64_t h64;
		{
			std::lock_guard lock(shard_of(h).mu);
			h64 = key_hash64(s.key);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == slot_state::closing);
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
		if (ghost) { policy_ghost(h64); }
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
	std::atomic<std::uint64_t> policy_passes_{0}, promoted_{0}, decayed_{0}, evictions_{0}, evict_demotes_{0}, busy_{0}, ghost_hits_{0}, ghost_adds_{0};
	// The policy: the queues (small = 1, main = 2) and the ghost under policy_mu_; the pass's own state.
	mutable std::mutex policy_mu_;
	std::uint32_t qhead_[3] = {none, none, none}, qtail_[3] = {none, none, none};
	std::uint64_t qsize_[3] = {0, 0, 0};
	ghost_set ghost_;
	std::uint64_t soft_open_ = ~std::uint64_t(0), small_cap_ = ~std::uint64_t(0);
	std::atomic<std::uint64_t> soft_hot_{~std::uint64_t(0)};
	std::atomic<std::uint64_t> pending_maps_{0};  // the maps of demotions begun and not ended
	std::atomic<bool> policy_running_{false};
	std::vector<std::pair<std::uint64_t, std::uint32_t>> cands_;
	std::uint32_t scan_at_ = 0;
	std::atomic<std::uint64_t> waited_[slot_states] = {};
	std::atomic<std::uint64_t> edges_[slot_states][slot_states] = {};
};

}  // namespace strata
