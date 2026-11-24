// The catalog: the partition's non-cold indexes, their slots and pins (cold-tier D4, D5; CT-R13,
// CT-R15, CT-R29, CT-R30, CT-R31, CT-R34, CT-R35, CT-R41). Every call on a catalog index runs under
// a pin. A slot's 64-bit word `pins:24 | shut:1 | state:3 | freq:2 | gen:34` is the only place a pin, a state
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
inline int& payload_depth() noexcept {
	thread_local int depth = 0;
	return depth;
}
}  // namespace catalog_detail

// Marks the calling thread as running a slot payload's own work while it lives (D169): a
// replication group holds it around each item its chore runs. H holds its slot's index from
// open_payload to close_payload, which every close runs before index.close(), and its calls take
// the write lease the catalog's chores need, so the debug check that a call holds a pin (CT-R15)
// passes them as it passes a pin.
class payload_work {
public:
	payload_work() noexcept { ++catalog_detail::payload_depth(); }
	~payload_work() { --catalog_detail::payload_depth(); }
	payload_work(const payload_work&) = delete;
	payload_work& operator=(const payload_work&) = delete;
};

namespace catalog_detail {

// pins:24 | shut:1 | state:3 | freq:2 | gen:34, high to low. `shut`: shutdown met the slot pinned, so the unpin that
// takes it to `{0, open/owed}` begins its close in the same compare-exchange; sticky until the slot is free.
struct word {
	static constexpr unsigned gen_bits = 34, freq_shift = 34, state_shift = 36, pin_shift = 40;
	static constexpr std::uint64_t shut = std::uint64_t(1) << (state_shift + 3);
	static_assert(slot_states <= 8, "a state fits the word's three bits");
	static constexpr std::uint64_t gen_mask = (std::uint64_t(1) << gen_bits) - 1;
	static constexpr std::uint64_t one_pin = std::uint64_t(1) << pin_shift;
	static constexpr std::uint64_t one_freq = std::uint64_t(1) << freq_shift;
	static constexpr std::uint64_t max_pins = (std::uint64_t(1) << 24) - 1;

