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
// required timer, 1 ms doubling to 64 ms), the scope ends before the final compare-exchange that hands the
// waiters their pins, and a failed wake fails the request pins, keeps the error, backs off (10 ms
// doubling to 10 s) and counts toward the bound unless it was too_many_open. begin_bound_close() is
// the sweep's compare-exchange for a slot at the bound.
//
// The sweep and shutdown (D5, D6; CT-R4, CT-R6, CT-R36): a pass, run as a chore on the host's timer
// (1 Hz) and posted, coalesced, by a soft-cap crossing, visits a bounded slice of slots and touches no
// file. It erases negative entries past their time, takes `{0, open}` to `{0, demoting}` for an idle
// or over-cap hot slot, `{0, open/owed}` to `{0, closing}` for an idle warm one (the slot's maps count
// zero and close_ready() decide in the chore), starts the bound close, and retries a close that left
// closing() true. shutdown() commits and closes every slot. H is destroyed by a chore of its own, after
// the close chore has returned.
//
// The closing policy is S3-FIFO (D6, CT-R10): over `soft_open` (or the descriptors' or vnodes' soft
// cap) the pass closes by the policy's order. A slot enters the small FIFO, or the main one when its
// key's hash is in the ghost; the small FIFO's head moves to the main FIFO if a use set its
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
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace strata {

enum class pin_kind : unsigned { request, chore };

// What a pin wants of an absent or closed index (F1). `open`: the index as it is. `create`: also pass a
// missing index's negative entry and make the index. `resident`: only an index already in the catalog,
// never opening one; a chore pin's (the autocommit and FLUSH). A resident pin that waits on an `opening`, `waking`
// or `demoting` slot takes that transition's outcome: when the open fails it gets the open's error, not an empty pin.
enum class open_intent : unsigned char { open, create, resident };

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
	// A pin past this would carry out of the field and wrap the count to zero.
	static bool full(std::uint64_t w) noexcept { return pins(w) == max_pins; }
	static slot_state state(std::uint64_t w) noexcept { return static_cast<slot_state>((w >> state_shift) & 15); }
	static unsigned freq(std::uint64_t w) noexcept { return unsigned((w >> freq_shift) & 3); }
	static std::uint64_t gen(std::uint64_t w) noexcept { return w & gen_mask; }
	static std::uint64_t make(std::uint64_t pins, slot_state s, unsigned freq, std::uint64_t gen) noexcept {
		return pins << pin_shift | std::uint64_t(s) << state_shift | std::uint64_t(freq) << freq_shift | (gen & gen_mask);
	}
	// w with one more pin, and one more frequency if `use` (saturating at 3); the caller has checked !full(w).
	static std::uint64_t add_pin(std::uint64_t w, bool use) noexcept { return add_use(w, use) + one_pin; }
	// w with one more frequency if `use` (saturating at 3), no pin: a synchronous pin that starts a transition.
	static std::uint64_t add_use(std::uint64_t w, bool use) noexcept { return w + (use && freq(w) < 3 ? one_freq : 0); }
	static constexpr std::uint64_t freq_mask = std::uint64_t(3) << freq_shift;
	static std::uint64_t with_state(std::uint64_t w, slot_state s) noexcept { return (w & ~(std::uint64_t(15) << state_shift)) | std::uint64_t(s) << state_shift; }
	static std::uint64_t with_pins(std::uint64_t w, std::uint64_t p) noexcept { return (w & ~(~std::uint64_t(0) << pin_shift)) | p << pin_shift; }
	static std::uint64_t next_gen(std::uint64_t w) noexcept { return (w & ~gen_mask) | ((w + 1) & gen_mask); }
};

// One waiting pin or shutdown, on the heap (task's lock_waiter shape): its list and its awaiter each hold
// a reference, so a delivery never writes into a frame destroyed while it waited. Exactly one of the
// delivery and the awaiter's destruction sets `st` last: a delivery before the suspension makes
// await_suspend decline, and one that meets `abandoned` releases the pin it carries.
struct waiter {
	enum : int { registered, suspended, delivered, abandoned };
	waiter *next = nullptr, *prev = nullptr;
	std::atomic<int> st{registered};
	std::atomic<int> refs{1};  // the awaiter's; joining a list adds the list's
	bool linked = false;       // on its list; under *mu
	bool pins = false;         // a pin waiter: a delivery without an error hands it a counted pin
	std::mutex* mu = nullptr;  // the mutex of the list it joined; null before it joins and once ~catalog orphans it
	std::coroutine_handle<> h;
	task::scheduler_ref sched;
	pin_kind kind = pin_kind::request;
	std::uint32_t idx = 0;
	std::uint64_t gen = 0;
	std::exception_ptr error;

	void retain() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
	void release() noexcept {
		if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) { delete this; }
	}
};

// A FIFO of waiters under the mutex of its owner (a shard, or shutdown's). Circular and doubly linked, so one
// pointer in the slot finds both ends: `head->prev` is the tail.
struct wait_list {
	waiter* head = nullptr;
	void push(waiter* w) noexcept {
		if (head) {
			w->next = head;
			w->prev = head->prev;
			head->prev->next = w;
			head->prev = w;
		} else {
			head = w->next = w->prev = w;
		}
		w->linked = true;
	}
	void unlink(waiter* w) noexcept {
		if (w->next == w) {
			head = nullptr;
		} else {
			w->prev->next = w->next;
			w->next->prev = w->prev;
			if (head == w) { head = w->next; }
		}
		w->next = w->prev = nullptr;
		w->linked = false;
	}
	// The whole list in order and null-terminated, for a delivery outside the mutex: from here on no awaiter can
	// unlink one of them.
	waiter* take() noexcept {
		waiter* h = std::exchange(head, nullptr);
		if (!h) { return nullptr; }
		h->prev->next = nullptr;
		for (waiter* x = h; x; x = x->next) { x->linked = false; }
		return h;
	}
	template <class F>
	void each(F f) const {
		for (const waiter* x = head; x;) {
			f(*x);
			x = x->next == head ? nullptr : x->next;
		}
	}
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

// A pin after shutdown() began.
class catalog_closed final : public std::runtime_error {
public:
	catalog_closed() : std::runtime_error("strata: the catalog is shut down") {}
};

// What shutdown() reports: the keys whose commit failed, closed uncommitted (a host with a WAL replays them).
struct shutdown_result {
	std::vector<std::string> uncommitted;
};

// Why a slot is not yet free (catalog::shutdown_report): what a host logs before it abandons an unfinished
// shutdown by exiting (D5).
enum class shutdown_hold : std::uint8_t {
	pinned,        // a pin is held
	transition,    // an open, wake, demotion or close is running
	lease_busy,    // the shutdown close met the index's lease held, and retries
	close_throws,  // close() threw, and is retried 64 ms apart until it does not
	view_held,     // close() left a view open past its pin (a host bug), retried until it goes
	waiting,       // nothing holds it: its close has not begun yet
};
struct shutdown_holder {
	std::string key;
	std::uint8_t state = 0;  // a slot_state
	std::uint64_t pins = 0;
	shutdown_hold why = shutdown_hold::waiting;
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
	// What a waiting pin's token runs (task's lock_cancel shape).
	struct cancel_pin {
		catalog* c;
		waiter* w;
		void operator()() const noexcept {
			if (w->mu) { c->cancel_wait(w); }  // null: ~catalog orphaned it, and c is gone
		}
	};
	enum class outcome { granted, failed, waiting, would_wait, none };
	struct request {
		std::string_view key;
		pin_kind kind;
		bool use;
		open_intent intent = open_intent::open;
	};
	struct acquired {
		outcome out = outcome::would_wait;
		std::uint32_t idx = 0;
		std::uint64_t gen = 0;
		std::exception_ptr error;
	};
	static constexpr std::size_t shard_count = 64, chunk_slots = 256, max_chunks = 16384;
	static constexpr std::uint32_t none = ~std::uint32_t(0);

	// Members run from the widest to the narrowest, so that padding is none: a slot is 184 B with libc++'s string
	// (D4's 192 B on libstdc++'s), plus the host's payload.
	struct slot {
		std::atomic<std::uint64_t> word{0};
		// The use time.
		std::atomic<std::uint64_t> last_use{0};
		// Under the shard mutex.
		catalog_detail::wait_list waiters;
		std::string key;
		// Declared before the IO and the index, so it goes after them: their reservations return into it.
		resource_counts counts;
		// Written by a transition before its final compare-exchange; read after a pin's.
		std::unique_ptr<kronuz::journal::IO> io;
		std::unique_ptr<Index> index;  // allocated by the open, freed by the close: a cold or free slot holds none
		reader_type reader;
		std::optional<H> payload;
		// A failed open's or a failed wake's error and retry time; `probe_at` is read by the sweep without the mutex,
		// the rest under it.
		std::exception_ptr error;
		std::uint64_t retry_at = 0;
		std::atomic<std::uint64_t> probe_at{0};  // the sweep's next try: after a failed commit at the bound, or a refusal
		std::atomic<std::uint32_t> key_hash{0};  // atomic: the sweep reads it to find a slot's shard
		// The relief a begun demotion (its maps) or close (descriptors, vnodes) will bring: booked in the catalog's
		// pending counts, released when the transition ends, so a pass counts what is in flight. A demotion for a
		// closing cap books the close it leads to in pend_desc and pend_vnodes too (`due`), until the slot's close
		// begins or a pin takes it. Each booking and release follows a compare-exchange that excludes the others.
		std::atomic<std::uint32_t> pend_maps{0};
		std::atomic<std::uint16_t> pend_desc{0}, pend_vnodes{0};  // saturating
		// The holders still to unpin in a replace's `closing`; the last posts the replace close (F4).
		std::atomic<std::uint32_t> drain{0};
		// The policy's queue links (under policy_mu_); `q_next` is also the free list's link while the slot is free,
		// when it is in no queue.
		std::uint32_t q_prev = none, q_next = none;
		std::uint16_t last_open = 0;  // when the last replacement's open was due, in 2^27 ns (134 ms) units modulo 2^16
		std::uint8_t streak = 0;      // consecutive failed wakes of any kind (saturating): the backoff's exponent
		std::uint8_t fences = 0;      // replacements within 10 s of each other (saturating): the reopen backoff's exponent
		// A failed open's or a failed wake's count, saturating (the bound counts the wake's, not too_many_open). Read by
		// the sweep without the mutex.
		std::atomic<std::uint8_t> failures{0};
		std::atomic<bool> commit_failed{false};
		std::atomic<bool> to_ghost{false};  // the pass chose this close from the small FIFO: its hash enters the ghost when it ends
		std::uint8_t q = 0;                 // the policy's queue (under policy_mu_): 0 none, 1 small, 2 main
		// A close that left closing() true (a view kept past its pin): the sweep, and shutdown, retry try_finish_close().
		std::atomic<bool> stuck{false}, retrying{false};
		std::atomic<std::uint8_t> due{0};     // a demotion for a closing cap booked its close (see pend_desc)
		std::atomic<std::uint8_t> holdup{0};  // what holds a shutdown close back (the report's reason)
		// Under the shard mutex: a create pinner asked (`create`); the running open attempt had it (`attempt_create`);
		// the cached error is index_not_created (`absent`); a replace close is under way (`replacing`); a replacement
		// happened (`replaced_once`).
		bool create : 1 = false, attempt_create : 1 = false, absent : 1 = false, replacing : 1 = false, replaced_once : 1 = false;
	};
	struct shard {
		mutable std::mutex mu;
		index_detail::hash_slots table;
	};


public:

	// One open attempt, as the host sees it (F1, F3, F5).
	struct opening {
		std::string_view key;
		io_counts counts;     // the slot's: what the host opens for this index reserves here (io_counts::reserve)
		bool create = false;  // a create pin asked: make the index when there is none
	};

