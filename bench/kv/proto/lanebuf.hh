// The write-buffer bake-off's prototype (docs/design/write-buffer.md, step 0): three candidates
// from one source.
//   C   lanes + one shared version map + a write group whose leader runs stage 1; scans read
//       shared sorted runs (main, delta, and a private tail) built off the write path on a pool
//       and installed by the lease holder, only once a scan asked (WB-17).
//   A'  the same, with a concurrent skip list (tower inside the record) for scans; V waits for the
//       group's last link.
//   F   the fallback (D14): one lane, an exclusive lease per transaction, no pre-staging.
// buf_index<order_kind, conc_kind> selects them. Readers are protected by strata's hazard
// registry (WB-8); retired objects ride the view they were replaced under (D10). Pre-resolution
// is keyed on a component epoch that only freezes, installs and merges move (WB-18).
#pragma once

#include "lanebuf_core.hh"

namespace lanebuf {

template <order_kind O, conc_kind C>
class buf_index {
public:
	static constexpr bool kSkip = O == order_kind::skiplist;
	static constexpr bool kGroup = C == conc_kind::group;
	using registry = strata::reader_registry;

	// ---- structures
#if LANEBUF_E == 0
	struct runs_t {
		std::shared_ptr<const std::vector<u32>> main = std::make_shared<const std::vector<u32>>();
		std::vector<u32> delta;
		u64 cover_m = 0, cover_d = 0;  // position-index counts the main run and the delta cover
		u64 cover() const noexcept { return cover_d; }
		std::size_t bytes() const { return (main->capacity() + delta.capacity()) * 4 + sizeof(*this); }
	};
#else
	// E1: runs of prefixed entries in a binary counter (a new run merges into the last while it is not
	// more than twice its size), so a build sorts the backlog alone and merges amortize to O(log n).
	struct runs_t {
		std::vector<std::shared_ptr<const std::vector<ent>>> lv;  // biggest first
		u64 cover_pos = 0;                                        // position-index count the runs cover
		u64 cover() const noexcept { return cover_pos; }
		std::size_t bytes() const {
			std::size_t b = sizeof(*this);
			for (const auto& l : lv) { b += l->capacity() * sizeof(ent); }
			return b;
		}
	};
#endif
#if LANEBUF_E == 0
	using run_t = std::vector<u32>;  // a sorted run of record references
#else
	using run_t = std::vector<ent>;  // a sorted run of prefixed entries
#endif
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
		u64 base;  // positions are stored as 32-bit deltas from here
		chunk_dir dir;
		std::atomic<lane*> lanes{nullptr};
		std::atomic<table*> tab;
		pos_index pi;
		std::atomic<const runs_t*> runs{nullptr};   // C: the installed run pair
		std::atomic<const runs_t*> ready{nullptr};  // C: a finished build, waiting for the lease holder
		mutable std::atomic<bool> want{false};      // C: a scan found a long tail
		bool building = false;                      // lease holder only
		std::atomic<bool> job_live{false};          // E2: an eager build job is looping on this buffer
		skip_t sl;                                  // A'
		// lease holder only
		std::size_t used = 0, nrec = 0;
		std::vector<pending_del> pending;
		lane* fl = nullptr;  // F: the lease holder's lane
		explicit buffer(u64 b) : base(b) { tab.store(table::make(4), std::memory_order_relaxed); }
		buffer(const buffer&) = delete;
		~buffer() {
			for (lane* l = lanes.load(); l;) {
				lane* n = l->next;
				delete l;
				l = n;
			}
			table::destroy(tab.load());
			delete runs.load();
			delete ready.load();
		}
		char* ptr(u32 ref) const noexcept { return dir.get(ref >> 17) + (std::size_t(ref & 0x1FFFF) << 3); }
		rec at(u32 ref) const noexcept { return rec{ptr(ref)}; }
		// A lane of this buffer for one pre-stage: a free one on the push-only list, else a new one.
		lane* claim_lane() {
			for (lane* l = lanes.load(std::memory_order_acquire); l; l = l->next) {
				bool f = false;
				if (!l->busy.load(std::memory_order_relaxed) && l->busy.compare_exchange_strong(f, true, std::memory_order_acquire, std::memory_order_relaxed)) { return l; }
			}
			auto* l = new lane;
			lane* h = lanes.load(std::memory_order_relaxed);
			do {
				l->next = h;
			} while (!lanes.compare_exchange_weak(h, l, std::memory_order_release, std::memory_order_relaxed));
			return l;
		}
		static void release_lane(lane* l) { l->busy.store(false, std::memory_order_release); }
	};

	struct view {
		u64 gen = 0;     // moves at every publication
		u64 cepoch = 0;  // moves at freezes, installs, merges only (WB-18)
		u64 pos = 0;     // the position the view's positionless state reflects (WB-1)
		std::shared_ptr<buffer> active;
		std::vector<std::shared_ptr<buffer>> frozen;  // oldest first
		std::vector<segment> segs;                    // oldest first
	};
	using guard = typename registry::template guard<view>;

	// ---- the write group's queue
	enum : u32 { S_WAIT = 0, S_LEADER = 1, S_PAR = 2, S_DONE = 3, S_PARKED = 0x100 };
	enum class chore_kind : u8 { none, freeze, install, add_segment, merge, maint };
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
	static constexpr u32 kNoOrd = 0xFFFFFFFFu;
	struct merge_plan {
		std::vector<u32> ids;                  // the source segments
		std::vector<std::vector<u32>> omap;    // per source: its ordinal -> the output's (kNoOrd: not carried)
		std::vector<std::pair<u32, u32>> src;  // the output's ordinal -> (source index, source ordinal)
	};
	struct writer {
		std::atomic<u32> state{S_WAIT};
		writer* older = nullptr;
		writer* newer = nullptr;
		lane* ln = nullptr;
		unsigned nops = 0;
		buffer* buf = nullptr;
		u64 cepoch = 0;
		bool preres = false, staged = false;
		chore_kind chore = chore_kind::none;
		segment seg;
		merge_plan plan;
		bool accepted = false;
		u64 pos = 0;
		op_in in[kMaxOps];
		pop ops[kMaxOps];
		op_out out[kMaxOps];
		guard g;
	};
	using txn = writer;
	struct group_ctx {
		lane* ln = nullptr;  // a lane of `lb` the lease holder owns for restaging
		buffer* lb = nullptr;
		bool own = false;  // claimed here, released at the group's end
	};