	static std::uint64_t pins(std::uint64_t w) noexcept { return w >> pin_shift; }
	// A pin past this would carry out of the field and wrap the count to zero.
	static bool full(std::uint64_t w) noexcept { return pins(w) == max_pins; }
	static slot_state state(std::uint64_t w) noexcept { return static_cast<slot_state>((w >> state_shift) & 7); }
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
	static std::uint64_t with_state(std::uint64_t w, slot_state s) noexcept { return (w & ~(std::uint64_t(7) << state_shift)) | std::uint64_t(s) << state_shift; }
	static std::uint64_t with_pins(std::uint64_t w, std::uint64_t p) noexcept { return (w & ~(~std::uint64_t(0) << pin_shift)) | p << pin_shift; }
	static std::uint64_t next_gen(std::uint64_t w) noexcept { return (w & ~gen_mask) | ((w + 1) & gen_mask); }
};

// One waiting pin or shutdown, on the heap (task's lock_waiter shape): its list and its awaiter each hold
// a reference, so a delivery never writes into a frame destroyed while it waited. A delivery before the
// suspension makes await_suspend decline; one to a suspended waiter holds `posting` while it posts the
// resume, and the awaiter's destruction waits through it; one that meets `abandoned` releases the pin it
// carries. docs/mechanisms/catalog-delivery.md has who may touch what in each state.
struct waiter {
	enum : int { registered, suspended, posting, delivered, abandoned };
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
		void operator()() const noexcept;
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
	static constexpr unsigned max_exponent = 15;  // a slot's 4-bit streak and fences: backoff_ns() is flat from 11

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
		// The relief a begun demotion (its maps) or close (descriptors and vnodes, in `counts.booked`) will bring:
		// booked in the catalog's pending counts, released when the transition ends, so a pass counts what is in
		// flight. A demotion for a closing cap books the close it leads to too (`due`), until the slot's close begins
		// or a pin takes it. Each booking and release follows a compare-exchange that excludes the others; a close's
		// booking falls with the counts it covers (resource_counts::booked).
		std::atomic<std::uint32_t> pend_maps{0};
		// The holders still to unpin in a replace's `closing`; the last posts the replace close (F4).
		std::atomic<std::uint32_t> drain{0};
		// The policy's queue links (under policy_mu_); `q_next` is also the free list's link while the slot is free,
		// when it is in no queue.
		std::uint32_t q_prev = none, q_next = none;
		std::uint16_t last_open = 0;  // when the last replacement's open was due, in 2^27 ns (134 ms) units modulo 2^16
		// Under the shard mutex, saturating at max_exponent: consecutive failed wakes of any kind (the backoff's
		// exponent); replacements within 10 s of each other (the reopen backoff's).
		std::uint8_t streak : 4 = 0, fences : 4 = 0;
		// A failed open's or a failed wake's count, saturating (the bound counts the wake's, not too_many_open). Read by
		// the sweep without the mutex.
		std::atomic<std::uint8_t> failures{0};
		std::atomic<bool> commit_failed{false};
		std::atomic<bool> to_ghost{false};  // the pass chose this close from the small FIFO: its hash enters the ghost when it ends
		std::uint8_t q = 0;                 // the policy's queue (under policy_mu_): 0 none, 1 small, 2 main
		// A close that left closing() true (a view kept past its pin): the sweep, and shutdown, retry try_finish_close().
		std::atomic<bool> stuck{false}, retrying{false};
		std::atomic<std::uint8_t> due{0};     // a demotion for a closing cap booked its close (see pend_maps)
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
		kronuz::journal::IO* io = nullptr;  // the key's IO once resolved (null in resolve): H may borrow it for the slot's life
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
		// H's close, run by every close just before the index's, on the close chore with no lease held, so it may
		// wait on work that takes the lease: a replication group's close, whose checkpoint covers what it applied
		// (D169; docs/mechanisms/group-catalog.md). From its return or throw H no longer uses the index; a throw
		// is counted (payload_close_failures), names the key at shutdown, and the index closes regardless. A close
		// refused after it (close() threw, nothing closed) calls it again. Default: nothing.
		std::function<void(H&, Index&)> close_payload = [](H&, Index&) {};
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
		std::uint64_t stuck_closes = 0, stuck_retries = 0, close_failures = 0, shutdown_commits = 0, shutdown_commit_failures = 0, payload_close_failures = 0;
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
		pin& operator=(pin&& o) noexcept;
		pin(const pin&) = delete;
		pin& operator=(const pin&) = delete;
		~pin() { release(); }

		// Valid for the pin's life only; a host keeps neither past it (I7).
		Index& index() const noexcept { return *s_->index; }
		H& payload() const noexcept { return *s_->payload; }
		const reader_type& reader() const noexcept { return s_->reader; }
		handle where() const noexcept { return {idx_, gen_}; }
		explicit operator bool() const noexcept { return c_ != nullptr; }
		void release() noexcept;

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
		~pin_awaiter();
		bool await_ready();
		bool await_suspend(std::coroutine_handle<> h) noexcept;
		pin await_resume();

	private:
		friend class catalog;
		pin_awaiter(catalog& c, std::string_view key, pin_kind kind, bool use, handle hint, open_intent intent, task::cancel_token stop);
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
		~shutdown_awaiter();
		bool await_ready();
		bool await_suspend(std::coroutine_handle<> h) noexcept;
		shutdown_result await_resume();

	private:
		friend class catalog;
		explicit shutdown_awaiter(catalog& c) : c_(&c) {}
		catalog* c_;
		waiter* w_ = nullptr;
	};

