// A synchronous host loop over one of a journal Host's turn drivers (standalone-turn-log D1, D3,
// D7): the driver seals each group's open record through TurnDriver::seal, and this class gives a
// blocking owner thread what run() and durable() give a coroutine loop. Its turn mutex is the
// driver's loop context: every driver and group-table call but unseal(), wake() and joined() runs
// under it, and the thread holding it adopts the loop's group table. After each turn it tells
// each group's watcher how far the log is durable, the step a watcher coroutine on durable()
// takes. Its loop holds each turn for the writers the last one released, counted per writer
// (rocksdb-parity D5's cohort wait), so a closed loop of writers rides one barrier each, writers
// sharing a group too.
#pragma once

#include "journal/host.h"
#include "journal/turn_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace strata {

// A failed turn (standalone-turn-log D9): its records, and every one staged after it, may or may
// not be durable. The log fails until the host reopens it, and every index on it fences.
struct outcome_unknown : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// A bound on the bytes a record of `payload` bytes takes in a turn, framing included: its logical
// bytes L (the 25-byte header and the payload), 126 bytes per block it crosses (a block header
// naming one group, 78; a fragment header, 24; a block tail's padding, at most 24), and one block
// header naming many groups, at most half a block (framing.h's manifest rule).
inline std::uint64_t turn_framed_bytes(std::uint64_t payload, std::uint32_t block) {
	constexpr std::uint64_t per_block = 126;
	const std::uint64_t logical = kronuz::journal::turn_log_detail::record_header + payload;
	return logical + (logical + block - per_block - 1) / (block - per_block) * per_block + block / 2;
}

class turn_sealer {
public:
	using record = kronuz::journal::TurnRecord;
	using sealer = kronuz::journal::TurnDriver::Sealer;
	// A group's watcher: its records through `sequence` are durable.
	using watcher = std::function<void(std::uint64_t sequence)>;

	// The host's loop `loop`, opened with `identity` and `geometry` (HostOptions::turns); its
	// segments must hold two of the largest commands with their framing, so every turn the driver
	// admits fits an empty segment (D3).
	turn_sealer(kronuz::journal::Host& host, std::uint32_t loop, const kronuz::journal::Identity& identity, const kronuz::journal::TurnLogOptions& geometry,
		std::uint64_t command_bytes)
		: host_(host), turns_(host.driver(loop)), identity_(identity), loop_(loop), block_(geometry.block_bytes),
		  window_(geometry.retain_segments ? (geometry.retain_segments - 1) * geometry.segment_bytes : 0) {
		if (block_ < 512 || geometry.segment_bytes < 2 * turn_framed_bytes(command_bytes + 32, block_) + turn_framed_bytes(4, block_)) {
			throw std::invalid_argument("strata: the turn log's segments are smaller than two commands");
		}
	}
	turn_sealer(const turn_sealer&) = delete;
	turn_sealer& operator=(const turn_sealer&) = delete;

	// Registers the group's sealer with the driver and its watcher here, replacing an earlier
	// pair; `owner` names them for unseal(). Returns the group's last durable sequence.
	std::uint64_t seal(std::uint64_t group, sealer fn, watcher done, const void* owner) {
		const auto turn = hold_loop();
		mine(group);
		const std::uint64_t staged = turns_.seal(group, std::move(fn), owner);
		const std::lock_guard lock(mu_);
		groups_[group] = {std::make_shared<watcher>(std::move(done)), owner, turns_.sequence(group)};
		return staged;
	}
	// Any thread: drops the group's pair if `owner` registered it, once no call of its sealer runs.
	void unseal(std::uint64_t group, const void* owner) noexcept {
		turns_.unseal(group, owner);
		const std::lock_guard lock(mu_);
		if (auto it = groups_.find(group); it != groups_.end() && it->second.owner == owner) { groups_.erase(it); }
	}
	// Recovery (D6): what the log kept of a group, from its latest checkpoint record, and whether
	// recovery calls it lost. release() drops the copy once the group's index replayed it.
	std::vector<record> records(std::uint64_t group) {
		const auto turn = hold_loop();
		return turns_.records(group);
	}
	bool lost(std::uint64_t group) {
		const auto turn = hold_loop();
		return turns_.lost(group);
	}
	void release(std::uint64_t group) {
		const auto turn = hold_loop();
		turns_.release(group);
	}