	// What the host supplies (Q2). `resolve` returns the key's IO (a counted_io over the budget in
	// `opening.counts`) or throws index_not_created for a missing directory that `opening.create` does not
	// ask for; `open_index` opens it (the default makes it when `create` and none is there); `open_payload`
	// builds H in place and may then work on the index (a WAL replay): a throw fails the open, destroying
	// H, the reader, the index and the IO in that order. Chores run on `exec`.
	struct host {
		executor* exec = nullptr;
		resource_budget* budget = nullptr;
		std::function<std::unique_ptr<kronuz::journal::IO>(const opening&)> resolve;
		std::function<Index(kronuz::journal::IO&, const typename Index::options&, const opening&)> open_index = [](kronuz::journal::IO& io, const typename Index::options& o, const opening& op) {
			try {
				return Index::open(io, o);
			} catch (const index_not_created&) {
				if (!op.create) { throw; }
				return Index::create(io, o, fresh_uuid());
			}
		};
		// Default: a default-constructed H; an H that is not default-constructible needs the host's.
		std::function<void(const opening&, Index&, std::optional<H>&)> open_payload = default_payload();
		typename Index::options index_options;
		// Tests: called just before a compare-exchange of the word, with its site (never under a lock the
		// hook's action takes). A site that runs under the shard mutex passes locked = true.
		std::function<void(const char* site, bool locked)> before_cas;
		// Required. Calls `fn` once, `delay_ns` from now, on any thread; `fn` only posts a chore. A timer
		// that throws has not armed `fn`. A sleeping chore would hold its group's lane (the catalog's
		// transitions share one), so the catalog never waits on the executor.
		std::function<void(std::uint64_t delay_ns, std::function<void()> fn)> timer;
		// The commit chore the catalog runs, under the chore's lease with no pin held: at the bound close when
		// something is uncommitted (a failure ends the close), at shutdown, in the replace close, and in a demotion
		// when only a finished merge's output is unsynced and nothing else is uncommitted (S14). Default: index.commit().
		std::function<void(H&, Index&)> commit = [](H&, Index& ix) { ix.commit(); };
		// Failed wakes after which the sweep closes the slot (D5; I5-N2); too_many_open does not count.
		unsigned wake_bound = 8;  // at most 255: a slot counts failures in a byte
		// The sweep (D5, D6; estimates). The idle times are on the executor's clock. `soft_open` caps the
		// open indexes and `soft_hot` the hot ones, those with a mapped record (CT-R36), each at most the
		// budget's own; 0 leaves the budget's caps alone. A pass visits `pass_visits` slots and posts at most
		// `pass_posts` transitions. start() needs `timer`.
		std::uint64_t sweep_period_ns = 1'000'000'000, demote_idle_ns = 30'000'000'000, close_idle_ns = 600'000'000'000;
		std::uint64_t negative_ttl_ns = 10'000'000'000;
		std::uint64_t soft_open = 0, soft_hot = 0;
		std::size_t pass_visits = 256, pass_posts = 16;
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
		std::uint64_t promoted = 0, decayed = 0, evictions = 0, evict_demotes = 0, busy = 0, ghost_hits = 0, ghost_adds = 0;
		std::uint64_t wake_failures = 0, wake_overloads = 0, wake_gated = 0, bound_closes = 0, bound_commits = 0, commit_failures = 0;
		std::uint64_t passes = 0, pass_demotes = 0, pass_closes = 0, pass_reposts = 0, reclaims = 0, erased = 0;
		std::uint64_t stuck_closes = 0, stuck_retries = 0, close_failures = 0, shutdown_commits = 0, shutdown_commit_failures = 0;
		std::uint64_t merge_commits = 0, merge_commit_failures = 0, create_reposts = 0, replaces = 0, replace_commit_failures = 0, drain_unpins = 0, replace_reposts = 0;
		std::uint64_t unlinked = 0, gone_pins = 0;  // waiters removed before their delivery; pins delivered to a frame that was gone
		std::uint64_t cancels = 0;                  // waiting pins ended by their token
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
			if (c_) { std::exchange(c_, nullptr)->unpin(*s_, idx_); }
		}

	private:
		friend class catalog;
		pin(catalog* c, std::uint32_t idx, std::uint64_t gen) noexcept : c_(c), s_(&c->at(idx)), idx_(idx), gen_(gen) {}
		catalog* c_ = nullptr;
		slot* s_ = nullptr;
		std::uint32_t idx_ = 0;
		std::uint64_t gen_ = 0;
	};

	// `absent`: a resident pin found no index in the catalog (it holds no pin and made no slot).
	enum class try_status { pinned, would_wait, failed, absent };
	struct try_result {
		try_status status = try_status::would_wait;
		pin p;
		std::exception_ptr error;
	};

	// co_await catalog.pin(...): the pin, or the open's or the wake's error, or task::cancelled when the
	// caller's token is requested while it waits (task's lock(token) shape: a pin that need not wait is
	// taken whatever the token; a requested token never waits; a cancel that loses the race to a delivery
	// returns the delivery). A frame destroyed while it waits (task's teardown, D8 phase 3) leaves the
	// catalog consistent: its destructor unlinks a waiter no transition has taken, uncounting its pin, and
	// releases a pin delivered to it and never resumed; a delivery that finds it gone releases the pin
	// itself. The catalog outlives every frame that awaits it.
	class pin_awaiter {
	public:
		pin_awaiter(const pin_awaiter&) = delete;
		pin_awaiter& operator=(const pin_awaiter&) = delete;
		~pin_awaiter() {
			cb_.reset();  // waits for a cancel running elsewhere, which uses w_
			if (w_) { catalog::abandon(c_, std::exchange(w_, nullptr)); }
		}
		bool await_ready() {
			const bool may_wait = !stop_.requested();
			const acquired a = c_->acquire_impl(req_, hint_, may_wait ? &w_ : nullptr);
			r_ = a.out;
			if (r_ == outcome::waiting) { return false; }
			if (w_) { std::exchange(w_, nullptr)->release(); }  // made for a join that did not happen
			idx_ = a.idx, gen_ = a.gen, error_ = a.error;
			if (r_ == outcome::would_wait) { error_ = std::make_exception_ptr(task::cancelled{}); }
			return true;
		}
		bool await_suspend(std::coroutine_handle<> h) noexcept {
			w_->h = h;
			cb_.emplace(stop_, cancel_pin{c_, w_});  // a request already made runs it here: then st is delivered
			int expect = waiter::registered;
			return w_->st.compare_exchange_strong(expect, waiter::suspended, std::memory_order_acq_rel, std::memory_order_acquire);
		}
		pin await_resume() {
			cb_.reset();
			if (waiter* w = std::exchange(w_, nullptr)) {
				idx_ = w->idx, gen_ = w->gen, error_ = std::move(w->error);
				w->release();
			}
			if (error_) { std::rethrow_exception(error_); }
			if (r_ == outcome::none) { return pin(); }
			return pin(c_, idx_, gen_);
		}

	private:
		friend class catalog;
		pin_awaiter(catalog& c, std::string_view key, pin_kind kind, bool use, handle hint, open_intent intent, task::cancel_token stop)
			: c_(&c), req_{key, kind, use, intent}, hint_(hint), stop_(std::move(stop)) {}
		catalog* c_;
		request req_;
		handle hint_;
		task::cancel_token stop_;
		waiter* w_ = nullptr;  // set while it waits: joined a slot's list
		outcome r_ = outcome::waiting;
		std::uint32_t idx_ = 0;
		std::uint64_t gen_ = 0;
		std::exception_ptr error_;
		std::optional<task::cancel_callback<cancel_pin>> cb_;
	};

	// co_await catalog.shutdown(): resumes when every slot is closed, with the keys whose commit failed. A
	// frame destroyed while it waits unlinks its waiter, as a pin's does.
	class shutdown_awaiter {
	public:
		shutdown_awaiter(const shutdown_awaiter&) = delete;
		shutdown_awaiter& operator=(const shutdown_awaiter&) = delete;
		~shutdown_awaiter() {
			if (w_) { catalog::abandon(c_, std::exchange(w_, nullptr)); }
		}
		bool await_ready() {
			w_ = new waiter;
			if (!c_->join_shutdown(w_)) { return false; }
			std::exchange(w_, nullptr)->release();
			return true;
		}
		bool await_suspend(std::coroutine_handle<> h) noexcept {
			w_->h = h;
			int expect = waiter::registered;
			return w_->st.compare_exchange_strong(expect, waiter::suspended, std::memory_order_acq_rel, std::memory_order_acquire);
		}
		shutdown_result await_resume() {
			if (waiter* w = std::exchange(w_, nullptr)) { w->release(); }
			return c_->shutdown_outcome();
		}

	private:
		friend class catalog;
		explicit shutdown_awaiter(catalog& c) : c_(&c) {}
		catalog* c_;
		waiter* w_ = nullptr;
	};

	explicit catalog(host h) : h_(std::move(h)), life_(std::make_shared<life>(this)) {
		if (!h_.timer) { throw std::invalid_argument("strata: the catalog's host supplies a timer"); }
		if (!h_.open_payload) { throw std::logic_error("strata: an H that is not default-constructible needs host::open_payload"); }
		if (h_.wake_bound > 255) { throw std::invalid_argument("strata: host::wake_bound is at most 255 (the slot counts failures in a byte)"); }
		group_ = h_.exec->open();
		for (auto& c : chunks_) { c.store(nullptr, std::memory_order_relaxed); }
		if (h_.soft_open) {
			small_cap_ = h_.soft_open / 10;  // the small FIFO is a tenth of soft_open, the ghost half of it
			ghost_ = ghost_set(static_cast<std::size_t>(h_.soft_open / 2));
		}
	}
	catalog(const catalog&) = delete;
	catalog& operator=(const catalog&) = delete;
	// After shutdown(): every pin released and every chore finished. A catalog destroyed with slots open
	// asserts in debug builds and leaks their IO with its lock (D5), as ~basic_index leaks the lock; one
	// destroyed with a pin held asserts too, and that pin's release is a use after free. Waiters left (an
	// unfinished shutdown given up on) are orphaned, as task's ~event orphans its: their frames, destroyed
	// later, touch only their waiters, and a token requested later does nothing.
	~catalog() {
		stop_sweep();
		{
			std::lock_guard g(life_->mu);
			life_->c = nullptr;
		}
		for (;;) {  // a timer's callback posts before it is counted done, and a chore may arm another
			{
				std::unique_lock l(timers_mu_);
				timers_cv_.wait(l, [this] { return timers_ == 0; });
			}
			group_->wait();
			std::lock_guard l(timers_mu_);
			if (timers_ == 0) { break; }
		}
		orphan_waiters();
		release_chunks();
	}

	// --- the sweep and shutdown (D5, D6) --------------------------------------------------------------

	// Registers the budget's soft-cap listener and arms the sweep's timer.
	void start() {
		if (started_.exchange(true)) { return; }
		stopped_.store(false);
		listener_ = h_.budget->add_listener([l = life_] {
			std::lock_guard g(l->mu);
			if (l->c) { l->c->post_pass(); }
		});
		arm_period();
	}
	// No pass is posted after this and the timer is not re-armed; a pass already queued runs.
	void stop_sweep() {
		stopped_.store(true);
		if (const std::uint64_t id = std::exchange(listener_, 0)) { h_.budget->remove_listener(id); }
	}
	// Posts a pass unless one is queued or running: a crossing posts one however often the budget calls.
	void post_pass() {
		if (stopped_.load(std::memory_order_acquire) || pass_queued_.exchange(true, std::memory_order_acq_rel)) { return; }
		post([this] { run_pass(); });
	}
	// What one pass may do: slots the window visits and queue candidates examined (a move, a decrement or a
	// requeue counts), and transitions posted. `lifted()` is the policy tests' (and the stress's) unlimited pass.
	struct policy_limits {
		std::uint64_t visits = 256, transitions = 16;
		static policy_limits lifted() noexcept { return {~std::uint64_t(0), ~std::uint64_t(0)}; }
	};
	struct policy_result {
		bool again = false;  // it posted some and the soft caps still call for more
		std::uint64_t visited = 0, promoted = 0, decayed = 0, closes = 0, demotes = 0, busy = 0;
	};

	// One pass, a chore: visits at most `pass_visits` slots from where the last one stopped and posts at most
	// `pass_posts` transitions. True when it posted some and the soft caps still call for more.
	bool pass() { return policy_pass({h_.pass_visits, h_.pass_posts}).again; }

