// The segments' key ranges for a point lookup (rocksdb-parity D9, docs/mechanisms/sorted-runs.md).
// A run, a segment whose range no other segment's overlaps, is kept by first key and found by
// binary search; the other segments are probed as before. A key inside a run's range is in no
// other segment's, so a lookup probes that run and the rest, not every run.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace strata {

class key_ranges {
public:
	// The positions a lookup probes, oldest first: the rest and the run holding the key, if any.
	class probes {
	public:
		std::size_t size() const noexcept { return all_ ? n_ : n_ + (run_ != none); }
		std::size_t operator[](std::size_t i) const noexcept {
			if (all_) { return i; }
			if (run_ == none || i < split_) { return rest_[i]; }
			return i == split_ ? run_ : rest_[i - 1];
		}

	private:
		friend class key_ranges;
		const std::uint32_t* rest_ = nullptr;
		std::size_t n_ = 0, split_ = 0;  // the rest's count; where the run goes among them
		std::uint32_t run_ = none;
		bool all_ = false;  // ranges out of date: every position
	};

	// Rebuilds for a list of `segments` positions: add each known range or unknown position,
	// then finish(). Until finish(), and after a throw in between, find() probes everything.
	void start(std::size_t segments) noexcept {
		valid_ = false;
		runs_.clear();
		rest_.clear();
		segments_ = segments;
	}
	// A segment whose keys span [first, last]; the views must outlive the next start().
	void add(std::size_t at, std::string_view first, std::string_view last) { runs_.push_back({first, last, std::uint32_t(at)}); }
	// A segment whose range is not known (not mapped): it may hold any key.
	void add_unknown(std::size_t at) { rest_.push_back(std::uint32_t(at)); }
	void finish() {
		std::sort(runs_.begin(), runs_.end(), [](const entry& a, const entry& b) { return a.first < b.first; });
		// Sorted by first key, a range overlaps none when it starts past every earlier range's
		// end and ends before the next one starts; the overlapping ones join the rest.
		std::vector<bool> overlaps(runs_.size(), false);
		std::string_view reach;
		for (std::size_t k = 0; k < runs_.size(); ++k) {
			overlaps[k] = (k && !(reach < runs_[k].first)) || (k + 1 < runs_.size() && !(runs_[k].last < runs_[k + 1].first));
			if (!k || reach < runs_[k].last) { reach = runs_[k].last; }
		}
		std::size_t kept = 0;
		for (std::size_t k = 0; k < runs_.size(); ++k) {
			if (overlaps[k]) {
				rest_.push_back(runs_[k].at);
			} else {
				runs_[kept++] = runs_[k];
			}
		}
		runs_.resize(kept);
		std::sort(rest_.begin(), rest_.end());
		valid_ = true;
	}

	// The probes for `key` over a list of `segments` positions, which must be the list the
	// ranges were built for; a list of another length gets every position.
	probes find(std::string_view key, std::size_t segments) const noexcept {
		probes p;
		if (!valid_ || segments != segments_) {
			p.all_ = true;
			p.n_ = segments;
			return p;
		}
		p.rest_ = rest_.data();
		p.n_ = rest_.size();
		// The last run starting at or before the key holds it when it ends at or after it.
		const auto it = std::upper_bound(runs_.begin(), runs_.end(), key, [](std::string_view k, const entry& e) { return k < e.first; });
		if (it != runs_.begin() && !(std::prev(it)->last < key)) {
			p.run_ = std::prev(it)->at;
			p.split_ = std::size_t(std::lower_bound(rest_.begin(), rest_.end(), p.run_) - rest_.begin());
		}
		return p;
	}
	std::size_t runs() const noexcept { return runs_.size(); }

private:
	static constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
	struct entry {
		std::string_view first, last;
		std::uint32_t at;
	};
	std::vector<entry> runs_;          // disjoint ranges, by first key
	std::vector<std::uint32_t> rest_;  // unknown or overlapping, oldest first
	std::size_t segments_ = 0;
	bool valid_ = false;
};

}  // namespace strata
