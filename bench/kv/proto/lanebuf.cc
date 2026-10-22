// The write-buffer bake-off's driver (docs/design/write-buffer.md, step 0). Modes:
//   fill | overwrite   T1/T4: W writers on one index          (--writers --records --ops --value --segments --seg-records)
//   rww                T2/T3: readers beside writers          (--kind get|scan --writers --readers --write-mb-s)
//   requests           T11: request threads over many indexes (--rs 1,4,8,16 --dists uniform,zipf --mixes 1w3r,3w1r)
//                      --path group (T11b: the write group / lease) | applier (T11a: proposers + appliers, --appliers N --turn-us D)
//   freeze             T6: freeze cost against buffered records and pending segment deletes
//   many               T7: heap bytes per buffer after one write (--count 1000)
//   check              T8/T9: a model check of concurrent writers, readers, freezes, flushes and merges
//   cases              the architect's failure cases (WB-1, WB-2, WB-3)
//   litmus             T10: the sequencer fills a header and stores the slot; readers verify
// --impl c | a | f selects the candidate: C (runs), A' (skip list), F (the single-writer lease).
// One JSON line per measurement.
#include "lanebuf.hh"
#include "lanebuf_work.hh"

#include <map>
#include <set>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

using namespace lanebuf;
using lbwork::args;