	// The pass under `lim`. A window of slots from the cursor: negative entries erased, stuck closes retried, the bound
	// close, hot slots demoted (idle, or over `soft_hot` or the maps' share, oldest use first), warm slots idle long
	// enough closed, and closes a demotion booked. Then, while a closing cap (`soft_open`, the descriptors' or the
	// vnodes' share) is over, the policy chooses what closes: S3-FIFO's small and main heads (`evict`).
	policy_result policy_pass(const policy_limits& lim = {}) {
		policy_result r;
		passes_.fetch_add(1, std::memory_order_relaxed);
		h_.budget->tick();
		const bool down = shutting_.load(std::memory_order_acquire);
		const std::uint64_t now = exec_now();
		pass_at_.store(now, std::memory_order_relaxed);
		// What the caps ask for, in the unit each is released in: hot indexes and mapped records (demotion). The
		// demotions and closes begun and not ended are relief already posted, and come off the shares.
		const auto sub = [](std::uint64_t x, std::uint64_t by) { return x > by ? x - by : 0; };
		std::uint64_t over_hot = 0;
		if (h_.soft_hot != 0) {  // hot indexes: slots whose maps count is above zero (C22)
			const std::uint64_t hot = counts_.holders_mapped.load(std::memory_order_relaxed);
			const std::uint64_t pending = pending_hot_.load(std::memory_order_relaxed);  // read after the count: see closing_over()
			over_hot = sub(hot > h_.soft_hot ? hot - h_.soft_hot : 0, pending);
		}
		const std::uint64_t maps_share = std::max(share(resource::maps), share(resource::vnodes)), pending_maps = pending_maps_.load(std::memory_order_relaxed);
		std::uint64_t over_maps = sub(maps_share, pending_maps);
		struct candidate {
			std::uint32_t idx;
			std::uint64_t last, idle, maps;
			bool hot, due;
		};
		std::vector<candidate> cands;
		std::uint64_t posts = 0;
		const std::uint32_t made = slots_made();
		const std::uint64_t visits = std::min<std::uint64_t>(lim.visits, made);
		for (std::uint64_t v = 0; v < visits; ++v) {
			if (cursor_ >= made) { cursor_ = 0; }
			const std::uint32_t i = cursor_++;
			slot& s = at(i);
			const std::uint64_t w = s.word.load(std::memory_order_acquire);
			const slot_state st = word::state(w);
			if (st == slot_state::failed) {
				if (word::pins(w) == 0) { (void)erase_failed(i, now, false); }
			} else if (st == slot_state::closing) {
				if (posts < lim.transitions && s.stuck.load(std::memory_order_acquire) && !s.retrying.exchange(true, std::memory_order_acq_rel)) {
					++posts;
					post([this, i] { run_stuck_retry(i); });
				}
			} else if ((st == slot_state::open || st == slot_state::owed) && word::pins(w) == 0 && !down) {
				if (st == slot_state::owed && posts < lim.transitions && begin_bound_close(i)) {
					++posts;
					continue;
				}
				if (now < s.probe_at.load(std::memory_order_relaxed)) { continue; }  // refused lately, or a failed commit's probe
				const std::uint64_t last = s.last_use.load(std::memory_order_relaxed), maps = s.counts.get(resource::maps);
				const bool hot = st == slot_state::open && maps != 0;
				const bool due = !hot && s.due.load(std::memory_order_acquire) != 0;
				cands.push_back({i, last, now > last ? now - last : 0, maps, hot, due});
			}
		}
		std::sort(cands.begin(), cands.end(), [](const candidate& a, const candidate& b) { return a.last < b.last; });
		for (const candidate& c : cands) {
			if (posts >= lim.transitions) { break; }
			if (c.due) {  // a slot demoted for a closing cap whose close the demotion's end could not begin
				if (begin_due_close(c.idx)) {
					++posts, ++r.closes;
					pass_closes_.fetch_add(1, std::memory_order_relaxed);
				}
			} else if (c.hot) {
				if (c.idle >= h_.demote_idle_ns || over_hot + over_maps != 0) {
					if (begin_demote(c.idx)) {
						++posts, ++r.demotes;
						pass_demotes_.fetch_add(1, std::memory_order_relaxed);
						over_hot = sub(over_hot, 1);
						over_maps = sub(over_maps, c.maps);
					}
				}
			} else if (c.idle >= h_.close_idle_ns) {  // idle long enough: closed whatever the caps say, and not ghosted
				if (begin_close(c.idx)) {
					++posts, ++r.closes;
					pass_closes_.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		evict(lim, r, posts);
		r.again = posts != 0 && (over_hot + over_maps != 0 || closing_over() != 0);
		return r;
	}

	// A task the host awaits before destroying the catalog: new pins fail with catalog_closed; each slot is
	// closed once unpinned, after the host's commit chore if anything is uncommitted (a commit that fails
	// closes it uncommitted and names it); the awaiting task resumes when no slot is left.
	[[nodiscard]] shutdown_awaiter shutdown() { return shutdown_awaiter(*this); }
	bool shutting_down() const noexcept { return shutting_.load(std::memory_order_acquire); }
	bool shut_down() const noexcept { return shut_done_.load(std::memory_order_acquire); }
	// Set on a thread inside a close chore: H's destructor may assert it is not (Xapiand D4).
	static bool in_close_chore() noexcept { return close_depth_ > 0; }

	// A pin by key. `use`: one per request, by key (CT-R31); a re-pin within the request is not. `intent`
	// (F1): `create` also passes a missing index's negative entry; `resident` (chore pins only) never opens
	// and answers an empty pin (operator bool false) for a key with no open or owed index. `stop`: the
	// caller's token (a request's, a deadline's, a chore's stop); requested while the pin waits, the pin
	// throws task::cancelled.
	[[nodiscard]] pin_awaiter pin_request(std::string_view key, bool use = true, open_intent intent = open_intent::open, task::cancel_token stop = {}) {
		return pin_awaiter(*this, key, pin_kind::request, use, {}, checked(pin_kind::request, intent), std::move(stop));
	}
	[[nodiscard]] pin_awaiter pin_chore(std::string_view key, bool use = false, open_intent intent = open_intent::open, task::cancel_token stop = {}) {
		return pin_awaiter(*this, key, pin_kind::chore, use, {}, intent, std::move(stop));
	}
	// A re-pin by handle: one compare-exchange while the slot is open and `gen` matches, else by key.
	[[nodiscard]] pin_awaiter repin(handle hint, std::string_view key, pin_kind kind = pin_kind::request, bool use = false, open_intent intent = open_intent::open, task::cancel_token stop = {}) {
		return pin_awaiter(*this, key, kind, use, hint, checked(kind, intent), std::move(stop));
	}
	// Starts the open or the wake as a pin would and answers would_wait, holding no pin (CT-R35).
	try_result try_pin(std::string_view key, pin_kind kind = pin_kind::request, bool use = true, open_intent intent = open_intent::open) {
		const acquired a = acquire_impl({key, kind, use, checked(kind, intent)}, {}, nullptr);
		try_result r;
		switch (a.out) {
		case outcome::granted: r.status = try_status::pinned, r.p = pin(this, a.idx, a.gen); break;
		case outcome::failed: r.status = try_status::failed, r.error = a.error; break;
		case outcome::none: r.status = try_status::absent; break;
		default: r.status = try_status::would_wait; break;
		}
		return r;
	}

	// A holder whose call threw index_fenced hands its pin back: the catalog replaces the index (F4). The slot
	// goes to `closing`, new pins by key wait and by handle fall back to the key, the other holders drain by
	// unpinning, the last one posts the close, and the waiters get a fresh index from the host's open_payload.
	// A pin on `waking` or on a drain already begun is a plain unpin.
	void reopen(pin&& p) {
		if (!p.c_) { return; }
		slot& s = *p.s_;
		const std::uint32_t idx = p.idx_;
		p.c_ = nullptr;
		bool post_close = false, replaced = false;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			for (;;) {
				const slot_state st = word::state(cur);
				if (st != slot_state::open && st != slot_state::owed) { break; }
				const std::uint64_t holders = word::pins(cur) - 1;
				s.drain.store(std::uint32_t(holders), std::memory_order_relaxed);
				hook("reopen", true);
				if (s.word.compare_exchange_weak(cur, word::with_pins(word::with_state(cur, slot_state::closing), holders), std::memory_order_acq_rel, std::memory_order_acquire)) {
					hook("reopen.exchanged", true);  // a holder's unpin from here on is a drain unpin: `drain` is already stored
					note(st, slot_state::closing);
					release_due(s);
					book(s, resource::descriptors);
					post_close = holders == 0;
					replaced = true;
					break;
				}
			}
		}
		if (!replaced) {
			unpin(s, idx);
		} else if (post_close) {
			post([this, idx] { run_replace(idx); });
		}
	}

	// One entry of snapshot().
	struct slot_view {
		std::string key;
		slot_state state;
		std::uint64_t pins;
	};
	// Every slot that is not free, under its shard's mutex, with no access to an index (F6): FLUSH pins each
	// key `resident`, and diagnostics read the state counts. A slot freed or reused meanwhile is skipped.
	std::vector<slot_view> snapshot() const {
		std::vector<slot_view> out;
		const std::uint32_t n = slots_made();
		for (std::uint32_t i = 0; i < n; ++i) {
			const slot& s = at(i);
			const std::uint32_t h = s.key_hash.load(std::memory_order_acquire);
			std::lock_guard l(shard_of(h).mu);
			// The word first: a new owner's word (acquire) shows its key_hash, so the recheck skips it and `key` is never read
			// under the wrong shard's mutex.
			const std::uint64_t w = s.word.load(std::memory_order_acquire);
			hook("snapshot.slot", true);
			if (s.key_hash.load(std::memory_order_acquire) != h) { continue; }
			if (word::state(w) != slot_state::free) { out.push_back({s.key, word::state(w), word::pins(w)}); }
		}
		return out;
	}

	// --- the sweep's compare-exchanges (item 5 replaces the chores; the word's rows are final) ------

	// `{0, open}` to `{0, demoting}` and a demotion chore. False when a pin or a transition holds it. `due`: the
	// demotion is for a closing cap, and the close it leads to is booked as relief (B2).
	bool begin_demote(std::uint32_t idx, bool due = false, bool ghost = false) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_acquire);
		for (;;) {
			if (word::pins(w) != 0 || word::state(w) != slot_state::open) { return false; }
			hook("sweep.demote", false);
			if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::demoting), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(slot_state::open, slot_state::demoting);
		book(s, resource::maps);
		if (due) { book_due(s, ghost); }
		post([this, idx] { run_demote(idx); });
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
		release_due(s);
		book(s, resource::descriptors);
		post([this, idx, prior] { run_close(idx, prior, close_mode::sweep); });
		return true;
	}
	// The close a demotion booked, begun at the demotion's end or by a pass; it carries the ghost mark the booking holds.
	bool begin_due_close(std::uint32_t idx) {
		slot& s = at(idx);
		const bool ghost = s.due.load(std::memory_order_acquire) == 2;
		if (ghost) { s.to_ghost.store(true, std::memory_order_relaxed); }
		if (begin_close(idx)) { return true; }
		if (ghost) { s.to_ghost.store(false, std::memory_order_relaxed); }
		return false;
	}
	// The policy's reset of a promoted slot's frequency (D6): a fetch-op on the frequency only.
	void clear_freq(std::uint32_t idx) {
		hook("policy.clear", false);
		at(idx).word.fetch_and(~word::freq_mask, std::memory_order_acq_rel);
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
		release_due(s);
		book(s, resource::descriptors);
		post([this, idx] { run_close(idx, slot_state::owed, close_mode::bound); });
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

	// --- the closing policy: S3-FIFO and the hot cap (D6; CT-R10, CT-R31, CT-R41) ----------------

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
	std::uint64_t ghost_capacity() const noexcept { return ghost_.capacity(); }
	// The ghost's bytes now (nodes and table); under the policy mutex.
	std::uint64_t ghost_bytes() const {
		std::lock_guard l(policy_mu_);
		return ghost_.bytes();
	}

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
		s.waiters.each([&](const waiter&) { ++n; });
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
		o.promoted = promoted_.load(), o.decayed = decayed_.load(), o.evictions = evictions_.load();
		o.evict_demotes = evict_demotes_.load(), o.busy = busy_.load(), o.ghost_hits = ghost_hits_.load(), o.ghost_adds = ghost_adds_.load();
		o.wake_failures = wake_failures_.load(), o.wake_overloads = wake_overloads_.load(), o.wake_gated = wake_gated_.load();
		o.bound_closes = bound_closes_.load(), o.bound_commits = bound_commits_.load(), o.commit_failures = commit_failures_.load();
		o.passes = passes_.load(), o.pass_demotes = pass_demotes_.load(), o.pass_closes = pass_closes_.load(), o.pass_reposts = pass_reposts_.load();
		o.reclaims = reclaims_.load(), o.erased = erased_.load(), o.stuck_closes = stuck_closes_.load(), o.stuck_retries = stuck_retries_.load();
		o.close_failures = close_failures_.load(), o.shutdown_commits = shutdown_commits_.load(), o.shutdown_commit_failures = shutdown_commit_failures_.load();
		o.merge_commits = merge_commits_.load(), o.merge_commit_failures = merge_commit_failures_.load(), o.create_reposts = create_reposts_.load(), o.replaces = replaces_.load(), o.replace_commit_failures = replace_commit_failures_.load();
		o.drain_unpins = drain_unpins_.load(), o.replace_reposts = replace_reposts_.load();
		o.unlinked = unlinked_.load(), o.gone_pins = gone_pins_.load(), o.cancels = cancels_.load();
		for (unsigned i = 0; i < slot_states; ++i) {
			o.waited[i] = waited_[i].load();
			for (unsigned j = 0; j < slot_states; ++j) { o.edges[i][j] = edges_[i][j].load(); }
		}
		return o;
	}
	const resource_counts& counts() const noexcept { return counts_; }
	// Slots whose bound close failed to commit and have not closed or committed since (D5).
	std::size_t failing() const noexcept { return failing_.load(std::memory_order_relaxed); }
	// Slots not free, and slots with an index (opening to closing): what the soft_open cap counts.
	std::size_t occupied() const noexcept { return occupied_.load(std::memory_order_relaxed); }
	std::size_t open_slots() const noexcept { return live_.load(std::memory_order_relaxed); }
	// Relief of the demotions and closes begun and not ended (maps; descriptors; vnodes).
	std::uint64_t pending_maps() const noexcept { return pending_maps_.load(std::memory_order_relaxed); }
	std::uint64_t pending_descriptors() const noexcept { return pending_desc_.load(std::memory_order_relaxed); }
	std::uint64_t pending_vnodes() const noexcept { return pending_vnodes_.load(std::memory_order_relaxed); }
	// Hot indexes (slots with a mapped record: what `soft_hot` caps), demotions of hot indexes in flight, and
	// closes booked by a demotion for a closing cap (B2).
	std::uint64_t hot_indexes() const noexcept { return counts_.holders_mapped.load(std::memory_order_relaxed); }
	std::uint64_t pending_hot() const noexcept { return pending_hot_.load(std::memory_order_relaxed); }
	std::uint64_t pending_open() const noexcept { return pending_open_.load(std::memory_order_relaxed); }
	bool due_of(std::uint32_t idx) const noexcept { return at(idx).due.load(std::memory_order_relaxed) != 0; }
	std::uint64_t count_of(std::uint32_t idx, resource r) const noexcept { return at(idx).counts.get(r); }
	bool stuck_of(std::uint32_t idx) const noexcept { return at(idx).stuck.load(std::memory_order_relaxed); }
	unsigned streak_of(std::uint32_t idx) const noexcept { return at(idx).streak; }  // test: a quiescent slot
	unsigned wake_failures_of(std::uint32_t idx) const noexcept { return at(idx).failures.load(std::memory_order_relaxed); }
	std::uint32_t drain_of(std::uint32_t idx) const noexcept { return at(idx).drain.load(std::memory_order_relaxed); }  // test: holders a replace waits for
	std::uint64_t probe_at_of(std::uint32_t idx) const noexcept { return at(idx).probe_at.load(std::memory_order_relaxed); }
	static constexpr std::uint64_t repost_first_ns = 1'000'000, repost_cap_ns = 64'000'000;  // D5 (estimates)
	static constexpr std::uint64_t commit_probe_ns = 10'000'000'000ull;      // D5
	static constexpr std::uint64_t fence_window_ns = 10'000'000'000ull;      // F4: fences closer than this back the reopen off
	static constexpr unsigned time_unit_bits = 27;                            // the slot keeps that time in 134 ms units (16 bits)
	// Every slot not yet free, and why (C23): what a host logs before it abandons an unfinished shutdown by
	// exiting. Each key is read under its shard's mutex.
	std::vector<shutdown_holder> shutdown_report() const {
		std::vector<shutdown_holder> out;
		const std::uint32_t n = slots_made();
		for (std::uint32_t i = 0; i < n; ++i) {
			const slot& s = at(i);
			const std::uint32_t h = s.key_hash.load(std::memory_order_acquire);
			std::lock_guard l(shard_of(h).mu);
			const std::uint64_t w = s.word.load(std::memory_order_acquire);
			const slot_state st = word::state(w);
			if (st == slot_state::free || s.key_hash.load(std::memory_order_relaxed) != h) { continue; }
			shutdown_hold why = shutdown_hold::waiting;
			const std::uint8_t held = s.holdup.load(std::memory_order_relaxed);
			if (st == slot_state::closing && s.stuck.load(std::memory_order_acquire)) {
				why = shutdown_hold::view_held;
			} else if (st == slot_state::closing && held == hold_throws) {
				why = shutdown_hold::close_throws;
			} else if (st == slot_state::closing && held == hold_lease) {
				why = shutdown_hold::lease_busy;
			} else if (word::pins(w) != 0) {
				why = shutdown_hold::pinned;
			} else if (st != slot_state::open && st != slot_state::owed && st != slot_state::failed) {
				why = shutdown_hold::transition;
			}
			out.push_back({s.key, std::uint8_t(st), word::pins(w), why});
		}
		return out;
	}
	// The size of a slot, for a test that holds it to D4's figure.
	static constexpr std::size_t slot_bytes() noexcept { return sizeof(slot); }
	// Sets a slot's pin count (its other fields kept), for a test that needs the cap.
	void set_pins_for_test(std::uint32_t idx, std::uint64_t pins) noexcept {
		slot& s = at(idx);
		std::uint64_t cur = s.word.load(std::memory_order_acquire);
		while (!s.word.compare_exchange_weak(cur, word::with_pins(cur, pins), std::memory_order_acq_rel, std::memory_order_acquire)) {}
	}
	// A slot's index, for a test that holds the slot in a state; null unless built.
	Index* index_of(std::uint32_t idx) noexcept { return at(idx).index.get(); }
	// Lets the posted chores of a finished test run to their end.
	void wait_chores() { group_->wait(); }

private:
	friend class pin;
	friend class pin_awaiter;