	explicit catalog(host h);
	catalog(const catalog&) = delete;
	catalog& operator=(const catalog&) = delete;
	// After shutdown(): every pin released and every chore finished. A catalog destroyed with slots open
	// asserts in debug builds and leaks their IO with its lock (D5), as ~basic_index leaks the lock; one
	// destroyed with a pin held asserts too, and that pin's release is a use after free. Waiters left (an
	// unfinished shutdown given up on) are orphaned, as task's ~event orphans its: their frames, destroyed
	// later, touch only their waiters, and a token requested later does nothing.
	~catalog();

	// --- the sweep and shutdown (D5, D6) --------------------------------------------------------------

	// Registers the budget's soft-cap listener and arms the sweep's timer.
	void start();
	// No pass is posted after this and the timer is not re-armed; a pass already queued runs.
	void stop_sweep();
	// Posts a pass unless one is queued or running: a crossing posts one however often the budget calls.
	void post_pass();
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
	policy_result policy_pass(const policy_limits& lim = {});

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
	[[nodiscard]] pin_awaiter pin_request(std::string_view key, bool use = true, open_intent intent = open_intent::open, task::cancel_token stop = {});
	[[nodiscard]] pin_awaiter pin_chore(std::string_view key, bool use = false, open_intent intent = open_intent::open, task::cancel_token stop = {});
	// A re-pin by handle: one compare-exchange while the slot is open and `gen` matches, else by key.
	[[nodiscard]] pin_awaiter repin(handle hint, std::string_view key, pin_kind kind = pin_kind::request, bool use = false, open_intent intent = open_intent::open, task::cancel_token stop = {});
	// Starts the open or the wake as a pin would and answers would_wait, holding no pin (CT-R35).
	try_result try_pin(std::string_view key, pin_kind kind = pin_kind::request, bool use = true, open_intent intent = open_intent::open);

	// A holder whose call threw index_fenced hands its pin back: the catalog replaces the index (F4). The slot
	// goes to `closing`, new pins by key wait and by handle fall back to the key, the other holders drain by
	// unpinning, the last one posts the close, and the waiters get a fresh index from the host's open_payload.
	// A pin on `waking` or on a drain already begun is a plain unpin.
	void reopen(pin&& p);

	// One entry of snapshot().
	struct slot_view {
		std::string key;
		slot_state state;
		std::uint64_t pins;
	};
	// Every slot that is not free, under its shard's mutex, with no access to an index (F6): FLUSH pins each
	// key `resident`, and diagnostics read the state counts. A slot freed or reused meanwhile is skipped.
	std::vector<slot_view> snapshot() const;

	// --- the sweep's compare-exchanges (item 5 replaces the chores; the word's rows are final) ------

	// `{0, open}` to `{0, demoting}` and a demotion chore. False when a pin or a transition holds it. `due`: the
	// demotion is for a closing cap, and the close it leads to is booked as relief (B2).
	bool begin_demote(std::uint32_t idx, bool due = false, bool ghost = false);
	// `{0, open/owed}` to `{0, closing}` and a close chore; the chore ends at the prior state unless the
	// slot's maps count is zero and close_ready() (the sweep reads fields only after its compare-exchange).
	// `unused`: also only while the frequency is clear, decided by the same compare-exchange (the policy's).
	bool begin_close(std::uint32_t idx, bool unused = false);
	// The close a demotion booked, begun at the demotion's end or by a pass; it carries the ghost mark the booking holds.
	bool begin_due_close(std::uint32_t idx);
	// The policy's reset of a promoted slot's frequency (D6): a fetch-op on the frequency only.
	void clear_freq(std::uint32_t idx);
	// `{0, owed}` to `{0, closing}` for a slot whose wake failed `wake_bound` times (D5; the sweep's call,
	// after the probe time of a failed commit), and a close chore that commits first if anything is
	// uncommitted. False when a pin or a transition holds it or the count is below the bound. The chore
	// ends at `owed` when the commit fails (the probe time is set, failing() counts it) or the slot is
	// not ready after it; the sweep reads the count only as a hint, the chore checks it after the exchange.
	bool begin_bound_close(std::uint32_t idx);
	// The policy's decrement (D6): the frequency down by one, no lower than zero.
	void decay(std::uint32_t idx);