namespace {

strata::reader_registry& registry() {
	static strata::reader_registry r(128);
	return r;
}

std::size_t heap_bytes() {
#if defined(__APPLE__)
	malloc_statistics_t st{};
	malloc_zone_statistics(nullptr, &st);
	return st.size_in_use;
#else
	return 0;
#endif
}

// Documents for a segment of `n` keys of ids [first, first + n): sorted by key, docid = id + 1.
inline segment build_segment(u32 id, u64 first, u64 n, std::size_t vsz, kvbench::values& gen) {
	std::vector<std::pair<std::string, u64>> ks;
	ks.reserve(n);
	for (u64 i = 0; i < n; ++i) { ks.emplace_back(kvbench::key_of(first + i), first + i); }
	std::sort(ks.begin(), ks.end());
	std::array<bool, 256> used{};
	used['Q'] = true;
	for (char c = '0'; c <= '9'; ++c) { used[std::uint8_t(c)] = true; }
	seg_stream st(keys::alphabet_of(used));
	for (auto& [k, i] : ks) { st.add(k, gen.next(vsz), i + 1, 1); }
	return st.finish(id);
}

template <class I>
struct lb_base {
	std::vector<std::unique_ptr<I>> ix;
	u32 next_seg = 1;
	explicit lb_base(unsigned k, bool timing) {
		for (unsigned i = 0; i < k; ++i) {
			ix.push_back(std::make_unique<I>(registry()));
			ix.back()->set_leader_timing(timing);
		}
	}
	struct ticket {
		I* ix;
		unsigned idx;
	};
	ticket checkout(unsigned i) { return {ix[i].get(), i}; }
	bool get(ticket& t, std::string_view key, std::string& out) { return t.ix->get(key, &out).found; }
	std::size_t scan(ticket& t, std::string_view from, unsigned n, std::uint64_t& bytes) {
		return t.ix->scan(from, n, [&](std::string_view k, std::string_view v) { bytes += v.size() + k.size(); });
	}
	void checkin(ticket&) {}
	void quiesce() {
		for (auto& i : ix) {
			i->scan("", 1, [](std::string_view, std::string_view) {});  // a scan asks for the run build
			i->quiesce();
		}
	}
	// S segments of M records each, in a key space the workload never touches.
	void seed(unsigned S, u64 M, u64 vsz) {
		kvbench::values gen(3);
		for (unsigned s = 0; s < S; ++s) { ix[0]->add_segment(build_segment(next_seg++, (u64(1) << 40) + u64(s) * M, M, vsz, gen)); }
		ix[0]->raise_docid(u64(1) << 41);
	}
	// One small index whose records live in one segment, as strata's writer leaves them.
	void load_index(unsigned i, u64 n, u64 vsz, kvbench::values& gen) {
		ix[i]->add_segment(build_segment(1, 0, n, vsz, gen));
		ix[i]->raise_docid(n + 1);
	}
	std::string stats_json() {
		stats_t s;
		for (auto& i : ix) {
			const stats_t x = i->stats();
			s.records += x.records;
			s.lane_cap += x.lane_cap;
			s.lane_used += x.lane_used;
			s.table_bytes += x.table_bytes;
			s.runs_bytes += x.runs_bytes;
			s.pi_bytes += x.pi_bytes;
			s.groups += x.groups;
			s.group_ops += x.group_ops;
			s.leader_ns += x.leader_ns;
			s.longest_ns = std::max(s.longest_ns, x.longest_ns);
			for (int h = 0; h < 17; ++h) { s.hist[h] += x.hist[h]; }
			s.run_builds += x.run_builds;
			s.run_build_ns += x.run_build_ns;
			s.tail_sum += x.tail_sum;
			s.tail_n += x.tail_n;
			s.tail_max = std::max(s.tail_max, x.tail_max);
			s.restaged += x.restaged;
			s.preres_used += x.preres_used;
			s.preres_redone += x.preres_redone;
			s.views += x.views;
			s.installs += x.installs;
		}
		const double rec = s.records ? double(s.records) : 1.0;
		std::string h;
		for (int i = 1; i < 17; ++i) { h += (i > 1 ? "," : "") + std::to_string(s.hist[i]); }
		return lbwork::fmt("{\"records\":%zu,\"acct_bytes_per_rec\":%.1f,\"lane_used_per_rec\":%.1f,\"table_per_rec\":%.1f,\"pi_per_rec\":%.1f,\"runs_per_rec\":%.1f,\"groups\":%llu,"
			"\"group_ops\":%llu,\"leader_ns\":%llu,\"longest_group_ns\":%llu,\"group_hist\":[%s],\"run_builds\":%llu,\"run_build_ns\":%llu,\"tail_avg\":%.1f,\"tail_max\":%llu,"
			"\"restaged\":%llu,\"preres_used\":%llu,\"preres_redone\":%llu,\"views\":%llu,\"installs\":%llu}",
			s.records, double(s.lane_cap + s.table_bytes + s.runs_bytes + s.pi_bytes) / rec, double(s.lane_used) / rec, double(s.table_bytes) / rec, double(s.pi_bytes) / rec,
			double(s.runs_bytes) / rec, (unsigned long long)s.groups, (unsigned long long)s.group_ops, (unsigned long long)s.leader_ns, (unsigned long long)s.longest_ns, h.c_str(),
			(unsigned long long)s.run_builds, (unsigned long long)s.run_build_ns, s.tail_n ? double(s.tail_sum) / double(s.tail_n) : 0.0, (unsigned long long)s.tail_max,
			(unsigned long long)s.restaged, (unsigned long long)s.preres_used, (unsigned long long)s.preres_redone, (unsigned long long)s.views, (unsigned long long)s.installs);
	}
};

// T1, T2/T3, T11b: a write is one journal-less transaction through the write group (C, A') or the lease (F).
template <class I>
struct lb_engine : lb_base<I> {
	using lb_base<I>::lb_base;
	using ticket = typename lb_base<I>::ticket;
	bool put(ticket& t, std::string_view key, std::string_view value) {
		op_in o{op_kind::put, key, value, -1};
		write_out w;
		return t.ix->write(&o, 1, w);
	}
};

// T11a: proposers pre-stage (C, A') or just submit (F); one applier thread per partition applies the
// entries in order after a modeled log turn, then delivers the verdicts.
template <class I>
struct app_engine : lb_base<I> {
	using ticket = typename lb_base<I>::ticket;
	struct entry {
		typename I::txn w;
		std::atomic<u32> done{0};
		entry* next = nullptr;
		I* ix = nullptr;
	};
	struct applier {
		std::atomic<entry*> inbox{nullptr};
		std::thread th;
		std::atomic<u64> busy_ns{0}, entries{0}, turns{0};
	};
	std::vector<std::unique_ptr<applier>> ap;
	double turn_us;
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	app_engine(unsigned k, bool timing, unsigned appliers, double turn) : lb_base<I>(k, timing), turn_us(turn) {
		for (unsigned a = 0; a < appliers; ++a) {
			ap.push_back(std::make_unique<applier>());
			applier* p = ap.back().get();
			p->th = std::thread([this, p] { loop(*p); });
		}
	}
	~app_engine() {
		for (auto& a : ap) {
			entry stop;
			push(*a, &stop);
			a->th.join();
		}
	}
	static void push(applier& a, entry* e) {
		entry* h = a.inbox.load(std::memory_order_relaxed);
		do {
			e->next = h;
		} while (!a.inbox.compare_exchange_weak(h, e, std::memory_order_acq_rel, std::memory_order_relaxed));
		a.inbox.notify_one();
	}
	void loop(applier& a) {
		for (;;) {
			entry* n = a.inbox.exchange(nullptr, std::memory_order_acquire);
			for (int spin = 0; !n && spin < 200; ++spin) {
				cpu_relax();
				if (a.inbox.load(std::memory_order_relaxed)) { n = a.inbox.exchange(nullptr, std::memory_order_acquire); }
			}
			for (int spin = 0; !n && spin < 200; ++spin) {
				std::this_thread::yield();
				if (a.inbox.load(std::memory_order_relaxed)) { n = a.inbox.exchange(nullptr, std::memory_order_acquire); }
			}
			if (!n) {
				a.inbox.wait(nullptr, std::memory_order_acquire);
				continue;
			}
			const auto b0 = std::chrono::steady_clock::now();
			entry* rev = nullptr;  // arrival order
			while (n) {
				entry* nx = n->next;
				n->next = rev;
				rev = n;
				n = nx;
			}
			if (turn_us > 0) {  // the log turn: a proposal is applied only after it
				const auto until = b0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::micro>(turn_us));
				while (std::chrono::steady_clock::now() < until) { cpu_relax(); }
			}
			bool stop = false;
			unsigned cnt = 0;
			for (entry* e = rev; e;) {
				entry* nx = e->next;
				if (!e->ix) {
					stop = true;
				} else {
					typename I::group_ctx ctx;
					e->ix->apply_txn(e->w, ctx);
					e->ix->apply_end(ctx, 1);
					++cnt;
					e->done.store(1, std::memory_order_release);
					e->done.notify_one();
				}
				e = nx;
			}
			a.entries.fetch_add(cnt, std::memory_order_relaxed);
			a.turns.fetch_add(1, std::memory_order_relaxed);
			a.busy_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - b0).count()), std::memory_order_relaxed);
			if (stop) { return; }
		}
	}
	bool put(ticket& t, std::string_view key, std::string_view value) {
		entry e;
		e.ix = t.ix;
		op_in o{op_kind::put, key, value, -1};
		t.ix->txn_prepare(e.w, &o, 1);
		push(*ap[t.idx % ap.size()], &e);
		// wait for the verdict: a short spin, then yields (so the applier gets the core when threads outnumber
		// cores), then a futex wait
		for (int i = 0; i < 200 && !e.done.load(std::memory_order_acquire); ++i) { cpu_relax(); }
		for (int i = 0; i < 400 && !e.done.load(std::memory_order_acquire); ++i) { std::this_thread::yield(); }
		while (!e.done.load(std::memory_order_acquire)) { e.done.wait(0, std::memory_order_acquire); }
		write_out out;
		return t.ix->txn_finish(e.w, out);
	}
	std::string stats_json() {
		u64 busy = 0, ent = 0, turns = 0;
		for (auto& a : ap) {
			busy += a->busy_ns.load();
			ent += a->entries.load();
			turns += a->turns.load();
		}
		const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		return lbwork::fmt("{\"appliers\":%zu,\"applier_busy_s\":%.3f,\"since_open_s\":%.3f,\"applier_entries\":%llu,\"applier_turns\":%llu,\"base\":%s}", ap.size(), double(busy) * 1e-9, wall,
			(unsigned long long)ent, (unsigned long long)turns, lb_base<I>::stats_json().c_str());
	}
};

