// The turn driver's seal (standalone-turn-log D3), ahead of journal's own. TurnDriver::seal(group,
// fn) registers a group's sealer, which the driver calls before it encodes each turn, so each
// index's open record joins the turn as one batch record. journal 7c08540 lacks it, so this shim
// holds the sealers in front of a TurnDriver: turn() seals what the groups hold, commits the turn,
// then tells each group's watcher, the step a watcher coroutine on TurnDriver::durable takes.
// basic_index reaches the driver only through this class, so journal's seal replaces it here.
#pragma once

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
#include <vector>

namespace strata {

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
	// A group's sealer: its next whole record if it frames within `room` bytes, or with any size
	// when `first` (a turn's first record is always admitted), else none. Runs on the turn's
	// thread and takes the group's record mutex itself.
	using sealer = std::function<std::optional<record>(std::uint64_t room, bool first)>;
	// A group's watcher: its records through `sequence` are durable.
	using watcher = std::function<void(std::uint64_t sequence)>;

	// `geometry` is the driver's log geometry; its segments must hold two of the largest commands
	// with their framing, so every turn the budget admits fits an empty segment (D3).
	turn_sealer(kronuz::journal::TurnDriver& turns, const kronuz::journal::TurnLogOptions& geometry, std::uint64_t command_bytes)
		: turns_(turns), block_(geometry.block_bytes), budget_(geometry.segment_bytes / 2) {
		if (block_ < 512 || geometry.segment_bytes < 2 * turn_framed_bytes(command_bytes + 32, block_) + turn_framed_bytes(4, block_)) {
			throw std::invalid_argument("strata: the turn log's segments are smaller than two commands");
		}
	}
	turn_sealer(const turn_sealer&) = delete;
	turn_sealer& operator=(const turn_sealer&) = delete;

	// Registers the group's sealer and watcher, replacing an earlier pair; `owner` names them for
	// unseal(). Returns the group's last durable sequence.
	std::uint64_t seal(std::uint64_t group, sealer fn, watcher done, const void* owner) {
		const std::lock_guard turn(turn_mu_);
		const std::lock_guard lock(mu_);
		const std::uint64_t durable = turns_.sequence(group);
		groups_[group] = {std::make_shared<sealer>(std::move(fn)), std::make_shared<watcher>(std::move(done)), owner, durable};
		return durable;
	}
	// Drops the group's pair if `owner` registered it. A turn in progress may still call them.
	void unseal(std::uint64_t group, const void* owner) noexcept {
		const std::lock_guard lock(mu_);
		if (auto it = groups_.find(group); it != groups_.end() && it->second.owner == owner) { groups_.erase(it); }
	}
	// Any thread: an open record took its first transaction.
	void wake() noexcept {
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
		std::uint64_t used = 0;
		bool first = true;
		for (auto& [group, e] : groups) {
			while (first || used < budget_) {
				std::optional<record> r = (*e.seal)(first ? ~std::uint64_t(0) : budget_ - used, first);
				if (!r) { break; }
				used += turn_framed_bytes(r->payload.size(), block_);
				first = false;
				turns_.stage(std::move(*r));
			}
		}
		if (turns_.idle()) { return false; }
		try {
			turns_.commit();
		} catch (...) {
			const std::lock_guard lock(mu_);
			failure_ = std::current_exception();
			cv_.notify_all();
			throw;
		}
		// The watchers first, then the durable sequences a wait reads: a wait returns after V moved.
		for (auto& [group, e] : groups) {
			const std::uint64_t s = turns_.sequence(group);
			if (s > e.durable) { (*e.done)(s); }
			e.durable = s;
		}
		const std::lock_guard lock(mu_);
		for (const auto& [group, e] : groups) {
			if (auto it = groups_.find(group); it != groups_.end() && it->second.seal == e.seal) { it->second.durable = e.durable; }
		}
		cv_.notify_all();
		return true;
	}
	// Returns once the group's records through `sequence` are durable and its watcher has run.
	// While a loop serves turns it waits for them; otherwise the caller writes them inline, as
	// journal::run's blocking entry does.
	void await(std::uint64_t group, std::uint64_t sequence) {
		for (;;) {
			{
				std::unique_lock lock(mu_);
				if (failure_) { std::rethrow_exception(failure_); }
				const auto it = groups_.find(group);
				if (it == groups_.end()) { throw std::logic_error("strata: a turn-log wait on a group with no sealer"); }
				if (it->second.durable >= sequence) { return; }
				if (served_) {
					++waiting_;
					cv_.wait(lock, [&] {
						const auto at = groups_.find(group);
						return failure_ || !served_ || at == groups_.end() || at->second.durable >= sequence;
					});
					--waiting_;
					continue;
				}
			}
			if (!turn()) { throw std::logic_error("strata: a turn-log wait with nothing to write"); }
		}
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
	struct entry {
		std::shared_ptr<sealer> seal;
		std::shared_ptr<watcher> done;
		const void* owner = nullptr;
		std::uint64_t durable = 0;
	};
	kronuz::journal::TurnDriver& turns_;
	const std::uint32_t block_;
	const std::uint64_t budget_;
	std::mutex turn_mu_;  // one turn at a time, and every call into the driver
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::map<std::uint64_t, entry> groups_;
	std::exception_ptr failure_;
	std::size_t waiting_ = 0;
	bool woken_ = false, served_ = false, stopping_ = false;
};

}  // namespace strata