	// --- the closing policy: S3-FIFO and the hot cap (D6; CT-R10, CT-R31, CT-R41) ----------------

	struct queue_sizes_t {
		std::uint64_t small = 0, main = 0, ghost = 0;
	};
	queue_sizes_t queue_sizes() const;
	// The queue a slot is in: 0 none, 1 small, 2 main.
	unsigned queue_of(std::uint32_t idx) const;
	std::uint64_t small_cap() const noexcept { return small_cap_; }
	std::uint64_t ghost_capacity() const noexcept { return ghost_.capacity(); }
	// The ghost's bytes now (nodes and table); under the policy mutex.
	std::uint64_t ghost_bytes() const;

	// --- observation (tests, the sweep, T4) ---------------------------------------------------

	std::uint64_t misses() const noexcept { return misses_.load(std::memory_order_relaxed); }
	std::uint32_t slots_made() const noexcept;
	std::uint64_t word_of(std::uint32_t idx) const noexcept { return at(idx).word.load(std::memory_order_acquire); }
	static std::uint64_t pins_of(std::uint64_t w) noexcept { return word::pins(w); }
	static slot_state state_of(std::uint64_t w) noexcept { return word::state(w); }
	static unsigned freq_of(std::uint64_t w) noexcept { return word::freq(w); }
	static std::uint64_t gen_of(std::uint64_t w) noexcept { return word::gen(w); }
	std::uint64_t last_use_of(std::uint32_t idx) const noexcept { return at(idx).last_use.load(std::memory_order_relaxed); }
	std::size_t waiters_of(std::uint32_t idx) const;
	std::string key_at(std::uint32_t idx) const;
	std::size_t table_size() const;
	stats_t stats() const noexcept;
	const resource_counts& counts() const noexcept { return counts_; }
	// Slots whose bound close failed to commit and have not closed or committed since (D5).
	std::size_t failing() const noexcept { return failing_.load(std::memory_order_relaxed); }
	// Slots not free, and slots with an index (opening to closing): what the soft_open cap counts.
	std::size_t occupied() const noexcept { return occupied_.load(std::memory_order_relaxed); }
	std::size_t open_slots() const noexcept { return live_.load(std::memory_order_relaxed); }
	// Relief of the demotions and closes begun and not ended (maps; descriptors; vnodes).
	std::uint64_t pending_maps() const noexcept { return pending_maps_.load(std::memory_order_relaxed); }
	std::uint64_t pending_descriptors() const noexcept { return counts_.booking(resource::descriptors); }
	std::uint64_t pending_vnodes() const noexcept { return counts_.booking(resource::vnodes); }
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
	std::vector<shutdown_holder> shutdown_report() const;
	// The size of a slot, for a test that holds it to D4's figure.
	static constexpr std::size_t slot_bytes() noexcept { return sizeof(slot); }
	// Sets a slot's pin count (its other fields kept), for a test that needs the cap.
	void set_pins_for_test(std::uint32_t idx, std::uint64_t pins) noexcept;
	// A slot's index, for a test that holds the slot in a state; null unless built.
	Index* index_of(std::uint32_t idx) noexcept { return at(idx).index.get(); }
	// Lets the posted chores of a finished test run to their end.
	void wait_chores() { group_->wait(); }

private:
	friend class pin;
	friend class pin_awaiter;

	static std::function<void(const opening&, Index&, std::optional<H>&)> default_payload();
	// `resident` is a chore pin's.
	static open_intent checked(pin_kind kind, open_intent intent);
	static std::array<char, 16> fresh_uuid();
	// The debug check behind Index::guard_calls (CT-R15): a call is made under a pin, inside a transition, or by
	// the slot's payload (payload_work).
	static bool pin_held(std::uint64_t w) noexcept;