	// ---- construction
	explicit buf_index(registry& reg) : reg_(reg) {
		auto* v = new view;
		v->active = std::make_shared<buffer>(0);
		view_.store(v, std::memory_order_release);
	}
	buf_index(const buf_index&) = delete;
	~buf_index() {
		job_pool::get().drain();
		retired_.clear();
		delete view_.load();
	}

	// ---- reads
	struct read_ctx {
		guard g;
		u64 v = 0;  // the read position
		const view& vw() const { return *g; }
	};
	u64 load_v() const noexcept { return vis_.load(std::memory_order_acquire); }
	u64 visible() const noexcept { return vis_.load(std::memory_order_acquire); }
	// Reads at max(V, view.position) (WB-1): V loaded first, the view acquired after.
	read_ctx begin_read_at(u64 v) const {
		read_ctx c;
		c.g = reg_.acquire(view_);
#if LANEBUF_MUTANT == 8
		c.v = v;  // mutant: read at V whatever the view reflects
#else
		c.v = std::max(v, c.g->pos);
#endif
		return c;
	}
	read_ctx begin_read() const { return begin_read_at(load_v()); }

	got get(std::string_view key, std::string* out = nullptr) const {
		const read_ctx c = begin_read();
		return get_in(c, key, out);
	}
	got get_in(const read_ctx& c, std::string_view key, std::string* out) const {
		const view& vw = c.vw();
		got r;
		r.at = c.v;
		const u64 h = xxh64::hash(key);
		if (const u32 ref = chain_in(*vw.active, key, h, c.v)) { return finish(*vw.active, ref, out, r); }
		for (std::size_t i = vw.frozen.size(); i-- > 0;) {
			if (const u32 ref = chain_in(*vw.frozen[i], key, h, c.v)) { return finish(*vw.frozen[i], ref, out, r); }
		}
		for (std::size_t i = vw.segs.size(); i-- > 0;) {
			const segment& sg = vw.segs[i];
			if (const std::size_t o = sg.find(key, h); o != segment::npos && sg.is_live(o)) {
				r.found = true;
				r.docid = sg.d->docid[o];
				r.version = sg.d->version[o];
				if (out) { out->assign(sg.d->value(o)); }
				return r;
			}
		}
		return r;
	}

	// Up to n live entries from `from` at the read position; visit(key, value) each.
	template <class F>
	std::size_t scan(std::string_view from, std::size_t n, F&& visit, u64* at = nullptr) const {
		const read_ctx c = begin_read();
		if (at) { *at = c.v; }
		return scan_in(c, from, n, std::forward<F>(visit));
	}

	// ---- writes
	// One transaction on a journal-less index. Returns whether it was accepted.
	bool write(const op_in* ops, unsigned n, write_out& out) {
		if constexpr (kGroup) {
			writer w;
			txn_prepare(w, ops, n);
			run_group(w);
			return txn_finish(w, out);
		} else {
			return write_lease(ops, n, out);
		}
	}

	// The applier path (D7): a proposer prepares, the one applier applies in order, the proposer
	// finishes. prepare() pre-stages into a lane (C and A') or only records the operations (F).
	void txn_prepare(writer& w, const op_in* ops, unsigned n) {
		w.nops = n;
		for (unsigned k = 0; k < n; ++k) {
			w.in[k] = ops[k];
			w.ops[k].kind = ops[k].kind;
			w.ops[k].expected = ops[k].expected;
		}
		if constexpr (kGroup) {
			w.g = reg_.acquire(view_);
			const view& vw = *w.g;
			buffer& b = *vw.active;
			w.buf = &b;
			w.ln = b.claim_lane();
			w.cepoch = vw.cepoch;
			const bool lower = has_lower(vw);
			w.preres = lower;
			w.staged = true;
			for (unsigned k = 0; k < n; ++k) {
				pop& o = w.ops[k];
				o.hash = xxh64::hash(ops[k].key);
				o.ref = stage_record(b, *w.ln, ops[k].key, ops[k].value, ops[k].kind == op_kind::del, k);
				if (lower) { resolve(vw, ops[k].key, o.hash, o.res); }
			}
		}
	}
	// The lease holder (the group's leader, or the applier): apply each entry in order, then end
	// the group (publishes V, installs finished builds, frees what no reader holds).
	void apply_txn(writer& w, group_ctx& ctx) {
		const view* vw = view_.load(std::memory_order_relaxed);
		if (w.chore != chore_kind::none) {
			run_chore(w, *vw);
			return;
		}
		buffer& b = *vw->active;
		if constexpr (kGroup) {
			if (w.buf != &b) {
#if LANEBUF_MUTANT != 10
				restage(w, b, ctx);  // the writer pre-staged into a buffer that has since been sealed
#endif
			}
		} else {
			if (!w.staged) { stage_now(w, b); }
		}
		stage1(w, b, *vw);
	}
	void apply_end(group_ctx& ctx, unsigned members, u64 t0ns = 0) {
		view_.load(std::memory_order_relaxed)->active->pi.publish();
#if LANEBUF_MUTANT != 4
		vis_.store(pos_, std::memory_order_release);
#endif
		++stats_.groups;
		stats_.group_ops += members;
		++stats_.hist[std::min(members, 16u)];
		if (t0ns) {
			const u64 d = now_ns() - t0ns;
			stats_.leader_ns += d;
			stats_.longest_ns = std::max(stats_.longest_ns, d);
		}
		if (ctx.own) { buffer::release_lane(ctx.ln); }
		ctx = group_ctx{};
		maintain();
		++since_reclaim_;
		if (!retired_.empty() && (since_reclaim_ >= 8 || retired_.size() > 32)) {
			since_reclaim_ = 0;
			reclaim();
		}
	}
	bool txn_finish(writer& w, write_out& out) {
		out.accepted = w.accepted;
		out.pos = w.pos;
		for (unsigned k = 0; k < w.nops; ++k) { out.ops[k] = w.out[k]; }
		if (w.ln) { buffer::release_lane(w.ln); }
		w.ln = nullptr;
		w.g.release();
		return w.accepted;
	}

	// Chores run as writerless groups (D2).
	void freeze() { chore(chore_kind::freeze, segment{}); }
	void install_segment(segment s) { chore(chore_kind::install, std::move(s)); }
	void add_segment(segment s) { chore(chore_kind::add_segment, std::move(s)); }
	void merge_install(segment out, merge_plan plan) { chore(chore_kind::merge, std::move(out), std::move(plan)); }
	void maintenance() { chore(chore_kind::maint, segment{}); }
	// Waits for builds in flight and installs what they finished.
	void quiesce() {
		for (int i = 0; i < 64; ++i) {
			job_pool::get().drain();
			maintenance();
			if (!builds_in_flight()) { break; }
		}
	}
	u64 lost_pending() const noexcept { return stats_.lost_pending; }
	// Setup and tests, single-threaded: segments added before the first write name docids up to d.
	void raise_docid(u64 d) noexcept { docid_ = std::max(docid_, d); }
	// One entry applied as a group of one (the test holds the lease).
	void apply_for_test(writer& w) {
		group_ctx ctx;
		apply_txn(w, ctx);
		apply_end(ctx, 1);
	}
	u64 positions() const { return pos_; }
	void set_leader_timing(bool on) { time_leader_ = on; }