// ---- T6: freeze cost ----------------------------------------------------------------------

template <class I>
void freeze_sweep(const args& a, const char* impl) {
	const unsigned trials = unsigned(a.num("--trials", 3));
	for (const std::string& ns : lbwork::split(a.str("--sizes", "1000,10000,100000,1000000"))) {
		const u64 N = std::strtoull(ns.c_str(), nullptr, 10);
		for (const unsigned rep : {0u, 10u}) {
			for (unsigned t = 0; t < trials; ++t) {
				I ix(registry());
				kvbench::values gen(5);
				ix.add_segment(build_segment(1, 0, N, 100, gen));
				ix.raise_docid(N + 1);
				kvbench::rng r(11 + t);
				for (u64 i = 0; i < N; ++i) {
					const bool rp = r.below(100) < rep;
					const std::string k = kvbench::key_of(rp ? r.below(N) : (u64(1) << 41) + i);
					op_in o{op_kind::put, k, gen.next(100), -1};
					write_out w;
					ix.write(&o, 1, w);
				}
				const std::size_t pend = ix.active_buffer().pending.size();
				const auto t0 = kvbench::clock::now();
				ix.freeze();
				const double us = kvbench::since(t0) * 1e6;
				segment f;
				const auto t1 = kvbench::clock::now();
				ix.build_flush_segment(f, 2);
				const double flush_ms = kvbench::since(t1) * 1e3;
				std::printf("{\"impl\":\"%s\",\"buffered\":%llu,\"replace_pct\":%u,\"trial\":%u,\"pending_deletes\":%zu,\"freeze_us\":%.2f,\"flush_build_ms\":%.2f}\n", impl, (unsigned long long)N, rep, t,
					pend, us, flush_ms);
				std::fflush(stdout);
			}
		}
	}
}

// ---- T6: resident bytes per buffered record, kv_proto_bufmem's method: RSS at N1 and at N2 buffered
// records (nothing flushed), the difference over N2 - N1, written by W writers sharing the buffer.

template <class I>
void bytes_mode(const args& a, const char* impl) {
	const unsigned W = unsigned(a.num("--writers", 1)), trials = unsigned(a.num("--trials", 3));
	const u64 N1 = a.num("--first", 100000), N2 = a.num("--second", 300000), vsz = a.num("--value", 100);
	for (unsigned t = 0; t < trials; ++t) {
		lb_engine<I> e(1, false);
		auto fill_to = [&](u64 from, u64 to) {
			lbwork::run_threads(W, [&](unsigned w) {
				auto tk = e.checkout(0);
				kvbench::values gen(40 + w);
				const u64 per = (to - from) / W;
				for (u64 i = 0; i < per; ++i) { e.put(tk, kvbench::key_of(from + u64(w) * per + i), gen.next(vsz)); }
			});
		};
		fill_to(0, N1);
		const std::size_t h1 = heap_bytes();
		const auto u1 = kvbench::sample();
		fill_to(N1, N2);
		const std::size_t h2 = heap_bytes();
		const auto u2 = kvbench::sample();
		const stats_t st = e.ix[0]->stats();
		const double n = double(N2 - N1);
		std::printf("{\"impl\":\"%s\",\"writers\":%u,\"trial\":%u,\"value\":%llu,\"rss_per_record\":%.1f,\"heap_per_record\":%.1f,\"acct_per_record\":%.1f,\"lane_per_record\":%.1f,\"slot_table_per_record\":%.1f,"
			"\"pos_index_per_record\":%.1f,\"lanes\":%zu,\"records\":%zu}\n",
			impl, W, t, (unsigned long long)vsz, double(u2.rss - u1.rss) / n, double(h2 - h1) / n, double(st.lane_cap + st.table_bytes + st.pi_bytes + st.runs_bytes) / double(st.records),
			double(st.lane_cap) / double(st.records), double(st.table_bytes) / double(st.records), double(st.pi_bytes) / double(st.records), st.lanes, st.records);
		std::fflush(stdout);
	}
}

// ---- T7: heap bytes per buffer after one write ---------------------------------------------

template <class I>
void many(const args& a, const char* impl) {
	const unsigned K = unsigned(a.num("--count", 1000));
	const unsigned trials = unsigned(a.num("--trials", 3));
	for (unsigned t = 0; t < trials; ++t) {
		const std::size_t h0 = heap_bytes();
		std::vector<std::unique_ptr<I>> v;
		for (unsigned i = 0; i < K; ++i) { v.push_back(std::make_unique<I>(registry())); }
		const std::size_t h1 = heap_bytes();
		for (unsigned i = 0; i < K; ++i) {
			op_in o{op_kind::put, "Q000000000000001", "value-of-100-bytes-0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567", -1};
			write_out w;
			v[i]->write(&o, 1, w);
		}
		const std::size_t h2 = heap_bytes();
		std::printf("{\"impl\":\"%s\",\"count\":%u,\"trial\":%u,\"idle_bytes_per_index\":%.0f,\"after_one_write_bytes_per_index\":%.0f}\n", impl, K, t, double(h1 - h0) / K, double(h2 - h0) / K);
		std::fflush(stdout);
	}
}

