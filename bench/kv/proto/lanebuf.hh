// The write-buffer bake-off's prototype (docs/design/write-buffer.md, step 0): three candidates
// from one source.
//   C   lanes + one shared version map + a write group whose leader runs stage 1; scans read
//       shared sorted runs (main, delta, and a private tail).
//   A'  the same, with a concurrent skip list (tower inside the record) for scans; V waits for the
//       group's last link.
//   F   the fallback (D14): one lane, an exclusive lease per transaction, no pre-staging.
// index<order_kind, conc_kind> selects them. Benchmark code: no engine dependency.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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

namespace lanebuf {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

inline constexpr u32 kPending = 0xFFFFFFFFu;  // header not yet filled by the leader
inline constexpr u32 kNever = 0xFFFFFFFEu;    // a rejected transaction's record
inline constexpr unsigned kLanes = 8;
inline constexpr unsigned kMaxOps = 4;
inline constexpr unsigned kMaxHeight = 12;
inline constexpr unsigned kChunkHdr = 8;       // chunk header: u32 bytes used
inline constexpr std::size_t kTailMax = 256;   // D9: a private tail past this builds a delta
inline constexpr std::size_t kDeltaMax = 4096; // a delta past this merges into the main run

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

inline u64 mum(u64 a, u64 b) noexcept {
	const unsigned __int128 r = (unsigned __int128)a * b;
	return u64(r) ^ u64(r >> 64);
}
inline u64 hash64(const char* p, std::size_t n) noexcept {
	u64 a = 0xa0761d6478bd642full ^ n, b = 0xe7037ed1a0b428dbull;
	while (n > 16) {
		u64 x, y;
		std::memcpy(&x, p, 8);
		std::memcpy(&y, p + 8, 8);
		a = mum(x ^ b, y ^ a);
		p += 16;
		n -= 16;
	}
	char t[16] = {};
	std::memcpy(t, p, n);
	u64 x, y;
	std::memcpy(&x, t, 8);
	std::memcpy(&y, t + 8, 8);
	return mum(mum(x ^ 0x8ebc6af09c88c6e3ull, y ^ a) ^ b, n ^ 0x589965cc75374cc3ull);
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

// ---- epoch domain: what readers hold, who frees (D10) -------------------------------------

class epoch {
public:
	static constexpr unsigned kSlots = 256;
	static constexpr u64 kIdle = 0;  // zero-initialized slots are idle; epochs start at 1
	struct alignas(64) slot {
		std::atomic<u64> e;
		std::atomic<u32> taken;
	};
	static inline std::atomic<u64> global{1};
	static inline slot slots[kSlots] = {};
	static inline std::atomic<unsigned> high{0};

	struct local {
		int idx = -1;
		unsigned depth = 0;
		~local() {
			if (idx >= 0) {
				slots[idx].e.store(kIdle, std::memory_order_release);
				slots[idx].taken.store(0, std::memory_order_release);
			}
		}
	};
	static local& tls() {
		thread_local local l;
		return l;
	}
	static void enter() {
		auto& l = tls();
		if (l.depth++ != 0) { return; }
		if (l.idx < 0) {
			for (unsigned i = 0;; i = (i + 1) % kSlots) {
				u32 z = 0;
				if (slots[i].taken.compare_exchange_strong(z, 1, std::memory_order_acq_rel)) {
					l.idx = int(i);
					unsigned h = high.load(std::memory_order_relaxed);
					while (h < i + 1 && !high.compare_exchange_weak(h, i + 1, std::memory_order_acq_rel)) {}
					break;
				}
			}
		}
		slots[l.idx].e.store(global.load(std::memory_order_acquire), std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
	}
	static void leave() {
		auto& l = tls();
		if (--l.depth == 0) { slots[l.idx].e.store(kIdle, std::memory_order_release); }
	}
	static u64 advance() noexcept { return global.fetch_add(1, std::memory_order_acq_rel); }
	static u64 min_active() noexcept {
		std::atomic_thread_fence(std::memory_order_seq_cst);
		u64 m = ~u64(0);
		const unsigned n = high.load(std::memory_order_acquire);
		for (unsigned i = 0; i < n; ++i) {
			const u64 e = slots[i].e.load(std::memory_order_acquire);
			if (e != kIdle) { m = std::min(m, e); }
		}
		return m;
	}
	struct guard {
		guard() { enter(); }
		~guard() { leave(); }
		guard(const guard&) = delete;
		guard& operator=(const guard&) = delete;
	};
};

// Retired objects: pushed by any thread, freed by the lease holder once no reader can hold them.
class retire_list {
public:
	retire_list() = default;
	retire_list(const retire_list&) = delete;
	~retire_list() {
		drain();
		free_all(held_);
	}
	template <class T>
	void retire(T* p) {
#if LANEBUF_MUTANT == 7
		delete p;  // mutant: freed at once, a reader may still hold it
		return;
#endif
		auto* n = new node{nullptr, 0, [](void* q) { delete static_cast<T*>(q); }, p};
		n->stamp = epoch::advance();
		n->next = in_.load(std::memory_order_relaxed);
		while (!in_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {}
	}
	bool pending() const noexcept { return in_.load(std::memory_order_relaxed) != nullptr || held_ != nullptr; }
	void reclaim() {
		drain();
		if (!held_) { return; }
		const u64 m = epoch::min_active();
		node** pp = &held_;
		while (*pp) {
			node* n = *pp;
			if (n->stamp < m) {
				*pp = n->next;
				n->del(n->p);
				delete n;
			} else {
				pp = &n->next;
			}
		}
	}

private:
	struct node {
		node* next;
		u64 stamp;
		void (*del)(void*);
		void* p;
	};
	void drain() {
		node* n = in_.exchange(nullptr, std::memory_order_acquire);
		while (n) {
			node* nx = n->next;
			n->next = held_;
			held_ = n;
			n = nx;
		}
	}
	static void free_all(node* n) {
		while (n) {
			node* nx = n->next;
			n->del(n->p);
			delete n;
			n = nx;
		}
	}
	std::atomic<node*> in_{nullptr};
	node* held_ = nullptr;
};

// ---- records ------------------------------------------------------------------------------
// [tower: 4 B per level, A' only][docid u64][version u64][pos u32][prev u32][flags u8]
// [key length varint][value length varint][key][value], 8-aligned. A reference names the header.

constexpr std::size_t tower_bytes(unsigned height) noexcept { return align8(std::size_t(height) * 4); }
inline std::size_t rec_size(std::size_t klen, std::size_t vlen, unsigned height) noexcept {
	return align8(tower_bytes(height) + 24 + 1 + vsize(klen) + vsize(vlen) + klen + vlen);
}

struct rec {
	char* h;
	u64& docid() const noexcept { return *reinterpret_cast<u64*>(h); }
	u64& version() const noexcept { return *reinterpret_cast<u64*>(h + 8); }
	std::atomic<u32>& pos() const noexcept { return *reinterpret_cast<std::atomic<u32>*>(h + 16); }
	u32& prev() const noexcept { return *reinterpret_cast<u32*>(h + 20); }
	u8 flags() const noexcept { return u8(h[24]); }
	bool tomb() const noexcept { return flags() & 1; }
	unsigned height() const noexcept { return flags() >> 4; }
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

inline int cmp_keys(std::string_view a, std::string_view b) noexcept {
	const std::size_t n = std::min(a.size(), b.size());
	if (const int c = std::memcmp(a.data(), b.data(), n)) { return c; }
	return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}
// Scan order: key ascending, then version descending.
inline bool rec_less(const rec& a, const rec& b) noexcept {
	const int c = cmp_keys(a.key(), b.key());
	return c < 0 || (c == 0 && a.version() > b.version());
}

// ---- lanes --------------------------------------------------------------------------------

class lane {
public:
	static constexpr unsigned inl = 16, max_chunks = 256;
	lane() {
		for (auto& c : inl_) { c.store(nullptr, std::memory_order_relaxed); }
	}
	lane(const lane&) = delete;
	~lane() {
		const unsigned n = nchunks_;
		for (unsigned c = 0; c < n; ++c) { std::free(chunk(c)); }
		if (auto* e = ext_.load()) { delete[] e; }
	}
	static std::size_t chunk_cap(unsigned c) noexcept { return c >= 8 ? (std::size_t(1) << 20) : (std::size_t(4096) << c); }
	char* chunk(unsigned c) const noexcept {
		if (c < inl) { return inl_[c].load(std::memory_order_acquire); }
		return ext_.load(std::memory_order_acquire)[c - inl].load(std::memory_order_acquire);
	}
	u32 chunk_used(unsigned c) const noexcept { return *reinterpret_cast<u32*>(chunk(c)); }
	u64 end() const noexcept { return end_.load(std::memory_order_acquire); }
	// Writer only. Returns the record's start offset in the current chunk; chunk in `c`.
	char* alloc(std::size_t bytes, unsigned& c, u32& off) {
		if (!nchunks_) { new_chunk(0, bytes); }
		if (off_ + bytes > cap_) {
			*reinterpret_cast<u32*>(chunk(cur_)) = off_;
			new_chunk(cur_ + 1, bytes);
		}
		c = cur_;
		off = off_;
		char* p = chunk(cur_) + off_;
		off_ += u32(bytes);
		return p;
	}
	void publish() noexcept { end_.store((u64(cur_) << 32) | off_, std::memory_order_release); }
	std::size_t bytes_used() const noexcept {
		std::size_t b = 0;
		for (unsigned c = 0; c + 1 < nchunks_; ++c) { b += chunk_used(c); }
		return b + (nchunks_ ? off_ : 0);
	}

private:
	void new_chunk(unsigned c, std::size_t need) {
		if (c >= max_chunks || need + kChunkHdr > (std::size_t(1) << 20)) {
			std::fprintf(stderr, "lanebuf: lane full or record too large\n");
			std::abort();
		}
		const std::size_t cap = std::max(chunk_cap(c), need + kChunkHdr);
		char* p = static_cast<char*>(std::malloc(cap));
		*reinterpret_cast<u32*>(p) = 0;
		if (c < inl) {
			inl_[c].store(p, std::memory_order_release);
		} else {
			if (!ext_.load(std::memory_order_relaxed)) { ext_.store(new std::atomic<char*>[max_chunks - inl](), std::memory_order_release); }
			ext_.load(std::memory_order_relaxed)[c - inl].store(p, std::memory_order_release);
		}
		cur_ = c;
		nchunks_ = c + 1;
		off_ = kChunkHdr;
		cap_ = u32(cap);
		bytes_cap_ += cap;
	}
	std::atomic<char*> inl_[inl];
	std::atomic<std::atomic<char*>*> ext_{nullptr};
	std::atomic<u64> end_{u64(kChunkHdr)};
	unsigned cur_ = 0, nchunks_ = 0;
	u32 off_ = 0, cap_ = 0;
	std::size_t bytes_cap_ = 0;

public:
	std::size_t capacity() const noexcept { return bytes_cap_; }
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

// ---- segments (stubs for the freeze and pre-resolution paths) -----------------------------

struct seg_data {
	std::vector<std::string> keys, values;
	std::vector<u64> docid, version;
	std::vector<u8> tomb;  // empty: none; tombstones are kept so versions do not depend on flush timing
	bool is_tomb(std::size_t o) const noexcept { return !tomb.empty() && tomb[o]; }
};
using live_bits = std::vector<u64>;
struct segment {
	u32 id = 0;
	std::shared_ptr<const seg_data> d;
	std::shared_ptr<const live_bits> live;
	static constexpr std::size_t npos = ~std::size_t(0);
	std::size_t find(std::string_view key) const {
		const auto& k = d->keys;
		auto it = std::lower_bound(k.begin(), k.end(), key, [](const std::string& a, std::string_view b) { return cmp_keys(a, b) < 0; });
		if (it == k.end() || cmp_keys(*it, key) != 0) { return npos; }
		return std::size_t(it - k.begin());
	}
	bool is_live(std::size_t o) const { return ((*live)[o >> 6] >> (o & 63)) & 1; }
	std::size_t size() const { return d->keys.size(); }
};
inline segment make_segment(u32 id, std::shared_ptr<seg_data> d) {
	segment s;
	s.id = id;
	auto bits = std::make_shared<live_bits>((d->keys.size() + 63) / 64, ~u64(0));
	s.d = std::move(d);
	s.live = std::move(bits);
	return s;
}

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
	u64 at = 0;  // the visible position the read used
};

struct stats_t {
	std::size_t records = 0, lane_cap = 0, lane_used = 0, table_bytes = 0, runs_bytes = 0, rejected_bytes = 0;
	u64 groups = 0, group_ops = 0, leader_ns = 0;
	u64 hist[17] = {};  // group sizes 1..16+
	u64 run_builds = 0, run_build_ns = 0, tail_sum = 0, tail_n = 0, chain_follow = 0, restaged = 0, lost_pending = 0;
};

// ---- the index ----------------------------------------------------------------------------

template <order_kind O, conc_kind C>
class buf_index {
public:
	static constexpr bool kSkip = O == order_kind::skiplist;
	static constexpr bool kGroup = C == conc_kind::group;

	// ---- structures
	struct runs_t {
		std::vector<u32> main, delta;
		std::array<u64, kLanes> cm{}, cd{};  // per lane (chunk << 32 | offset) each run's cover reaches
		std::size_t bytes() const { return (main.capacity() + delta.capacity()) * 4 + sizeof(*this); }
	};
	struct skip_t {
		std::atomic<u32> head[kMaxHeight];
		std::atomic<unsigned> maxh{1};
		skip_t() {
			for (auto& h : head) { h.store(0, std::memory_order_relaxed); }
		}
	};
	struct pending_del {
		u32 seg;
		u32 ord;
	};

	struct buffer {
		buf_index* ix;
		u64 base;  // positions are stored as 32-bit deltas from here
		std::atomic<lane*> lanes[kLanes];
		std::atomic<table*> tab;
		std::size_t used = 0;           // leader only: occupied slots
		std::vector<pending_del> pending;  // leader only
		std::size_t nrec = 0;           // leader only: accepted records
		std::atomic<runs_t*> runs{nullptr};
		skip_t sl;
		explicit buffer(buf_index* i, u64 b) : ix(i), base(b) {
			for (auto& l : lanes) { l.store(nullptr, std::memory_order_relaxed); }
			tab.store(table::make(4), std::memory_order_relaxed);
			if constexpr (!kGroup) { lanes[0].store(new lane, std::memory_order_relaxed); }
			if constexpr (!kSkip) { runs.store(new runs_t(initial_runs()), std::memory_order_relaxed); }
		}
		buffer(const buffer&) = delete;
		~buffer() {
			for (auto& l : lanes) { delete l.load(); }
			table::destroy(tab.load());
			delete runs.load();
		}
		static runs_t initial_runs() {
			runs_t r;
			r.cm.fill(kChunkHdr);
			r.cd.fill(kChunkHdr);
			return r;
		}
		lane* lane_of(unsigned l) {
			lane* L = lanes[l].load(std::memory_order_acquire);
			return L;
		}
		lane* lane_make(unsigned l) {  // the lane's owner only
			lane* L = lanes[l].load(std::memory_order_relaxed);
			if (!L) {
				L = new lane;
				lanes[l].store(L, std::memory_order_release);
			}
			return L;
		}
		char* ptr(u32 ref) const noexcept {
			lane* L = lanes[kGroup ? (ref >> 25) : 0].load(std::memory_order_acquire);
			return L->chunk((ref >> 17) & 0xFF) + (std::size_t(ref & 0x1FFFF) << 3);
		}
		rec at(u32 ref) const noexcept { return rec{ptr(ref)}; }
		static u32 make_ref(unsigned l, unsigned chunk, u32 off) noexcept { return (u32(l) << 25) | (u32(chunk) << 17) | (off >> 3); }
	};

	struct view {
		u64 gen = 0;
		u64 freeze_pos = 0;
		std::shared_ptr<buffer> active;
		std::vector<std::shared_ptr<buffer>> frozen;  // oldest first
		std::vector<segment> segs;                    // oldest first
	};

	// ---- the write group's queue
	enum : u32 { S_WAIT = 0, S_LEADER = 1, S_PAR = 2, S_DONE = 3, S_PARKED = 0x100 };
	enum class chore_kind : u8 { none, freeze, install, add_segment, merge };
	struct resolution {
		u8 kind = 0;  // 0 none, 1 segment, 2 frozen buffer
		bool live = false;
		u32 seg = 0, ord = 0;
		u64 docid = 0, version = 0;
	};
	struct pop {
		op_kind kind;
		i64 expected;
		u32 ref;
		u64 hash;
		resolution res;
	};
	struct writer {
		std::atomic<u32> state{S_WAIT};
		writer* older = nullptr;
		writer* newer = nullptr;
		unsigned lane = 0, nops = 0;
		buffer* buf = nullptr;
		u64 gen = 0;
		bool preres = false;
		chore_kind chore = chore_kind::none;
		segment seg;
		std::vector<u32> ids;  // merge: the source segment ids
		bool accepted = false;
		u64 pos = 0;
		pop ops[kMaxOps];
		op_out out[kMaxOps];
	};

	// ---- construction
	buf_index() {
		auto* v = new view;
		v->active = std::make_shared<buffer>(this, 0);
		view_.store(v, std::memory_order_release);
	}
	buf_index(const buf_index&) = delete;
	~buf_index() {
		retired_.reclaim();
		delete view_.load();
	}

	// ---- lanes (the checkout's claim)
	unsigned claim_lane() {
		if constexpr (!kGroup) { return 0; }
		for (unsigned spins = 0;; ++spins) {
			for (unsigned i = 0; i < kLanes; ++i) {
				bool f = false;
				if (!lane_busy_[i].load(std::memory_order_relaxed) && lane_busy_[i].compare_exchange_strong(f, true, std::memory_order_acq_rel)) { return i; }
			}
			if (spins > 100) { std::this_thread::yield(); }
			cpu_relax();
		}
	}
	void release_lane(unsigned l) {
		if constexpr (kGroup) { lane_busy_[l].store(false, std::memory_order_release); }
	}

	// ---- reads
	struct scope {
		u64 v;
		const view* vw;
	};
	// Inside an epoch guard.
	scope open_scope() const { return finish_scope(load_v()); }
	// The two halves of opening a scope, split so a test can schedule a freeze between them.
	u64 load_v() const noexcept { return vis_.load(std::memory_order_acquire); }
	// A view past v carries positionless state (segment live bits) above v: read at its position.
	scope finish_scope(u64 v) const {
		for (;;) {
			const view* vw = view_.load(std::memory_order_acquire);
#if LANEBUF_MUTANT == 8
			return {v, vw};  // mutant: read at v whatever the view reflects
#endif
			if (vw->freeze_pos <= v) { return {v, vw}; }
			v = vis_.load(std::memory_order_acquire);
		}
	}
	u64 visible() const noexcept { return vis_.load(std::memory_order_acquire); }

	// Point read at the current visible position. `out` receives the value when non-null.
	got get(std::string_view key, std::string* out = nullptr) const {
		epoch::guard g;
		const scope s = open_scope();
		return get_in(s, key, out);
	}
	got get_in(const scope& s, std::string_view key, std::string* out) const {
		got r;
		r.at = s.v;
		const u64 h = hash64(key.data(), key.size());
		u32 ref = chain_in(*s.vw->active, key, h, s.v);
		if (ref) { return finish(*s.vw->active, ref, out, r); }
		for (std::size_t i = s.vw->frozen.size(); i-- > 0;) {
			ref = chain_in(*s.vw->frozen[i], key, h, s.v);
			if (ref) { return finish(*s.vw->frozen[i], ref, out, r); }
		}
		for (std::size_t i = s.vw->segs.size(); i-- > 0;) {
			const segment& sg = s.vw->segs[i];
			if (const std::size_t o = sg.find(key); o != segment::npos && sg.is_live(o)) {
				if (sg.d->is_tomb(o)) { return r; }
				r.found = true;
				r.docid = sg.d->docid[o];
				r.version = sg.d->version[o];
				if (out) { out->assign(sg.d->values[o]); }
				return r;
			}
		}
		return r;
	}

	// Scan from `from`: up to n live entries at the visible position; visit(key, value) each.
	template <class F>
	std::size_t scan(std::string_view from, std::size_t n, F&& visit, u64* at = nullptr) const {
		epoch::guard g;
		const scope s = open_scope();
		if (at) { *at = s.v; }
		return scan_in(s, from, n, std::forward<F>(visit));
	}

	// ---- writes
	// One transaction. Returns whether it was accepted; `out` carries per-op results.
	bool write(unsigned lane, const op_in* ops, unsigned n, write_out& out) {
		epoch::guard g;
		if constexpr (kGroup) {
			return write_group(lane, ops, n, out);
		} else {
			return write_lease(ops, n, out);
		}
	}
	// Chores run as writerless groups (D2).
	void freeze() { chore(chore_kind::freeze, segment{}); }
	void install_segment(segment s) { chore(chore_kind::install, std::move(s)); }
	void add_segment(segment s) { chore(chore_kind::add_segment, std::move(s)); }
	// A merge of segments `ids` into `out` (built by build_merge_segment) installs as a lease operation.
	void merge_install(segment out, std::vector<u32> ids) { chore(chore_kind::merge, std::move(out), std::move(ids)); }
	// Flushes omit tombstones (the engine's segments hold none): versions then restart after a delete.
	void set_drop_tombstones(bool on) { drop_tomb_ = on; }
	u64 lost_pending() const noexcept { return stats_.lost_pending; }
	// Live copies of a key across segments and frozen buffers' flushed state (format 14's one-live-copy invariant).
	std::size_t live_copies(std::string_view key) const {
		epoch::guard g;
		std::size_t n = 0;
		for (const segment& sg : view_.load()->segs) {
			if (const std::size_t o = sg.find(key); o != segment::npos && sg.is_live(o) && !sg.d->is_tomb(o)) { ++n; }
		}
		return n;
	}
	bool build_merge_segment(segment& out, u32 id, const std::vector<u32>& ids) {
		epoch::guard g;
		const view* vw = view_.load(std::memory_order_acquire);
		std::map<std::string, std::array<std::size_t, 2>, std::less<>> best;  // key -> (segment index, ordinal)
		auto d = std::make_shared<seg_data>();
		std::vector<const segment*> src;
		for (u32 sid : ids) {
			for (const segment& sg : vw->segs) {
				if (sg.id == sid) { src.push_back(&sg); }
			}
		}
		if (src.size() != ids.size()) { return false; }
		for (std::size_t i = 0; i < src.size(); ++i) {
			for (std::size_t o = 0; o < src[i]->size(); ++o) {
				if (!src[i]->is_live(o)) { continue; }
				auto it = best.find(src[i]->d->keys[o]);
				if (it == best.end() || src[it->second[0]]->d->version[it->second[1]] < src[i]->d->version[o]) { best[src[i]->d->keys[o]] = {i, o}; }
			}
		}
		for (auto& [k, io] : best) {
			const segment& sg = *src[io[0]];
			d->keys.push_back(k);
			d->values.push_back(sg.d->values[io[1]]);
			d->docid.push_back(sg.d->docid[io[1]]);
			d->version.push_back(sg.d->version[io[1]]);
			d->tomb.push_back(sg.d->is_tomb(io[1]));
		}
		out = make_segment(id, std::move(d));
		return true;
	}

	// ---- introspection (call when quiescent)
	stats_t stats() const {
		stats_t st = stats_;
		st.run_builds = rs_.run_builds.load();
		st.run_build_ns = rs_.run_build_ns.load();
		st.tail_sum = rs_.tail_sum.load();
		st.tail_n = rs_.tail_n.load();
		const view* vw = view_.load();
		auto add = [&](const buffer& b) {
			for (unsigned l = 0; l < kLanes; ++l) {
				if (lane* L = b.lanes[l].load()) {
					st.lane_cap += L->capacity();
					st.lane_used += L->bytes_used();
				}
			}
			st.records += b.nrec;
			st.table_bytes += b.tab.load()->bytes();
			if (auto* r = b.runs.load()) { st.runs_bytes += r->bytes(); }
		};
		add(*vw->active);
		for (auto& f : vw->frozen) { add(*f); }
		return st;
	}
	const view* current_view() const { return view_.load(std::memory_order_acquire); }
	buffer& active_buffer() const { return *view_.load()->active; }
	void set_leader_timing(bool on) { time_leader_ = on; }
	u64 positions() const { return pos_; }

	// Builds a segment from the oldest frozen buffer's newest records (the flush's sort).
	bool build_flush_segment(segment& out, u32 id) {
		epoch::guard g;
		const view* vw = view_.load(std::memory_order_acquire);
		if (vw->frozen.empty()) { return false; }
		buffer& b = *vw->frozen.front();
		std::vector<u32> refs;
		ordered_refs(b, refs);
		auto d = std::make_shared<seg_data>();
		std::string_view last;
		bool have = false;
		for (u32 ref : refs) {
			rec r = b.at(ref);
			if (have && cmp_keys(r.key(), last) == 0) { continue; }  // older version of a key
			last = r.key();
			have = true;
			if (r.tomb() && drop_tomb_) { continue; }
			d->keys.emplace_back(r.key());
			d->tomb.push_back(r.tomb());
			d->values.emplace_back(r.value());
			d->docid.push_back(r.docid());
			d->version.push_back(r.version());
		}
		out = make_segment(id, std::move(d));
		return true;
	}

private:
	friend struct test_access;
	// ---- chain reading
	u32 find_ref(const buffer& b, std::string_view key, u64 h) const noexcept {
		const table* t = b.tab.load(std::memory_order_acquire);
		const u32 tag = u32(h >> 32);
		u32 i = slot_home(tag, t->bits);
		auto* sl = const_cast<table*>(t)->slots();
		for (;;) {
			const u64 s = sl[i].load(std::memory_order_acquire);
			if (!s) { return 0; }
			if (slot_tag(s) == tag && cmp_keys(b.at(slot_ref(s)).key(), key) == 0) { return slot_ref(s); }
			i = (i + 1) & t->mask;
		}
	}
	// The newest record of the key at or below v (0: none).
	u32 chain_in(const buffer& b, std::string_view key, u64 h, u64 v) const noexcept {
		u32 ref = find_ref(b, key, h);
		while (ref) {
			rec r = b.at(ref);
			const u32 p = r.pos().load(std::memory_order_relaxed);
			if (p != kNever && p != kPending && b.base + p <= v) { return ref; }
			ref = r.prev();
		}
		return 0;
	}
	got finish(const buffer& b, u32 ref, std::string* out, got r) const {
		rec rc = b.at(ref);
		if (rc.tomb()) { return r; }
		r.found = true;
		r.docid = rc.docid();
		r.version = rc.version();
		if (out) { out->assign(rc.value()); }
		return r;
	}

	// ---- pre-stage: copy a record into the writer's lane
	u32 stage_record(buffer& b, unsigned l, std::string_view key, std::string_view value, bool tomb) {
		lane* L = b.lane_make(kGroup ? l : 0);
		const unsigned h = kSkip ? random_height() : 0;
		const std::size_t sz = rec_size(key.size(), value.size(), h);
		unsigned c;
		u32 off;
		char* p = L->alloc(sz, c, off);
		std::memset(p, 0, tower_bytes(h));
		char* hd = p + tower_bytes(h);
		std::memset(hd, 0, 24);
		reinterpret_cast<std::atomic<u32>*>(hd + 16)->store(kPending, std::memory_order_relaxed);
		hd[24] = char((tomb ? 1 : 0) | (h << 4));
		char* q = put_varint(put_varint(hd + 25, key.size()), value.size());
		std::memcpy(q, key.data(), key.size());
		std::memcpy(q + key.size(), value.data(), value.size());
		L->publish();
		return buffer::make_ref(kGroup ? l : 0, c, off + u32(tower_bytes(h)));
	}
	static unsigned random_height() noexcept {
		thread_local u64 s = 0x9E3779B97F4A7C15ull ^ (u64)(std::uintptr_t)&s;
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		unsigned h = 1;
		u64 x = s;
		while (h < kMaxHeight && (x & 3) == 0) {
			++h;
			x >>= 2;
		}
		return h;
	}

	// ---- resolution against frozen buffers and segments (pre-resolution, D4)
	void resolve(const view& vw, std::string_view key, u64 h, resolution& r) const {
		r = resolution{};
		for (std::size_t i = vw.frozen.size(); i-- > 0;) {
			const buffer& b = *vw.frozen[i];
			u32 ref = find_ref(b, key, h);
			while (ref && b.at(ref).pos().load(std::memory_order_relaxed) == kNever) { ref = b.at(ref).prev(); }
			if (ref) {
				rec x = b.at(ref);
				r.kind = 2;
				r.live = !x.tomb();
				r.docid = x.docid();
				r.version = x.version();
				return;
			}
		}
		for (std::size_t i = vw.segs.size(); i-- > 0;) {
			const segment& sg = vw.segs[i];
			if (const std::size_t o = sg.find(key); o != segment::npos && sg.is_live(o)) {
				r.kind = 1;
				r.live = !sg.d->is_tomb(o);
				r.seg = sg.id;
				r.ord = u32(o);
				r.docid = sg.d->docid[o];
				r.version = sg.d->version[o];
				return;
			}
		}
	}
	bool has_lower(const view& vw) const noexcept { return !vw.frozen.empty() || !vw.segs.empty(); }

	// ---- the version map, leader side
	struct locate_t {
		table* t;
		u32 i;       // slot index of the key, or the empty slot
		u64 old;     // the slot's value (0: new key)
		u32 cur;     // the newest non-rejected record (0: none)
	};
	locate_t locate(buffer& b, std::string_view key, u64 h) const noexcept {
		table* t = b.tab.load(std::memory_order_relaxed);
		const u32 tag = u32(h >> 32);
		u32 i = slot_home(tag, t->bits);
		auto* sl = t->slots();
		for (;;) {
			const u64 s = sl[i].load(std::memory_order_relaxed);
			if (!s) { return {t, i, 0, 0}; }
			if (slot_tag(s) == tag && cmp_keys(b.at(slot_ref(s)).key(), key) == 0) {
				u32 ref = slot_ref(s);
				while (ref && b.at(ref).pos().load(std::memory_order_relaxed) == kNever) { ref = b.at(ref).prev(); }
				return {t, i, s, ref};
			}
			i = (i + 1) & t->mask;
		}
	}
	void grow(buffer& b) {
		table* old = b.tab.load(std::memory_order_relaxed);
		table* nt = table::make(old->bits + 1);
		auto* os = old->slots();
		auto* ns = nt->slots();
		for (u32 i = 0; i <= old->mask; ++i) {
			const u64 s = os[i].load(std::memory_order_relaxed);
			if (!s) { continue; }
			u32 j = slot_home(slot_tag(s), nt->bits);
			while (ns[j].load(std::memory_order_relaxed)) { j = (j + 1) & nt->mask; }
			ns[j].store(s, std::memory_order_relaxed);
		}
		b.tab.store(nt, std::memory_order_release);
		struct tdel {
			table* t;
			~tdel() { table::destroy(t); }
		};
		retired_.retire(new tdel{old});
	}
	void store_slot(std::atomic<u64>& sl, u64 v) noexcept {
#if LANEBUF_MUTANT == 2
		sl.store(v, std::memory_order_relaxed);
#else
		sl.store(v, std::memory_order_release);
#endif
	}

	// ---- stage 1 for one transaction (the lease holder)
	struct undo_t {
		u32 i;
		u64 old;
		u32 ref;
		table* t;
	};
	bool stage1(writer& w, buffer& b, const view& vw) {
		const u64 pos = ++pos_;
		const u32 pd = u32(pos - b.base);
		w.pos = pos;
		undo_t undo[kMaxOps];
		unsigned nu = 0;
		const std::size_t pend0 = b.pending.size();
		const u64 docid0 = docid_;
		bool ok = true;
#if LANEBUF_MUTANT == 4
		vis_.store(pos, std::memory_order_release);  // mutant: V before stage 1 ends
#endif
		for (unsigned k = 0; k < w.nops; ++k) {
			pop& o = w.ops[k];
			rec nr = b.at(o.ref);
			const std::string_view key = nr.key();
			if ((b.used + 1) * 8 > (std::size_t(b.tab.load(std::memory_order_relaxed)->mask) + 1) * 7) { grow(b); }
			locate_t L = locate(b, key, o.hash);
			bool live;
			u64 base_version = 0, base_docid = 0;
			bool have_prior = false;
			resolution res;
			if (L.cur) {
				rec c = b.at(L.cur);
				live = !c.tomb();
				base_version = c.version();
				base_docid = c.docid();
				have_prior = true;
			} else if (has_lower(vw)) {
#if LANEBUF_MUTANT == 5
				const bool usable = w.preres;  // mutant: pre-resolution trusted across a changed generation
#else
				const bool usable = w.preres && w.gen == vw.gen;
#endif
				if (usable) { res = o.res; } else { resolve(vw, key, o.hash, res); }
				live = res.live;
				if (res.kind) {
					base_version = res.version;
					base_docid = res.docid;
					have_prior = true;
				}
			} else {
				live = false;
			}
			const i64 ex = o.expected;
			bool pass = true;
			if (o.kind == op_kind::del) {
				pass = live && (ex < 0 || u64(ex) == base_version);
			} else if (ex == 0) {
				pass = !live;
			} else if (ex > 0) {
				pass = live && u64(ex) == base_version;
			}
			if (!pass) {
				ok = false;
				break;
			}
			const u64 docid = live ? base_docid : ++docid_;
			const u64 version = have_prior ? base_version + 1 : 1;
			w.out[k] = {true, docid, version};
			if (res.kind == 1 && res.live) { b.pending.push_back({res.seg, res.ord}); }
			const u64 nslot = pack_slot(u32(o.hash >> 32), o.ref);
#if LANEBUF_MUTANT == 1
			// mutant: the slot is published before the header is filled
			if (L.old) {
				L.t->slots()[L.i].store(nslot, std::memory_order_release);
			} else {
				++b.used;
				L.t->slots()[L.i].store(nslot, std::memory_order_release);
			}
#endif
			nr.docid() = docid;
			nr.version() = version;
			nr.prev() = L.cur;
			nr.pos().store(pd, std::memory_order_relaxed);
#if LANEBUF_MUTANT != 1
			if (!L.old) { ++b.used; }
			store_slot(L.t->slots()[L.i], nslot);
#endif
			undo[nu++] = {L.i, L.old, o.ref, L.t};
			++b.nrec;
		}
		if (!ok) {
			for (unsigned k = nu; k-- > 0;) {
#if LANEBUF_MUTANT != 3
				b.at(undo[k].ref).pos().store(kNever, std::memory_order_release);
#endif
				if (undo[k].old) { undo[k].t->slots()[undo[k].i].store(undo[k].old, std::memory_order_release); }
				--b.nrec;
			}
			docid_ = docid0;
			b.pending.resize(pend0);
			for (unsigned k = nu; k < w.nops; ++k) { b.at(w.ops[k].ref).pos().store(kNever, std::memory_order_release); }
			for (unsigned k = 0; k < w.nops; ++k) { w.out[k].ok = false; }
			rejected_bytes_ += 0;
		}
		w.accepted = ok;
		return ok;
	}

	// Copies a writer's records into the leader's lane of the current buffer (stale pre-stage).
	void restage(writer& w, buffer& b, unsigned leader_lane) {
		++stats_.restaged;
		for (unsigned k = 0; k < w.nops; ++k) {
			pop& o = w.ops[k];
			rec old = w.buf->at(o.ref);
			const bool tomb = old.tomb();
			const std::string_view key = old.key(), value = old.value();
			const u32 nref = stage_record(b, leader_lane, key, value, tomb);
			old.pos().store(kNever, std::memory_order_release);
			o.ref = nref;
		}
		w.buf = &b;
	}

	// ---- a group's members, in order (the lease holder)
	void apply_member(writer& w, unsigned leader_lane) {
		view* vw = view_.load(std::memory_order_relaxed);
		if (w.chore != chore_kind::none) {
			run_chore(w, *vw);
			return;
		}
		buffer& b = *vw->active;
		if (w.buf != &b) { restage(w, b, leader_lane); }
		if (!kGroup || (!w.preres)) {
			// lease mode and unresolved writers resolve inside stage 1
		}
		stage1(w, b, *vw);
	}

	// ---- chores
	void chore(chore_kind k, segment s, std::vector<u32> ids = {}) {
		epoch::guard g;
		writer w;
		w.chore = k;
		w.seg = std::move(s);
		w.ids = std::move(ids);
		if constexpr (kGroup) {
			run_group(w);
		} else {
			lease_lock();
			apply_member(w, 0);
			lease_unlock();
		}
	}
	void run_chore(writer& w, const view& vw) {
		switch (w.chore) {
		case chore_kind::freeze: do_freeze(vw); break;
		case chore_kind::install: do_install(vw, w.seg); break;
		case chore_kind::add_segment: do_add_segment(vw, w.seg); break;
		case chore_kind::merge: do_merge_install(vw, w.seg, w.ids); break;
		default: break;
		}
	}
	view* clone_view(const view& vw) const {
		auto* nv = new view;
		nv->gen = vw.gen + 1;
		nv->freeze_pos = vis_.load(std::memory_order_relaxed);
		nv->active = vw.active;
		nv->frozen = vw.frozen;
		nv->segs = vw.segs;
		return nv;
	}
	void publish_view(view* nv, const view& old) {
		view_.store(nv, std::memory_order_release);
		retired_.retire(const_cast<view*>(&old));
	}
	void do_freeze(const view& vw) {
		view* nv = clone_view(vw);
		buffer& sealed = *vw.active;
		// pending segment deletes: copy-on-write live bits (D8)
		if (!sealed.pending.empty()) {
			std::sort(sealed.pending.begin(), sealed.pending.end(), [](auto& a, auto& b) { return a.seg < b.seg || (a.seg == b.seg && a.ord < b.ord); });
			std::size_t i = 0;
			while (i < sealed.pending.size()) {
				const u32 id = sealed.pending[i].seg;
				segment* sg = nullptr;
				for (auto& s : nv->segs) {
					if (s.id == id) { sg = &s; }
				}
				if (!sg) {  // the segment is gone (a merge installed): its pending deletes are lost
					for (; i < sealed.pending.size() && sealed.pending[i].seg == id; ++i) { ++stats_.lost_pending; }
					continue;
				}
				auto bits = std::make_shared<live_bits>(*sg->live);
				for (; i < sealed.pending.size() && sealed.pending[i].seg == id; ++i) {
					const u32 o = sealed.pending[i].ord;
					(*bits)[o >> 6] &= ~(u64(1) << (o & 63));
				}
				sg->live = std::move(bits);
			}
		}
		nv->frozen.push_back(vw.active);
		nv->active = std::make_shared<buffer>(this, pos_);
		publish_view(nv, vw);
	}
	void do_merge_install(const view& vw, const segment& out, const std::vector<u32>& ids) {
		view* nv = clone_view(vw);
		std::vector<segment> segs;
		bool placed = false;
		for (auto& sg : nv->segs) {
			if (std::find(ids.begin(), ids.end(), sg.id) != ids.end()) {
				if (!placed) { segs.push_back(out); }
				placed = true;
			} else {
				segs.push_back(sg);
			}
		}
		nv->segs = std::move(segs);
		publish_view(nv, vw);
	}
	void do_add_segment(const view& vw, const segment& s) {
		view* nv = clone_view(vw);
		nv->segs.push_back(s);
		publish_view(nv, vw);
	}
	// Installs a flush of the oldest frozen buffer: keys shadowed by newer buffers die in it.
	void do_install(const view& vw, const segment& s) {
		view* nv = clone_view(vw);
		segment sg = s;
		auto bits = std::make_shared<live_bits>(*sg.live);
		for (std::size_t o = 0; o < sg.size(); ++o) {
			const std::string_view key = sg.d->keys[o];
			const u64 h = hash64(key.data(), key.size());
			bool shadowed = false;
			auto shadow = [&](const buffer& b) {
				u32 ref = find_ref(b, key, h);
				while (ref && b.at(ref).pos().load(std::memory_order_relaxed) == kNever) { ref = b.at(ref).prev(); }
				return ref != 0;
			};
			shadowed = shadow(*vw.active);
			for (std::size_t i = 1; !shadowed && i < vw.frozen.size(); ++i) { shadowed = shadow(*vw.frozen[i]); }
			if (shadowed) { (*bits)[o >> 6] &= ~(u64(1) << (o & 63)); }
		}
		sg.live = std::move(bits);
		nv->frozen.erase(nv->frozen.begin());
		nv->segs.push_back(std::move(sg));
		publish_view(nv, vw);
	}

	// ---- the lease (fallback)
	void lease_lock() {
		u32 z = 0;
		if (lease_.compare_exchange_strong(z, 1, std::memory_order_acquire)) { return; }
		for (int i = 0; i < 200; ++i) {
			cpu_relax();
			z = 0;
			if (lease_.load(std::memory_order_relaxed) == 0 && lease_.compare_exchange_weak(z, 1, std::memory_order_acquire)) { return; }
		}
		while (lease_.exchange(2, std::memory_order_acquire) != 0) { lease_.wait(2, std::memory_order_relaxed); }
	}
	void lease_unlock() {
		if (lease_.exchange(0, std::memory_order_release) == 2) { lease_.notify_one(); }
	}
	bool write_lease(const op_in* ops, unsigned n, write_out& out) {
		writer w;
		w.nops = n;
		lease_lock();
		view* vw = view_.load(std::memory_order_relaxed);
		buffer& b = *vw->active;
		w.buf = &b;
		for (unsigned k = 0; k < n; ++k) {
			w.ops[k].kind = ops[k].kind;
			w.ops[k].expected = ops[k].expected;
			w.ops[k].hash = hash64(ops[k].key.data(), ops[k].key.size());
			w.ops[k].ref = stage_record(b, 0, ops[k].key, ops[k].value, ops[k].kind == op_kind::del);
		}
		stage1(w, b, *vw);
		vis_.store(pos_, std::memory_order_release);
		++stats_.groups;
		++stats_.hist[0];
		if (retired_.pending()) { retired_.reclaim(); }
		lease_unlock();
		return finish_write(w, out);
	}
	bool finish_write(const writer& w, write_out& out) {
		out.accepted = w.accepted;
		out.pos = w.pos;
		for (unsigned k = 0; k < w.nops; ++k) { out.ops[k] = w.out[k]; }
		return w.accepted;
	}

	// ---- the write group
	static u32 await_change(std::atomic<u32>& st, u32 from) {
		for (int i = 0; i < 2000; ++i) {
			const u32 s = st.load(std::memory_order_acquire);
			if ((s & 0xFF) != from) { return s & 0xFF; }
			cpu_relax();
		}
		u32 s = st.load(std::memory_order_acquire);
		while ((s & 0xFF) == from) {
			if (!(s & S_PARKED)) {
				if (!st.compare_exchange_weak(s, s | S_PARKED, std::memory_order_acq_rel)) { continue; }
				s |= S_PARKED;
			}
			st.wait(s, std::memory_order_acquire);
			s = st.load(std::memory_order_acquire);
		}
		return s & 0xFF;
	}
	static void post(std::atomic<u32>& st, u32 to) {
		if (st.exchange(to, std::memory_order_release) & S_PARKED) { st.notify_one(); }
	}

	bool write_group(unsigned lane, const op_in* ops, unsigned n, write_out& out) {
		writer w;
		view* vw = view_.load(std::memory_order_acquire);
		buffer& b = *vw->active;
		w.lane = lane;
		w.nops = n;
		w.buf = &b;
		w.gen = vw->gen;
		const bool lower = has_lower(*vw);
		w.preres = lower;
		for (unsigned k = 0; k < n; ++k) {
			pop& o = w.ops[k];
			o.kind = ops[k].kind;
			o.expected = ops[k].expected;
			o.hash = hash64(ops[k].key.data(), ops[k].key.size());
			o.ref = stage_record(b, lane, ops[k].key, ops[k].value, ops[k].kind == op_kind::del);
			if (lower) { resolve(*vw, ops[k].key, o.hash, o.res); }
		}
		run_group(w);
		return finish_write(w, out);
	}

	// Link, then lead or wait. Returns when the writer's verdict is in.
	void run_group(writer& w) {
		writer* older = head_.load(std::memory_order_relaxed);
		do {
			w.older = older;
		} while (!head_.compare_exchange_weak(older, &w, std::memory_order_acq_rel, std::memory_order_relaxed));
		if (older != nullptr) {
			u32 st = await_change(w.state, S_WAIT);
			if (st == S_PAR) {
				link_own(w);
				par_pending_.fetch_sub(1, std::memory_order_acq_rel);
				st = await_change(w.state, S_PAR);
			}
			if (st == S_DONE) { return; }
		}
		lead(w);
	}

	void link_own(writer& w) {
		if constexpr (kSkip) {
			if (!w.accepted) { return; }
			for (unsigned k = 0; k < w.nops; ++k) { skip_insert(*w.buf, w.ops[k].ref); }
		}
	}

	void lead(writer& self) {
		static constexpr unsigned kCap = 64;
		const bool timed = time_leader_;
		const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		writer* last = head_.load(std::memory_order_acquire);
		for (writer* x = last; x != &self; x = x->older) { x->older->newer = x; }
		writer* members[kCap];
		unsigned nm = 0;
		members[nm++] = &self;
		const bool self_chore = self.chore != chore_kind::none;
		writer* g_last = &self;
		while (!self_chore && g_last != last && nm < kCap) {
			writer* nx = g_last->newer;
			if (nx->chore != chore_kind::none) { break; }
			members[nm++] = nx;
			g_last = nx;
		}
		const unsigned leader_lane = self.lane;
		for (unsigned i = 0; i < nm; ++i) { apply_member(*members[i], leader_lane); }
		if constexpr (kSkip) {
			if (!self_chore) {
				par_pending_.store(int(nm) - 1, std::memory_order_relaxed);
				for (unsigned i = 1; i < nm; ++i) { post(members[i]->state, S_PAR); }
				link_own(self);
				while (par_pending_.load(std::memory_order_acquire) > 0) { cpu_relax(); }
			}
		}
#if LANEBUF_MUTANT != 4
		vis_.store(pos_, std::memory_order_release);
#endif
		++stats_.groups;
		stats_.group_ops += nm;
		++stats_.hist[std::min(nm, 16u)];
		if (retired_.pending()) { retired_.reclaim(); }
		// hand over: the oldest writer linked after the group leads next
		writer* exp = g_last;
		if (!head_.compare_exchange_strong(exp, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
			writer* x = exp;
			while (x->older != g_last) { x = x->older; }
			x->older = nullptr;
			post(x->state, S_LEADER);
		}
		for (unsigned i = 1; i < nm; ++i) { post(members[i]->state, S_DONE); }
		if (timed) { stats_.leader_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
	}

	// ---- the skip list (A')
	std::atomic<u32>& sl_next(buffer& b, u32 node, unsigned lvl) const noexcept { return node ? b.at(node).tower(lvl) : b.sl.head[lvl]; }
	// Node `n` sorts before (key, version).
	bool sl_before(buffer& b, u32 n, std::string_view key, u64 ver) const noexcept {
		rec r = b.at(n);
		const int c = cmp_keys(r.key(), key);
		return c < 0 || (c == 0 && r.version() > ver);
	}
	void skip_insert(buffer& b, u32 ref) {
		rec r = b.at(ref);
		const unsigned height = r.height();
		const std::string_view key = r.key();
		const u64 ver = r.version();
		unsigned top = b.sl.maxh.load(std::memory_order_relaxed);
		while (height > top && !b.sl.maxh.compare_exchange_weak(top, height, std::memory_order_acq_rel)) {}
		top = std::max(top, height);
		u32 prev[kMaxHeight], next[kMaxHeight];
		u32 x = 0;
		for (unsigned l = top; l-- > 0;) {
			for (;;) {
				const u32 n = sl_next(b, x, l).load(std::memory_order_acquire);
				if (n && sl_before(b, n, key, ver)) {
					x = n;
				} else {
					prev[l] = x;
					next[l] = n;
					break;
				}
			}
		}
		for (unsigned l = 0; l < height; ++l) {
			for (;;) {
				r.tower(l).store(next[l], std::memory_order_relaxed);
				u32 exp = next[l];
				if (sl_next(b, prev[l], l).compare_exchange_strong(exp, ref, std::memory_order_release, std::memory_order_acquire)) { break; }
				u32 y = prev[l];
				for (;;) {
					const u32 n = sl_next(b, y, l).load(std::memory_order_acquire);
					if (n && sl_before(b, n, key, ver)) {
						y = n;
					} else {
						prev[l] = y;
						next[l] = n;
						break;
					}
				}
			}
		}
	}
	u32 skip_seek(buffer& b, std::string_view key) const {
		u32 x = 0;
		for (unsigned l = b.sl.maxh.load(std::memory_order_acquire); l-- > 0;) {
			for (;;) {
				const u32 n = sl_next(b, x, l).load(std::memory_order_acquire);
				if (n && sl_before(b, n, key, ~u64(0))) {
					x = n;
				} else {
					break;
				}
			}
		}
		return sl_next(b, x, 0).load(std::memory_order_acquire);
	}

	// ---- runs (C)
	static bool refs_less(const buffer& b, u32 x, u32 y) noexcept { return rec_less(b.at(x), b.at(y)); }
	// Walks every lane from `from`. cover: stops at the first record not yet visible at v and
	// advances the cursor; tail: skips what is not visible and keeps the cursor.
	template <bool Cover>
	void collect(buffer& b, const std::array<u64, kLanes>& from, u64 v, std::array<u64, kLanes>* out, std::vector<u32>& refs) const {
		for (unsigned l = 0; l < kLanes; ++l) {
			lane* L = b.lanes[l].load(std::memory_order_acquire);
			if (!L) {
				if (out) { (*out)[l] = from[l]; }
				continue;
			}
			const u64 end = L->end();
			unsigned c = unsigned(from[l] >> 32);
			u32 o = u32(from[l]);
			const unsigned ec = unsigned(end >> 32);
			const u32 eo = u32(end);
			for (;;) {
				const u32 limit = c < ec ? L->chunk_used(c) : eo;
				if (o >= limit) {
					if (c < ec) {
						++c;
						o = kChunkHdr;
						continue;
					}
					break;
				}
				rec r{L->chunk(c) + o};
				const u32 p = r.pos().load(std::memory_order_relaxed);
				if (p == kNever) {
				} else if (p == kPending || b.base + p > v) {
					if (Cover) { break; }
				} else {
					refs.push_back(buffer::make_ref(l, c, o));
				}
				o += u32(r.size());
			}
			if (out) { (*out)[l] = (u64(c) << 32) | o; }
		}
	}
	std::vector<u32> merge_refs(const buffer& b, const std::vector<u32>& x, const std::vector<u32>& y) const {
		std::vector<u32> r(x.size() + y.size());
		std::merge(x.begin(), x.end(), y.begin(), y.end(), r.begin(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
		return r;
	}
	// The runs a reader at v merges, with its private tail. Builds a delta when the tail is long.
	struct run_view {
		const runs_t* r;
		std::vector<u32> tail;
	};
	run_view runs_at(buffer& b, u64 v) const {
		runs_t* cur = b.runs.load(std::memory_order_acquire);
		run_view rv;
		collect<false>(b, cur->cd, v, nullptr, rv.tail);
		rs_.tail_sum.fetch_add(rv.tail.size(), std::memory_order_relaxed);
		rs_.tail_n.fetch_add(1, std::memory_order_relaxed);
		if (rv.tail.size() > kTailMax) {
			const auto t0 = std::chrono::steady_clock::now();
			auto* nr = new runs_t;
			std::vector<u32> fresh;
			collect<true>(b, cur->cd, v, &nr->cd, fresh);
			std::sort(fresh.begin(), fresh.end(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
			nr->cm = cur->cm;
			nr->main = cur->main;
			nr->delta = merge_refs(b, cur->delta, fresh);
			if (nr->delta.size() > kDeltaMax) {
				nr->main = merge_refs(b, nr->main, nr->delta);
				nr->delta.clear();
				nr->cm = nr->cd;
			}
			runs_t* exp = cur;
			if (b.runs.compare_exchange_strong(exp, nr, std::memory_order_acq_rel)) {
				retired_.retire(cur);
				cur = nr;
			} else {
				delete nr;
				cur = exp;
			}
			rv.tail.clear();
			collect<false>(b, cur->cd, v, nullptr, rv.tail);
			rs_.run_builds.fetch_add(1, std::memory_order_relaxed);
			rs_.run_build_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
		}
		std::sort(rv.tail.begin(), rv.tail.end(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
		rv.r = cur;
		return rv;
	}
	// Every record of a frozen buffer in scan order (nothing is pending in it).
	void ordered_refs(buffer& b, std::vector<u32>& refs) const {
		if constexpr (kSkip) {
			for (u32 n = b.sl.head[0].load(std::memory_order_acquire); n; n = b.at(n).tower(0).load(std::memory_order_acquire)) { refs.push_back(n); }
		} else {
			std::array<u64, kLanes> from;
			from.fill(kChunkHdr);
			collect<false>(b, from, ~u64(0) >> 1, nullptr, refs);
			std::sort(refs.begin(), refs.end(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
		}
	}

	// ---- scans: a k-way merge over buffers (runs or skip list) and segments
	struct source {
		enum kind_t { run, skip, seg } kind;
		buffer* b = nullptr;
		const std::vector<u32>* a = nullptr;
		std::size_t i = 0;
		u32 node = 0;
		const segment* sg = nullptr;
		u64 v = 0;
		// current entry
		bool valid = false;
		std::string_view key;
		u64 version = 0;
		u32 ref = 0;
		std::size_t ord = 0;
	};
	bool visible_rec(const buffer& b, rec r, u64 v) const noexcept {
		const u32 p = r.pos().load(std::memory_order_relaxed);
		return p != kNever && p != kPending && b.base + p <= v;
	}
	void settle(source& s) const {
		s.valid = false;
		switch (s.kind) {
		case source::run:
			while (s.i < s.a->size()) {
				const u32 ref = (*s.a)[s.i];
				rec r = s.b->at(ref);
				if (visible_rec(*s.b, r, s.v)) {
					s.valid = true;
					s.key = r.key();
					s.version = r.version();
					s.ref = ref;
					return;
				}
				++s.i;
			}
			return;
		case source::skip:
			while (s.node) {
				rec r = s.b->at(s.node);
				if (visible_rec(*s.b, r, s.v)) {
					s.valid = true;
					s.key = r.key();
					s.version = r.version();
					s.ref = s.node;
					return;
				}
				s.node = r.tower(0).load(std::memory_order_acquire);
			}
			return;
		case source::seg:
			while (s.i < s.sg->size()) {
				if (s.sg->is_live(s.i)) {
					s.valid = true;
					s.key = s.sg->d->keys[s.i];
					s.version = s.sg->d->version[s.i];
					s.ord = s.i;
					return;
				}
				++s.i;
			}
			return;
		}
	}
	void step(source& s) const {
		switch (s.kind) {
		case source::run: ++s.i; break;
		case source::skip: s.node = s.b->at(s.node).tower(0).load(std::memory_order_acquire); break;
		case source::seg: ++s.i; break;
		}
		settle(s);
	}
	static std::size_t lower_run(const buffer& b, const std::vector<u32>& a, std::string_view key) {
		std::size_t lo = 0, hi = a.size();
		while (lo < hi) {
			const std::size_t mid = (lo + hi) / 2;
			if (cmp_keys(b.at(a[mid]).key(), key) < 0) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return lo;
	}
	void add_buffer_sources(std::vector<source>& srcs, std::vector<run_view>& keep, buffer& b, u64 v, std::string_view from) const {
		if constexpr (kSkip) {
			source s;
			s.kind = source::skip;
			s.b = &b;
			s.v = v;
			s.node = skip_seek(b, from);
			settle(s);
			srcs.push_back(s);
		} else {
			keep.push_back(runs_at(b, v));
			const run_view& rv = keep.back();
			for (const std::vector<u32>* a : {&rv.r->main, &rv.r->delta, &rv.tail}) {
				source s;
				s.kind = source::run;
				s.b = &b;
				s.a = a;
				s.v = v;
				s.i = lower_run(b, *a, from);
				settle(s);
				srcs.push_back(s);
			}
		}
	}
	template <class F>
	std::size_t scan_in(const scope& sc, std::string_view from, std::size_t n, F&& visit) const {
		std::vector<source> srcs;
		std::vector<run_view> keep;
		keep.reserve(sc.vw->frozen.size() + 1);
		add_buffer_sources(srcs, keep, *sc.vw->active, sc.v, from);
		for (std::size_t i = sc.vw->frozen.size(); i-- > 0;) { add_buffer_sources(srcs, keep, *sc.vw->frozen[i], sc.v, from); }
		for (const segment& sg : sc.vw->segs) {
			source s;
			s.kind = source::seg;
			s.sg = &sg;
			s.i = std::lower_bound(sg.d->keys.begin(), sg.d->keys.end(), from, [](const std::string& a, std::string_view b) { return cmp_keys(a, b) < 0; }) - sg.d->keys.begin();
			settle(s);
			srcs.push_back(s);
		}
		std::size_t got_n = 0;
		while (got_n < n) {
			source* min = nullptr;
			for (auto& s : srcs) {
				if (s.valid && (!min || cmp_keys(s.key, min->key) < 0)) { min = &s; }
			}
			if (!min) { break; }
			const std::string key_copy_view = std::string(min->key);  // stable across steps
			source* best = nullptr;
			for (auto& s : srcs) {
				if (s.valid && cmp_keys(s.key, key_copy_view) == 0 && (!best || s.version > best->version)) { best = &s; }
			}
			bool tomb = false;
			std::string_view value;
			if (best->kind == source::seg) {
				tomb = best->sg->d->is_tomb(best->ord);
				value = best->sg->d->values[best->ord];
			} else {
				rec r = best->b->at(best->ref);
				tomb = r.tomb();
				value = r.value();
			}
			if (!tomb) {
				visit(std::string_view(key_copy_view), value);
				++got_n;
			}
			for (auto& s : srcs) {
				while (s.valid && cmp_keys(s.key, key_copy_view) == 0) { step(s); }
			}
		}
		return got_n;
	}

	struct rstats {
		std::atomic<u64> run_builds{0}, run_build_ns{0}, tail_sum{0}, tail_n{0};
	};

	// ---- state
	std::atomic<view*> view_{nullptr};
	std::atomic<u64> vis_{0};
	std::atomic<writer*> head_{nullptr};
	std::atomic<int> par_pending_{0};
	std::atomic<u32> lease_{0};
	std::atomic<bool> lane_busy_[kLanes] = {};
	mutable retire_list retired_;
	// lease holder only
	u64 pos_ = 0, docid_ = 0;
	std::size_t rejected_bytes_ = 0;
	bool time_leader_ = false;
	bool drop_tomb_ = false;
	stats_t stats_;
	mutable rstats rs_;
};

}  // namespace lanebuf