	// Live copies of a key across segments (the one-live-copy invariant).
	std::size_t live_copies(std::string_view key) const {
		const read_ctx c = begin_read();
		const u64 h = xxh64::hash(key);
		std::size_t n = 0;
		for (const segment& sg : c.vw().segs) {
			if (const std::size_t o = sg.find(key, h); o != segment::npos && sg.is_live(o)) { ++n; }
		}
		return n;
	}

	// A merge of segments `ids` (a pool job in the engine): the live documents, newest copy of a key.
	bool build_merge_segment(segment& out, merge_plan& plan, u32 id, const std::vector<u32>& ids) {
		const read_ctx c = begin_read();
		const view& vw = c.vw();
		std::vector<segment> src;
		for (u32 sid : ids) {
			for (const segment& sg : vw.segs) {
				if (sg.id == sid) { src.push_back(sg); }
			}
		}
		if (src.size() != ids.size()) { return false; }
		struct item {
			std::string key;
			unsigned si;
			u32 ord;
		};
		std::vector<item> items;
		std::array<bool, 256> used{};
		for (unsigned si = 0; si < src.size(); ++si) {
			keys::reader::cursor cur(src[si].d->kr);
			while (cur.next()) {
				if (!src[si].is_live(cur.ordinal())) { continue; }
				items.push_back({std::string(cur.key()), si, cur.ordinal()});
				for (char ch : items.back().key) { used[u8(ch)] = true; }
			}
		}
		std::stable_sort(items.begin(), items.end(), [](const item& a, const item& b) { return a.key < b.key; });
		plan = merge_plan{};
		plan.ids = ids;
		plan.omap.resize(src.size());
		for (unsigned si = 0; si < src.size(); ++si) { plan.omap[si].assign(src[si].size(), kNoOrd); }
		seg_stream st(keys::alphabet_of(used));
		for (std::size_t i = 0; i < items.size(); ++i) {
			if (i + 1 < items.size() && items[i + 1].key == items[i].key) { continue; }  // the last (newest) copy wins
			const item& it = items[i];
			const seg_data& d = *src[it.si].d;
			plan.omap[it.si][it.ord] = u32(st.size());
			plan.src.push_back({it.si, it.ord});
			st.add(it.key, d.value(it.ord), d.docid[it.ord], d.version[it.ord]);
		}
		out = st.finish(id);
		return true;
	}

	// Builds a segment from the oldest frozen buffer's newest records (the flush's sort). Tombstones
	// are not written: the engine's segments hold none.
	bool build_flush_segment(segment& out, u32 id) {
		const read_ctx c = begin_read();
		const view& vw = c.vw();
		if (vw.frozen.empty()) { return false; }
		buffer& b = *vw.frozen.front();
		std::vector<u32> refs;
		ordered_refs(b, refs);
		std::vector<u32> keep;
		std::array<bool, 256> used{};
		std::string_view last;
		bool have = false;
		for (u32 ref : refs) {
			rec r = b.at(ref);
			if (have && cmp_keys(r.key(), last) == 0) { continue; }  // an older record of the key
			last = r.key();
			have = true;
			if (r.tomb()) { continue; }
			keep.push_back(ref);
			for (char ch : last) { used[u8(ch)] = true; }
		}
		seg_stream st(keys::alphabet_of(used));
		for (u32 ref : keep) {
			rec r = b.at(ref);
			st.add(r.key(), r.value(), r.docid(), r.version());
		}
		out = st.finish(id);
		out.from = &b;
		return true;
	}

