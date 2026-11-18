// The key-value benchmark's operation stream (rocksdb-survey.md, section 4, "Driver and
// generator"): keys, values, and YCSB's distributions from one seed, compiled into every
// engine's driver so the streams are identical and neither pays for the other's RNG.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace kvbench {

// xoshiro256** seeded through splitmix64.
class rng {
public:
	explicit rng(std::uint64_t seed) {
		for (auto& x : s_) {
			seed += 0x9E3779B97F4A7C15ull;
			std::uint64_t z = seed;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			x = z ^ (z >> 31);
		}
	}
	std::uint64_t next() noexcept {
		const std::uint64_t r = rotl(s_[1] * 5, 7) * 9, t = s_[1] << 17;
		s_[2] ^= s_[0];
		s_[3] ^= s_[1];
		s_[1] ^= s_[2];
		s_[0] ^= s_[3];
		s_[2] ^= t;
		s_[3] = rotl(s_[3], 45);
		return r;
	}
	std::uint64_t below(std::uint64_t n) noexcept { return std::uint64_t((unsigned __int128)next() * n >> 64); }
	double unit() noexcept { return double(next() >> 11) * 0x1.0p-53; }

private:
	static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
	std::uint64_t s_[4];
};

// A bijection on [0, 2^49): record ids scatter over the key space, as YCSB's hashed keys do.
inline std::uint64_t scatter(std::uint64_t i) noexcept {
	constexpr std::uint64_t mask = (std::uint64_t(1) << 49) - 1;
	i &= mask;
	i = (i * 0x9E3779B97F4A7C15ull) & mask;
	i ^= i >> 24;
	i = (i * 0xBF58476D1CE4E5B9ull) & mask;
	i ^= i >> 21;
	return i;
}

// Keys are 16 bytes: the index's key prefix "Q" and 15 decimal digits of scatter(id), so
// byte order is numeric order of the scattered id. Ids from 2^48 up name absent keys.
inline constexpr std::size_t key_size = 16;
inline constexpr std::uint64_t absent_base = std::uint64_t(1) << 48;
inline void make_key(std::uint64_t id, char* out) noexcept {
	std::uint64_t v = scatter(id);
	out[0] = 'Q';
	for (int i = 15; i >= 1; --i) {
		out[i] = char('0' + v % 10);
		v /= 10;
	}
}
inline std::string key_of(std::uint64_t id) {
	std::string k(key_size, '\0');
	make_key(id, k.data());
	return k;
}

// Values as db_bench's RandomGenerator makes them: a 1 MiB pool of 100-byte pieces, each 50
// random printable bytes written twice (compression ratio 0.5), read at a rolling offset.
class values {
public:
	explicit values(std::uint64_t seed) : pool_((1u << 20) + (1u << 16), '\0') {
		rng r(seed ^ 0x5A5A5A5A5A5A5A5Aull);
		for (std::size_t at = 0; at < pool_.size(); at += 100) {
			char piece[50];
			for (char& c : piece) { c = char(' ' + r.below(95)); }
			for (std::size_t i = 0; i < 100 && at + i < pool_.size(); ++i) { pool_[at + i] = piece[i % 50]; }
		}
	}
	std::string_view next(std::size_t size) noexcept {
		if (pos_ + size > pool_.size()) { pos_ = 0; }
		const std::string_view v(pool_.data() + pos_, size);
		pos_ += size;
		return v;
	}

private:
	std::string pool_;
	std::size_t pos_ = 0;
};