	slot& at(std::uint32_t i) const noexcept { return chunks_[i / chunk_slots].load(std::memory_order_acquire)[i % chunk_slots]; }
	shard& shard_of(std::uint32_t h) const noexcept { return const_cast<shard&>(shards_[h & (shard_count - 1)]); }
	void hook(const char* site, bool locked) const;
	void hook_noexcept(const char* site) const noexcept;
	void note(slot_state from, slot_state to) noexcept;
	// Books a begun demotion's maps and hot index, or a begun close's descriptors and vnodes, as relief in flight.
	void book(slot& s, resource r) noexcept;
	// A close's descriptors and vnodes: what the slot holds now, falling with its counts as its index lets them go,
	// so closing_over() never subtracts relief for counts already dropped (resource_counts::booked).
	void book_close(slot& s) noexcept;
	void unbook_close(slot& s) noexcept;
	void unbook(slot& s) noexcept;
	// A demotion for a closing cap (B2): the close it leads to counts as relief from now on.
	void book_due(slot& s, bool ghost) noexcept;
	// The booked close is begun (its own booking follows) or will not come (a pin took the slot).
	void release_due(slot& s) noexcept;
	void post(std::function<void()> f);

	std::uint32_t alloc_slot();
	void free_slot(std::uint32_t i);

	waiter* take(slot& s) noexcept { return s.waiters.take(); }

	// Registers the request as a waiter on the slot it counted itself on (under the shard mutex); the
	// list takes a reference.
	void join(slot& s, waiter* w, const request& r, std::uint32_t idx, slot_state met);

	// Delivers a taken list outside the mutex: a pin (error null) or the error, then drops the list's
	// reference. A suspended waiter is `posting` until its resume is posted, so its frame's destruction
	// returns only after the post; one whose frame is gone (`abandoned`) is not resumed, and the pin it
	// was handed is released here.
	void deliver(waiter* list, std::uint64_t gen, const std::exception_ptr& error) noexcept;

	// Removes a waiter no transition has taken from its list, uncounting its pin (a waiter's pin is
	// counted, never a holder's); the list's reference passes to the caller. False when it was taken,
	// so its delivery is under way. Its counts go in under the mutex: once it is released, the transition's end may
	// close the slot and shutdown end the catalog.
	bool unlink_waiter(waiter* w, bool cancel = false) noexcept;

	// What a requested token does to a waiting pin: unlinked and uncounted, it is delivered task::cancelled.
	// A waiter a transition has taken is left to its delivery (the cancel lost the race).
	void cancel_wait(waiter* w) noexcept;

	// A frame destroyed while it waited: unlinked if still listed; else its delivery has run (the pin it
	// was handed is released here), is posting its resume (waited for, then as run), or will find it
	// abandoned and release the pin. Once it returns, no delivery touches the waiter's scheduler. A waiter
	// that never joined a list (the pin did not wait, or threw first), or that ~catalog orphaned, leaves `c` alone.
	static void abandon(catalog* c, waiter* w) noexcept;

	// `*w`: the waiter a waiting pin joins with, made before the compare-exchange that counts it, so a
	// failed allocation changes nothing; null for a synchronous pin, which never waits.
	acquired acquire_impl(const request& r, handle hint, waiter** w);

	// The by-handle fast path: one compare-exchange while open and `gen` matches (no mutex).
	bool try_fast(handle hint, const request& r, acquired& a);

	// The compare-exchange that ends the last pin is the unpin's last touch of the catalog, or begins the shutdown
	// close: once the slot is `{0, open/owed}`, shutdown may close it and the host destroy the catalog.
	void unpin(slot& s, std::uint32_t idx) noexcept;
	// The catalog's own soft caps post a pass once a full pass's worth over, net of the relief in flight: on the period
	// alone, each second's misses and maps pile up above them (T3); one pass per excess slot costs throughput. One at a time.
	void over_soft() noexcept;
	// A holder's unpin in a drain: the acquire pairs with the exchange that began the drain (a release), so the
	// count it stored is seen; the last holder posts the replace close.
	void drain_unpin(slot& s, std::uint32_t idx) noexcept;
	std::uint64_t exec_now() const { return h_.exec->now(); }
	// A miss that counted itself and made no slot takes the count back in a chore: it may be the count shutdown
	// waits on last, and ~catalog waits for chores, not for this thread. Without memory for the chore, inline.
	void uncount_miss() noexcept;


