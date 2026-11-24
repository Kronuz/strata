// The seekable scan of the root's namespace records (quest placement A2): a caller's matcher reads
// the records' paths in path order and seeks past every run of keys it cannot match, so a pattern
// reads the subtrees it can match and not the rest. How it works:
// docs/mechanisms/placement-root.md, "The seekable scan".
#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::placement {

// Records in path order: segment by segment (the parts between `/`, never empty), so a path comes
// before its descendants and they before its next sibling (`twitter`, `twitter/user`,
// `twitter-a`). Each call returns the path of the record it lands on, valid until the next call,
// or nullopt past the last. Calls only move forward.
template <class S>
concept scan_source = requires(S& s, std::string_view path) {
	{ s.seek(path) } -> std::same_as<std::optional<std::string_view>>;  // the first record >= `path`
	{ s.skip(path) } -> std::same_as<std::optional<std::string_view>>;  // the first one past `path`'s subtree
	{ s.next() } -> std::same_as<std::optional<std::string_view>>;      // the record after the current one
};

// A pattern as an automaton over a path's segments. A state that is neither `live` nor accepting
// is dead: every step from it stays dead.
//   step(s, seg)       the state after `seg`
//   accepts(s)         the path read so far is listed
//   live(s)            some longer path may be listed
//   skip_to(s, after)  for the children of a path in state `s`: the least segment greater than
//                      `after` whose subtree may hold a listed path; `after` itself when it cannot
//                      tell (the scan reads on), nullopt when no greater segment may. `after` is ""
//                      before the first child. Every segment it passes over must be dead from `s`.
template <class M>
concept scan_matcher = requires(M& m, const typename M::state& s, std::string_view segment) {
	{ m.start() } -> std::same_as<typename M::state>;
	{ m.step(s, segment) } -> std::same_as<typename M::state>;
	{ m.accepts(s) } -> std::convertible_to<bool>;
	{ m.live(s) } -> std::convertible_to<bool>;
	{ m.skip_to(s, segment) } -> std::same_as<std::optional<std::string>>;
};

// The work a scan did: records read and seeks made (`seek` and `skip`). `complete` is false when
// it stopped at its bound with records left unread.
struct scan_work {
	std::size_t visits = 0;
	std::size_t seeks = 0;
	bool complete = true;
};

// Calls `emit(path)` for each record `matcher` lists, in path order, reading at most `max_visits`
// records. Each seek lands past the record before it, so a record is read at most once; a pattern
// confined to the subtree of a path P of depth d reads at most |subtree(P)| + d + 1 records.
template <scan_source Source, scan_matcher Matcher, class Emit>
scan_work scan(Source& source, Matcher& matcher, std::size_t max_visits, Emit&& emit) {
	using state = typename Matcher::state;
	struct frame {
		std::string segment;
		state after;
	};
	std::vector<frame> frames;  // the segments of the current record read so far, with their states
	const state top = matcher.start();
	scan_work work;
	std::string target;

	const auto parent = [&](std::size_t depth) -> const state& { return depth == 0 ? top : frames[depth - 1].after; };
	const auto child = [&](std::size_t depth, std::string_view segment) -> const std::string& {
		target.clear();
		for (std::size_t i = 0; i < depth; ++i) {
			target += frames[i].segment;
			target += '/';
		}
		target += segment;
		return target;
	};
	// Past the subtree of frames[depth], and of each ancestor whose remaining children are dead.
	const auto leave = [&](std::size_t depth) -> std::optional<std::string_view> {
		for (;;) {
			const auto& dead = frames[depth].segment;
			if (const auto next = matcher.skip_to(parent(depth), dead)) {
				++work.seeks;
				return *next == dead ? source.skip(child(depth, dead)) : source.seek(child(depth, *next));
			}
			if (depth == 0) {
				return std::nullopt;
			}
			--depth;
		}
	};
	// Into the children of the path frames hold (none: the top).
	const auto enter = [&]() -> std::optional<std::string_view> {
		const std::size_t depth = frames.size();
		const auto first = matcher.skip_to(parent(depth), "");
		if (!first) {
			return depth == 0 ? std::nullopt : leave(depth - 1);
		}
		if (first->empty() && depth > 0) {
			return source.next();
		}
		++work.seeks;
		return source.seek(child(depth, *first));
	};

	for (auto at = enter(); at; ) {
		if (++work.visits > max_visits) {
			--work.visits;
			work.complete = false;
			break;
		}
		const std::string_view path = *at;
		// Keep the frames of the ancestors this record shares with the last one.
		std::size_t depth = 0, from = 0;
		for (;; ++depth) {
			const auto cut = path.find('/', from);
			if (cut == std::string_view::npos || depth == frames.size() || frames[depth].segment != path.substr(from, cut - from)) {
				break;
			}
			from = cut + 1;
		}
		frames.resize(depth);
		for (;; ++depth) {
			const auto cut = path.find('/', from);
			const auto segment = path.substr(from, cut == std::string_view::npos ? cut : cut - from);
			auto after = matcher.step(parent(depth), segment);
			frames.push_back({std::string(segment), std::move(after)});
			const state& s = frames.back().after;
			if (cut == std::string_view::npos) {
				if (matcher.accepts(s)) {
					emit(path);
				}
				at = matcher.live(s) ? enter() : leave(depth);
				break;
			}
			if (!matcher.live(s)) {  // an ancestor no record below can match
				at = leave(depth);
				break;
			}
			from = cut + 1;
		}
	}
	return work;
}

}  // namespace strata::placement