// YCSB's ZipfianGenerator (Gray et al.), theta 0.99, over [0, n), with zeta extended
// incrementally as n grows.
class zipfian {
public:
	explicit zipfian(std::uint64_t n, double theta = 0.99) : theta_(theta), alpha_(1.0 / (1.0 - theta)), zeta2_(zeta(0, 2, 0)) { resize(n); }
	// YCSB's scrambled zipfian draws from 10^10 items with this precomputed zeta.
	zipfian(std::uint64_t n, double theta, double zetan) : theta_(theta), alpha_(1.0 / (1.0 - theta)), zeta2_(zeta(0, 2, 0)), n_(n), zetan_(zetan) { set_eta(); }
	void resize(std::uint64_t n) {
		if (n == n_) { return; }
		zetan_ = n > n_ ? zeta(n_, n, zetan_) : zeta(0, n, 0);
		n_ = n;
		set_eta();
	}
	std::uint64_t next(rng& r) const noexcept {
		const double u = r.unit(), uz = u * zetan_;
		if (uz < 1.0) { return 0; }
		if (uz < 1.0 + std::pow(0.5, theta_)) { return 1 < n_ ? 1 : 0; }
		const auto v = std::uint64_t(double(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
		return v < n_ ? v : n_ - 1;
	}
	std::uint64_t size() const noexcept { return n_; }

private:
	double zeta(std::uint64_t from, std::uint64_t to, double sum) const {
		for (std::uint64_t i = from; i < to; ++i) { sum += 1.0 / std::pow(double(i + 1), theta_); }
		return sum;
	}
	void set_eta() { eta_ = (1.0 - std::pow(2.0 / double(n_), 1.0 - theta_)) / (1.0 - zeta2_ / zetan_); }
	double theta_, alpha_, zeta2_;
	std::uint64_t n_ = 0;
	double zetan_ = 0, eta_ = 0;
};

inline std::uint64_t fnv1a64(std::uint64_t v) noexcept {
	std::uint64_t h = 0xCBF29CE484222325ull;
	for (int i = 0; i < 8; ++i) {
		h ^= v & 0xFF;
		h *= 0x100000001B3ull;
		v >>= 8;
	}
	return h;
}

// The many-index row's routing: FNV-1a over the key's bytes, the same in every driver.
inline std::uint64_t route_hash(std::string_view key) noexcept {
	std::uint64_t h = 0xCBF29CE484222325ull;
	for (const char c : key) {
		h ^= std::uint8_t(c);
		h *= 0x100000001B3ull;
	}
	return h;
}

// YCSB's ScrambledZipfianGenerator: zipfian ranks over 10^10 items, hashed onto [0, n).
class scrambled_zipfian {
public:
	scrambled_zipfian() : z_(10000000000ull, 0.99, 26.46902820178302) {}
	std::uint64_t next(rng& r, std::uint64_t n) const noexcept { return fnv1a64(z_.next(r)) % n; }

private:
	zipfian z_;
};

// YCSB's SkewedLatestGenerator: the newest records most likely.
class latest {
public:
	explicit latest(std::uint64_t n) : z_(n) {}
	std::uint64_t next(rng& r, std::uint64_t n) {
		z_.resize(n);
		return n - 1 - z_.next(r);
	}

private:
	zipfian z_;
};

enum class op_kind : std::uint8_t { put, get, update, insert, scan, rmw, erase };

struct op {
	op_kind kind;
	std::uint64_t id;
	std::uint32_t len = 0;  // scan length
};

// The workloads. db_bench's rows load first except fill*, which are the load; YCSB's run
// over a loaded table. `records` is the table's size, ids [0, records).
//   deleterandom      each of `ops` loaded keys deleted once, in random order
//   readall           every loaded key read once, in random order (the pass after deleterandom)
//   multireadrandom   readrandom's keys, issued as multigets of `batch`
//   bulkload          fillrandom's keys with automatic merges off, then a timed forced merge
//   readwhilewriting  readrandom's keys, with the same thread writing at a fixed rate between reads
enum class workload { fillseq, fillrandom, overwrite, readrandom, readmissing, readseq, seekrandom, ycsb_a, ycsb_b, ycsb_c, ycsb_d, ycsb_e, ycsb_f,
	deleterandom, readall, multireadrandom, bulkload, readwhilewriting };
inline constexpr workload last_workload = workload::readwhilewriting;

inline const char* name_of(workload w) {
	switch (w) {
	case workload::fillseq: return "fillseq";
	case workload::fillrandom: return "fillrandom";
	case workload::overwrite: return "overwrite";
	case workload::readrandom: return "readrandom";
	case workload::readmissing: return "readmissing";
	case workload::readseq: return "readseq";
	case workload::seekrandom: return "seekrandom";
	case workload::ycsb_a: return "ycsb_a";
	case workload::ycsb_b: return "ycsb_b";
	case workload::ycsb_c: return "ycsb_c";
	case workload::ycsb_d: return "ycsb_d";
	case workload::ycsb_e: return "ycsb_e";
	case workload::ycsb_f: return "ycsb_f";
	case workload::deleterandom: return "deleterandom";
	case workload::readall: return "readall";
	case workload::multireadrandom: return "multireadrandom";
	case workload::bulkload: return "bulkload";
	case workload::readwhilewriting: return "readwhilewriting";
	}
	return "?";
}
inline bool parse(std::string_view s, workload& w) {
	for (int i = 0; i <= int(last_workload); ++i) {
		if (s == name_of(workload(i))) {
			w = workload(i);
			return true;
		}
	}
	return false;
}
inline bool is_fill(workload w) { return w == workload::fillseq || w == workload::fillrandom || w == workload::bulkload; }

// One thread's stream: thread t of T owns the ids congruent to t modulo T, so a sharded engine
// and a shared one see the same operations. `ops` caps the run's length; readseq is one pass.
class stream {
public:
	stream(workload w, std::uint64_t seed, unsigned t, unsigned threads, std::uint64_t records, std::uint64_t ops)
		: w_(w),
		  r_(seed * 1000003 + t + (w == workload::readall ? 0x51ED270B : 0)),
		  wr_(seed * 7919 + t + 12345),
		  t_(t),
		  threads_(threads),
		  own_(records / threads + (t < records % threads)),
		  next_insert_(own_),
		  ops_(ops),
		  latest_(own_ ? own_ : 1) {
		if (w == workload::fillseq || w == workload::fillrandom || w == workload::bulkload || w == workload::deleterandom || w == workload::readall) {
			order_.resize(own_);
			for (std::uint64_t i = 0; i < own_; ++i) { order_[i] = global(i); }
			if (w == workload::fillseq) {
				std::sort(order_.begin(), order_.end(), [](std::uint64_t a, std::uint64_t b) { return scatter(a) < scatter(b); });
			} else {
				for (std::uint64_t i = own_; i > 1; --i) { std::swap(order_[i - 1], order_[r_.below(i)]); }
			}
			ops_ = w == workload::deleterandom ? std::min(ops, own_) : own_;
		}
	}
	// The load for every non-fill workload: fillrandom's stream with the same seed.
	static stream load(std::uint64_t seed, unsigned t, unsigned threads, std::uint64_t records) { return stream(workload::fillrandom, seed + 7777, t, threads, records, 0); }

	bool next(op& o) {
		if (done_ >= ops_) { return false; }
		const std::uint64_t i = done_++;
		switch (w_) {
		case workload::fillseq:
		case workload::fillrandom:
		case workload::bulkload: o = {op_kind::put, order_[i]}; return true;
		case workload::deleterandom: o = {op_kind::erase, order_[i]}; return true;
		case workload::readall: o = {op_kind::get, order_[i]}; return true;
		case workload::multireadrandom:
		case workload::readwhilewriting: o = {op_kind::get, uniform()}; return true;
		case workload::overwrite: o = {op_kind::put, uniform()}; return true;
		case workload::readrandom: o = {op_kind::get, uniform()}; return true;
		case workload::readmissing: o = {op_kind::get, absent_base + uniform()}; return true;
		case workload::seekrandom: o = {op_kind::scan, uniform(), 10}; return true;
		case workload::readseq: o = {op_kind::scan, 0, 0}; return true;
		case workload::ycsb_a: o = {r_.unit() < 0.5 ? op_kind::get : op_kind::update, zipf()}; return true;
		case workload::ycsb_b: o = {r_.unit() < 0.95 ? op_kind::get : op_kind::update, zipf()}; return true;
		case workload::ycsb_c: o = {op_kind::get, zipf()}; return true;
		case workload::ycsb_d:
			if (r_.unit() < 0.05) {
				o = {op_kind::insert, global(next_insert_++)};
			} else {
				o = {op_kind::get, global(latest_.next(r_, next_insert_))};
			}
			return true;
		case workload::ycsb_e:
			if (r_.unit() < 0.05) {
				o = {op_kind::insert, global(next_insert_++)};
			} else {
				o = {op_kind::scan, zipf(), std::uint32_t(1 + r_.below(100))};
			}
			return true;
		case workload::ycsb_f: o = {r_.unit() < 0.5 ? op_kind::get : op_kind::rmw, zipf()}; return true;
		}
		return false;
	}
	std::uint64_t own() const noexcept { return own_; }
	// readwhilewriting's next write: a uniform loaded key, from its own generator so the reads
	// are readrandom's whatever the writes do.
	std::uint64_t write_id() noexcept { return global(wr_.below(own_)); }

private:
	std::uint64_t global(std::uint64_t local) const noexcept { return local * threads_ + t_; }
	std::uint64_t uniform() noexcept { return global(r_.below(own_)); }
	std::uint64_t zipf() noexcept { return global(zipf_.next(r_, next_insert_)); }

	workload w_;
	rng r_, wr_;
	unsigned t_, threads_;
	std::uint64_t own_, next_insert_, ops_, done_ = 0;
	std::vector<std::uint64_t> order_;
	scrambled_zipfian zipf_;
	latest latest_;
};

}  // namespace kvbench