// ---- T8/T9: the model check ---------------------------------------------------------------

struct model_entry {
	bool live = false;
	u64 docid = 0, version = 0;
	std::string value;
};
struct logged_op {
	op_kind kind;
	std::string key, value;
	i64 expected;
};
struct logged_txn {
	u64 pos;
	std::vector<logged_op> ops;
	bool accepted;
	std::vector<op_out> out;
};
struct read_obs {
	u64 v;
	std::string key;
	got g;
	std::string value;
};
struct scan_item {
	std::string key, value;
};
struct scan_obs {
	u64 v;
	std::string from;
	std::size_t n;
	std::vector<scan_item> items;
};

// The seeded state: even keys live in `nseg` segments at version 1, docid 1.. in key order.
struct seed_t {
	std::vector<std::vector<seg_item>> segs;
	std::map<std::string, model_entry> state;
	u64 last_docid = 0;
};
inline seed_t make_seed(u64 K, unsigned nseg) {
	seed_t s;
	s.segs.resize(nseg);
	u64 doc = 0;
	for (u64 i = 0; i < K; i += 2) {
		char b[16];
		std::snprintf(b, sizeof b, "k%04llu", (unsigned long long)i);
		seg_item it;
		it.key = b;
		it.value = "seed-" + it.key;
		it.docid = ++doc;
		it.version = 1;
		s.state[it.key] = {true, it.docid, 1, it.value};
		s.segs[(i / 2) % nseg].push_back(std::move(it));
	}
	s.last_docid = doc;
	return s;
}

template <class I>
std::size_t frozen_count(I& ix) {
	const auto c = ix.begin_read();
	return c.vw().frozen.size();
}