	// --- the policy's queues (under policy_mu_) ---------------------------------------------------

	static std::uint64_t key_hash64(std::string_view key) noexcept { return ::wordwise::hash(key.data(), key.size()); }
	void push_tail_locked(std::uint32_t idx, unsigned q);
	void unlink_locked(std::uint32_t idx);
	// An open slot enters the small FIFO, or the main one when its hash is in the ghost (before its open ends).
	void policy_enqueue(std::uint32_t idx);
	// A slot leaves the queue it is in (a close ends).
	void policy_unlink(std::uint32_t idx);
	// The ghost remembers the hash of a key the pass closed from the small FIFO, its frequency clear.
	void policy_ghost(std::uint64_t h);
	// Puts a slot back at the tail of `q` unless it is in a queue, `gen` moved on (closed or reused), or it is closing or free.
	// Invariant: a slot is queued only while its state is not closing or free, as read under `policy_mu_`; `finish_close` unlinks
	// under it after the state is `closing`, so a requeue is either refused here or unlinked after. A close that ends at its prior
	// state requeues the slot itself (`run_close`).
	void policy_requeue(std::uint32_t idx, std::uint64_t gen, unsigned q);

	// What the closing caps still ask for, in closes: the open slots over `soft_open` and the descriptors' and vnodes'
	// shares, each less the closes begun or booked (relief already posted: a refused close books none).
	std::uint64_t closing_over() const noexcept;
	// The next candidate: the small FIFO's head while it is over its share, else the main FIFO's, taken out
	// of its queue; false when both are empty. `round`: the queued slots, for the caller's bound.
	bool pick(std::uint32_t& idx, std::uint64_t& gen, unsigned& q, std::uint64_t& round);

	// Closes slots in S3-FIFO order while a closing cap is over (`closing_over()`). A candidate used since it entered is
	// not taken: small moves to main with the frequency cleared, main decrements. A hot one is demoted for the close it
	// books (B2); a warm one is closed. Whatever is pinned, in a transition or used since is put back, its generation checked.
	void evict(const policy_limits& lim, policy_result& r, std::uint64_t& posts);

	// --- transitions ---------------------------------------------------------------------------

	void run_open(std::uint32_t idx);

	// Open ends: `{p, opening}` to `{p, open}` handing every waiter its pin, or `{0, failed}`, `gen` + 1,
	// with the waiters failed and their pins removed in the same compare-exchange (CT-R29, CT-R34).
	static bool is_absent(const std::exception_ptr& e);

	void finish_open(std::uint32_t idx, const std::exception_ptr& err);

	// The wake's gate (D5; CT-R34, CT-R40), the one place that decides whether a request pin may start a
	// wake: null, or the wake_failed a failed wake's backoff fails it with. Under the shard mutex. The
	// bound does not gate: the sweep closes a slot at it (begin_bound_close).
	std::exception_ptr wake_gate(const slot& s);

	// Splits a taken list: the request waiters stay in `list` (counted in the result, to fail), the
	// chore waiters go to `chores`, keeping their pins.
	static std::uint64_t split_requests(waiter*& list, waiter*& chores) noexcept;

	// A state the transition ends at, handing the waiters their pins, or `waking` when the end is
	// `owed` and a request waiter needs the wake (it waits for the wake's end, as does a chore waiter).
	// The wake's gate applies to that composed edge too: inside a failed wake's backoff the end is
	// `owed`, the request waiters fail with the gate's error and lose their pins in the same
	// compare-exchange, and the chore waiters are handed theirs.
	void end_at(std::uint32_t idx, slot_state from, slot_state target);