	// ---- introspection (call when quiescent)
	stats_t stats() const {
		stats_t st = stats_;
		st.run_builds = rs_.run_builds.load();
		st.run_build_ns = rs_.run_build_ns.load();
		st.tail_sum = rs_.tail_sum.load();
		st.tail_n = rs_.tail_n.load();
		st.tail_max = rs_.tail_max.load();
		st.preres_used = rs_.preres_used.load();
		st.preres_redone = rs_.preres_redone.load();
		const view* vw = view_.load();
		auto add = [&](const buffer& b) {
			++st.buffers;
			for (lane* l = b.lanes.load(); l; l = l->next) {
				st.lane_used += l->used();
				++st.lanes;
			}
			st.lane_cap += b.dir.bytes();
			st.dir_bytes += b.dir.dir_bytes();
			st.records += b.nrec;
			st.table_bytes += b.tab.load()->bytes();
			st.pi_bytes += b.pi.bytes();
			if (auto* r = b.runs.load()) { st.runs_bytes += r->bytes(); }
		};
		add(*vw->active);
		for (auto& f : vw->frozen) { add(*f); }
		return st;
	}
	const view* current_view() const { return view_.load(std::memory_order_acquire); }
	buffer& active_buffer() const { return *view_.load()->active; }
	std::size_t chain_follows() const { return rs_.chain.load(); }

private:
	struct rstats {
		std::atomic<u64> run_builds{0}, run_build_ns{0}, tail_sum{0}, tail_n{0}, tail_max{0}, chain{0}, preres_used{0}, preres_redone{0};
	};
	static u64 now_ns() noexcept { return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

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
	// The newest record of the key at or below r (0: none).
	u32 chain_in(const buffer& b, std::string_view key, u64 h, u64 r) const noexcept {
		u32 ref = find_ref(b, key, h);
		while (ref) {
			rec x = b.at(ref);
			if (b.base + x.pd() <= r) { return ref; }
			rs_.chain.fetch_add(1, std::memory_order_relaxed);
			ref = x.prev();
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

	// ---- pre-stage: copy a record into a lane, header left for the sequencer
	u32 stage_record(buffer& b, lane& ln, std::string_view key, std::string_view value, bool tomb, unsigned opi) {
		const unsigned h = kSkip ? random_height() : 0;
		const std::size_t sz = rec_size(key.size(), value.size(), h);
		u32 c, off;
		char* p = ln.alloc(b.dir, sz, c, off);
		std::memset(p, 0, tower_bytes(h));
		char* hd = p + tower_bytes(h);
		std::memset(hd, 0, 24);
		hd[24] = char((tomb ? 1 : 0) | (opi << 1) | (h << 4));
		char* q = put_varint(put_varint(hd + 25, key.size()), value.size());
		std::memcpy(q, key.data(), key.size());
		std::memcpy(q + key.size(), value.data(), value.size());
		return make_ref(c, off + u32(tower_bytes(h)));
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
	// F's applier and lease path: the lease holder copies and hashes itself.
	void stage_now(writer& w, buffer& b) {
		if (!b.fl) { b.fl = b.claim_lane(); }
		w.buf = &b;
		w.preres = false;
		for (unsigned k = 0; k < w.nops; ++k) {
			pop& o = w.ops[k];
			o.hash = xxh64::hash(w.in[k].key);
			o.ref = stage_record(b, *b.fl, w.in[k].key, w.in[k].value, w.in[k].kind == op_kind::del, k);
		}
		w.staged = true;
	}

	// ---- resolution against frozen buffers and segments (pre-resolution, D4)
	void resolve(const view& vw, std::string_view key, u64 h, resolution& r) const {
		r = resolution{};
		for (std::size_t i = vw.frozen.size(); i-- > 0;) {
			const buffer& b = *vw.frozen[i];
			if (const u32 ref = find_ref(b, key, h)) {
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
			if (const std::size_t o = sg.find(key, h); o != segment::npos && sg.is_live(o)) {
				r.kind = 1;
				r.live = true;
				r.seg = sg.id;
				r.ord = u32(o);
				r.docid = sg.d->docid[o];
				r.version = sg.d->version[o];
				return;
			}
		}
	}
	static bool has_lower(const view& vw) noexcept { return !vw.frozen.empty() || !vw.segs.empty(); }

	// ---- the version map, sequencer side
	struct locate_t {
		table* t;
		u32 i;    // the key's slot, or the empty slot it would take
		u64 old;  // the slot's value (0: new key)
		u32 cur;  // the key's newest record (0: none)
	};
	locate_t locate(buffer& b, std::string_view key, u64 h) const noexcept {
		table* t = b.tab.load(std::memory_order_relaxed);
		const u32 tag = u32(h >> 32);
		u32 i = slot_home(tag, t->bits);
		auto* sl = t->slots();
		for (;;) {
			const u64 s = sl[i].load(std::memory_order_relaxed);
			if (!s) { return {t, i, 0, 0}; }
			if (slot_tag(s) == tag && cmp_keys(b.at(slot_ref(s)).key(), key) == 0) { return {t, i, s, slot_ref(s)}; }
			i = (i + 1) & t->mask;
		}
	}

	// ---- retirement (D10): what a replaced object waits for
	using gptr = std::unique_ptr<void, void (*)(void*)>;
	struct retired_view {
		std::unique_ptr<const view> v;
		std::vector<gptr> g;  // objects replaced while v was current
	};
	template <class T>
	void retire_obj(T* p) {
#if LANEBUF_MUTANT == 7
		delete p;  // mutant: freed at once, a reader may still hold it
		return;
#endif
		garbage_.emplace_back(p, [](void* q) { delete static_cast<T*>(q); });
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
		retire_obj(new tdel{old});
		bump_view();
	}
	void ensure_slots(buffer& b, unsigned n) {
		while ((b.used + n) * 8 > (std::size_t(b.tab.load(std::memory_order_relaxed)->mask) + 1) * 7) { grow(b); }
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
		table* t;
	};
	bool stage1(writer& w, buffer& b, const view& vw) {
		ensure_slots(b, w.nops);  // before any slot changes, so an undo always meets the current table
		const u64 pos = ++pos_;
		const u32 pd = u32(pos - b.base);
		w.pos = pos;
		undo_t undo[kMaxOps];
		unsigned nu = 0;
		const std::size_t pend0 = b.pending.size();
		const u64 pi0 = b.pi.working();
		const u64 docid0 = docid_;
		bool ok = true;
#if LANEBUF_MUTANT == 4
		vis_.store(pos, std::memory_order_release);  // mutant: V before stage 1 ends
#endif
		for (unsigned k = 0; k < w.nops; ++k) {
			pop& o = w.ops[k];
			rec nr = b.at(o.ref);
			const std::string_view key = nr.key();
			locate_t L = locate(b, key, o.hash);
			bool live = false;
			u64 base_version = 0, base_docid = 0;
			resolution res;
			if (L.cur) {
				rec c = b.at(L.cur);
				live = !c.tomb();
				base_version = c.version();
				base_docid = c.docid();
			} else if (has_lower(vw)) {
#if LANEBUF_MUTANT == 5
				const bool usable = w.preres;  // mutant: pre-resolution trusted across a changed component epoch
#else
				const bool usable = w.preres && w.cepoch == vw.cepoch;
#endif
				if (usable) {
					res = o.res;
					rs_.preres_used.fetch_add(1, std::memory_order_relaxed);
				} else {
					resolve(vw, key, o.hash, res);
					if (w.preres) { rs_.preres_redone.fetch_add(1, std::memory_order_relaxed); }
				}
				live = res.live;
				if (res.kind) {
					base_version = res.version;
					base_docid = res.docid;
				}
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
			// A delete leaves no tombstone behind in a segment, so a re-add after one is a new
			// document at version 1 whenever it lands, as the engine's evaluate does.
			const u64 docid = o.kind == op_kind::del ? base_docid : live ? base_docid : ++docid_;
			const u64 version = o.kind == op_kind::del ? base_version : live ? base_version + 1 : 1;
			w.out[k] = {true, docid, version};
			if (res.kind == 1 && res.live) { b.pending.push_back({res.seg, res.ord}); }
			const u64 nslot = pack_slot(u32(o.hash >> 32), o.ref);
#if LANEBUF_MUTANT == 1
			if (L.old) {  // mutant: the slot is published before the header is filled
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
			b.pi.push(o.ref);
			undo[nu++] = {L.i, L.old, L.t};
			++b.nrec;
		}
		if (!ok) {
			// Restore the slots this transaction changed and rewind the position index: nobody has
			// seen either, and the records stay in the lane, unreferenced.
			for (unsigned k = nu; k-- > 0;) {
#if LANEBUF_MUTANT != 3
				undo[k].t->slots()[undo[k].i].store(undo[k].old, std::memory_order_release);
#endif
				if (!undo[k].old) { --b.used; }
				--b.nrec;
			}
			b.pi.rewind(pi0);
			docid_ = docid0;
			b.pending.resize(pend0);
			for (unsigned k = 0; k < w.nops; ++k) { w.out[k].ok = false; }
		}
		w.accepted = ok;
		return ok;
	}

	// Copies a writer's records into a lane of the current buffer: the writer pre-staged into a
	// buffer a freeze has sealed since (WB-3). The late appends into the sealed lane are unreachable.
	void restage(writer& w, buffer& b, group_ctx& ctx) {
		if (ctx.lb != &b) {
			if (ctx.own) { buffer::release_lane(ctx.ln); }
			ctx.ln = b.claim_lane();
			ctx.lb = &b;
			ctx.own = true;
		}
		++stats_.restaged;
		for (unsigned k = 0; k < w.nops; ++k) {
			pop& o = w.ops[k];
			rec old = w.buf->at(o.ref);
			o.ref = stage_record(b, *ctx.ln, old.key(), old.value(), old.tomb(), old.opi());
		}
		w.buf = &b;
	}

	// ---- chores
	void chore(chore_kind k, segment s, merge_plan plan = {}) {
		writer w;
		w.chore = k;
		w.seg = std::move(s);
		w.plan = std::move(plan);
		if constexpr (kGroup) {
			run_group(w);
		} else {
			lease_lock();
			group_ctx ctx;
			apply_txn(w, ctx);
			apply_end(ctx, 1);
			lease_unlock();
		}
	}
	void run_chore(writer& w, const view& vw) {
		switch (w.chore) {
		case chore_kind::freeze: do_freeze(vw); break;
		case chore_kind::install: do_install(vw, w.seg); break;
		case chore_kind::add_segment: do_add_segment(vw, w.seg); break;
		case chore_kind::merge: do_merge_install(vw, w.seg, w.plan); break;
		default: break;
		}
	}
	// A new view with the same components (a replaced table or run pair), or with the position
	// and epoch moved (a freeze, install or merge: positionless state changed).
	view* clone_view(const view& vw, bool components) const {
		auto* nv = new view;
		nv->gen = vw.gen + 1;
		nv->cepoch = vw.cepoch + (components ? 1 : 0);
		nv->pos = components ? pos_ : vw.pos;
		nv->active = vw.active;
		nv->frozen = vw.frozen;
		nv->segs = vw.segs;
		return nv;
	}
	void publish_view(view* nv, const view& old) {
		registry::publish(view_, static_cast<const view*>(nv));
		retired_.push_back({std::unique_ptr<const view>(&old), std::move(garbage_)});
		garbage_.clear();
		++stats_.views;
	}
	void bump_view() {
		const view* cur = view_.load(std::memory_order_relaxed);
		publish_view(clone_view(*cur, false), *cur);
	}
	void do_freeze(const view& vw) {
		view* nv = clone_view(vw, true);
		buffer& sealed = *vw.active;
		// pending segment deletes become copy-on-write live bits (D8)
		if (!sealed.pending.empty()) {
			std::sort(sealed.pending.begin(), sealed.pending.end(), [](auto& a, auto& b) { return a.seg < b.seg || (a.seg == b.seg && a.ord < b.ord); });
			std::size_t i = 0;
			while (i < sealed.pending.size()) {
				const u32 id = sealed.pending[i].seg;
				segment* sg = nullptr;
				for (auto& s : nv->segs) {
					if (s.id == id) { sg = &s; }
				}
				if (!sg) {  // the segment is gone: its pending deletes were not carried
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
		sealed.pi.publish();
		nv->frozen.push_back(vw.active);
		nv->active = std::make_shared<buffer>(pos_);
		publish_view(nv, vw);
	}
	// A merge's install (WB-2): the output's bits carry the deletes set on its sources since the
	// merge was built, and the active buffer's pending deletes naming a source follow to the output.
	void do_merge_install(const view& vw, const segment& out, const merge_plan& plan) {
		view* nv = clone_view(vw, true);
		segment o = out;
		auto bits = std::make_shared<live_bits>(*o.live);
		for (std::size_t i = 0; i < plan.src.size(); ++i) {
			const u32 sid = plan.ids[plan.src[i].first];
			for (const segment& sg : vw.segs) {
				if (sg.id == sid && !sg.is_live(plan.src[i].second)) { (*bits)[i >> 6] &= ~(u64(1) << (i & 63)); }
			}
		}
		o.live = std::move(bits);
#if LANEBUF_MUTANT != 9
		buffer& act = *vw.active;
		std::size_t keep = 0;
		for (std::size_t i = 0; i < act.pending.size(); ++i) {
			pending_del p = act.pending[i];
			const auto it = std::find(plan.ids.begin(), plan.ids.end(), p.seg);
			if (it != plan.ids.end()) {
				const u32 m = plan.omap[std::size_t(it - plan.ids.begin())][p.ord];
				if (m == kNoOrd) { continue; }  // the source's copy is not in the output: fenced
				p = {o.id, m};
				++stats_.remapped;
			}
			act.pending[keep++] = p;
		}
		act.pending.resize(keep);
#endif
		std::vector<segment> segs;
		bool placed = false;
		for (auto& sg : nv->segs) {
			if (std::find(plan.ids.begin(), plan.ids.end(), sg.id) != plan.ids.end()) {
				if (!placed) { segs.push_back(o); }
				placed = true;
			} else {
				segs.push_back(sg);
			}
		}
		nv->segs = std::move(segs);
		publish_view(nv, vw);
	}
	void do_add_segment(const view& vw, const segment& s) {
		view* nv = clone_view(vw, true);
		nv->segs.push_back(s);
		publish_view(nv, vw);
	}
	// A flush's install: the oldest frozen buffer becomes a segment; a key a newer buffer holds
	// (a replace or a delete after the freeze) is dead in it.
	void do_install(const view& vw, const segment& s) {
		view* nv = clone_view(vw, true);
		segment sg = s;
		auto bits = std::make_shared<live_bits>(*sg.live);
		keys::reader::cursor cur(sg.d->kr);
		while (cur.next()) {
			const std::string_view key = cur.key();
			const u64 h = xxh64::hash(key);
			bool shadowed = find_ref(*vw.active, key, h) != 0;
			for (std::size_t i = 1; !shadowed && i < vw.frozen.size(); ++i) { shadowed = find_ref(*vw.frozen[i], key, h) != 0; }
			if (shadowed) {
				const u32 o = cur.ordinal();
				(*bits)[o >> 6] &= ~(u64(1) << (o & 63));
			}
		}
		sg.live = std::move(bits);
		nv->frozen.erase(nv->frozen.begin());
		nv->segs.push_back(std::move(sg));
		publish_view(nv, vw);
	}

	// Frees from the oldest retired view while no hazard record names it: an object replaced under
	// a view may be held by that view's readers and by any older view's, so the prefix is the unit.
	void reclaim() {
		if (retired_.empty()) { return; }
		named_.clear();
		reg_.scan(++scan_clock_, [&](const void* p) { named_.push_back(p); });
		std::sort(named_.begin(), named_.end());
		while (!retired_.empty()) {
#if LANEBUF_MUTANT != 13
			if (std::binary_search(named_.begin(), named_.end(), static_cast<const void*>(retired_.front().v.get()))) { break; }
#endif
			retired_.pop_front();
		}
	}

	// ---- runs (C): built on the pool when a scan asked, installed by the lease holder (WB-17)
	static bool refs_less(const buffer& b, u32 x, u32 y) noexcept {
		rec a = b.at(x), c = b.at(y);
		const int k = cmp_keys(a.key(), c.key());
		return k < 0 || (k == 0 && a.seq() > c.seq());  // key ascending, newest first
	}
#if LANEBUF_E == 0
	static std::vector<u32> merge_refs(const buffer& b, const std::vector<u32>& x, const std::vector<u32>& y) {
		std::vector<u32> r(x.size() + y.size());
		std::merge(x.begin(), x.end(), y.begin(), y.end(), r.begin(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
		return r;
	}
#else
	static bool ent_less(const buffer& b, const ent& x, const ent& y) noexcept {
		if (x.hi != y.hi) { return x.hi < y.hi; }
		if (x.lo != y.lo) { return x.lo < y.lo; }
		return x.ref != y.ref && refs_less(b, x.ref, y.ref);
	}
	static std::vector<ent> merge_ents(const buffer& b, const std::vector<ent>& x, const std::vector<ent>& y) {
		std::vector<ent> r(x.size() + y.size());
		std::merge(x.begin(), x.end(), y.begin(), y.end(), r.begin(), [&](const ent& p, const ent& q) { return ent_less(b, p, q); });
		return r;
	}
#endif
	bool builds_in_flight() const {
		const view* vw = view_.load();
		auto busy = [](const buffer& b) { return b.building || b.ready.load() || b.want.load(); };
		if (busy(*vw->active)) { return true; }
		for (auto& f : vw->frozen) {
			if (busy(*f)) { return true; }
		}
		return false;
	}
	void build_once(std::shared_ptr<buffer>& sb, const runs_t* cur, u64 n1) {
		const auto t0 = std::chrono::steady_clock::now();
		buffer& b = *sb;
		const u64 from = cur ? cur->cover() : 0;
#if LANEBUF_E == 0
		std::vector<u32> fresh;
		fresh.reserve(std::size_t(n1 - from));
		for (u64 i = from; i < n1; ++i) { fresh.push_back(b.pi.at(i)); }
		std::sort(fresh.begin(), fresh.end(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
		auto* nr = new runs_t;
		if (cur) {
			nr->main = cur->main;
			nr->cover_m = cur->cover_m;
			nr->delta = merge_refs(b, cur->delta, fresh);
		} else {
			nr->delta = std::move(fresh);
		}
		nr->cover_d = n1;
		if (nr->delta.size() > kDeltaMax) {
			nr->main = std::make_shared<const std::vector<u32>>(merge_refs(b, *nr->main, nr->delta));
			nr->delta.clear();
			nr->cover_m = n1;
		}
#else
		std::vector<ent> fresh;
		fresh.reserve(std::size_t(n1 - from));
		for (u64 i = from; i < n1; ++i) {
			const u32 ref = b.pi.at(i);
			fresh.push_back(make_ent(b.at(ref).key(), ref));
		}
		std::sort(fresh.begin(), fresh.end(), [&](const ent& p, const ent& q) { return ent_less(b, p, q); });
		auto* nr = new runs_t;
		if (cur) { nr->lv = cur->lv; }
		nr->lv.push_back(std::make_shared<const std::vector<ent>>(std::move(fresh)));
		while (nr->lv.size() >= 2 && nr->lv[nr->lv.size() - 2]->size() <= 2 * nr->lv.back()->size()) {
			auto merged = std::make_shared<const std::vector<ent>>(merge_ents(b, *nr->lv[nr->lv.size() - 2], *nr->lv.back()));
			nr->lv.pop_back();
			nr->lv.back() = std::move(merged);
		}
		nr->cover_pos = n1;
#endif
		rs_.run_builds.fetch_add(1, std::memory_order_relaxed);
		rs_.run_build_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
		b.ready.store(nr, std::memory_order_release);
	}
	void build_job(std::shared_ptr<buffer> sb, const runs_t* cur, u64 n1) {
		buffer& b = *sb;
		build_once(sb, cur, n1);
#if LANEBUF_E >= 2
		// E2: the builder stays on the buffer while scans keep asking: it waits for the lease holder to
		// install its result (until 300 us pass), then builds the next backlog at once.
		auto idle0 = std::chrono::steady_clock::now();
		for (;;) {
			if (b.ready.load(std::memory_order_acquire)) {
				if (std::chrono::steady_clock::now() - idle0 > std::chrono::microseconds(300)) { break; }
				cpu_relax();
				continue;
			}
			const runs_t* now = b.runs.load(std::memory_order_acquire);
			const u64 n2 = b.pi.count();
			if (!b.want.load(std::memory_order_relaxed) || n2 <= (now ? now->cover() : 0) + kTailMin) { break; }
			build_once(sb, now, n2);
			idle0 = std::chrono::steady_clock::now();
		}
		b.job_live.store(false, std::memory_order_release);
#endif
	}
	void maintain_buffer(const std::shared_ptr<buffer>& sb, bool& bump) {
		buffer& b = *sb;
		if (const runs_t* r = b.ready.exchange(nullptr, std::memory_order_acquire)) {
			const runs_t* old = b.runs.exchange(r, std::memory_order_release);
			if (old) { retire_obj(const_cast<runs_t*>(old)); }
			b.building = false;
			bump = true;
			++stats_.installs;
		}
#if LANEBUF_E >= 2
		if (b.want.load(std::memory_order_relaxed) && !b.building && !b.job_live.load(std::memory_order_acquire)) {
#else
		if (b.want.load(std::memory_order_relaxed) && !b.building) {
#endif
			const u64 n1 = b.pi.count();
			const runs_t* cur = b.runs.load(std::memory_order_relaxed);
			if (n1 > (cur ? cur->cover() : 0) + kTailMax) {
				b.want.store(false, std::memory_order_relaxed);
				b.building = true;
#if LANEBUF_E >= 2
				b.job_live.store(true, std::memory_order_release);
#endif
				job_pool::get().post([this, sb, cur, n1] { build_job(sb, cur, n1); });
			}
		}
	}
	void maintain() {
		if constexpr (!kSkip) {
			const view* vw = view_.load(std::memory_order_relaxed);
			bool bump = false;
			maintain_buffer(vw->active, bump);
			for (auto& f : vw->frozen) { maintain_buffer(f, bump); }
			if (bump) { bump_view(); }
		}
	}
	// A writerless group when nobody writes, so a scan's request for a build is served (D9).
	void poke() const {
		if (head_.load(std::memory_order_relaxed) == nullptr) { const_cast<buf_index*>(this)->maintenance(); }
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
		for (unsigned k = 0; k < n; ++k) {
			w.in[k] = ops[k];
			w.ops[k].kind = ops[k].kind;
			w.ops[k].expected = ops[k].expected;
		}
		lease_lock();
		const u64 t0 = time_leader_ ? now_ns() : 0;
		group_ctx ctx;
		apply_txn(w, ctx);
		apply_end(ctx, 1, t0);
		lease_unlock();
		return txn_finish(w, out);
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
		const u64 t0 = time_leader_ ? now_ns() : 0;
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
		group_ctx ctx;
		if constexpr (kGroup) {
			if (!self_chore && self.buf == view_.load(std::memory_order_relaxed)->active.get()) {
				ctx.ln = self.ln;
				ctx.lb = self.buf;
			}
		}
		for (unsigned i = 0; i < nm; ++i) { apply_txn(*members[i], ctx); }
		if constexpr (kSkip) {
			if (!self_chore) {
				par_pending_.store(int(nm) - 1, std::memory_order_relaxed);
				for (unsigned i = 1; i < nm; ++i) { post(members[i]->state, S_PAR); }
				link_own(self);
				while (par_pending_.load(std::memory_order_acquire) > 0) { cpu_relax(); }
			}
		}
		apply_end(ctx, nm, t0);
		// hand over: the oldest writer linked after the group leads next
		writer* exp = g_last;
		if (!head_.compare_exchange_strong(exp, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
			writer* x = exp;
			while (x->older != g_last) { x = x->older; }
			x->older = nullptr;
			post(x->state, S_LEADER);
		}
		for (unsigned i = 1; i < nm; ++i) { post(members[i]->state, S_DONE); }
	}

	// ---- the skip list (A')
	std::atomic<u32>& sl_next(buffer& b, u32 node, unsigned lvl) const noexcept { return node ? b.at(node).tower(lvl) : b.sl.head[lvl]; }
	// Node `n` sorts before (key, pd): key ascending, newest first.
	bool sl_before(buffer& b, u32 n, std::string_view key, u64 seq) const noexcept {
		rec r = b.at(n);
		const int c = cmp_keys(r.key(), key);
		return c < 0 || (c == 0 && r.seq() > seq);
	}
	void skip_insert(buffer& b, u32 ref) {
		rec r = b.at(ref);
		const unsigned height = r.height();
		const std::string_view key = r.key();
		const u64 pd = r.seq();
		unsigned top = b.sl.maxh.load(std::memory_order_relaxed);
		while (height > top && !b.sl.maxh.compare_exchange_weak(top, height, std::memory_order_acq_rel)) {}
		top = std::max(top, height);
		u32 prev[kMaxHeight], next[kMaxHeight];
		u32 x = 0;
		for (unsigned l = top; l-- > 0;) {
			for (;;) {
				const u32 n = sl_next(b, x, l).load(std::memory_order_acquire);
				if (n && sl_before(b, n, key, pd)) {
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
					if (n && sl_before(b, n, key, pd)) {
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

	// Every record of a buffer in scan order (the flush's walk): the skip list, or the position
	// index sorted. Nothing is pending in a frozen buffer.
	void ordered_refs(buffer& b, std::vector<u32>& refs) const {
		if constexpr (kSkip) {
			for (u32 n = b.sl.head[0].load(std::memory_order_acquire); n; n = b.at(n).tower(0).load(std::memory_order_acquire)) { refs.push_back(n); }
		} else {
			const u64 n = b.pi.count();
			refs.reserve(std::size_t(n));
			for (u64 i = 0; i < n; ++i) { refs.push_back(b.pi.at(i)); }
			std::sort(refs.begin(), refs.end(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
		}
	}

	// ---- scans: a merge over buffers (runs or skip list) and segments
	struct source {
		enum kind_t { run, skip, seg } kind;
		const buffer* b = nullptr;
		const run_t* a = nullptr;
		std::size_t i = 0;
		u32 node = 0;
		const segment* sg = nullptr;
		std::unique_ptr<keys::reader::range> rg;
		u64 r = 0;
		bool valid = false;
		std::string_view key;
		u64 pd = 0;  // seq
		u32 ref = 0;
		std::size_t ord = 0;
	};
	static bool visible_rec(const buffer& b, rec x, u64 r) noexcept { return b.base + x.pd() <= r; }
	void settle(source& s) const {
		s.valid = false;
		switch (s.kind) {
		case source::run:
			while (s.i < s.a->size()) {
				const u32 ref = run_ref(*s.a, s.i);
				rec x = s.b->at(ref);
				if (visible_rec(*s.b, x, s.r)) {
					s.valid = true;
					s.key = x.key();
					s.pd = x.seq();
					s.ref = ref;
					return;
				}
				++s.i;
			}
			return;
		case source::skip:
			while (s.node) {
				rec x = s.b->at(s.node);
				if (visible_rec(*s.b, x, s.r)) {
					s.valid = true;
					s.key = x.key();
					s.pd = x.seq();
					s.ref = s.node;
					return;
				}
				s.node = x.tower(0).load(std::memory_order_acquire);
			}
			return;
		case source::seg:
			while (s.rg->valid()) {
				const std::size_t o = s.rg->ordinal();
				if (s.sg->is_live(o)) {
					s.valid = true;
					s.key = s.rg->key();
					s.ord = o;
					return;
				}
				s.rg->next();
			}
			return;
		}
	}
	void step(source& s) const {
		switch (s.kind) {
		case source::run: ++s.i; break;
		case source::skip: s.node = s.b->at(s.node).tower(0).load(std::memory_order_acquire); break;
		case source::seg: s.rg->next(); break;
		}
		settle(s);
	}
#if LANEBUF_E == 0
	static u32 run_ref(const run_t& a, std::size_t i) noexcept { return a[i]; }
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
#else
	static u32 run_ref(const run_t& a, std::size_t i) noexcept { return a[i].ref; }
	static std::size_t lower_run(const buffer& b, const std::vector<ent>& a, std::string_view key) {
		const ent q = make_ent(key, 0);
		std::size_t lo = 0, hi = a.size();
		while (lo < hi) {
			const std::size_t mid = (lo + hi) / 2;
			const ent& e = a[mid];
			bool less;
			if (e.hi != q.hi) {
				less = e.hi < q.hi;
			} else if (e.lo != q.lo) {
				less = e.lo < q.lo;
			} else {
				less = cmp_keys(b.at(e.ref).key(), key) < 0;
			}
			if (less) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return lo;
	}
#endif
	// The runs a reader at r merges, with its private tail: the position index past the delta's
	// cover, up to r, sorted by the reader. A long tail asks for a build.
	struct run_view {
		const runs_t* r;
		run_t tail;
	};
	run_view runs_at(const buffer& b, u64 r, std::string_view from) const {
		run_view rv;
		rv.r = b.runs.load(std::memory_order_acquire);
		const u64 cover = rv.r ? rv.r->cover() : 0, n = b.pi.count();
		u64 seen = 0;
		for (u64 i = cover; i < n; ++i) {
			const u32 ref = b.pi.at(i);
			rec x = b.at(ref);
			if (b.base + x.pd() > r) { break; }
			++seen;
#if LANEBUF_E == 0
			if (cmp_keys(x.key(), from) >= 0) { rv.tail.push_back(ref); }
#else
			if (cmp_keys(x.key(), from) >= 0) { rv.tail.push_back(make_ent(x.key(), ref)); }
#endif
		}
		rs_.tail_sum.fetch_add(seen, std::memory_order_relaxed);
		rs_.tail_n.fetch_add(1, std::memory_order_relaxed);
		u64 mx = rs_.tail_max.load(std::memory_order_relaxed);
		while (seen > mx && !rs_.tail_max.compare_exchange_weak(mx, seen, std::memory_order_relaxed)) {}
		if (seen > kTailMax && !b.want.exchange(true, std::memory_order_relaxed)) { poke(); }
#if LANEBUF_E == 0
		std::sort(rv.tail.begin(), rv.tail.end(), [&](u32 p, u32 q) { return refs_less(b, p, q); });
#else
		std::sort(rv.tail.begin(), rv.tail.end(), [&](const ent& p, const ent& q) { return ent_less(b, p, q); });
#endif
		return rv;
	}
	void add_buffer_sources(std::vector<source>& srcs, std::vector<run_view>& keep, const buffer& cb, u64 r, std::string_view from) const {
		buffer& b = const_cast<buffer&>(cb);
		if constexpr (kSkip) {
			source s;
			s.kind = source::skip;
			s.b = &b;
			s.r = r;
			s.node = skip_seek(b, from);
			settle(s);
			srcs.push_back(std::move(s));
		} else {
			keep.push_back(runs_at(b, r, from));
			const run_view& rv = keep.back();
			auto add_run = [&](const run_t* a) {
				source s;
				s.kind = source::run;
				s.b = &b;
				s.a = a;
				s.r = r;
				s.i = lower_run(b, *a, from);
				settle(s);
				srcs.push_back(std::move(s));
			};
#if LANEBUF_E == 0
			static const std::vector<u32> none;
			add_run(rv.r ? rv.r->main.get() : &none);
			add_run(rv.r ? &rv.r->delta : &none);
#else
			if (rv.r) {
				for (const auto& l : rv.r->lv) { add_run(l.get()); }
			}
#endif
			add_run(&rv.tail);
		}
	}
	template <class F>
	std::size_t scan_in(const read_ctx& c, std::string_view from, std::size_t n, F&& visit) const {
		const view& vw = c.vw();
		std::vector<source> srcs;
		std::vector<run_view> keep;
		keep.reserve(vw.frozen.size() + 1);
		srcs.reserve(24 * (vw.frozen.size() + 1) + vw.segs.size());
		add_buffer_sources(srcs, keep, *vw.active, c.v, from);
		for (std::size_t i = vw.frozen.size(); i-- > 0;) { add_buffer_sources(srcs, keep, *vw.frozen[i], c.v, from); }
		for (std::size_t i = vw.segs.size(); i-- > 0;) {
			source s;
			s.kind = source::seg;
			s.sg = &vw.segs[i];
			s.rg = std::make_unique<keys::reader::range>(vw.segs[i].d->kr, from);
			settle(s);
			srcs.push_back(std::move(s));
		}
		std::size_t got_n = 0;
		while (got_n < n) {
			const source* min = nullptr;
			for (auto& s : srcs) {
				if (s.valid && (!min || cmp_keys(s.key, min->key) < 0)) { min = &s; }
			}
			if (!min) { break; }
			char kbuf[96];
			std::string kheap;
			std::string_view kv;
			if (min->key.size() <= sizeof kbuf) {
				std::memcpy(kbuf, min->key.data(), min->key.size());
				kv = {kbuf, min->key.size()};
			} else {
				kheap.assign(min->key);
				kv = kheap;
			}
			// The first source in priority order holding the key wins; within one buffer, the newest record.
			const source* best = nullptr;
			for (auto& s : srcs) {
				if (!s.valid || cmp_keys(s.key, kv) != 0) { continue; }
				if (!best) {
					best = &s;
				} else if (s.b && s.b == best->b && s.pd > best->pd) {
					best = &s;
				}
			}
			bool tomb = false;
			std::string_view value;
			if (best->kind == source::seg) {
				value = best->sg->d->value(best->ord);
			} else {
				rec x = best->b->at(best->ref);
				tomb = x.tomb();
				value = x.value();
			}
			if (!tomb) {
				visit(kv, value);
				++got_n;
			}
			for (auto& s : srcs) {
				while (s.valid && cmp_keys(s.key, kv) == 0) { step(s); }
			}
		}
		return got_n;
	}

	// ---- state
	registry& reg_;
	std::atomic<const view*> view_{nullptr};
	std::atomic<u64> vis_{0};
	std::atomic<writer*> head_{nullptr};
	std::atomic<int> par_pending_{0};
	std::atomic<u32> lease_{0};
	// lease holder only
	u64 pos_ = 0, docid_ = 0;
	bool time_leader_ = false;
	unsigned since_reclaim_ = 0;
	u64 scan_clock_ = 0;
	std::vector<gptr> garbage_;
	std::deque<retired_view> retired_;
	std::vector<const void*> named_;
	stats_t stats_;
	mutable rstats rs_;
};

}  // namespace lanebuf