template <class I>
int check(const args& a) {
	const unsigned W = unsigned(a.num("--writers", 4)), Rd = unsigned(a.num("--readers", 3));
	const u64 T = a.num("--txns", 20000), K = a.num("--keys", 64);
	const bool chores = a.num("--chores", 1) != 0;
	const unsigned NSEG = 3;
	const seed_t seed = make_seed(K, NSEG);
	I ix(registry());
	std::atomic<u32> seg_id{100};
	for (unsigned s = 0; s < NSEG; ++s) { ix.add_segment(make_segment(s + 1, seed.segs[s])); }
	ix.raise_docid(seed.last_docid);
	std::atomic<unsigned> writers_done{0};
	std::vector<std::vector<logged_txn>> logs(W);
	std::vector<std::vector<read_obs>> robs(Rd);
	std::vector<std::vector<scan_obs>> sobs(Rd);
	std::vector<u64> vbad(Rd);
	std::atomic<u64> freezes{0}, flushes{0}, merges{0};
	std::atomic<bool> stop{false};
	auto key_of = [&](u64 i) {
		char b[16];
		std::snprintf(b, sizeof b, "k%04llu", (unsigned long long)(i % K));
		return std::string(b);
	};
	std::vector<std::thread> th;
	for (unsigned w = 0; w < W; ++w) {
		th.emplace_back([&, w] {
			kvbench::rng r(100 + w);
			for (u64 s = 0; s < T; ++s) {
				const unsigned n = 1 + unsigned(r.below(3));
				std::vector<logged_op> lops(n);
				op_in ops[kMaxOps];
				for (unsigned k = 0; k < n; ++k) {
					auto& o = lops[k];
					o.key = key_of(r.below(K));
					o.kind = r.below(100) < 25 ? op_kind::del : op_kind::put;
					o.expected = -1;
					if (r.below(100) < 30) {
						const unsigned c = unsigned(r.below(4));
						if (c == 0) {
							o.expected = 0;
						} else if (c == 1) {
							o.expected = i64(1 + r.below(4));
						} else {
							const got g = ix.get(o.key);
							o.expected = g.found ? i64(g.version) : 0;
						}
					}
					char vb[160];
					const int vl = std::snprintf(vb, sizeof vb, "w%u.s%llu.o%u.", w, (unsigned long long)s, k);
					o.value.assign(vb, std::size_t(vl));
					o.value.append(std::size_t(r.below(60)), char('a' + r.below(26)));
					ops[k] = {o.kind, o.key, o.value, o.expected};
				}
				write_out out;
				ix.write(ops, n, out);
				logged_txn lt;
				lt.pos = out.pos;
				lt.ops = std::move(lops);
				lt.accepted = out.accepted;
				lt.out.assign(out.ops, out.ops + n);
				logs[w].push_back(std::move(lt));
				if (r.below(64) == 0) { std::this_thread::yield(); }
			}
			writers_done.fetch_add(1);
		});
	}
	for (unsigned rd = 0; rd < Rd; ++rd) {
		th.emplace_back([&, rd] {
			kvbench::rng r(500 + rd);
			u64 last_v = 0;
			std::string val;
			while (!stop.load(std::memory_order_relaxed)) {
				const u64 m = r.below(10);
				if (m < 6) {
					read_obs o;
					o.key = key_of(r.below(K));
					o.g = ix.get(o.key, &val);
					o.v = o.g.at;
					o.value = val;
					if (o.v < last_v) { ++vbad[rd]; }
					last_v = o.v;
					robs[rd].push_back(std::move(o));
				} else if (m < 8) {
					scan_obs s;
					s.from = key_of(r.below(K));
					s.n = 1 + r.below(8);
					ix.scan(s.from, s.n, [&](std::string_view k, std::string_view v) { s.items.push_back({std::string(k), std::string(v)}); }, &s.v);
					if (s.v < last_v) { ++vbad[rd]; }
					last_v = s.v;
					sobs[rd].push_back(std::move(s));
				} else {
					// a held view: several reads at one position, across a pause (a freeze or install may land between V and the view)
					const u64 v0 = ix.load_v();
					for (u64 y = r.below(40); y > 0; --y) { cpu_relax(); }
					if (r.below(4) == 0) { std::this_thread::sleep_for(std::chrono::microseconds(r.below(400))); }
					const auto sc = ix.begin_read_at(v0);
					for (int i = 0; i < 3; ++i) {
						read_obs o;
						o.key = key_of(r.below(K));
						o.g = ix.get_in(sc, o.key, &val);
						o.v = sc.v;
						o.value = val;
						robs[rd].push_back(std::move(o));
						if (i == 1) { std::this_thread::yield(); }
					}
					if (sc.v < last_v) { ++vbad[rd]; }
					last_v = sc.v;
				}
			}
		});
	}
	std::thread chore_thread;
	if (chores) {
		chore_thread = std::thread([&] {
			kvbench::rng r(900);
			while (!stop.load(std::memory_order_relaxed)) {
				std::this_thread::sleep_for(std::chrono::microseconds(50 + r.below(400)));
				const u64 what = r.below(10);
				if (what < 6) {
					ix.freeze();
					freezes.fetch_add(1);
					segment s;
					while (frozen_count(ix) > 1 || (r.below(2) == 0 && frozen_count(ix) > 0)) {
						if (ix.build_flush_segment(s, seg_id.fetch_add(1))) {
							ix.install_segment(s);
							flushes.fetch_add(1);
						} else {
							break;
						}
					}
				} else if (what < 9) {
					// merge two segments; deletes may be pending on them when it installs (WB-2)
					std::vector<u32> ids;
					{
						const auto c = ix.begin_read();
						const auto& sg = c.vw().segs;
						if (sg.size() >= 2) {
							const std::size_t i = r.below(sg.size() - 1);
							ids = {sg[i].id, sg[i + 1].id};
						}
					}
					if (!ids.empty()) {
						segment out;
						typename I::merge_plan plan;
						if (ix.build_merge_segment(out, plan, seg_id.fetch_add(1), ids)) {
							if (r.below(2)) { std::this_thread::sleep_for(std::chrono::microseconds(100 + r.below(500))); }  // writes land meanwhile
							ix.merge_install(out, plan);
							merges.fetch_add(1);
						}
					}
				} else {
					ix.maintenance();
				}
			}
		});
	}
	while (writers_done.load() < W) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	stop.store(true);
	for (auto& t : th) { t.join(); }
	if (chore_thread.joinable()) { chore_thread.join(); }

	// ---- the model: replay every transaction in position order
	std::vector<const logged_txn*> all;
	for (auto& l : logs) {
		for (auto& t : l) { all.push_back(&t); }
	}
	std::sort(all.begin(), all.end(), [](auto* x, auto* y) { return x->pos < y->pos; });
	struct hist_ent {
		u64 pos;
		model_entry e;
	};
	std::map<std::string, std::vector<hist_ent>> hist;
	std::map<std::string, model_entry> cur = seed.state;
	for (auto& [k, e] : seed.state) { hist[k].push_back({0, e}); }
	u64 docid = seed.last_docid, bad = 0, accepted = 0;
	auto report = [&](const char* what, const std::string& detail) {
		if (++bad <= 8) { std::fprintf(stderr, "MISMATCH %s: %s\n", what, detail.c_str()); }
	};
	for (std::size_t i = 0; i < all.size(); ++i) {
		const logged_txn& t = *all[i];
		if (i && all[i - 1]->pos == t.pos) { report("duplicate position", std::to_string(t.pos)); }
		std::map<std::string, model_entry> tmp;  // the transaction's view of the keys it touches
		const u64 docid0 = docid;
		bool ok = true;
		std::vector<op_out> exp(t.ops.size());
		auto get_e = [&](const std::string& k) -> model_entry& {
			auto it = tmp.find(k);
			if (it != tmp.end()) { return it->second; }
			auto c = cur.find(k);
			return tmp.emplace(k, c == cur.end() ? model_entry{} : c->second).first->second;
		};
		for (std::size_t k = 0; k < t.ops.size() && ok; ++k) {
			const auto& o = t.ops[k];
			model_entry& e = get_e(o.key);
			bool pass = true;
			if (o.kind == op_kind::del) {
				pass = e.live && (o.expected < 0 || u64(o.expected) == e.version);
			} else if (o.expected == 0) {
				pass = !e.live;
			} else if (o.expected > 0) {
				pass = e.live && u64(o.expected) == e.version;
			}
			if (!pass) {
				ok = false;
				break;
			}
			if (o.kind == op_kind::del) {
				exp[k] = {true, e.docid, e.version};
				e.live = false;
				e.value.clear();
			} else {
				const u64 d = e.live ? e.docid : ++docid;
				e.version = e.live ? e.version + 1 : 1;  // a re-add after a delete is a new document
				e.docid = d;
				e.live = true;
				e.value = o.value;
				exp[k] = {true, d, e.version};
			}
		}
		if (ok != t.accepted) { report("verdict", "pos " + std::to_string(t.pos) + " model " + (ok ? "accept" : "reject")); }
		if (ok) {
			++accepted;
			for (auto& [k, e] : tmp) {
				cur[k] = e;
				hist[k].push_back({t.pos, e});
			}
			for (std::size_t k = 0; k < t.ops.size(); ++k) {
				if (!t.accepted) { break; }
				if (t.out[k].docid != exp[k].docid || t.out[k].version != exp[k].version) { report("docid/version", "pos " + std::to_string(t.pos) + " op " + std::to_string(k)); }
			}
		} else {
			docid = docid0;
		}
	}
	auto model_at = [&](const std::string& k, u64 v) -> const model_entry* {
		auto it = hist.find(k);
		if (it == hist.end()) { return nullptr; }
		const model_entry* r = nullptr;
		for (auto& h : it->second) {
			if (h.pos <= v) {
				r = &h.e;
			} else {
				break;
			}
		}
		return r;
	};
	u64 nreads = 0, nscans = 0;
	for (auto& rv : robs) {
		for (auto& o : rv) {
			++nreads;
			const model_entry* m = model_at(o.key, o.v);
			const bool live = m && m->live;
			if (o.g.found != live) {
				report("read presence", o.key + " at " + std::to_string(o.v) + (live ? " model live" : " model absent"));
			} else if (live && (o.g.docid != m->docid || o.g.version != m->version || o.value != m->value)) {
				report("read content", o.key + " at " + std::to_string(o.v) + " v" + std::to_string(o.g.version) + " vs v" + std::to_string(m->version));
			}
		}
	}
	for (auto& sv : sobs) {
		for (auto& s : sv) {
			++nscans;
			std::vector<scan_item> want;
			for (auto it = hist.lower_bound(s.from); it != hist.end() && want.size() < s.n; ++it) {
				const model_entry* m = model_at(it->first, s.v);
				if (m && m->live) { want.push_back({it->first, m->value}); }
			}
			bool same = want.size() == s.items.size();
			for (std::size_t i = 0; same && i < want.size(); ++i) { same = want[i].key == s.items[i].key && want[i].value == s.items[i].value; }
			if (!same) {
				std::string d = "from " + s.from + " at " + std::to_string(s.v) + " got";
				for (auto& it : s.items) { d += " " + it.key + "=" + it.value; }
				d += " | want";
				for (auto& it : want) { d += " " + it.key + "=" + it.value; }
				report("scan", d);
			}
		}
	}
	for (unsigned i = 0; i < Rd; ++i) {
		if (vbad[i]) { report("V decreased", "reader " + std::to_string(i)); }
	}
	// T9: replay the log order on one thread against a fresh single-lane index.
	{
		buf_index<order_kind::runs, conc_kind::lease> rp(registry());
		for (unsigned s = 0; s < NSEG; ++s) { rp.add_segment(make_segment(s + 1, seed.segs[s])); }
		rp.raise_docid(seed.last_docid);
		for (const logged_txn* t : all) {
			op_in ops[kMaxOps];
			for (std::size_t k = 0; k < t->ops.size(); ++k) { ops[k] = {t->ops[k].kind, t->ops[k].key, t->ops[k].value, t->ops[k].expected}; }
			write_out out;
			rp.write(ops, unsigned(t->ops.size()), out);
			if (out.accepted != t->accepted) {
				report("replay verdict", "pos " + std::to_string(t->pos));
				continue;
			}
			for (std::size_t k = 0; k < t->ops.size() && out.accepted; ++k) {
				if (out.ops[k].docid != t->out[k].docid || out.ops[k].version != t->out[k].version) { report("replay docid/version", "pos " + std::to_string(t->pos)); }
			}
		}
	}
	// the final state, and one live copy of each key across segments
	{
		std::vector<scan_item> got_items;
		ix.scan("", ~std::size_t(0) >> 1, [&](std::string_view k, std::string_view v) { got_items.push_back({std::string(k), std::string(v)}); });
		std::vector<scan_item> want;
		for (auto& [k, e] : cur) {
			if (e.live) { want.push_back({k, e.value}); }
		}
		bool same = want.size() == got_items.size();
		for (std::size_t i = 0; same && i < want.size(); ++i) { same = want[i].key == got_items[i].key && want[i].value == got_items[i].value; }
		if (!same) { report("final state", "got " + std::to_string(got_items.size()) + " want " + std::to_string(want.size())); }
		for (u64 i = 0; i < K; ++i) {
			if (ix.live_copies(key_of(i)) > 1) { report("two live copies", key_of(i)); }
		}
	}
	const stats_t st = ix.stats();
	if (st.lost_pending) { report("lost pending deletes", std::to_string(st.lost_pending)); }
	std::printf("{\"check\":\"%s\",\"txns\":%zu,\"accepted\":%llu,\"reads\":%llu,\"scans\":%llu,\"freezes\":%llu,\"flushes\":%llu,\"merges\":%llu,\"groups\":%llu,\"group_ops\":%llu,"
		"\"restaged\":%llu,\"pending_remapped\":%llu,\"preres_used\":%llu,\"preres_redone\":%llu,\"run_builds\":%llu,\"mismatches\":%llu}\n",
		bad ? "FAIL" : "ok", all.size(), (unsigned long long)accepted, (unsigned long long)nreads, (unsigned long long)nscans, (unsigned long long)freezes.load(),
		(unsigned long long)flushes.load(), (unsigned long long)merges.load(), (unsigned long long)st.groups, (unsigned long long)st.group_ops, (unsigned long long)st.restaged,
		(unsigned long long)st.remapped, (unsigned long long)st.preres_used, (unsigned long long)st.preres_redone, (unsigned long long)st.run_builds, (unsigned long long)bad);
	return bad ? 1 : 0;
}