	// The group table (D7): membership lives in the host, never in the index.
	const kronuz::journal::Identity& identity() const noexcept { return identity_; }
	kronuz::journal::GroupStatus status(std::uint64_t group) {
		const auto turn = hold_loop();
		mine(group);
		return host_.status(group);
	}
	// A bounded group's bound (GroupStatus::bounded): the highest sequence damage to its log could
	// hold. An index whose head ends below it loses the group: lose(), durable when it returns.
	std::uint64_t bound(std::uint64_t group) {
		const auto turn = hold_loop();
		return host_.bound(group);
	}
	void lose(std::uint64_t group) {
		const auto turn = hold_loop();
		host_.lose(group);
	}
	// The table's checkpoint record of the group, once it closed: its sequence and payload.
	std::optional<std::pair<std::uint64_t, std::string>> table_record(std::uint64_t group) {
		const auto turn = hold_loop();
		const auto e = host_.entry(group);
		if (!e || e->manifest.empty()) { return std::nullopt; }
		return std::pair{e->base, e->manifest};
	}
	// Each is durable when it returns.
	void create(std::uint64_t group) {
		const auto turn = hold_loop();
		mine(group);
		host_.create_group(group);
	}
	void reopen(std::uint64_t group) {
		const auto turn = hold_loop();
		host_.reopen_group(group);
	}
	// Leaves the log: the table takes the group's last record, a durable checkpoint covering every
	// batch, and the driver forgets the group and its sealer.
	void close(std::uint64_t group, std::uint64_t sequence, std::string payload) {
		const auto turn = hold_loop();
		host_.close_group(group, sequence, std::move(payload));
		const std::uint64_t retired = turns_.retired(group);
		const std::lock_guard lock(mu_);
		groups_.erase(group);
		closed_[group] = retired;
		cv_.notify_all();  // its waiters fail with group_closed
	}
	// The last sequence a closed life of the group could hand out (TurnDriver::retired).
	std::uint64_t retired(std::uint64_t group) {
		const auto turn = hold_loop();
		return turns_.retired(group);
	}
	// Stages a record for the next turn (a reopened group's table record, D7).
	void stage(kronuz::journal::TurnRecord r) {
		const auto turn = hold_loop();
		turns_.stage(std::move(r));
		const std::lock_guard lock(mu_);
		woken_ = true;
		cv_.notify_all();
	}
	// The group recovered: stored, whatever the log holds (host.h's attach).
	void attach(std::uint64_t group) {
		const auto turn = hold_loop();
		host_.attach(group);
	}
	// The group's last durable sequence.
	std::uint64_t sequence(std::uint64_t group) {
		const auto turn = hold_loop();
		return turns_.sequence(group);
	}
	bool failed() const {
		const std::lock_guard lock(mu_);
		return bool(failure_);
	}
	// Any thread: a group holds a record to seal.
	void wake() noexcept {
		turns_.wake();
		const std::lock_guard lock(mu_);
		woken_ = true;
		cv_.notify_all();
	}
	// Any thread, after a writer's join (its transaction joined a record, `opened` it, or found it
	// full): an opened record wakes the loop, and a writer the last turn released has arrived (D5).
	void joined(bool opened) noexcept {
		owed& o = owed_here();
		if (o.log != this) {
			if (opened) { wake(); }
			return;
		}
		o.log = nullptr;
		if (opened) { turns_.wake(); }
		const std::lock_guard lock(mu_);
		woken_ = woken_ || opened;
		--owing_;
		if (o.turn == turns_written_) { ++arrivals_; }
		cv_.notify_all();
	}
	// Writes one turn of what the groups hold, if anything: false when there is nothing. A failed
	// turn fails the log (fail-stop, D9): every later turn and wait throws its failure.
	bool turn() {
		const auto turn = hold_loop();
		std::vector<std::pair<std::uint64_t, entry>> groups;
		std::size_t late = 0;
		{
			const std::lock_guard lock(mu_);
			if (failure_) { std::rethrow_exception(failure_); }
			woken_ = false;
			groups.assign(groups_.begin(), groups_.end());
			// Released writers not back as this turn starts: the last turn's, once its cohort wait
			// ran out, and any an earlier turn released.
			const std::size_t missing = released_ - std::min(arrivals_, released_);
			late = owing_ - std::min(missing, owing_) + (expired_ ? missing : 0);
		}
		const std::uint64_t before = turns_.log().last_turn();
		const auto start = std::chrono::steady_clock::now();
		try {
			if (turns_.idle()) { turns_.wake(); }  // asks the sealers: commit() then writes nothing when none seals
			turns_.commit();
		} catch (...) {
			const std::lock_guard lock(mu_);
			failure_ = std::current_exception();
			cv_.notify_all();
			throw;
		}
		if (turns_.log().last_turn() == before) { return false; }
		const auto barrier = std::chrono::steady_clock::now() - start;
		// The watchers first, then the durable sequences a wait reads: a wait returns after V moved.
		for (auto& [group, e] : groups) {
			const std::uint64_t s = turns_.sequence(group);
			if (s > e.durable) { (*e.done)(s); }
			e.durable = s;
		}
		const std::lock_guard lock(mu_);
		for (const auto& [group, e] : groups) {
			if (auto it = groups_.find(group); it != groups_.end() && it->second.done == e.done && it->second.durable != e.durable) {
				it->second.durable = e.durable;
				it->second.turn = turns_written_ + 1;
			}
		}
		last_barrier_ = barrier;
		late_ += late;
		++turns_written_;
		cohort_ = released_writers();
		owing_ += released_;
		arrivals_ = 0;
		expired_ = false;
		cv_.notify_all();
		return true;
	}
	// Returns once the group's records through `sequence` are durable and its watcher has run.
	// While a loop serves turns it waits for them; otherwise the caller writes them inline, as
	// journal::run's blocking entry does, rereading after each turn: another thread's may have
	// written them. A failed turn throws outcome_unknown (D9); a group that closes first, or a
	// sequence a closed life retired, throws group_closed, as durable() does.
	void await(std::uint64_t group, std::uint64_t sequence) {
		std::uint64_t released = 0;
		for (bool idle = false;;) {
			{
				std::unique_lock lock(mu_);
				if (failure_) { unknown(); }
				const auto it = groups_.find(group);
				if (it == groups_.end() || sequence <= retired_locked(group)) {
					if (it == groups_.end() && !closed_.contains(group)) { throw std::logic_error("strata: a turn-log wait on a group with no sealer"); }
					throw kronuz::journal::group_closed(group, sequence);
				}
				if (it->second.durable >= sequence) {
					acked_here() = {released ? released : it->second.turn, released != 0};
					return;
				}
				if (served_) {
					parked p(*this, {group, sequence});
					cv_.wait(lock, [&] {
						const auto at = groups_.find(group);
						return failure_ || !served_ || at == groups_.end() || sequence <= retired_locked(group) || at->second.durable >= sequence;
					});
					idle = false;
					p.owe();
					released = p.w.released;
					continue;
				}
				if (idle) { throw std::logic_error("strata: a turn-log wait with nothing to write"); }
			}
			idle = !inline_turn();
		}
	}
	// Returns once `ready()` holds, which a sealer's call makes true (a record handed to a turn):
	// woken by sealed() while a loop serves turns, else after turns written inline. `ready` reads
	// no lock of the group's. Throws as await() does.
	template <class Ready>
	void await_sealed(std::uint64_t group, Ready ready) {
		for (bool idle = false;;) {
			{
				std::unique_lock lock(mu_);
				if (failure_) { unknown(); }
				if (!groups_.contains(group)) { throw kronuz::journal::group_closed(group, 0); }
				if (ready()) { return; }
				if (served_) {
					parked p(*this, {group, 0, [](const void* r) { return (*static_cast<const Ready*>(r))(); }, &ready});
					cv_.wait(lock, [&] { return failure_ || !served_ || !groups_.contains(group) || ready(); });
					idle = false;
					p.owe();
					continue;
				}
				if (idle) { throw std::logic_error("strata: a turn-log wait with nothing to write"); }
			}
			idle = !inline_turn();
		}
	}
	// From a sealer that handed out a record: wakes await_sealed().
	void sealed() {
		const std::lock_guard lock(mu_);
		cv_.notify_all();
	}
	// Whether a loop serves the turns: writers then wait in await() rather than write turns.
	void served(bool on) {
		const std::lock_guard lock(mu_);
		served_ = on;
		cv_.notify_all();
	}
	// The loop: writes turns while records wait, until stop().
	void serve() {
		served(true);
		struct end {
			turn_sealer& s;
			~end() { s.served(false); }
		} ended{*this};
		for (;;) {
			while (turn()) {
				// D5: the next turn waits for the joins of the writers this one released, at
				// most 1/16 of its barrier, so they ride it rather than the one after.
				std::unique_lock lock(mu_);
				expired_ = !cv_.wait_for(lock, last_barrier_ / 16, [&] { return stopping_ || arrivals_ >= cohort_; });
			}
			std::unique_lock lock(mu_);
			cv_.wait(lock, [&] { return woken_ || stopping_; });
			if (stopping_) { return; }
		}
	}
	void stop() {
		const std::lock_guard lock(mu_);
		stopping_ = true;
		cv_.notify_all();
	}
	// Writers blocked in await() or await_sealed(), and the turns written (tests).
	std::size_t waiting() const {
		const std::lock_guard lock(mu_);
		return waiters_.size();
	}
	std::uint64_t turns_written() const {
		const std::lock_guard lock(mu_);
		return turns_written_;
	}
	// The released writers each written turn started without, summed (D5, tests): those its
	// cohort wait gave up on at its bound, and those an earlier turn released. A writer the turn
	// waited for and started without, its bound not run out, is never counted.
	std::uint64_t late_arrivals() const {
		const std::lock_guard lock(mu_);
		return late_;
	}
	// This thread's last await() (tests): the turn that made it durable, and whether that turn
	// released it from a wait (then it owed the next turn an arrival).
	struct ack {
		std::uint64_t turn = 0;
		bool released = false;
	};
	static ack last_ack() noexcept { return acked_here(); }
	std::uint32_t block_bytes() const noexcept { return block_; }
	// The retention window, retain_segments - 1 segments, and the groups that logged commands
	// since their last checkpoint, which share it (rocksdb-parity D2 item 2).
	std::uint64_t window_bytes() const noexcept { return window_; }
	std::uint64_t writers() const noexcept { return writers_.load(std::memory_order_relaxed); }
	void writing(bool on) noexcept { on ? writers_.fetch_add(1, std::memory_order_relaxed) : writers_.fetch_sub(1, std::memory_order_relaxed); }

private:
	// A waiter's own turn, when no loop serves them: false when it wrote nothing.
	bool inline_turn() {
		try {
			return turn();
		} catch (...) {
			const std::lock_guard lock(mu_);
			if (failure_) { unknown(); }
			throw;
		}
	}
	// A writer blocked in await() (its sequence) or await_sealed() (its predicate), listed under mu_,
	// and the turn that released it.
	struct waiter {
		std::uint64_t group, sequence;
		bool (*ready)(const void*) = nullptr;
		const void* arg = nullptr;
		std::uint64_t released = 0;
	};
	struct parked {
		turn_sealer& s;
		waiter w;
		parked(turn_sealer& to, waiter of) : s(to), w(of) { s.waiters_.push_back(&w); }
		~parked() { std::erase(s.waiters_, &w); }
		parked(const parked&) = delete;
		parked& operator=(const parked&) = delete;
		// Under mu_, once the wait returns: a released writer owes its turn an arrival.
		void owe() const noexcept {
			if (w.released) { owed_here() = {&s, w.released}; }
		}
	};
	// The arrival a writer's thread owes: its next join on `log` counts for `log`'s cohort while
	// `turn` is the last turn written.
	struct owed {
		const turn_sealer* log = nullptr;
		std::uint64_t turn = 0;
	};
	static owed& owed_here() noexcept {
		thread_local owed o;
		return o;
	}
	static ack& acked_here() noexcept {
		thread_local ack a;
		return a;
	}
	// Under mu_, at a turn's end: the waiting writers it released, each marked with the turn, in
	// released_, and the cohort the next turn waits for, one per writer. Each joins a record when
	// it returns (joined()); a writer in await_sealed() counts once its record sealed.
	std::size_t released_writers() {
		released_ = 0;
		for (waiter* w : waiters_) {
			const auto it = groups_.find(w->group);
			if (w->ready ? w->ready(w->arg) : it != groups_.end() && it->second.durable >= w->sequence) {
				w->released = turns_written_;
				++released_;
			}
		}
		return released_;
	}
	std::uint64_t retired_locked(std::uint64_t group) const {
		const auto it = closed_.find(group);
		return it == closed_.end() ? 0 : it->second;
	}
	// The turn mutex, held: the calling thread then owns the loop's group table, which the mutex
	// hands over after the last holder's table calls (journal host-ranges.md, the handoff).
	[[nodiscard]] std::unique_lock<std::mutex> hold_loop() {
		std::unique_lock turn(turn_mu_);
		host_.adopt_loop(loop_);
		return turn;
	}
	void mine(std::uint64_t group) const {
		if (host_.loop(group) != loop_) { throw std::invalid_argument("strata: the group belongs to another loop"); }
	}
	[[noreturn]] void unknown() const {
		try {
			std::rethrow_exception(failure_);
		} catch (const std::exception& e) {
			throw outcome_unknown(std::string("strata: the turn log failed: ") + e.what());
		} catch (...) {
			throw outcome_unknown("strata: the turn log failed");
		}
	}
	struct entry {
		std::shared_ptr<watcher> done;
		const void* owner = nullptr;
		std::uint64_t durable = 0;
		std::uint64_t turn = 0;  // the turn that last moved `durable`
	};
	kronuz::journal::Host& host_;
	kronuz::journal::TurnDriver& turns_;
	const kronuz::journal::Identity identity_;
	const std::uint32_t loop_, block_;
	const std::uint64_t window_;
	std::atomic<std::uint64_t> writers_{0};
	std::mutex turn_mu_;  // the driver's loop: one turn at a time, and every driver call but unseal and wake
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::map<std::uint64_t, entry> groups_;
	std::map<std::uint64_t, std::uint64_t> closed_;  // groups closed here, and what each retired
	std::exception_ptr failure_;
	std::vector<waiter*> waiters_;
	std::chrono::steady_clock::duration last_barrier_{};
	std::size_t cohort_ = 0, arrivals_ = 0;  // D5: the writers the last turn released, and those back since
	std::size_t released_ = 0, owing_ = 0;  // the writers the last turn released; all released, not back
	std::uint64_t turns_written_ = 0, late_ = 0;
	bool woken_ = false, served_ = false, stopping_ = false, expired_ = false;
};

}  // namespace strata
