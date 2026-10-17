/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The simulator's engine (design D2): one thread runs events in (time, sequence) order, so
// equal times keep scheduling order and nothing depends on heap layout or addresses. An
// event's identity is its 64-bit sequence, which never repeats. Every event folds its time,
// kind, and node into a 64-bit trace hash, logged every 65,536 events (after the labels fold,
// before the handler runs) so two runs that diverge bisect to the first differing window.

#pragma once

#include "clock.hh"
#include "random.hh"

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kronuz::simulation {

class engine {
public:
	using event = std::function<void()>;
	struct id {
		std::uint32_t slot = 0;
		std::uint64_t sequence = 0;
		bool operator==(const id&) const = default;
	};
	static constexpr std::uint64_t checkpoint_interval = 65'536;

	explicit engine(std::uint64_t seed) : root_(seed) {}
	engine(const engine&) = delete;
	engine& operator=(const engine&) = delete;

	time_ns now() const noexcept { return now_; }
	// The root stream; components take children of it by label (design D2).
	const stream& random() const noexcept { return root_; }

	// Schedules `e` at time t (not before now). kind and node label it in the trace, so two
	// runs that order same-time events differently hash differently.
	id at(time_ns t, event e, std::uint64_t kind, std::uint64_t node) {
		if (t < now_) { throw std::invalid_argument("event in the past"); }
		std::uint32_t slot;
		if (free_.empty()) {
			if (slots_.size() == std::numeric_limits<std::uint32_t>::max()) { throw std::length_error("too many events"); }
			slot = std::uint32_t(slots_.size()); slots_.emplace_back();
		} else { slot = free_.back(); free_.pop_back(); }
		auto& body = slots_[slot];
		body.fn = std::move(e); body.kind = kind; body.node = node; body.sequence = sequence_; body.live = true;
		push({t, sequence_++, slot});
		++pending_;
		return {slot, body.sequence};
	}
	id after(time_ns delay, event e, std::uint64_t kind, std::uint64_t node) {
		if (delay > never - now_) { throw std::invalid_argument("event past the end of time"); }
		return at(now_ + delay, std::move(e), kind, node);
	}
	// False when the event already ran or was canceled.
	bool cancel(id which) {
		if (which.slot >= slots_.size()) { return false; }
		auto& body = slots_[which.slot];
		if (!body.live || body.sequence != which.sequence) { return false; }
		release(which.slot); --pending_;
		return true;
	}

	// Runs the next event; false when none is pending.
	bool step() {
		while (!heap_.empty()) {
			const auto top = pop();
			auto& body = slots_[top.slot];
			if (!body.live || body.sequence != top.sequence) { continue; }
			now_ = top.time;
			fold(top.time); fold(body.kind); fold(body.node);
			auto fn = std::move(body.fn);
			release(top.slot); --pending_;
			if (++executed_ % checkpoint_interval == 0) { checkpoints_.push_back(hash_); }
			fn();
			return true;
		}
		return false;
	}
	// Runs every event at or before t, then sets the time to t.
	std::uint64_t run_until(time_ns t) {
		std::uint64_t ran = 0;
		while (next_time() <= t && step()) { ++ran; }
		if (t > now_) { now_ = t; }
		return ran;
	}
	// Runs until no event is pending; time stays at the last event's.
	std::uint64_t drain() {
		std::uint64_t ran = 0;
		while (step()) { ++ran; }
		return ran;
	}
	std::uint64_t run_steps(std::uint64_t n) {
		std::uint64_t ran = 0;
		while (ran < n && step()) { ++ran; }
		return ran;
	}
	// The time of the next live event; never when none.
	time_ns next_time() {
		while (!heap_.empty()) {
			const auto& top = heap_.front(); const auto& body = slots_[top.slot];
			if (body.live && body.sequence == top.sequence) { return top.time; }
			pop();
		}
		return never;
	}

	// Folds a payload digest into the trace hash, for state an event changed.
	void trace(std::uint64_t value) noexcept { fold(value); }
	std::uint64_t trace_hash() const noexcept { return hash_; }
	const std::vector<std::uint64_t>& checkpoints() const noexcept { return checkpoints_; }
	std::uint64_t executed() const noexcept { return executed_; }
	std::size_t pending() const noexcept { return pending_; }

private:
	// A canceled event's entry stays in the heap until its time; it matches its slot's body only
	// while the body holds the same sequence, so a reused slot never fires a stale entry.
	struct entry { time_ns time; std::uint64_t sequence; std::uint32_t slot; };
	static_assert(sizeof(entry) == 24);
	struct body { event fn; std::uint64_t kind = 0, node = 0, sequence = 0; bool live = false; };
	static bool before(const entry& a, const entry& b) noexcept { return a.time != b.time ? a.time < b.time : a.sequence < b.sequence; }

	void fold(std::uint64_t x) noexcept { hash_ = mix(hash_ ^ mix(x + 0x9e3779b97f4a7c15ULL)); }
	void release(std::uint32_t slot) {
		auto& b = slots_[slot];
		b.fn = nullptr; b.live = false;
		free_.push_back(slot);
	}
	// A 4-ary min-heap: shallower than binary, so fewer cache misses per operation.
	void push(entry e) {
		heap_.push_back(e);
		auto i = heap_.size() - 1;
		while (i > 0) {
			const auto parent = (i - 1) / 4;
			if (!before(heap_[i], heap_[parent])) { break; }
			std::swap(heap_[i], heap_[parent]); i = parent;
		}
	}
	entry pop() {
		const auto top = heap_.front();
		heap_.front() = heap_.back(); heap_.pop_back();
		std::size_t i = 0;
		for (;;) {
			std::size_t best = i;
			const auto first = 4 * i + 1;
			for (std::size_t c = first; c < first + 4 && c < heap_.size(); ++c) {
				if (before(heap_[c], heap_[best])) { best = c; }
			}
			if (best == i) { break; }
			std::swap(heap_[i], heap_[best]); i = best;
		}
		return top;
	}

	stream root_;
	time_ns now_ = 0;
	std::uint64_t sequence_ = 0, executed_ = 0, hash_ = 0;
	std::size_t pending_ = 0;
	std::vector<entry> heap_;
	std::vector<body> slots_;
	std::vector<std::uint32_t> free_;
	std::vector<std::uint64_t> checkpoints_;
};

}  // namespace kronuz::simulation