// ---- the architect's failure cases (docs/reviews/write-buffer-design.md WB-1, WB-2, WB-3) --
// Deterministic schedules on one thread; each prints one JSON line and returns whether it held.

segment seg_of(u32 id, std::initializer_list<std::pair<const char*, const char*>> kv, u64 first_docid = 1) {
	std::vector<seg_item> items;
	u64 doc = first_docid;
	for (auto& [k, v] : kv) {
		seg_item it;
		it.key = k;
		it.value = v;
		it.docid = doc++;
		items.push_back(std::move(it));
	}
	return make_segment(id, std::move(items));
}

// WB-1: a transfer of 10 from a to b (both in a segment, sum 100), then a freeze; a reader that
// loaded V before the transfer acquires the view after the freeze. It must see the transfer whole
// (or not at all), never the delete half.
template <class I>
bool case_wb1(const char* impl) {
	I ix(registry());
	ix.add_segment(seg_of(1, {{"a", "50"}, {"b", "50"}}));
	ix.raise_docid(2);
	const u64 v0 = ix.load_v();
	op_in ops[2] = {{op_kind::put, "a", "40", -1}, {op_kind::put, "b", "60", -1}};
	write_out w;
	ix.write(ops, 2, w);
	ix.freeze();
	bool ok = true;
	std::string detail;
	auto read_sum = [&](const char* what) {
		const auto sc = ix.begin_read_at(v0);
		std::string a, b;
		const bool fa = ix.get_in(sc, "a", &a).found, fb = ix.get_in(sc, "b", &b).found;
		const int sum = (fa ? std::atoi(a.c_str()) : 0) + (fb ? std::atoi(b.c_str()) : 0);
		if (sum != 100) {
			ok = false;
			detail += std::string(what) + ": a=" + (fa ? a : "None") + " b=" + (fb ? b : "None") + " sum=" + std::to_string(sum) + "; ";
		}
	};
	read_sum("after freeze");
	segment f;
	ix.build_flush_segment(f, 2);
	ix.install_segment(f);
	read_sum("after install");
	std::printf("{\"case\":\"WB-1 torn transfer\",\"impl\":\"%s\",\"result\":\"%s\",\"detail\":\"%s\"}\n", impl, ok ? "pass" : "FAIL", detail.c_str());
	return ok;
}

