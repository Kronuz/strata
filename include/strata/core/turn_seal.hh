// A synchronous host loop over one of a journal Host's turn drivers (standalone-turn-log D1, D3,
// D7): the driver seals each group's open record through TurnDriver::seal, and this class gives a
// blocking owner thread what run() and durable() give a coroutine loop. Its turn mutex is the
// driver's loop context: every driver and group-table call but unseal() and wake() runs under it.
// After each turn it tells each group's watcher how far the log is durable, the step a watcher
// coroutine on durable() takes.
#pragma once

#include "journal/host.h"
#include "journal/turn_driver.h"

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
		: host_(host), turns_(host.driver(loop)), identity_(identity), loop_(loop), block_(geometry.block_bytes) {
		if (block_ < 512 || geometry.segment_bytes < 2 * turn_framed_bytes(command_bytes + 32, block_) + turn_framed_bytes(4, block_)) {
			throw std::invalid_argument("strata: the turn log's segments are smaller than two commands");
		}
	}
	turn_sealer(const turn_sealer&) = delete;
	turn_sealer& operator=(const turn_sealer&) = delete;

	// Registers the group's sealer with the driver and its watcher here, replacing an earlier
	// pair; `owner` names them for unseal(). Returns the group's last durable sequence.
	std::uint64_t seal(std::uint64_t group, sealer fn, watcher done, const void* owner) {
		const std::lock_guard turn(turn_mu_);
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
		const std::lock_guard turn(turn_mu_);
		return turns_.records(group);
	}
	bool lost(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		return turns_.lost(group);
	}
	void release(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		turns_.release(group);
	}

	// The group table (D7): membership lives in the host, never in the index.
	const kronuz::journal::Identity& identity() const noexcept { return identity_; }
	kronuz::journal::GroupStatus status(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		mine(group);
		return host_.status(group);
	}
	// The table's checkpoint record of the group, once it closed: its sequence and payload.
	std::optional<std::pair<std::uint64_t, std::string>> table_record(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		const auto* e = host_.entry(group);
		if (!e || e->manifest.empty()) { return std::nullopt; }
		return std::pair{e->base, e->manifest};
	}
	// Each is durable when it returns.
	void create(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		mine(group);
		host_.create_group(group);
	}
	void reopen(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		host_.reopen_group(group);
	}
	// Leaves the log: the table takes the group's last record, a durable checkpoint covering every
	// batch, and the driver forgets the group and its sealer.
	void close(std::uint64_t group, std::uint64_t sequence, std::string payload) {
		const std::lock_guard turn(turn_mu_);
		host_.close_group(group, sequence, std::move(payload));
		const std::uint64_t retired = turns_.retired(group);
		const std::lock_guard lock(mu_);
		groups_.erase(group);
		closed_[group] = retired;
		cv_.notify_all();  // its waiters fail with group_closed
	}
	// The last sequence a closed life of the group could hand out (TurnDriver::retired).
	std::uint64_t retired(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		return turns_.retired(group);
	}
	// Stages a record for the next turn (a reopened group's table record, D7).
	void stage(kronuz::journal::TurnRecord r) {
		const std::lock_guard turn(turn_mu_);
		turns_.stage(std::move(r));
		const std::lock_guard lock(mu_);
		woken_ = true;
		cv_.notify_all();
	}
	// The group recovered: stored, whatever the log holds (host.h's attach).
	void attach(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		host_.attach(group);
	}
	// The group's last durable sequence.
	std::uint64_t sequence(std::uint64_t group) {
		const std::lock_guard turn(turn_mu_);
		return turns_.sequence(group);
	}
	bool failed() const {
		const std::lock_guard lock(mu_);
		return bool(failure_);
	}
	// Any thread, under the group's record mutex too: an open record took its first transaction.
	void wake() noexcept {
		turns_.wake();
		const std::lock_guard lock(mu_);
		woken_ = true;
		cv_.notify_all();
	}
	// Writes one turn of what the groups hold, if anything: false when there is nothing. A failed
	// turn fails the log (fail-stop, D9): every later turn and wait throws its failure.
	bool turn() {
		const std::lock_guard turn(turn_mu_);
		std::vector<std::pair<std::uint64_t, entry>> groups;
		{
			const std::lock_guard lock(mu_);
			if (failure_) { std::rethrow_exception(failure_); }
			woken_ = false;
			groups.assign(groups_.begin(), groups_.end());
		}
		const std::uint64_t before = turns_.log().last_turn();
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
		// The watchers first, then the durable sequences a wait reads: a wait returns after V moved.
		for (auto& [group, e] : groups) {
			const std::uint64_t s = turns_.sequence(group);
			if (s > e.durable) { (*e.done)(s); }
			e.durable = s;
		}
		const std::lock_guard lock(mu_);
		for (const auto& [group, e] : groups) {
			if (auto it = groups_.find(group); it != groups_.end() && it->second.done == e.done) { it->second.durable = e.durable; }
		}
		cv_.notify_all();
		return true;
	}
	// Returns once the group's records through `sequence` are durable and its watcher has run.
	// While a loop serves turns it waits for them; otherwise the caller writes them inline, as
	// journal::run's blocking entry does. A failed turn throws outcome_unknown (D9); a group that
	// closes first, or a sequence a closed life retired, throws group_closed, as durable() does.
	void await(std::uint64_t group, std::uint64_t sequence) {
		for (;;) {
			{
				std::unique_lock lock(mu_);
				if (failure_) { unknown(); }
				const auto it = groups_.find(group);
				if (it == groups_.end() || sequence <= retired_locked(group)) {
					if (it == groups_.end() && !closed_.contains(group)) { throw std::logic_error("strata: a turn-log wait on a group with no sealer"); }
					throw kronuz::journal::group_closed(group, sequence);
				}
				if (it->second.durable >= sequence) { return; }
				if (served_) {
					++waiting_;
					cv_.wait(lock, [&] {
						const auto at = groups_.find(group);
						return failure_ || !served_ || at == groups_.end() || sequence <= retired_locked(group) || at->second.durable >= sequence;
					});
					--waiting_;
					continue;
				}
			}
			bool wrote = false;
			try {
				wrote = turn();
			} catch (...) {
				const std::lock_guard lock(mu_);
				if (failure_) { unknown(); }
				throw;
			}
			if (!wrote) { throw std::logic_error("strata: a turn-log wait with nothing to write"); }
		}
	}
	// Returns once `ready()` holds, which a sealer's call makes true (a record handed to a turn):
	// woken by sealed() while a loop serves turns, else after turns written inline. `ready` reads
	// no lock of the group's. Throws as await() does.
	template <class Ready>
	void await_sealed(std::uint64_t group, Ready ready) {
		for (;;) {
			{
				std::unique_lock lock(mu_);
				if (failure_) { unknown(); }
				if (!groups_.contains(group)) { throw kronuz::journal::group_closed(group, 0); }
				if (ready()) { return; }
				if (served_) {
					++waiting_;
					cv_.wait(lock, [&] { return failure_ || !served_ || !groups_.contains(group) || ready(); });
					--waiting_;
					continue;
				}
			}
			bool wrote = false;
			try {
				wrote = turn();
			} catch (...) {
				const std::lock_guard lock(mu_);
				if (failure_) { unknown(); }
				throw;
			}
			if (!wrote && !ready()) { throw std::logic_error("strata: a turn-log wait with nothing to write"); }
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
			while (turn()) {}
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
	// Writers blocked in await() (tests).
	std::size_t waiting() const {
		const std::lock_guard lock(mu_);
		return waiting_;
	}
	std::uint32_t block_bytes() const noexcept { return block_; }

private:
	std::uint64_t retired_locked(std::uint64_t group) const {
		const auto it = closed_.find(group);
		return it == closed_.end() ? 0 : it->second;
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
	};
	kronuz::journal::Host& host_;
	kronuz::journal::TurnDriver& turns_;
	const kronuz::journal::Identity identity_;
	const std::uint32_t loop_, block_;
	std::mutex turn_mu_;  // the driver's loop: one turn at a time, and every driver call but unseal and wake
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::map<std::uint64_t, entry> groups_;
	std::map<std::uint64_t, std::uint64_t> closed_;  // groups closed here, and what each retired
	std::exception_ptr failure_;
	std::size_t waiting_ = 0;
	bool woken_ = false, served_ = false, stopping_ = false;
};

}  // namespace strata