	void post_wake_chore(std::uint32_t idx);

	// Runs `fn` as a chore after `delay_ns`, through the host's timer, which only posts it. A timer is
	// counted until its callback has posted; a timer that throws is not counted and the error reaches the
	// caller. A post that throws in the callback calls `failed` with its error there instead (R2), or, with
	// none, reaches the host's timer.
	void after(std::uint64_t delay_ns, std::function<void()> fn, std::function<void(std::exception_ptr)> failed = {});
	// after(), or a post at once when the host's timer throws (a reopen's or a replace's chore must not be lost).
	void after_or_post(std::uint64_t delay_ns, std::function<void()> fn);
	// The callback's last touch of the catalog: the destructor, waiting on the same mutex, frees it after.
	void timer_end();

	// The wake (D5): promote() under chore_scope, never waiting. A busy lease (a chore pin's caller holds
	// it) reposts through the timer, 1 ms doubling to 64 ms, counted and outside the failure bound. The
	// scope ends before the final compare-exchange, so a resumed request acquires from free (CT-R40).
	void run_wake(std::uint32_t idx, std::uint64_t delay = repost_first_ns);

	static bool overload(const std::exception_ptr& e);

	// Wake ends: `{p, waking}` to `{p, open}` with every waiter handed its pin; failed, `{p - w, owed}`
	// with the `w` request waiters failed and the chore waiters handed theirs (CT-R29, CT-R40). The
	// failure's fields are written before the exchange, under the mutex, so the next request pin that
	// meets `owed` sees the backoff; a success resets the count and the backoff.
	void finish_wake(std::uint32_t idx, const std::exception_ptr& err);

	// A finished background merge's output stays unsynced until a commit, and warm needs it durable: when that is
	// all that is uncommitted, the host's commit chore runs under the chore's lease (a failure leaves the slot as it
	// is, to try again on a later pass). Called with the lease held by the chore.
	void commit_merge_output(slot& s);

	// The demotion chore, under `demoting`: demote() makes the twins and its publication frees the views no
	// reader holds; a map still counted after it is a retired view a reader kept, and reclaim(0) frees
	// the ones released since (CT-R39).
	void run_demote(std::uint32_t idx);

	// A transition the slot refused (a close not ready, a demotion that left maps) is not tried again before the
	// next period: a pass that reposts while over a cap would otherwise retry it at once, forever, and keep its
	// group from ever being idle (B4's hang on Linux). The period counts from the pass that chose the slot, so the
	// next periodic pass, armed when that one ended, tries it again (N6).
	void hold_off(slot& s) noexcept;

	void clear_failing(slot& s) noexcept;

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
	void run_close(std::uint32_t idx, slot_state prior, close_mode mode = close_mode::sweep, std::uint64_t delay = repost_first_ns);

	// A fenced index leaves a chore's transition for `closing` (F4): `{p, demoting}` or `{p, waking}` to `{p, closing}`,
	// the replace close posted once the holders have drained. A demotion has none; a wake's are the pins that are not
	// waiters on the list, counted under the mutex (the waiters keep waiting and get the new index). The failure counts
	// toward neither the wake's backoff nor its bound.
	void replace_from(std::uint32_t idx, slot_state from);

	// The replace close (F4), under `closing` with no holder left: the host's commit chore if the index is not fenced
	// and has something uncommitted, then close() with no readiness check, then H and the index go and the slot
	// reopens in place for its waiters or frees. A busy lease retries through the timer, 1 ms doubling to 64 ms.
	void run_replace(std::uint32_t idx, std::uint64_t delay = repost_first_ns);

	// H's close before the index's, outside the lease (host::close_payload): a throw is counted and, at shutdown,
	// names the key; the index closes regardless.
	void close_payload(slot& s, bool down) noexcept;