// WB-2: a delete pending against a merge's source when the merge installs. The deleted key must
// not come back and must have one live copy.
template <class I>
bool case_wb2(const char* impl) {
	I ix(registry());
	ix.add_segment(seg_of(1, {{"k", "40"}}));
	ix.add_segment(seg_of(2, {{"j", "1"}}, 2));
	ix.raise_docid(2);
	write_out w;
	op_in put{op_kind::put, "k", "50", -1};
	ix.write(&put, 1, w);  // replaces the segment copy: a pending delete on segment 1
	segment m;
	typename I::merge_plan plan;
	ix.build_merge_segment(m, plan, 3, {1, 2});
	ix.merge_install(m, plan);  // installs with the delete still pending
	ix.freeze();                // applies the pending delete: to the merge's output when it was carried
	segment f;
	ix.build_flush_segment(f, 4);
	ix.install_segment(f);
	const std::size_t copies = ix.live_copies("k");
	op_in del{op_kind::del, "k", "", -1};
	ix.write(&del, 1, w);
	ix.freeze();
	ix.build_flush_segment(f, 5);
	ix.install_segment(f);
	std::string v;
	const bool found = ix.get("k", &v).found;
	const bool ok = copies == 1 && !found;
	std::printf("{\"case\":\"WB-2 delete resurrected across a merge install\",\"impl\":\"%s\",\"result\":\"%s\",\"live_copies_after_flush\":%zu,"
		"\"get_k_after_delete\":\"%s\",\"lost_pending_deletes\":%llu}\n",
		impl, ok ? "pass" : "FAIL", copies, found ? v.c_str() : "absent", (unsigned long long)ix.lost_pending());
	return ok;
}

// WB-3: a writer pre-stages, a freeze seals its buffer, then it links. The record is copied into
// the new buffer (once), seen by reads, and the flush of the sealed buffer does not carry it.
template <class I>
bool case_wb3(const char* impl) {
	if constexpr (!I::kGroup) {
		std::printf("{\"case\":\"WB-3 freeze while a writer pre-stages\",\"impl\":\"%s\",\"result\":\"n/a (no pre-staging)\"}\n", impl);
		return true;
	} else {
		I ix(registry());
		ix.add_segment(seg_of(1, {{"a", "1"}}));
		ix.raise_docid(1);
		write_out w;
		op_in first{op_kind::put, "b", "2", -1};
		ix.write(&first, 1, w);
		typename I::txn t;
		op_in late{op_kind::put, "c", "3", -1};
		ix.txn_prepare(t, &late, 1);  // pre-staged into the first buffer
		ix.freeze();                  // which is sealed before the writer links
		ix.apply_for_test(t);         // the leader copies it into the new buffer
		write_out out;
		ix.txn_finish(t, out);
		std::string vb, vc;
		const bool fb = ix.get("b", &vb).found, fc = ix.get("c", &vc).found;
		segment f;
		ix.build_flush_segment(f, 2);
		const bool sealed_has_c = f.find("c", xxh64::hash(std::string_view("c"))) != segment::npos;
		const bool ok = out.accepted && fb && fc && vc == "3" && !sealed_has_c && ix.stats().restaged == 1;
		std::printf("{\"case\":\"WB-3 freeze while a writer pre-stages\",\"impl\":\"%s\",\"result\":\"%s\",\"restaged\":%llu,\"sealed_flush_has_c\":%s}\n", impl, ok ? "pass" : "FAIL",
			(unsigned long long)ix.stats().restaged, sealed_has_c ? "true" : "false");
		return ok;
	}
}

