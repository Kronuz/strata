// The write-buffer bake-off's building blocks (docs/design/write-buffer.md, step 0): the fixed
// chunk directory (WB-19), lanes, records, the version map's table, the per-buffer position
// index (WB-4), real segments (WB-8: strata's key filter and dictionary), and a small job pool.
// Benchmark code: no engine dependency beyond strata's key dictionary and hazard registry.
#pragma once

#include "strata/core/hazard.hh"
#include "strata/core/keys.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#ifndef LANEBUF_MUTANT
#define LANEBUF_MUTANT 0
#endif
#ifndef LANEBUF_PROF
#define LANEBUF_PROF 0  // scan phase timers (TSC cycles), printed to stderr at exit
#endif
#ifndef LANEBUF_CAP
#define LANEBUF_CAP 16384  // levels up to this many entries belong to the fresh job, bigger ones to the compactor
#endif
#ifndef LANEBUF_MERGE
#define LANEBUF_MERGE 0  // the scan's merge compares 8-byte key prefixes first and finds the minimum in one pass
#endif

namespace lanebuf {

namespace keys = strata::keys;

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

inline constexpr unsigned kMaxOps = 4;
inline constexpr unsigned kMaxHeight = 12;
inline constexpr u32 kChunkHdr = 8;              // every chunk starts 8 bytes in, so a reference is never 0
#ifndef LANEBUF_TAILMAX
#define LANEBUF_TAILMAX 64
#endif
inline constexpr std::size_t kTailMax = LANEBUF_TAILMAX;     // D9: a private tail past this asks for a build (the design's estimate was 256; 64 measured better)
inline constexpr u32 kNone = 0xFFFFFFFFu;

enum class order_kind { runs, skiplist };
enum class conc_kind { group, lease };
enum class op_kind : u8 { put, del };

inline void cpu_relax() noexcept {
#if defined(__x86_64__)
	_mm_pause();
#elif defined(__aarch64__)
	asm volatile("yield");
#endif
}

inline unsigned vsize(u64 v) noexcept {
	unsigned n = 1;
	while (v >= 128) { v >>= 7; ++n; }
	return n;
}
inline char* put_varint(char* p, u64 v) noexcept {
	while (v >= 128) { *p++ = char(v | 128); v >>= 7; }
	*p++ = char(v);
	return p;
}
inline const char* get_varint(const char* p, u64& v) noexcept {
	v = 0;
	for (unsigned s = 0;; s += 7) {
		const u8 b = u8(*p++);
		v |= u64(b & 127) << s;
		if (!(b & 128)) { break; }
	}
	return p;
}
constexpr std::size_t align8(std::size_t n) noexcept { return (n + 7) & ~std::size_t(7); }

inline int cmp_keys(std::string_view a, std::string_view b) noexcept {
	const std::size_t n = std::min(a.size(), b.size());
	if (const int c = std::memcmp(a.data(), b.data(), n)) { return c; }
	return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

// The first 8 bytes of a key, big-endian and zero padded: a < b whenever pre(a) < pre(b).
inline u64 key_pre(std::string_view key) noexcept {
	if (key.size() >= 8) {
		u64 w;
		std::memcpy(&w, key.data(), 8);
		return __builtin_bswap64(w);
	}
	u64 p = 0;
	for (std::size_t i = 0; i < key.size(); ++i) { p |= u64(u8(key[i])) << (56 - 8 * i); }
	return p;
}

// ---- the fixed chunk directory (WB-19) ----------------------------------------------------
// Chunk indexes are claimed by fetch_add; pointers live in a directory that is never replaced:
// 8 inline entries, then 12 blocks of 8 << k entries, each installed by compare-and-swap on first
// use and freed with the buffer. 32,768 chunks at most; a buffer that fills them freezes.

class chunk_dir {
public:
	static constexpr unsigned kInl = 8, kBlocks = 12;
	static constexpr u32 kMax = 32768;
	chunk_dir() {
		for (auto& c : inl_) { c.store(nullptr, std::memory_order_relaxed); }
		for (auto& b : blk_) { b.store(nullptr, std::memory_order_relaxed); }
	}
	chunk_dir(const chunk_dir&) = delete;
	~chunk_dir() {
		const u32 n = std::min<u32>(n_.load(std::memory_order_relaxed), kMax);
		for (u32 i = 0; i < n; ++i) { std::free(get(i)); }
		for (auto& b : blk_) { std::free(b.load(std::memory_order_relaxed)); }
	}
	u32 claim() {
		const u32 i = n_.fetch_add(1, std::memory_order_relaxed);
		if (i >= kMax) {
			std::fprintf(stderr, "lanebuf: chunk table full (the buffer would freeze)\n");
			std::abort();
		}
		return i;
	}
	char* get(u32 i) const noexcept {
		if (i < kInl) { return inl_[i].load(std::memory_order_acquire); }
		const unsigned k = (31 - unsigned(__builtin_clz(i))) - 3;
		return blk_[k].load(std::memory_order_acquire)[i - (8u << k)].load(std::memory_order_acquire);
	}
	void set(u32 i, char* p) {
		if (i < kInl) {
			inl_[i].store(p, std::memory_order_release);
			return;
		}
		const unsigned k = (31 - unsigned(__builtin_clz(i))) - 3;
		std::atomic<char*>* blk = blk_[k].load(std::memory_order_acquire);
		if (!blk) {
			auto* fresh = static_cast<std::atomic<char*>*>(std::calloc(std::size_t(8) << k, sizeof(std::atomic<char*>)));
			if (blk_[k].compare_exchange_strong(blk, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) {
				blk = fresh;
			} else {
				std::free(fresh);
			}
		}
		blk[i - (8u << k)].store(p, std::memory_order_release);
	}
	u32 claimed() const noexcept { return n_.load(std::memory_order_relaxed); }
	void add_bytes(std::size_t b) noexcept { bytes_.fetch_add(b, std::memory_order_relaxed); }
	std::size_t bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }
	std::size_t dir_bytes() const noexcept {
		std::size_t b = sizeof(*this);
		for (unsigned k = 0; k < kBlocks; ++k) {
			if (blk_[k].load(std::memory_order_relaxed)) { b += (std::size_t(8) << k) * sizeof(void*); }
		}
		return b;
	}

private:
	std::atomic<char*> inl_[kInl];
	std::atomic<std::atomic<char*>*> blk_[kBlocks];
	std::atomic<u32> n_{0};
	std::atomic<std::size_t> bytes_{0};
};

// ---- lanes --------------------------------------------------------------------------------
// A lane belongs to one buffer and to one pre-stage at a time (busy from claim to verdict); it
// sits on a push-only list the readers never touch (WB-19).

class lane {
public:
	std::atomic<bool> busy{true};
	lane* next = nullptr;
	// the claimer only
	static std::size_t chunk_cap(unsigned n) noexcept { return n >= 8 ? (std::size_t(1) << 20) : (std::size_t(4096) << n); }
	// Returns the record's start; `idx` is the chunk, `off` the offset of the start.
	char* alloc(chunk_dir& dir, std::size_t bytes, u32& idx, u32& off) {
		if (!base_ || off_ + bytes > cap_) { new_chunk(dir, bytes); }
		idx = idx_;
		off = off_;
		char* p = base_ + off_;
		off_ += u32(bytes);
		used_ += bytes;
		return p;
	}
	std::size_t used() const noexcept { return used_; }

private:
	void new_chunk(chunk_dir& dir, std::size_t need) {
		if (need + kChunkHdr > (std::size_t(1) << 20)) {
			std::fprintf(stderr, "lanebuf: record too large\n");
			std::abort();
		}
		const std::size_t cap = std::max(chunk_cap(nchunks_), need + kChunkHdr);
		idx_ = dir.claim();
		base_ = static_cast<char*>(std::malloc(cap));
		dir.set(idx_, base_);
		dir.add_bytes(cap);
		off_ = kChunkHdr;
		cap_ = u32(cap);
		++nchunks_;
	}
	char* base_ = nullptr;
	u32 idx_ = 0, off_ = 0, cap_ = 0;
	unsigned nchunks_ = 0;
	std::size_t used_ = 0;
};

// ---- records ------------------------------------------------------------------------------
// [tower: 4 B per level, A' only][docid u64][version u64][pos u32][prev u32][flags u8]
// [key length varint][value length varint][key][value], 8-aligned. A reference names the header:
// chunk index (15 bits) and offset in 8-byte units (17 bits). No field is written once the
// record is published (the map slot's release store); pos is atomic so enumerations reading it
// beside the sequencer are race-free by construction.

constexpr std::size_t tower_bytes(unsigned height) noexcept { return align8(std::size_t(height) * 4); }
inline std::size_t rec_size(std::size_t klen, std::size_t vlen, unsigned height) noexcept {
	return align8(tower_bytes(height) + 24 + 1 + vsize(klen) + vsize(vlen) + klen + vlen);
}
inline u32 make_ref(u32 chunk, u32 hoff) noexcept { return (chunk << 17) | (hoff >> 3); }

struct rec {
	char* h;
	u64& docid() const noexcept { return *reinterpret_cast<u64*>(h); }
	u64& version() const noexcept { return *reinterpret_cast<u64*>(h + 8); }
	std::atomic<u32>& pos() const noexcept { return *reinterpret_cast<std::atomic<u32>*>(h + 16); }
	u32& prev() const noexcept { return *reinterpret_cast<u32*>(h + 20); }
	u8 flags() const noexcept { return u8(h[24]); }
	bool tomb() const noexcept { return flags() & 1; }
	unsigned opi() const noexcept { return (flags() >> 1) & 3; }  // the operation's index in its transaction
	unsigned height() const noexcept { return flags() >> 4; }
	u32 pd() const noexcept { return pos().load(std::memory_order_relaxed); }
	// Orders a key's records: position, then the operation within the transaction.
	u64 seq() const noexcept { return (u64(pd()) << 2) | opi(); }
	std::string_view key() const noexcept {
		u64 kl, vl;
		const char* p = get_varint(get_varint(h + 25, kl), vl);
		return {p, std::size_t(kl)};
	}
	std::string_view value() const noexcept {
		u64 kl, vl;
		const char* p = get_varint(get_varint(h + 25, kl), vl);
		return {p + kl, std::size_t(vl)};
	}
	std::size_t size() const noexcept {
		u64 kl, vl;
		get_varint(get_varint(h + 25, kl), vl);
		return rec_size(kl, vl, height());
	}
	std::atomic<u32>& tower(unsigned level) const noexcept { return reinterpret_cast<std::atomic<u32>*>(h)[-1 - int(level)]; }
};

// ---- the version map ----------------------------------------------------------------------

struct table {
	u32 bits;
	u32 mask;
	std::atomic<u64>* slots() noexcept { return reinterpret_cast<std::atomic<u64>*>(this + 1); }
	static table* make(unsigned bits) {
		const std::size_t n = std::size_t(1) << bits;
		auto* t = static_cast<table*>(std::calloc(1, sizeof(table) + n * 8));
		t->bits = bits;
		t->mask = u32(n - 1);
		return t;
	}
	static void destroy(table* t) { std::free(t); }
	std::size_t bytes() const noexcept { return sizeof(table) + (std::size_t(mask) + 1) * 8; }
};
static_assert(sizeof(table) == 8);
inline u32 slot_home(u32 tag, unsigned bits) noexcept { return (tag * 0x9E3779B1u) >> (32 - bits); }
inline u64 pack_slot(u32 tag, u32 ref) noexcept { return (u64(tag) << 32) | ref; }
inline u32 slot_ref(u64 s) noexcept { return u32(s); }
inline u32 slot_tag(u64 s) noexcept { return u32(s >> 32); }

// A run entry: the first 8 bytes of the key (big-endian, zero padded) and the record reference, so
// a sort or a merge of runs compares registers and dereferences a record only on a prefix tie.
struct ent {
	u32 hi, lo, ref;
};
inline ent make_ent(std::string_view key, u32 ref) noexcept {
	const u64 p = key_pre(key);
	return {u32(p >> 32), u32(p), ref};
}

#if LANEBUF_PROF
inline std::atomic<u64> g_prof[10];
inline std::atomic<u64> g_tailh[24];  // tail sizes seen by scans, log2 buckets
struct prof_printer {
	~prof_printer() {
		const double n = double(g_prof[6].load()) ? double(g_prof[6].load()) : 1.0;
		std::fprintf(stderr, "TAILH log2(seen): ");
		for (int i = 0; i < 24; ++i) { std::fprintf(stderr, "%llu ", (unsigned long long)g_tailh[i].load()); }
		std::fprintf(stderr, "\n");
		std::fprintf(stderr, "PROF scans=%.0f cycles/scan: total=%.0f tailpass=%.0f select=%.0f buf_sources=%.0f segs=%.0f merge=%.0f nsrc=%.1f\n", n, double(g_prof[0].load()) / n, double(g_prof[1].load() - g_prof[2].load()) / n,
			double(g_prof[2].load()) / n, double(g_prof[3].load()) / n, double(g_prof[4].load()) / n, double(g_prof[5].load()) / n, double(g_prof[7].load()) / n);
	}
};
inline prof_printer g_prof_printer;
struct prof_t {
	unsigned idx;
	u64 t0 = __builtin_ia32_rdtsc();
	explicit prof_t(unsigned i) : idx(i) {}
	~prof_t() { g_prof[idx].fetch_add(__builtin_ia32_rdtsc() - t0, std::memory_order_relaxed); }
};
#define LB_PROF(name, i) prof_t name(i)
#else
#define LB_PROF(name, i) ((void)0)
#endif

// ---- the position index (WB-4) ------------------------------------------------------------
// One append-only array of record references per buffer, one per accepted write in position
// order: geometric blocks (64 << k entries) that are never replaced. The sequencer appends and
// publishes the count with release at the group's end; everyone else reads up to that count.

class pos_index {
public:
	static constexpr unsigned kBlocks = 26;
	using elem = ent;
	pos_index() {
		for (auto& b : blk_) { b = nullptr; }
	}
	pos_index(const pos_index&) = delete;
	~pos_index() {
		for (auto* b : blk_) { std::free(b); }
	}
	// the sequencer
	void push(elem ref) {
		const u64 j = n_ + 64;
		const unsigned k = (63 - unsigned(__builtin_clzll(j))) - 6;
		if (!blk_[k]) { blk_[k] = static_cast<elem*>(std::malloc((std::size_t(64) << k) * sizeof(elem))); }
		blk_[k][j - (u64(64) << k)] = ref;
		++n_;
	}
	u64 working() const noexcept { return n_; }
	void rewind(u64 n) noexcept { n_ = n; }
	void publish() noexcept { count_.store(n_, std::memory_order_release); }
	// any thread
	u64 count() const noexcept { return count_.load(std::memory_order_acquire); }
	elem raw(u64 i) const noexcept {
		const u64 j = i + 64;
		const unsigned k = (63 - unsigned(__builtin_clzll(j))) - 6;
		return blk_[k][j - (u64(64) << k)];
	}
	u32 at(u64 i) const noexcept { return raw(i).ref; }
	ent ent_at(u64 i) const noexcept { return raw(i); }
	std::size_t bytes() const noexcept {
		std::size_t b = sizeof(*this);
		for (unsigned k = 0; k < kBlocks; ++k) {
			if (blk_[k]) { b += (std::size_t(64) << k) * sizeof(elem); }
		}
		return b;
	}

private:
	elem* blk_[kBlocks];
	u64 n_ = 0;
	std::atomic<u64> count_{0};
};

// ---- real segments (WB-8) -----------------------------------------------------------------
// strata's key dictionary and filter (keys::sorted_encoder / keys::reader) hold the keys, so a
// lookup pays the real range check, filter probe and dictionary walk; ordinals follow key order.
// Documents (docid, version, value) sit in arrays by ordinal. Segments hold no tombstones.

struct seg_data {
	keys::sections sec;
	keys::reader kr;
	std::vector<u64> docid, version, voff;  // voff: n + 1 offsets into blob
	std::string blob;
	std::size_t n;
	seg_data(keys::sections s, std::size_t count) : sec(std::move(s)), kr(sec.keys, sec.index, count, nullptr, nullptr), n(count) {}
	std::string_view value(std::size_t o) const noexcept { return {blob.data() + voff[o], std::size_t(voff[o + 1] - voff[o])}; }
	std::size_t bytes() const noexcept { return sec.keys.size() + sec.index.size() + blob.size() + (docid.size() + version.size() + voff.size()) * 8 + kr.pinned_bytes(); }
};
using live_bits = std::vector<u64>;
struct segment {
	u32 id = 0;
	std::shared_ptr<const seg_data> d;
	std::shared_ptr<const live_bits> live;
	const void* from = nullptr;  // a flush's source buffer
	static constexpr std::size_t npos = ~std::size_t(0);
	std::size_t find(std::string_view key, u64 hash) const {
		const auto o = d->kr.find(key, hash);
		return o ? std::size_t(*o) : npos;
	}
	bool is_live(std::size_t o) const noexcept { return ((*live)[o >> 6] >> (o & 63)) & 1; }
	std::size_t size() const noexcept { return d->n; }
};

// Streams a segment's documents in strictly increasing key order.
class seg_stream {
public:
	explicit seg_stream(std::string_view alphabet) : enc_(alphabet) { voff_.push_back(0); }
	void add(std::string_view key, std::string_view value, u64 docid, u64 version) {
		enc_.add(key, strata::ordinal_t(docid_.size()));
		docid_.push_back(docid);
		version_.push_back(version);
		blob_.append(value);
		voff_.push_back(blob_.size());
	}
	std::size_t size() const noexcept { return docid_.size(); }
	segment finish(u32 id) {
		const std::size_t n = docid_.size();
		auto d = std::make_shared<seg_data>(enc_.finish(), n);
		d->docid = std::move(docid_);
		d->version = std::move(version_);
		d->voff = std::move(voff_);
		d->blob = std::move(blob_);
		segment s;
		s.id = id;
		s.live = std::make_shared<live_bits>((n + 63) / 64, ~u64(0));
		s.d = std::move(d);
		return s;
	}

private:
	keys::sorted_encoder enc_;
	std::vector<u64> docid_, version_, voff_;
	std::string blob_;
};

struct seg_item {
	std::string key, value;
	u64 docid = 0, version = 1;
};
inline std::string alphabet_of_keys(const std::vector<seg_item>& items) {
	std::array<bool, 256> used{};
	for (const auto& it : items) {
		for (char c : it.key) { used[u8(c)] = true; }
	}
	return keys::alphabet_of(used);
}
inline segment make_segment(u32 id, std::vector<seg_item> items) {
	std::sort(items.begin(), items.end(), [](const seg_item& a, const seg_item& b) { return a.key < b.key; });
	seg_stream s(alphabet_of_keys(items));
	for (const auto& it : items) { s.add(it.key, it.value, it.docid, it.version); }
	return s.finish(id);
}

// ---- a small job pool (the build jobs run off the write path) -----------------------------

class job_pool {
public:
	static job_pool& get() {
		static job_pool p;
		return p;
	}
	void set_threads(unsigned n) {
		std::lock_guard l(m_);
		want_ = n;
		while (th_.size() < want_) { th_.emplace_back([this] { loop(); }); }
	}
	void post(std::function<void()> f) {
		{
			std::lock_guard l(m_);
			q_.push_back(std::move(f));
			if (th_.empty()) { th_.emplace_back([this] { loop(); }); }
		}
		cv_.notify_one();
	}
	// Waits until no job is queued or running.
	void drain() {
		std::unique_lock l(m_);
		idle_.wait(l, [&] { return q_.empty() && busy_ == 0; });
	}
	~job_pool() {
		{
			std::lock_guard l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		for (auto& t : th_) { t.join(); }
	}

private:
	void loop() {
		std::unique_lock l(m_);
		for (;;) {
			cv_.wait(l, [&] { return stop_ || !q_.empty(); });
			if (q_.empty()) {
				if (stop_) { return; }
				continue;
			}
			auto f = std::move(q_.front());
			q_.pop_front();
			++busy_;
			l.unlock();
			f();
			l.lock();
			--busy_;
			if (q_.empty() && busy_ == 0) { idle_.notify_all(); }
		}
	}
	std::mutex m_;
	std::condition_variable cv_, idle_;
	std::deque<std::function<void()>> q_;
	std::vector<std::thread> th_;
	unsigned want_ = 1, busy_ = 0;
	bool stop_ = false;
};

// ---- inputs and outputs -------------------------------------------------------------------

struct op_in {
	op_kind kind = op_kind::put;
	std::string_view key, value;
	i64 expected = -1;  // -1 none; 0 the key must be absent; n the live version must be n
};
struct op_out {
	bool ok = false;
	u64 docid = 0, version = 0;
};
struct write_out {
	bool accepted = false;
	u64 pos = 0;
	op_out ops[kMaxOps];
};
struct got {
	bool found = false;
	u64 docid = 0, version = 0;
	u64 at = 0;  // the read position the read used
};

struct stats_t {
	std::size_t records = 0, lane_cap = 0, lane_used = 0, table_bytes = 0, runs_bytes = 0, pi_bytes = 0, dir_bytes = 0, lanes = 0, buffers = 0;
	u64 groups = 0, group_ops = 0, leader_ns = 0, longest_ns = 0;
	u64 hist[17] = {};  // group sizes 1..16+
	u64 run_builds = 0, run_build_ns = 0, tail_sum = 0, tail_n = 0, tail_max = 0, restaged = 0, lost_pending = 0, remapped = 0;
	u64 preres_used = 0, preres_redone = 0, views = 0, installs = 0;
};

}  // namespace lanebuf