	// A close the slot refused ends at its prior state, and the slot stays in the policy: the ghost mark goes and the
	// requeue follows the end (before it the slot is `closing` and the requeue refused). The generation is read while
	// the slot is still `closing` and ours, so a close another thread ends and a reopen after the end refuse the requeue.
	void close_refused(std::uint32_t idx, slot_state prior);

	// After close(): a closing() index (a view kept past its pin, a host bug) keeps the slot `closing`, is reported in
	// debug builds and counted, and the sweep retries try_finish_close(), as does shutdown on its own (B3); its pinners
	// stay suspended and no thread blocks (D5). Otherwise H and the index go in a chore of their own, off the close chore.
	void after_close(std::uint32_t idx, close_mode mode);

	// H, then the reader, the index and the IO go, outside the close chore (H may hold the host's group, and no
	// chore waits on its own group), and the slot ends (CT-R30).
	void retire(std::uint32_t idx);

	// Shutdown's retry of a stuck close, 64 ms apart until the view goes, whether or not the sweep runs (B3).
	void arm_stuck_retry(std::uint32_t idx);
	// The sweep's (or shutdown's) retry of a close that left closing() true.
	void run_stuck_retry(std::uint32_t idx);

	// Close ends: `{0, closing}` to free, `gen` + 1, erased under the mutex; with pinners, `{p, opening}`,
	// `gen` + 1: reopened in place (CT-R30).
	void finish_close(std::uint32_t idx);

	// The sweep's erase of a negative entry (D5): `{0, failed}` to `free` once `negative_ttl_ns` has passed
	// its retry time, or at once when `force` (shutdown). Under the shard mutex, as the table entry goes
	// with it; a pin that took the slot first (its retry) wins, and one after the erase misses and opens anew.
	bool erase_failed(std::uint32_t idx, std::uint64_t now, bool force);

	// --- shutdown --------------------------------------------------------------------------------------

	// Called where a slot may have become closable while shutting down: a transition's end, and shutdown's start.
	void kick(std::uint32_t idx) noexcept;

	// `{0, open/owed}` to `{0, closing}` and the shutdown close, or the erase of a negative entry; a pinned slot is
	// marked `shut` for its last unpin, and one in a transition is left to the transition's end.
	void shutdown_slot(std::uint32_t idx);
	// After the compare-exchange to `{0, closing}`: the post is the last touch, as the close may end the catalog.
	void begin_shutdown_close(slot& s, std::uint32_t idx, slot_state prior);

	bool join_shutdown(waiter* w);
	shutdown_result shutdown_outcome();
	// Once no slot is left (a free slot is on the free list), resumes the awaiting tasks.
	void maybe_complete_shutdown();

	// --- the sweep -------------------------------------------------------------------------------------

	void run_pass();

	// One timer armed at a time; its callback only posts a pass, which re-arms it when it ends.
	void arm_period();

	// The part of `excess` this catalog holds, no less than 1 while the budget is over (D6: each catalog
	// evicts in proportion to its share of the count).
	std::uint64_t share(resource r) const noexcept;

	// Called when the catalog is going: slots still holding an index are the host's bug (no shutdown()). Their
	// IO, with its lock and its reservations on the budget, is kept for good rather than destroyed (D5).
	void orphan_waiters() noexcept;
	void release_chunks() noexcept;

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
	std::atomic<std::uint64_t> stuck_closes_{0}, stuck_retries_{0}, close_failures_{0}, shutdown_commits_{0}, shutdown_commit_failures_{0}, payload_close_failures_{0};
	std::atomic<std::size_t> live_{0}, occupied_{0}, closing_{0};  // slots with an index; slots not free; slots closing
	std::atomic<std::uint64_t> pending_maps_{0};  // relief of demotions begun and not ended (closes': counts_.booked)
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

// The member definitions, one header per mechanism (docs/mechanisms/catalog-layout.md).
#include "strata/core/catalog_table.hh"
#include "strata/core/catalog_pin.hh"
#include "strata/core/catalog_transitions.hh"
#include "strata/core/catalog_policy.hh"
#include "strata/core/catalog_observe.hh"