template <class I>
int cases(const char* impl) {
	bool ok = case_wb1<I>(impl);
	ok = case_wb2<I>(impl) && ok;
	ok = case_wb3<I>(impl) && ok;
	return ok ? 0 : 1;
}

// ---- T10: the sequencer fills a header and stores the slot; a reader probes and checks it ----
// One key rewritten with value = its own version; a reader that sees a record whose value and
// version disagree has seen a header before it was filled.

template <class I>
int litmus(const args& a, const char* impl) {
	const u64 N = a.num("--iters", 200000);
	const unsigned Rd = unsigned(a.num("--readers", 2));
	I ix(registry());
	std::atomic<bool> stop{false};
	std::atomic<u64> reads{0}, torn{0};
	std::vector<std::thread> th;
	for (unsigned r = 0; r < Rd; ++r) {
		th.emplace_back([&] {
			std::string v;
			u64 n = 0, bad = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				for (int i = 0; i < 64; ++i) {
					const got g = ix.get("hot", &v);
					++n;
					if (g.found && (v != "v" + std::to_string(g.version) || g.docid != 1)) { ++bad; }
				}
			}
			reads.fetch_add(n);
			torn.fetch_add(bad);
		});
	}
	for (u64 i = 0; i < N; ++i) {
		const std::string v = "v" + std::to_string(i + 1);
		op_in o{op_kind::put, "hot", v, -1};
		write_out w;
		ix.write(&o, 1, w);
	}
	stop.store(true);
	for (auto& t : th) { t.join(); }
	std::printf("{\"litmus\":\"%s\",\"impl\":\"%s\",\"writes\":%llu,\"reads\":%llu,\"torn\":%llu}\n", torn.load() ? "FAIL" : "ok", impl, (unsigned long long)N, (unsigned long long)reads.load(),
		(unsigned long long)torn.load());
	return torn.load() ? 1 : 0;
}

template <class I>
int dispatch(const std::string& mode, const args& a, const char* impl) {
	if (mode == "check") { return check<I>(a); }
	if (mode == "cases") { return cases<I>(impl); }
	if (mode == "litmus") { return litmus<I>(a, impl); }
	if (mode == "freeze") {
		freeze_sweep<I>(a, impl);
		return 0;
	}
	if (mode == "many") {
		many<I>(a, impl);
		return 0;
	}
	if (mode == "bytes") {
		bytes_mode<I>(a, impl);
		return 0;
	}
	const bool timing = a.num("--leader-timing", 1) != 0;
	auto head = [&](const std::string& body) { std::printf("{\"impl\":\"%s\",\"result\":%s}\n", impl, body.c_str()); };
	if (mode == "fill" || mode == "overwrite") {
		job_pool::get().set_threads(1);
		lb_engine<I> e(1, timing);
		head(lbwork::fill(e, a));
	} else if (mode == "rww") {
		job_pool::get().set_threads(unsigned(a.num("--pool", 1)));
		lb_engine<I> e(1, timing);
		head(lbwork::rww(e, a));
	} else if (mode == "requests") {
		const unsigned K = unsigned(a.num("--indexes", 1000));
		const std::string path = a.str("--path", "applier");
		auto run = [&](auto& e) {
			lbwork::requests<std::remove_reference_t<decltype(e)>> rq(e, a);
			const auto u0 = kvbench::sample();
			const std::size_t h0 = heap_bytes();
			rq.preload(unsigned(a.num("--preload-threads", 4)));
			const auto u1 = kvbench::sample();
			std::printf("{\"impl\":\"%s\",\"path\":\"%s\",\"preload\":{\"records\":%llu,\"rss_delta\":%llu,\"heap_delta\":%zu,\"engine\":%s}}\n", impl, path.c_str(), (unsigned long long)rq.total_records(),
				(unsigned long long)(u1.rss - u0.rss), heap_bytes() - h0, e.stats_json().c_str());
			const double D = a.real("--seconds", 5);
			for (const auto& d : lbwork::split(a.str("--dists", "uniform,zipf"))) {
				for (const auto& m : lbwork::split(a.str("--mixes", "1w3r,3w1r"))) {
					for (const auto& r : lbwork::split(a.str("--rs", "1,4,8,16"))) { head(rq.run(unsigned(std::strtoul(r.c_str(), nullptr, 10)), d, m, D)); }
				}
			}
		};
		if (path == "applier") {
			app_engine<I> e(K, timing, unsigned(a.num("--appliers", 2)), a.real("--turn-us", 0));
			run(e);
		} else {
			lb_engine<I> e(K, timing);
			run(e);
		}
	} else {
		std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
		return 2;
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s MODE --impl c|a|f [options]\n", argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	const args a(argc - 1, argv + 1);
	const std::string impl = a.str("--impl", "c");
	if (impl == "c") { return dispatch<buf_index<order_kind::runs, conc_kind::group>>(mode, a, "C"); }
	if (impl == "a") { return dispatch<buf_index<order_kind::skiplist, conc_kind::group>>(mode, a, "A'"); }
	if (impl == "f") { return dispatch<buf_index<order_kind::runs, conc_kind::lease>>(mode, a, "F"); }
	std::fprintf(stderr, "unknown impl %s\n", impl.c_str());
	return 2;
}
