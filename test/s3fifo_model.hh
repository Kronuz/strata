// The S3-FIFO reference model of cold-tier S8 (Yang et al., SOSP 2023), the architect's `s8_variants`
// (strata-wt/cold-tier/.scratch/arch-r4) without its knobs: one pin per request, a small FIFO of a tenth of
// the capacity, a main FIFO, and a ghost of the main FIFO's size. Also the Zipf trace of the criterion.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <list>
#include <random>
#include <unordered_map>
#include <vector>

namespace s8 {

struct zipf {
	std::vector<double> cdf;
	zipf(std::size_t n, double theta) : cdf(n) {
		double h = 0;
		for (std::size_t i = 0; i < n; ++i) { h += 1.0 / std::pow(double(i + 1), theta); }
		double c = 0;
		for (std::size_t i = 0; i < n; ++i) {
			c += 1.0 / std::pow(double(i + 1), theta) / h;
			cdf[i] = c;
		}
	}
	template <class R>
	std::size_t draw(R& rng) const {
		const double u = std::uniform_real_distribution<double>(0, 1)(rng);
		return std::size_t(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
	}
};

// The criterion's trace: `warm` Zipf requests from mt19937_64 seeded 42, then (with `scan`) one access to every key
// in the order key * 2654435761 mod n, then `measured` more draws from the same stream. `measure_from`: where the
// measured part starts.
struct trace {
	std::vector<std::uint32_t> keys;
	std::size_t measure_from = 0;
	trace(std::size_t n, double theta, std::uint64_t warm, std::uint64_t measured, bool scan) {
		const zipf z(n, theta);
		std::mt19937_64 rng(42);
		keys.reserve(warm + (scan ? n : 0) + measured);
		for (std::uint64_t i = 0; i < warm; ++i) { keys.push_back(std::uint32_t(z.draw(rng))); }
		if (scan) {
			for (std::size_t key = 0; key < n; ++key) { keys.push_back(std::uint32_t((key * 2654435761u) % n)); }
		}
		measure_from = keys.size();
		for (std::uint64_t i = 0; i < measured; ++i) { keys.push_back(std::uint32_t(z.draw(rng))); }
	}
};

class model {
public:
	explicit model(std::size_t c) : small_cap_(c / 10), main_cap_(c - c / 10) {
		where_.reserve(c * 2);
		ghosts_.reserve(c * 2);
	}
	// A hit or a miss.
	bool access(std::int64_t key) {
		if (auto it = where_.find(key); it != where_.end()) {
			it->second.freq = std::uint8_t(std::min(3, int(it->second.freq) + 1));
			return true;
		}
		if (auto g = ghosts_.find(key); g != ghosts_.end()) {
			ghost_.erase(g->second);
			ghosts_.erase(g);
			insert_main(key, 0);
			return false;
		}
		if (small_.size() >= small_cap_) { evict_small(); }
		small_.push_front(key);
		where_[key] = entry{0, small_.begin()};
		return false;
	}

private:
	struct entry {
		std::uint8_t freq = 0;
		std::list<std::int64_t>::iterator it;
	};
	void ghost_add(std::int64_t key) {
		if (ghosts_.size() >= main_cap_) {
			ghosts_.erase(ghost_.back());
			ghost_.pop_back();
		}
		ghost_.push_front(key);
		ghosts_[key] = ghost_.begin();
	}
	void insert_main(std::int64_t key, std::uint8_t f) {
		while (main_.size() >= main_cap_) {
			const std::int64_t t = main_.back();
			main_.pop_back();
			entry& e = where_[t];
			if (e.freq > 0) {
				--e.freq;
				main_.push_front(t);
				e.it = main_.begin();
				continue;
			}
			where_.erase(t);
		}
		main_.push_front(key);
		where_[key] = entry{f, main_.begin()};
	}
	void evict_small() {
		const std::int64_t t = small_.back();
		small_.pop_back();
		const std::uint8_t f = where_[t].freq;
		where_.erase(t);
		if (f > 0) {
			insert_main(t, 0);
		} else {
			ghost_add(t);
		}
	}
	std::size_t small_cap_, main_cap_;
	std::list<std::int64_t> small_, main_, ghost_;
	std::unordered_map<std::int64_t, entry> where_;
	std::unordered_map<std::int64_t, std::list<std::int64_t>::iterator> ghosts_;
};

// The design's rule (D6) stated on lists, the oracle of the lock-step test: slots are evicted only over the capacity;
// the small FIFO's head while it is over its share (with a frequency to the main FIFO, the frequency cleared; else
// closed and its hash to the ghost), else the main FIFO's head (decremented and put back at the tail while its
// frequency is set, else closed, no ghost). A key in the ghost enters the main FIFO.
class model_total {
public:
	explicit model_total(std::size_t cap) : cap_(cap), small_cap_(cap / 10), ghost_cap_(cap - cap / 10) {}
	bool access(std::int64_t key) {
		if (auto it = freq_.find(key); it != freq_.end()) {
			it->second = std::min(3, it->second + 1);
			return true;
		}
		const auto g = ghost_at_.find(key);
		const bool seen = g != ghost_at_.end();
		if (seen) {
			ghost_.erase(g->second);
			ghost_at_.erase(g);
		}
		freq_[key] = 0;
		(seen ? main_ : small_).push_back(key);
		while (small_.size() + main_.size() > cap_) { evict(); }
		return false;
	}
	std::size_t resident() const { return small_.size() + main_.size(); }

private:
	void evict() {
		if (small_.size() > small_cap_ || main_.empty()) {
			const std::int64_t k = small_.front();
			small_.pop_front();
			if (freq_[k] > 0) {
				freq_[k] = 0;
				main_.push_back(k);
			} else {
				freq_.erase(k);
				ghost_.push_back(k);
				ghost_at_[k] = std::prev(ghost_.end());
				if (ghost_.size() > ghost_cap_) {
					ghost_at_.erase(ghost_.front());
					ghost_.pop_front();
				}
			}
		} else {
			const std::int64_t k = main_.front();
			main_.pop_front();
			if (freq_[k] > 0) {
				--freq_[k];
				main_.push_back(k);
			} else {
				freq_.erase(k);
			}
		}
	}
	std::size_t cap_, small_cap_, ghost_cap_;
	std::list<std::int64_t> small_, main_, ghost_;  // oldest first
	std::unordered_map<std::int64_t, int> freq_;
	std::unordered_map<std::int64_t, std::list<std::int64_t>::iterator> ghost_at_;
};

// The references of the criterion: {all 8M, first 1M} per cell.
struct reference {
	double theta;
	bool scan;
	double all, first;
};
inline constexpr reference references[] = {
	{0.99, false, 0.8160, 0.8146}, {0.99, true, 0.8159, 0.8135}, {1.2, false, 0.9583, 0.9512}, {1.2, true, 0.9577, 0.9510},
};

}  // namespace s8