	static std::function<void(const opening&, Index&, std::optional<H>&)> default_payload() {
		if constexpr (std::is_default_constructible_v<H>) {
			return [](const opening&, Index&, std::optional<H>& h) { h.emplace(); };
		} else {
			return {};
		}
	}
	// `resident` is a chore pin's.
	static open_intent checked(pin_kind kind, open_intent intent) {
		if (intent == open_intent::resident && kind != pin_kind::chore) { throw std::logic_error("strata: a resident pin is a chore pin"); }
		return intent;
	}
	static std::array<char, 16> fresh_uuid() {
		std::random_device rd;
		std::array<char, 16> u;
		for (auto& c : u) { c = char(rd()); }
		u[6] = char((u[6] & 0x0f) | 0x40), u[8] = char((u[8] & 0x3f) | 0x80);
		return u;
	}
	// The debug check behind Index::guard_calls (CT-R15): a call is made under a pin or inside a transition.
	static bool pin_held(std::uint64_t w) noexcept {
		switch (word::state(w)) {
		case slot_state::opening: case slot_state::waking: case slot_state::demoting: case slot_state::closing: return true;
		default: return word::pins(w) != 0;
		}
	}

	slot& at(std::uint32_t i) const noexcept { return chunks_[i / chunk_slots].load(std::memory_order_acquire)[i % chunk_slots]; }
	shard& shard_of(std::uint32_t h) const noexcept { return const_cast<shard&>(shards_[h & (shard_count - 1)]); }
	static std::uint16_t clamp16(std::uint64_t v) noexcept { return v > UINT16_MAX ? UINT16_MAX : std::uint16_t(v); }
	void hook(const char* site, bool locked) const {
		if (h_.before_cas) { h_.before_cas(site, locked); }
	}
	void hook_noexcept(const char* site) const noexcept {
		try {
			hook(site, false);
		} catch (...) {
		}
	}
	void note(slot_state from, slot_state to) noexcept {
		edges_[unsigned(from)][unsigned(to)].fetch_add(1, std::memory_order_relaxed);
		const auto has_index = [](slot_state x) { return x != slot_state::free && x != slot_state::failed; };
		if (has_index(from) != has_index(to)) { (has_index(to) ? live_.fetch_add(1, std::memory_order_relaxed) : live_.fetch_sub(1, std::memory_order_relaxed)); }
		if ((from == slot_state::closing) != (to == slot_state::closing)) { (to == slot_state::closing ? closing_.fetch_add(1, std::memory_order_relaxed) : closing_.fetch_sub(1, std::memory_order_relaxed)); }
		if (to == slot_state::free) { occupied_.fetch_sub(1, std::memory_order_relaxed); }  // a miss counts its own (B1)
	}
	// Books a begun demotion's maps and hot index, or a begun close's descriptors and vnodes, as relief in flight.
	void book(slot& s, resource r) noexcept {
		if (r == resource::maps) {
			const std::uint32_t m = std::uint32_t(s.counts.get(resource::maps));
			s.pend_maps.store(m, std::memory_order_relaxed);
			pending_maps_.fetch_add(m, std::memory_order_relaxed);
			pending_hot_.fetch_add(1, std::memory_order_relaxed);  // every begun demotion ends in end_at
		} else {
			const std::uint16_t d = clamp16(s.counts.get(resource::descriptors)), v = clamp16(s.counts.get(resource::vnodes));
			s.pend_desc.store(d, std::memory_order_relaxed);
			s.pend_vnodes.store(v, std::memory_order_relaxed);
			pending_desc_.fetch_add(d, std::memory_order_relaxed);
			pending_vnodes_.fetch_add(v, std::memory_order_relaxed);
		}
	}
	void unbook(slot& s) noexcept {
		pending_maps_.fetch_sub(s.pend_maps.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
		pending_desc_.fetch_sub(s.pend_desc.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
		pending_vnodes_.fetch_sub(s.pend_vnodes.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
	}
	// A demotion for a closing cap (B2): the close it leads to counts as relief from now on.
	void book_due(slot& s, bool ghost) noexcept {
		release_due(s);
		const std::uint16_t d = clamp16(s.counts.get(resource::descriptors)), v = clamp16(s.counts.get(resource::vnodes));
		s.pend_desc.store(d, std::memory_order_relaxed);
		s.pend_vnodes.store(v, std::memory_order_relaxed);
		pending_desc_.fetch_add(d, std::memory_order_relaxed);
		pending_vnodes_.fetch_add(v, std::memory_order_relaxed);
		pending_open_.fetch_add(1, std::memory_order_relaxed);
		s.due.store(ghost ? 2 : 1, std::memory_order_release);  // 2: the close enters the ghost (it left the small FIFO unused)
	}
	// The booked close is begun (its own booking follows) or will not come (a pin took the slot).
	void release_due(slot& s) noexcept {
		if (!s.due.exchange(0, std::memory_order_acq_rel)) { return; }
		pending_desc_.fetch_sub(s.pend_desc.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
		pending_vnodes_.fetch_sub(s.pend_vnodes.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
		pending_open_.fetch_sub(1, std::memory_order_relaxed);
	}
	void post(std::function<void()> f) {
		group_->post([f = std::move(f)](const task::cancel_token&) { f(); });
	}

	std::uint32_t alloc_slot() {
		std::lock_guard l(free_mu_);
		if (free_head_ != none) {
			const std::uint32_t i = free_head_;
			free_head_ = at(i).q_next;
			at(i).q_next = none;
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
		at(i).q_next = free_head_;
		free_head_ = i;
	}

	waiter* take(slot& s) noexcept { return s.waiters.take(); }

	// Registers the request as a waiter on the slot it counted itself on (under the shard mutex); the
	// list takes a reference.
	void join(slot& s, waiter* w, const request& r, std::uint32_t idx, slot_state met) {
		w->kind = r.kind;
		w->idx = idx;
		w->pins = true;
		w->mu = &shard_of(s.key_hash).mu;
		w->sched = task::current();
		w->retain();
		s.waiters.push(w);
		waited_[unsigned(met)].fetch_add(1, std::memory_order_relaxed);
	}

	// Delivers a taken list outside the mutex: a pin (error null) or the error, then drops the list's
	// reference. A waiter whose frame is gone (`abandoned`) is not resumed; the pin it was handed is
	// released here.
	void deliver(waiter* list, std::uint64_t gen, const std::exception_ptr& error) noexcept {
		for (waiter* w = list; w;) {
			waiter* next = w->next;
			w->gen = gen;
			w->error = error;
			const int was = w->st.exchange(waiter::delivered, std::memory_order_acq_rel);
			if (was == waiter::suspended) {
				const std::coroutine_handle<> h = w->h;
				const task::scheduler_ref sched = w->sched;
				if (sched) {
					sched.post(h);
				} else {
					h.resume();
				}
			} else if (was == waiter::abandoned && w->pins && !error) {
				gone_pins_.fetch_add(1, std::memory_order_relaxed);
				unpin(at(w->idx), w->idx);
			}
			w->release();
			w = next;
		}
	}

	// Removes a waiter no transition has taken from its list, uncounting its pin (a waiter's pin is
	// counted, never a holder's); the list's reference passes to the caller. False when it was taken,
	// so its delivery is under way.
	bool unlink_waiter(waiter* w) noexcept {
		{
			std::lock_guard l(*w->mu);
			if (!w->linked) { return false; }
			if (w->pins) {
				slot& s = at(w->idx);
				s.waiters.unlink(w);
				[[maybe_unused]] const std::uint64_t before = s.word.fetch_sub(word::one_pin, std::memory_order_acq_rel);
				assert(word::pins(before) > 0);
			} else {
				shut_list_.unlink(w);
			}
		}
		unlinked_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	// What a requested token does to a waiting pin: unlinked and uncounted, it is delivered task::cancelled.
	// A waiter a transition has taken is left to its delivery (the cancel lost the race).
	void cancel_wait(waiter* w) noexcept {
		hook_noexcept("cancel");
		if (!unlink_waiter(w)) { return; }
		cancels_.fetch_add(1, std::memory_order_relaxed);
		w->next = nullptr;
		deliver(w, 0, std::make_exception_ptr(task::cancelled{}));  // drops the list's reference
	}

	// A frame destroyed while it waited: unlinked if still listed; else its delivery has run (the pin it
	// was handed is released here) or will find it abandoned and release the pin. A waiter that never
	// joined a list (the pin did not wait, or threw first), or that ~catalog orphaned, leaves `c` alone.
	static void abandon(catalog* c, waiter* w) noexcept {
		if (!w->mu) {
		} else if (c->unlink_waiter(w)) {
			w->release();  // the list's
		} else if (w->st.exchange(waiter::abandoned, std::memory_order_acq_rel) == waiter::delivered && w->pins && !w->error) {
			c->gone_pins_.fetch_add(1, std::memory_order_relaxed);
			c->unpin(c->at(w->idx), w->idx);
		}
		w->release();
	}

	// `*w`: the waiter a waiting pin joins with, made before the compare-exchange that counts it, so a
	// failed allocation changes nothing; null for a synchronous pin, which never waits.
	acquired acquire_impl(const request& r, handle hint, waiter** w) {
		acquired a;
		if (shutting_.load(std::memory_order_acquire)) { return {outcome::failed, 0, 0, std::make_exception_ptr(catalog_closed())}; }
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
				const bool create = r.intent == open_intent::create, resident = r.intent == open_intent::resident;
				bool sets_create = false;
				for (;;) {
					const slot_state st = word::state(cur);
					if (word::full(cur)) { throw too_many_open("strata: a slot has no pin left"); }
					std::uint64_t next = 0;
					outcome out = outcome::granted;
					bool joins = false;
					sets_create = false;
					switch (st) {
					case slot_state::open:
						next = word::add_pin(cur, r.use);
						break;
					case slot_state::owed:
						if (r.kind == pin_kind::chore) {
							next = word::add_pin(cur, r.use);
							break;
						}
						if (std::exception_ptr gate = wake_gate(s)) {
							return {outcome::failed, idx, 0, std::move(gate)};
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
						if (resident && st == slot_state::closing) { return {outcome::none, idx, 0, nullptr}; }
						if (sync) {
							waited_[unsigned(st)].fetch_add(1, std::memory_order_relaxed);
							return {outcome::would_wait, idx, 0, nullptr};
						}
						next = word::add_pin(cur, r.use);
						out = outcome::waiting;
						joins = true;
						sets_create = create && (st == slot_state::opening || st == slot_state::closing);
						break;
					case slot_state::failed:
						if (resident) { return {outcome::none, idx, 0, nullptr}; }
						// A create pin passes the negative entry of a missing index (F1); any other error gates it as every pin.
						if (exec_now() < s.retry_at && !(create && s.absent)) {
							failed_fast_.fetch_add(1, std::memory_order_relaxed);
							return {outcome::failed, idx, 0, s.error};
						}
						next = word::next_gen(word::with_state(sync ? word::add_use(cur, r.use) : word::add_pin(cur, r.use), slot_state::opening));
						out = sync ? outcome::would_wait : outcome::waiting;
						joins = !sync;
						post_open = true;
						sets_create = true;
						retries_.fetch_add(1, std::memory_order_relaxed);
						break;
					case slot_state::free:
						assert(false && "a slot in the table is not free");
						return {outcome::failed, idx, 0, nullptr};
					}
					if (joins && !*w) { *w = new waiter; }
					hook("pin.key", true);
					if (!s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
						post_open = post_wake = false;
						continue;
					}
					if (st != word::state(next)) { note(st, word::state(next)); }
					if (sets_create) { s.create = st == slot_state::failed ? create : s.create || create; }
					if ((st == slot_state::open || st == slot_state::owed) && s.due.load(std::memory_order_relaxed)) { release_due(s); }
					a.out = out;
					a.idx = idx;
					a.gen = word::gen(next);
					if (joins) { join(s, *w, r, idx, st); }
					break;
				}
			} else {
				if (r.intent == open_intent::resident) { return {outcome::none, 0, 0, nullptr}; }
				if (!sync && !*w) { *w = new waiter; }
				// Counted before shutdown is checked, both sequentially consistent, as shutdown sets its flag before
				// it reads the count: either this miss sees shutdown, or shutdown sees this slot (B1).
				occupied_.fetch_add(1, std::memory_order_seq_cst);
				hook("pin.counted", true);
				if (shutting_.load(std::memory_order_seq_cst)) {
					occupied_.fetch_sub(1, std::memory_order_relaxed);
					lock.unlock();
					maybe_complete_shutdown();
					return {outcome::failed, 0, 0, std::make_exception_ptr(catalog_closed())};
				}
				try {
					idx = alloc_slot();
				} catch (...) {
					occupied_.fetch_sub(1, std::memory_order_relaxed);
					lock.unlock();
					maybe_complete_shutdown();
					throw;
				}
				slot& s = at(idx);
				try {
					s.key = std::string(r.key);
					s.key_hash = h;
					sh.table.insert(h, idx);
				} catch (...) {
					s.key.clear();
					free_slot(idx);
					occupied_.fetch_sub(1, std::memory_order_relaxed);
					lock.unlock();
					maybe_complete_shutdown();
					throw;
				}
				// A reused slot starts clean: nothing of its last key's backoff or use (I3-N3).
				s.failures.store(0, std::memory_order_relaxed);
				s.streak = 0;
				s.error = nullptr;
				s.retry_at = 0;
				s.probe_at.store(0, std::memory_order_relaxed);
				s.stuck.store(false, std::memory_order_relaxed);
				s.retrying.store(false, std::memory_order_relaxed);
				s.create = r.intent == open_intent::create, s.attempt_create = s.absent = s.replacing = false;
				s.fences = 0, s.replaced_once = false;
				s.holdup.store(0, std::memory_order_relaxed);
				s.last_use.store(exec_now(), std::memory_order_relaxed);
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
				if (!sync) { join(s, *w, r, idx, slot_state::opening); }
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
			if (word::full(cur)) { throw too_many_open("strata: a slot has no pin left"); }
			hook("pin.handle", false);
			if (s.word.compare_exchange_weak(cur, word::add_pin(cur, r.use), std::memory_order_acq_rel, std::memory_order_acquire)) {
				handle_pins_.fetch_add(1, std::memory_order_relaxed);
				if (s.due.load(std::memory_order_relaxed)) [[unlikely]] { release_due(s); }
				a = {outcome::granted, hint.slot, hint.gen, nullptr};
				return true;
			}
		}
	}

	void unpin(slot& s, std::uint32_t idx) noexcept {
		s.last_use.store(exec_now(), std::memory_order_relaxed);
		hook("unpin", false);
		[[maybe_unused]] const std::uint64_t before = s.word.fetch_sub(word::one_pin, std::memory_order_release);
		assert(word::pins(before) > 0);
		// A pin that ends in `closing` is a holder of a replace's drain (waiters hold no granted pin there).
		if (word::state(before) == slot_state::closing) {
			drain_unpin(s, idx);
		} else if (word::pins(before) == 1) {
			kick(idx);
		}
	}
	// A holder's unpin in a drain: the acquire pairs with the exchange that began the drain (a release), so the
	// count it stored is seen; the last holder posts the replace close.
	void drain_unpin(slot& s, std::uint32_t idx) noexcept {
		(void)s.word.load(std::memory_order_acquire);
		hook("drain.unpin", false);
		drain_unpins_.fetch_add(1, std::memory_order_relaxed);
		if (s.drain.fetch_sub(1, std::memory_order_acq_rel) == 1) { post([this, idx] { run_replace(idx); }); }
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
	// Puts a slot back at the tail of `q` unless it is in a queue, `gen` moved on (closed or reused), or it is closing or free.
	// Invariant: a slot is queued only while its state is not closing or free, as read under `policy_mu_`; `finish_close` unlinks
	// under it after the state is `closing`, so a requeue is either refused here or unlinked after. A close that ends at its prior
	// state requeues the slot itself (`run_close`).
	void policy_requeue(std::uint32_t idx, std::uint64_t gen, unsigned q) {
		std::lock_guard l(policy_mu_);
		const std::uint64_t w = at(idx).word.load(std::memory_order_acquire);
		const slot_state st = word::state(w);
		if (at(idx).q == 0 && word::gen(w) == gen && st != slot_state::closing && st != slot_state::free) { push_tail_locked(idx, q); }
	}

	// What the closing caps still ask for, in closes: the open slots over `soft_open` and the descriptors' and vnodes'
	// shares, each less the closes begun or booked (relief already posted: a refused close books none).
	std::uint64_t closing_over() const noexcept {
		const auto sub = [](std::uint64_t x, std::uint64_t by) { return x > by ? x - by : 0; };
		// Each cap is read before the relief posted against it, and the open count before the closing count (a close's end
		// lowers them in that order), so a race over-counts and never ends the pass short; the loads are statements because
		// the order of a call's arguments is unspecified.
		const std::uint64_t descriptors = share(resource::descriptors), pending_desc = pending_desc_.load(std::memory_order_relaxed);
		const std::uint64_t vnodes = share(resource::vnodes), pending_vnodes = pending_vnodes_.load(std::memory_order_relaxed);
		std::uint64_t over = sub(descriptors, pending_desc) + sub(vnodes, pending_vnodes);
		if (h_.soft_open != 0) {
			const std::uint64_t live = live_.load(std::memory_order_relaxed), closing = closing_.load(std::memory_order_relaxed);
			const std::uint64_t booked = pending_open_.load(std::memory_order_relaxed);
			over += sub(sub(live, closing + booked), h_.soft_open);
		}
		return over;
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

	// Closes slots in S3-FIFO order while a closing cap is over (`closing_over()`). A candidate used since it entered is
	// not taken: small moves to main with the frequency cleared, main decrements. A hot one is demoted for the close it
	// books (B2); a warm one is closed. Whatever is pinned, in a transition or used since is put back, its generation checked.
	void evict(const policy_limits& lim, policy_result& r, std::uint64_t& posts) {
		std::uint64_t skipped = 0;
		const std::uint64_t now = exec_now();
		for (std::uint32_t idx; r.visited < lim.visits && posts < lim.transitions && closing_over() != 0;) {
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
			if (now < s.probe_at.load(std::memory_order_relaxed)) {  // refused lately, or a failed commit's probe: not again yet
				busy_.fetch_add(1, std::memory_order_relaxed), ++r.busy;
				policy_requeue(idx, gen, q);
			} else if (st == slot_state::open && word::pins(w) == 0 && s.counts.get(resource::maps) != 0) {  // hot: demote first, for the close it books
				if (begin_demote(idx, true, q == 1)) {
					++posts, ++r.demotes;
					evict_demotes_.fetch_add(1, std::memory_order_relaxed), pass_demotes_.fetch_add(1, std::memory_order_relaxed);
				}
				policy_requeue(idx, gen, q);
			} else {
				if (st == slot_state::open || st == slot_state::owed) {
					if (q == 1) { s.to_ghost.store(true, std::memory_order_relaxed); }
					if (begin_close(idx, true)) {
						++posts, ++r.closes;
						evictions_.fetch_add(1, std::memory_order_relaxed), pass_closes_.fetch_add(1, std::memory_order_relaxed);
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

	// --- transitions ---------------------------------------------------------------------------

	void run_open(std::uint32_t idx) {
		slot& s = at(idx);
		std::exception_ptr err;
		opening op{s.key, io_counts{h_.budget, &counts_, &s.counts}, false};
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			op.create = s.attempt_create = s.create;
		}
		try {
			s.io = h_.resolve(op);
			s.index = std::make_unique<Index>(h_.open_index(*s.io, h_.index_options, op));
			s.index->guard_calls(&s.word, &pin_held);
			s.reader = s.index->reader();
			h_.open_payload(op, *s.index, s.payload);
			if (!s.payload) { throw std::logic_error("strata: open_payload did not make H"); }
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
	static bool is_absent(const std::exception_ptr& e) {
		if (!e) { return false; }
		try {
			std::rethrow_exception(e);
		} catch (const index_not_created&) {
			return true;
		} catch (...) {
			return false;
		}
	}

	void finish_open(std::uint32_t idx, const std::exception_ptr& err) {
		slot& s = at(idx);
		waiter* list;
		std::uint64_t gen;
		const bool absent = is_absent(err);
		if (!err) { policy_enqueue(idx); }
		{
			std::unique_lock lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == slot_state::opening);
			if (absent && s.create && !s.attempt_create) {  // a create pinner joined after the attempt began: run it again with the flag
				create_reposts_.fetch_add(1, std::memory_order_relaxed);
				lock.unlock();
				post([this, idx] { run_open(idx); });
				return;
			}
			const bool created = s.attempt_create;
			s.create = s.attempt_create = false;
			s.absent = absent && !created;  // a create pin passes only the entry of an attempt that did not create
			if (err) {
				if (s.failures.load(std::memory_order_relaxed) != UINT8_MAX) { s.failures.fetch_add(1, std::memory_order_relaxed); }
				s.error = err;
				s.retry_at = exec_now() + catalog_detail::backoff_ns(s.failures.load(std::memory_order_relaxed));
			} else {
				s.failures.store(0, std::memory_order_relaxed);
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
		kick(idx);
	}

	// The wake's gate (D5; CT-R34, CT-R40), the one place that decides whether a request pin may start a
	// wake: null, or the wake_failed a failed wake's backoff fails it with. Under the shard mutex. The
	// bound does not gate: the sweep closes a slot at it (begin_bound_close).
	std::exception_ptr wake_gate(const slot& s) {
		if (s.streak == 0 || exec_now() >= s.retry_at) { return nullptr; }
		failed_fast_.fetch_add(1, std::memory_order_relaxed);
		wake_gated_.fetch_add(1, std::memory_order_relaxed);
		return s.error;
	}

	// Splits a taken list: the request waiters stay in `list` (counted in the result, to fail), the
	// chore waiters go to `chores`, keeping their pins.
	static std::uint64_t split_requests(waiter*& list, waiter*& chores) noexcept {
		std::uint64_t n = 0;
		waiter *req = nullptr, *tail = nullptr;
		for (waiter* x = list; x;) {
			waiter* next = x->next;
			if (x->kind == pin_kind::request) {
				++n;
				x->next = req, req = x;
			} else {
				x->next = nullptr;
				(tail ? tail->next : chores) = x;
				tail = x;
			}
			x = next;
		}
		list = req;
		return n;
	}

	// A state the transition ends at, handing the waiters their pins, or `waking` when the end is
	// `owed` and a request waiter needs the wake (it waits for the wake's end, as does a chore waiter).
	// The wake's gate applies to that composed edge too: inside a failed wake's backoff the end is
	// `owed`, the request waiters fail with the gate's error and lose their pins in the same
	// compare-exchange, and the chore waiters are handed theirs.
	void end_at(std::uint32_t idx, slot_state from, slot_state target) {
		slot& s = at(idx);
		waiter *list = nullptr, *chores = nullptr;
		std::uint64_t gen;
		bool wake = false, close_due = false;
		std::exception_ptr gate;
		slot_state to = target;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == from);
			assert(from != slot_state::demoting || s.streak == 0);  // a demotion starts only from `{0, open}`: no backoff
			bool request_waiter = false;
			s.waiters.each([&](const waiter& w) { request_waiter |= w.kind == pin_kind::request; });
			if (target == slot_state::owed && request_waiter) {
				gate = wake_gate(s);
				wake = !gate;
			}
			to = wake ? slot_state::waking : target;
			std::uint64_t failed = 0;
			if (!wake) {
				list = take(s);
				if (gate) { failed = split_requests(list, chores); }
			}
			if (from == slot_state::demoting) {  // still `demoting`: no close can begin and take the booking meanwhile
				pending_maps_.fetch_sub(s.pend_maps.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
				pending_hot_.fetch_sub(1, std::memory_order_relaxed);
				// Its waiters take the slot, or the demotion left maps (a view held): the booked close will not come.
				if (word::pins(cur) != failed || s.counts.get(resource::maps) != 0) { release_due(s); }
				close_due = s.due.load(std::memory_order_relaxed) != 0;
			}
			for (;;) {
				hook("transition.end", true);
				const std::uint64_t next = word::with_pins(word::with_state(cur, to), word::pins(cur) - failed);
				if (s.word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
			}
			gen = word::gen(cur);
			note(from, to);
			if (from == slot_state::closing) { unbook(s); }
		}
		if (close_due && !stopped_.load(std::memory_order_acquire) && !shutting_.load(std::memory_order_acquire)) {
			// The booked close begins here, with no wait for a pass to come around to the slot (B5); a pin that took the
			// slot released the booking, and any other refusal leaves it for the next pass.
			if (begin_due_close(idx)) {
				pass_closes_.fetch_add(1, std::memory_order_relaxed);
			} else {
				post_pass();
			}
		}
		if (wake) {
			post_wake_chore(idx);
		} else {
			deliver(list, gen, gate);
			if (chores) { deliver(chores, gen, nullptr); }
			kick(idx);
		}
	}

	void post_wake_chore(std::uint32_t idx) {
		wakes_.fetch_add(1, std::memory_order_relaxed);
		post([this, idx] { run_wake(idx); });
	}

	// Runs `fn` as a chore after `delay_ns`, through the host's timer, which only posts it. A timer is
	// counted until its callback has posted; a timer that throws is not counted and the error reaches the
	// caller. A post that throws in the callback calls `failed` with its error there instead (R2), or, with
	// none, reaches the host's timer.
	void after(std::uint64_t delay_ns, std::function<void()> fn, std::function<void(std::exception_ptr)> failed = {}) {
		{
			std::lock_guard l(timers_mu_);
			++timers_;
		}
		try {
			h_.timer(delay_ns, [this, fn = std::move(fn), failed = std::move(failed)]() mutable {
				try {
					hook("timer.post", false);
					post(std::move(fn));
				} catch (...) {
					if (!failed) {
						timer_end();
						throw;
					}
					failed(std::current_exception());
				}
				timer_end();
			});
		} catch (...) {
			timer_end();
			throw;
		}
	}
	// after(), or a post at once when the host's timer throws (a reopen's or a replace's chore must not be lost).
	void after_or_post(std::uint64_t delay_ns, std::function<void()> fn) {
		try {
			after(delay_ns, fn);
		} catch (...) {
			post(std::move(fn));
		}
	}
	// The callback's last touch of the catalog: the destructor, waiting on the same mutex, frees it after.
	void timer_end() {
		std::lock_guard l(timers_mu_);
		--timers_;
		hook("timer.end", true);
		timers_cv_.notify_all();
	}

	// The wake (D5): promote() under chore_scope, never waiting. A busy lease (a chore pin's caller holds
	// it) reposts through the timer, 1 ms doubling to 64 ms, counted and outside the failure bound. The
	// scope ends before the final compare-exchange, so a resumed request acquires from free (CT-R40).
	void run_wake(std::uint32_t idx, std::uint64_t delay = repost_first_ns) {
		slot& s = at(idx);
		std::exception_ptr err;
		bool fenced = false;
		{
			const chore_scope chore{s.index->lease()};
			try {
				if (!chore) {
					reposts_.fetch_add(1, std::memory_order_relaxed);
					after(delay, [this, idx, next = std::min(delay * 2, repost_cap_ns)] { run_wake(idx, next); },
						[this, idx](std::exception_ptr e) noexcept { finish_wake(idx, e); });  // an unposted repost fails the wake
					return;
				}
				s.index->promote();
			} catch (const index_fenced&) {
				fenced = s.index->fenced();  // the wake cannot succeed on a fenced index; a fresh one can
				if (!fenced) { err = std::current_exception(); }
			} catch (...) {  // a timer that throws fails the wake, so no slot is left `waking`
				err = std::current_exception();
			}
		}
		if (fenced) {
			replace_from(idx, slot_state::waking);
			return;
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
		std::exception_ptr failure;  // built before the mutex: an allocation failure keeps the cause's own error
		if (err) {
			try {
				hook("wake.failure", false);
				failure = std::make_exception_ptr(wake_failed(err));
			} catch (...) {
				failure = std::current_exception();
			}
		}
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == slot_state::waking);
			list = take(s);
			std::uint64_t w = 0;
			if (err) {  // request waiters fail, chore waiters keep their pins
				w = split_requests(list, chores);
				if (overload(err)) {
					wake_overloads_.fetch_add(1, std::memory_order_relaxed);
				} else {
					if (s.failures.load(std::memory_order_relaxed) != UINT8_MAX) { s.failures.store(std::uint8_t(s.failures.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed); }
					wake_failures_.fetch_add(1, std::memory_order_relaxed);
				}
				if (s.streak != UINT8_MAX) { ++s.streak; }
				s.error = failure;
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
		kick(idx);
	}

	// A finished background merge's output stays unsynced until a commit, and warm needs it durable: when that is
	// all that is uncommitted, the host's commit chore runs under the chore's lease (a failure leaves the slot as it
	// is, to try again on a later pass). Called with the lease held by the chore.
	void commit_merge_output(slot& s) {
		if (!s.index->merge_output_unsynced()) { return; }
		merge_commits_.fetch_add(1, std::memory_order_relaxed);
		try {
			h_.commit(*s.payload, *s.index);
		} catch (...) {
			merge_commit_failures_.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// The demotion chore, under `demoting`: demote() makes the twins and its publication frees the views no
	// reader holds; a map still counted after it is a retired view a reader kept, and reclaim(0) frees
	// the ones released since (CT-R39).
	void run_demote(std::uint32_t idx) {
		slot& s = at(idx);
		bool owed = false, fenced = false;
		s.index->check_in();  // publishes finished merges and frees released views, so warm_quiescent() can hold (F6)
		{
			const chore_scope chore{s.index->lease()};
			if (chore) {
				hook("chore.demote", false);
				try {
					if (s.index->fenced()) {
						fenced = true;
					} else {
						commit_merge_output(s);
						owed = s.index->demote() && s.index->promote_owed();
						if (s.counts.get(resource::maps) != 0) { (void)s.index->reclaim(0); }
					}
				} catch (const index_fenced&) {
					fenced = s.index->fenced();
				} catch (...) {
				}
			}
		}
		if (fenced) {
			replace_from(idx, slot_state::demoting);
			return;
		}
		if (s.counts.get(resource::maps) != 0) { hold_off(s); }  // still hot: not again before the next period
		end_at(idx, slot_state::demoting, owed ? slot_state::owed : slot_state::open);
	}

	// A transition the slot refused (a close not ready, a demotion that left maps) is not tried again before the
	// next period: a pass that reposts while over a cap would otherwise retry it at once, forever, and keep its
	// group from ever being idle (B4's hang on Linux). The period counts from the pass that chose the slot, so the
	// next periodic pass, armed when that one ended, tries it again (N6).
	void hold_off(slot& s) noexcept {
		const std::uint64_t now = exec_now(), p = pass_at_.load(std::memory_order_relaxed);
		const std::uint64_t t = (p != 0 && p <= now && now - p < h_.sweep_period_ns ? p : now) + h_.sweep_period_ns;
		if (s.probe_at.load(std::memory_order_relaxed) < t) { s.probe_at.store(t, std::memory_order_relaxed); }
	}

	void clear_failing(slot& s) noexcept {
		if (s.commit_failed.exchange(false, std::memory_order_relaxed)) { failing_.fetch_sub(1, std::memory_order_relaxed); }
	}

	enum class close_mode { sweep, bound, shutdown, replace };
	struct close_depth {
		close_depth() noexcept { ++close_depth_; }
		~close_depth() { --close_depth_; }
	};

	// The close chore, under `closing`. The sweep's closes need close_ready() and a maps count of zero; a queued
	// reclaim is posted (it returns at once) and the slot ends at its prior state to close on a later pass
	// (CT-R4). At the wake's bound it first commits what is uncommitted with the host's commit chore, and
	// a failed commit ends at `owed` with a probe time. At shutdown it commits, then closes whatever the
	// state; a failed commit names the key and closes uncommitted. A close() that throws, or leaves
	// closing() true, does not strand the slot (I3-N6). At shutdown a busy lease retries 1 ms doubling to
	// 64 ms, and a throwing close() 64 ms apart until it does not.
	void run_close(std::uint32_t idx, slot_state prior, close_mode mode = close_mode::sweep, std::uint64_t delay = repost_first_ns) {
		slot& s = at(idx);
		const close_depth in_chore;
		const bool bound = mode == close_mode::bound, down = mode == close_mode::shutdown;
		bool ready = false, commit_failed = false, reclaimed = false, fenced = false;
		{
			const chore_scope chore{s.index->lease()};
			if (!chore && down) {  // a chore pin's caller holds the lease at {0, ...}: try again shortly
				s.holdup.store(hold_lease, std::memory_order_relaxed);
				after(delay, [this, idx, prior, next = std::min(delay * 2, repost_cap_ns)] { run_close(idx, prior, close_mode::shutdown, next); });
				return;
			}
			if (down) { s.holdup.store(0, std::memory_order_relaxed); }
			if (chore) {
				hook("chore.close", false);
				try {
					if (!down && s.index->fenced()) {
						fenced = true;
					} else if (down) {
						ready = true;
						if (s.index->uncommitted()) {
							shutdown_commits_.fetch_add(1, std::memory_order_relaxed);
							try {
								h_.commit(*s.payload, *s.index);
							} catch (...) {
								shutdown_commit_failures_.fetch_add(1, std::memory_order_relaxed);
								hook("shutdown.name", false);
								std::lock_guard g(shut_mu_);
								result_.uncommitted.push_back(s.key);
							}
						}
					} else if (!bound || s.failures.load(std::memory_order_relaxed) >= h_.wake_bound) {  // an exchange after a reset re-checks the bound
						commit_merge_output(s);
						ready = s.index->close_ready();
						if (ready && prior == slot_state::owed && s.counts.get(resource::maps) != 0) {  // warm with leftover maps (a commit's): demote them away
							(void)s.index->demote();
							if (s.counts.get(resource::maps) != 0) { (void)s.index->reclaim(0); }
						}
						ready = ready && s.counts.get(resource::maps) == 0;
						if (!ready && bound) {
							if (s.index->uncommitted()) {
								bound_commits_.fetch_add(1, std::memory_order_relaxed);
								try {
									h_.commit(*s.payload, *s.index);
								} catch (...) {
									commit_failed = true;
								}
							}
							if (!commit_failed && s.counts.get(resource::maps) != 0) {  // maps from this commit or a chore pin's
								(void)s.index->demote();
								ready = s.index->close_ready() && s.counts.get(resource::maps) == 0;
							}
						}
						if (!ready && !commit_failed && s.index->reclaim_pending() != 0) {
							reclaims_.fetch_add(1, std::memory_order_relaxed);
							(void)s.index->reclaim();
							reclaimed = true;
						}
					}
				} catch (const index_fenced&) {
					fenced = s.index->fenced();
				} catch (...) {
				}
			}
		}
		if (fenced) {  // a fenced index is replaced, not kept (F4)
			run_replace(idx);
			return;
		}
		if (commit_failed) {
			commit_failures_.fetch_add(1, std::memory_order_relaxed);
			s.probe_at.store(exec_now() + commit_probe_ns, std::memory_order_relaxed);
			if (!s.commit_failed.exchange(true, std::memory_order_relaxed)) { failing_.fetch_add(1, std::memory_order_relaxed); }
		}
		if (!ready) {
			if (!reclaimed && !commit_failed) { hold_off(s); }  // refused: not again before the next period
			close_refused(idx, prior);
			return;
		}
		try {
			hook("close.call", false);
			s.index->close();
		} catch (...) {
			close_failures_.fetch_add(1, std::memory_order_relaxed);
			if (!s.index->closing()) {  // refused, nothing changed: the sweep tries again, shutdown after a pause
				if (!down) {
					hold_off(s);
					close_refused(idx, prior);
				} else {  // retried while shutting down, as a stuck close is (C25)
					s.holdup.store(hold_throws, std::memory_order_relaxed);
					after(repost_cap_ns, [this, idx, prior] { run_close(idx, prior, close_mode::shutdown); });
				}
				return;
			}
		}
		after_close(idx, mode);
	}

	// A fenced index leaves a chore's transition for `closing` (F4): `{p, demoting}` or `{p, waking}` to `{p, closing}`,
	// the replace close posted once the holders have drained. A demotion has none; a wake's are the pins that are not
	// waiters on the list, counted under the mutex (the waiters keep waiting and get the new index). The failure counts
	// toward neither the wake's backoff nor its bound.
	void replace_from(std::uint32_t idx, slot_state from) {
		slot& s = at(idx);
		bool post_close;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			assert(word::state(cur) == from);
			std::uint64_t waiters = 0;
			if (from == slot_state::waking) {
				s.waiters.each([&](const waiter&) { ++waiters; });
			}
			std::uint64_t holders;
			for (;;) {
				holders = from == slot_state::waking ? word::pins(cur) - waiters : 0;
				s.drain.store(std::uint32_t(holders), std::memory_order_relaxed);
				hook("replace.begin", true);
				if (s.word.compare_exchange_weak(cur, word::with_state(cur, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
			}
			hook("replace.exchanged", true);  // as reopen's
			note(from, slot_state::closing);
			if (from == slot_state::demoting) {  // the demotion ends here, as end_at ends one; the close's relief is booked in its place
				pending_maps_.fetch_sub(s.pend_maps.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
				pending_hot_.fetch_sub(1, std::memory_order_relaxed);
				release_due(s);
			}
			book(s, resource::descriptors);
			post_close = holders == 0;
		}
		if (post_close) { post([this, idx] { run_replace(idx); }); }
	}

	// The replace close (F4), under `closing` with no holder left: the host's commit chore if the index is not fenced
	// and has something uncommitted, then close() with no readiness check, then H and the index go and the slot
	// reopens in place for its waiters or frees. A busy lease retries through the timer, 1 ms doubling to 64 ms.
	void run_replace(std::uint32_t idx, std::uint64_t delay = repost_first_ns) {
		slot& s = at(idx);
		const close_depth in_chore;
		{
			std::lock_guard lock(shard_of(s.key_hash).mu);
			s.replacing = true;
		}
		{
			const chore_scope chore{s.index->lease()};
			if (!chore) {
				replace_reposts_.fetch_add(1, std::memory_order_relaxed);
				after_or_post(delay, [this, idx, next = std::min(delay * 2, repost_cap_ns)] { run_replace(idx, next); });
				return;
			}
			hook("chore.replace", false);
			try {
				if (!s.index->fenced() && s.index->uncommitted()) {
					try {
						h_.commit(*s.payload, *s.index);
					} catch (...) {
						replace_commit_failures_.fetch_add(1, std::memory_order_relaxed);
					}
				}
			} catch (...) {
			}
		}
		try {
			hook("close.call", false);
			s.index->close();
		} catch (...) {
			close_failures_.fetch_add(1, std::memory_order_relaxed);
			if (!s.index->closing()) {  // refused, nothing changed: try again shortly
				after_or_post(repost_cap_ns, [this, idx] { run_replace(idx); });
				return;
			}
		}
		replaces_.fetch_add(1, std::memory_order_relaxed);
		after_close(idx, close_mode::replace);
	}

	// A close the slot refused ends at its prior state, and the slot stays in the policy: the ghost mark goes and the
	// requeue follows the end (before it the slot is `closing` and the requeue refused). The generation is read while
	// the slot is still `closing` and ours, so a close another thread ends and a reopen after the end refuse the requeue.
	void close_refused(std::uint32_t idx, slot_state prior) {
		slot& s = at(idx);
		s.to_ghost.store(false, std::memory_order_relaxed);
		const std::uint64_t gen = word::gen(s.word.load(std::memory_order_acquire));
		end_at(idx, slot_state::closing, prior);
		hook("refuse.end", false);
		policy_requeue(idx, gen, 2);
	}

	// After close(): a closing() index (a view kept past its pin, a host bug) keeps the slot `closing`, is reported in
	// debug builds and counted, and the sweep retries try_finish_close(), as does shutdown on its own (B3); its pinners
	// stay suspended and no thread blocks (D5). Otherwise H and the index go in a chore of their own, off the close chore.
	void after_close(std::uint32_t idx, close_mode mode) {
		slot& s = at(idx);
		if (s.index->closing()) {
			s.stuck.store(true, std::memory_order_release);
			unbook(s);  // the relief does not come until the view goes
			stuck_closes_.fetch_add(1, std::memory_order_relaxed);
			detail::owner_break("strata: a closed index is kept open by a view held past its pin");
			if (mode == close_mode::shutdown || shutting_.load(std::memory_order_acquire)) { arm_stuck_retry(idx); }
			return;
		}
		if (mode == close_mode::bound) { bound_closes_.fetch_add(1, std::memory_order_relaxed); }
		clear_failing(s);
		post([this, idx] { retire(idx); });
	}

	// H, then the reader, the index and the IO go, outside the close chore (H may hold the host's group, and no
	// chore waits on its own group), and the slot ends (CT-R30).
	void retire(std::uint32_t idx) {
		slot& s = at(idx);
		s.payload.reset();
		s.reader = {};
		s.index.reset();
		s.io.reset();
		s.stuck.store(false, std::memory_order_relaxed);
		s.retrying.store(false, std::memory_order_relaxed);
		finish_close(idx);
	}

	// Shutdown's retry of a stuck close, 64 ms apart until the view goes, whether or not the sweep runs (B3).
	void arm_stuck_retry(std::uint32_t idx) {
		slot& s = at(idx);
		if (s.retrying.exchange(true, std::memory_order_acq_rel)) { return; }  // a pass's retry or ours is on its way
		try {
			after(repost_cap_ns, [this, idx] { run_stuck_retry(idx); });
		} catch (...) {
			s.retrying.store(false, std::memory_order_release);
		}
	}
	// The sweep's (or shutdown's) retry of a close that left closing() true.
	void run_stuck_retry(std::uint32_t idx) {
		slot& s = at(idx);
		bool done = false;
		{
			const chore_scope chore{s.index->lease()};
			if (chore) {
				try {
					done = s.index->try_finish_close();
				} catch (...) {
				}
			}
		}
		stuck_retries_.fetch_add(1, std::memory_order_relaxed);
		if (done) {
			post([this, idx] { retire(idx); });
			return;
		}
		s.retrying.store(false, std::memory_order_release);
		if (shutting_.load(std::memory_order_acquire)) { arm_stuck_retry(idx); }
	}

	// Close ends: `{0, closing}` to free, `gen` + 1, erased under the mutex; with pinners, `{p, opening}`,
	// `gen` + 1: reopened in place (CT-R30).
	void finish_close(std::uint32_t idx) {
		slot& s = at(idx);
		const std::uint32_t h = s.key_hash;
		bool reopen = false;
		std::uint64_t delay = 0;
		policy_unlink(idx);
		const bool ghost = s.to_ghost.exchange(false, std::memory_order_relaxed);
		std::uint64_t h64;
		{
			std::lock_guard lock(shard_of(h).mu);
			h64 = key_hash64(s.key);
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
			unbook(s);
			if (reopen && s.replacing) {
				// A fence within 10 s of the last replacement's open counts as a failed open: the reopen waits for the
				// backoff (10 ms doubling to 10 s). A quiet 10 s starts the count over.
				const std::uint64_t now = exec_now();
				const bool repeat = s.replaced_once && std::uint16_t(std::uint16_t(now >> time_unit_bits) - s.last_open) <= (fence_window_ns >> time_unit_bits);
				s.fences = repeat ? std::uint8_t(std::min<unsigned>(s.fences + 1u, 255u)) : 0;
				delay = s.fences == 0 ? 0 : catalog_detail::backoff_ns(s.fences);
				s.last_open = std::uint16_t((now + delay) >> time_unit_bits);
				s.replaced_once = true;
			}
			s.replacing = false;
			if (!reopen) {
				const auto eq = [&](std::uint32_t v) { return v == idx; };
				shard_of(h).table.erase(h, eq);
				s.key.clear();
			}
		}
		if (ghost) { policy_ghost(h64); }
		if (reopen) {
			if (delay != 0) {
				after_or_post(delay, [this, idx] { run_open(idx); });
			} else {
				post([this, idx] { run_open(idx); });
			}
		} else {
			free_slot(idx);
			maybe_complete_shutdown();
		}
	}

	// The sweep's erase of a negative entry (D5): `{0, failed}` to `free` once `negative_ttl_ns` has passed
	// its retry time, or at once when `force` (shutdown). Under the shard mutex, as the table entry goes
	// with it; a pin that took the slot first (its retry) wins, and one after the erase misses and opens anew.
	bool erase_failed(std::uint32_t idx, std::uint64_t now, bool force) {
		slot& s = at(idx);
		const std::uint32_t h = s.key_hash.load(std::memory_order_acquire);
		hook("erase.pre", false);
		{
			std::lock_guard lock(shard_of(h).mu);
			if (s.key_hash.load(std::memory_order_acquire) != h) { return false; }  // the slot went to a key of another shard
			std::uint64_t cur = s.word.load(std::memory_order_acquire);
			for (;;) {
				if (word::state(cur) != slot_state::failed || word::pins(cur) != 0) { return false; }
				if (!force && now < s.retry_at + h_.negative_ttl_ns) { return false; }
				hook("sweep.erase", true);
				if (s.word.compare_exchange_weak(cur, word::next_gen(word::make(0, slot_state::free, 0, word::gen(cur))), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
			}
			note(slot_state::failed, slot_state::free);
			erased_.fetch_add(1, std::memory_order_relaxed);
			const auto eq = [&](std::uint32_t v) { return v == idx; };
			shard_of(h).table.erase(h, eq);
			s.key.clear();
			s.error = nullptr;
		}
		free_slot(idx);
		maybe_complete_shutdown();
		return true;
	}

	// --- shutdown --------------------------------------------------------------------------------------

	// Called where a slot may have become closable while shutting down: an unpin to zero, a transition's end.
	void kick(std::uint32_t idx) noexcept {
		if (!shutting_.load(std::memory_order_acquire)) { return; }
		try {
			shutdown_slot(idx);
		} catch (...) {
		}
	}

	// `{0, open/owed}` to `{0, closing}` and the shutdown close, or the erase of a negative entry; a slot that is
	// pinned or in a transition is left to the event that ends it (an unpin, a transition's end).
	void shutdown_slot(std::uint32_t idx) {
		slot& s = at(idx);
		std::uint64_t w = s.word.load(std::memory_order_acquire);
		slot_state prior;
		for (;;) {
			prior = word::state(w);
			if (prior == slot_state::closing && s.stuck.load(std::memory_order_acquire)) {  // the sweep may be stopped
				arm_stuck_retry(idx);
				return;
			}
			if (word::pins(w) != 0) { return; }
			if (prior == slot_state::failed) {
				(void)erase_failed(idx, 0, true);
				return;
			}
			if (prior != slot_state::open && prior != slot_state::owed) { return; }
			hook("shutdown.close", false);
			if (s.word.compare_exchange_weak(w, word::with_state(w, slot_state::closing), std::memory_order_acq_rel, std::memory_order_acquire)) { break; }
		}
		note(prior, slot_state::closing);
		release_due(s);
		post([this, idx, prior] { run_close(idx, prior, close_mode::shutdown); });
	}

	bool join_shutdown(waiter* w) {
		w->sched = task::current();
		{
			std::lock_guard l(shut_mu_);
			if (shut_done_.load(std::memory_order_acquire)) { return true; }
			w->mu = &shut_mu_;
			w->retain();
			shut_list_.push(w);
		}
		if (!shutting_.exchange(true, std::memory_order_seq_cst)) {  // before the count is read (B1)
			const std::uint32_t n = slots_made();
			for (std::uint32_t i = 0; i < n; ++i) { kick(i); }
		}
		maybe_complete_shutdown();
		return false;
	}
	shutdown_result shutdown_outcome() {
		std::lock_guard l(shut_mu_);
		return result_;
	}
	// Once no slot is left (a free slot is on the free list), resumes the awaiting tasks.
	void maybe_complete_shutdown() {
		if (!shutting_.load(std::memory_order_acquire) || occupied_.load(std::memory_order_seq_cst) != 0) { return; }
		waiter* list;
		{
			std::lock_guard l(shut_mu_);
			if (shut_done_.load(std::memory_order_acquire)) { return; }
			shut_done_.store(true, std::memory_order_release);
			list = shut_list_.take();
		}
		deliver(list, 0, nullptr);
	}

	// --- the sweep -------------------------------------------------------------------------------------

	void run_pass() {
		bool again = false;
		try {
			again = pass();
		} catch (...) {
		}
		pass_queued_.store(false, std::memory_order_release);  // a crossing from here on posts its own
		arm_period();
		if (again) {
			pass_reposts_.fetch_add(1, std::memory_order_relaxed);
			post_pass();
		}
	}

	// One timer armed at a time; its callback only posts a pass, which re-arms it when it ends.
	void arm_period() {
		if (stopped_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire) || shut_done_.load(std::memory_order_acquire)) { return; }
		if (period_armed_.exchange(true, std::memory_order_acq_rel)) { return; }
		h_.timer(h_.sweep_period_ns, [l = life_] {
			std::lock_guard g(l->mu);
			if (l->c) {
				l->c->period_armed_.store(false, std::memory_order_release);
				l->c->post_pass();
			}
		});
	}

	// The part of `excess` this catalog holds, no less than 1 while the budget is over (D6: each catalog
	// evicts in proportion to its share of the count).
	std::uint64_t share(resource r) const noexcept {
		const std::uint64_t ex = h_.budget->excess(r), used = h_.budget->used(r);
		if (ex == 0 || used == 0) { return 0; }
		return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(double(ex) * double(counts_.get(r)) / double(used)));
	}

	// Called when the catalog is going: slots still holding an index are the host's bug (no shutdown()). Their
	// IO, with its lock and its reservations on the budget, is kept for good rather than destroyed (D5).
	void orphan_waiters() noexcept {
		const auto orphan = [](catalog_detail::wait_list& l) {
			for (waiter* w = l.take(); w;) {
				waiter* next = w->next;
				w->mu = nullptr;  // the awaiter's destructor then only drops its reference
				w->release();     // the list's
				w = next;
			}
		};
		{
			std::lock_guard l(shut_mu_);
			orphan(shut_list_);
		}
		const std::uint32_t n = slots_made();
		for (std::uint32_t i = 0; i < n; ++i) {
			slot& s = at(i);
			std::lock_guard l(shard_of(s.key_hash).mu);
			orphan(s.waiters);
		}
	}
	void release_chunks() noexcept {
		struct leaked_slot {  // H stays in its slot: the chunk is kept for good, and H need not move
			std::unique_ptr<kronuz::journal::IO> io;
			std::unique_ptr<Index> index;
			reader_type reader;
		};
		const std::uint32_t n = next_new_;
		bool pinned = false, open = false;
		std::vector<bool> keep;
		try {
			keep.assign((n + chunk_slots - 1) / chunk_slots, false);
			for (std::uint32_t i = 0; i < n; ++i) {
				slot& s = at(i);
				const std::uint64_t w = s.word.load(std::memory_order_acquire);
				if (word::state(w) == slot_state::free) { continue; }
				pinned |= word::pins(w) != 0;
				if (word::state(w) == slot_state::failed) { continue; }
				open = true;
				keep[i / chunk_slots] = true;
				auto k = std::make_shared<leaked_slot>();
				k->io = std::move(s.io);
				k->index = std::move(s.index);
				k->reader = std::move(s.reader);
				detail::keep_forever(std::shared_ptr<const void>(std::move(k)));
				for (auto& c : s.counts.n) { c.store(0, std::memory_order_relaxed); }
			}
		} catch (...) {
			open = true;  // out of memory: keep every chunk
			keep.assign(max_chunks, true);
		}
		if (pinned) { detail::owner_break("strata: a catalog destroyed with a pin held"); }
		if (open) {
			detail::owner_break("strata: a catalog destroyed without shutdown(): its open indexes are leaked");
			for (auto& c : counts_.n) { c.store(0, std::memory_order_relaxed); }
			counts_.holders_mapped.store(0, std::memory_order_relaxed);
		}
		for (std::size_t i = 0; i < chunks_.size(); ++i) {
			slot* p = chunks_[i].load(std::memory_order_relaxed);
			if (!p) { continue; }
			if (i < keep.size() && keep[i]) {
				try { detail::keep_forever(std::shared_ptr<const void>(static_cast<const void*>(p), [](const void*) {})); } catch (...) {}
			} else {
				delete[] p;
			}
		}
	}

	// What the budget's listener and the sweep's timer hold, so a callback that outlives the catalog finds it gone.
	struct life {
		explicit life(catalog* c_) : c(c_) {}
		std::mutex mu;
		catalog* c;
	};
	static inline thread_local int close_depth_ = 0;
	static constexpr std::uint8_t hold_lease = 1, hold_throws = 2;  // slot::holdup

	host h_;
	std::unique_ptr<executor::group> group_;
	std::shared_ptr<life> life_;
	resource_counts counts_;
	std::array<shard, shard_count> shards_;
	std::array<std::atomic<slot*>, max_chunks> chunks_;
	mutable std::mutex free_mu_;
	std::uint32_t free_head_ = none, next_new_ = 0;
	std::atomic<std::uint64_t> misses_{0}, opens_{0}, open_failures_{0}, failed_fast_{0}, retries_{0}, stale_{0}, handle_pins_{0}, wakes_{0}, reposts_{0};
	std::atomic<std::uint64_t> promoted_{0}, decayed_{0}, evictions_{0}, evict_demotes_{0}, busy_{0}, ghost_hits_{0}, ghost_adds_{0};
	// The policy: the queues (small = 1, main = 2) and the ghost under policy_mu_; the pass's own state.
	mutable std::mutex policy_mu_;
	std::uint32_t qhead_[3] = {none, none, none}, qtail_[3] = {none, none, none};
	std::uint64_t qsize_[3] = {0, 0, 0};
	ghost_set ghost_;
	std::uint64_t small_cap_ = ~std::uint64_t(0);
	std::atomic<std::uint64_t> wake_failures_{0}, wake_overloads_{0}, wake_gated_{0}, bound_closes_{0}, bound_commits_{0}, commit_failures_{0};
	std::atomic<std::uint64_t> passes_{0}, pass_demotes_{0}, pass_closes_{0}, pass_reposts_{0}, reclaims_{0}, erased_{0};
	std::atomic<std::uint64_t> stuck_closes_{0}, stuck_retries_{0}, close_failures_{0}, shutdown_commits_{0}, shutdown_commit_failures_{0};
	std::atomic<std::size_t> live_{0}, occupied_{0}, closing_{0};  // slots with an index; slots not free; slots closing
	std::atomic<std::uint64_t> pending_maps_{0}, pending_desc_{0}, pending_vnodes_{0};  // relief of demotions and closes begun and not ended
	std::atomic<std::uint64_t> pending_hot_{0}, pending_open_{0};  // hot indexes being demoted; closes booked by a demotion (B2)
	std::atomic<bool> started_{false}, stopped_{false}, period_armed_{false}, pass_queued_{false};
	std::atomic<bool> shutting_{false}, shut_done_{false};
	std::uint64_t listener_ = 0;
	std::atomic<std::uint64_t> pass_at_{0};  // when the last pass began
	std::uint32_t cursor_ = 0;  // the sweep's place in the slots; only a pass touches it
	std::mutex shut_mu_;        // shutdown's waiters and result
	catalog_detail::wait_list shut_list_;
	shutdown_result result_;
	std::atomic<std::uint64_t> merge_commits_{0}, merge_commit_failures_{0}, create_reposts_{0}, replaces_{0}, replace_commit_failures_{0}, drain_unpins_{0}, replace_reposts_{0};
	std::atomic<std::uint64_t> unlinked_{0}, gone_pins_{0}, cancels_{0};
	std::mutex timers_mu_;
	std::condition_variable timers_cv_;
	unsigned timers_ = 0;  // host timers armed and not yet fired
	std::atomic<std::size_t> failing_{0};
	std::atomic<std::uint64_t> waited_[slot_states] = {};
	std::atomic<std::uint64_t> edges_[slot_states][slot_states] = {};
};

}  // namespace strata
