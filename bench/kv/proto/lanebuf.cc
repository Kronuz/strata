// The write-buffer bake-off's driver (docs/design/write-buffer.md, step 0). Modes:
//   fill | overwrite   T1/T4: W writers on one index          (--writers --records --ops --value)
//   rww                T2/T3: readers beside writers          (--kind get|scan --writers --readers --write-mb-s)
//   requests           T11: request threads over many indexes (--rs 1,4,8,16 --dists uniform,zipf --mixes 1w3r,3w1r)
//   freeze             T6: freeze cost against buffered records and pending segment deletes
//   check              T8/T9: a model check of concurrent writers, readers, freezes and flushes
// --impl c | a | f selects the candidate: C (runs), A' (skip list), F (the single-writer lease).
// One JSON line per measurement.
#include "lanebuf.hh"
#include "lanebuf_work.hh"

#include <map>
#include <set>

using namespace lanebuf;
using lbwork::args;

namespace {

template <class I>
struct lb_engine {
	std::vector<std::unique_ptr<I>> ix;
	explicit lb_engine(unsigned k, bool timing) {
		for (unsigned i = 0; i < k; ++i) {
			ix.push_back(std::make_unique<I>());
			ix.back()->set_leader_timing(timing);
		}
	}
	struct ticket {
		I* ix;
		unsigned lane = ~0u;
	};
	ticket checkout(unsigned i) { return {ix[i].get(), ~0u}; }
	bool put(ticket& t, std::string_view key, std::string_view value) {
		if (t.lane == ~0u) { t.lane = t.ix->claim_lane(); }
		op_in o{op_kind::put, key, value, -1};
		write_out w;
		return t.ix->write(t.lane, &o, 1, w);
	}
	bool get(ticket& t, std::string_view key, std::string& out) { return t.ix->get(key, &out).found; }
	std::size_t scan(ticket& t, std::string_view from, unsigned n, std::uint64_t& bytes) {
		return t.ix->scan(from, n, [&](std::string_view k, std::string_view v) { bytes += v.size() + k.size(); });
	}
	void checkin(ticket& t) {
		if (t.lane != ~0u) { t.ix->release_lane(t.lane); }
		t.lane = ~0u;
	}
	void quiesce() {}
	std::string stats_json() {
		stats_t s;
		for (auto& i : ix) {
			const stats_t x = i->stats();
			s.records += x.records;
			s.lane_cap += x.lane_cap;
			s.lane_used += x.lane_used;
			s.table_bytes += x.table_bytes;
			s.runs_bytes += x.runs_bytes;
			s.groups += x.groups;
			s.group_ops += x.group_ops;
			s.leader_ns += x.leader_ns;
			for (int h = 0; h < 17; ++h) { s.hist[h] += x.hist[h]; }
			s.run_builds += x.run_builds;
			s.run_build_ns += x.run_build_ns;
			s.tail_sum += x.tail_sum;
			s.tail_n += x.tail_n;
			s.restaged += x.restaged;
		}
		const double rec = s.records ? double(s.records) : 1.0;
		std::string h;
		for (int i = 1; i < 17; ++i) { h += (i > 1 ? "," : "") + std::to_string(s.hist[i]); }
		return lbwork::fmt("{\"records\":%zu,\"acct_bytes_per_rec\":%.1f,\"used_bytes_per_rec\":%.1f,\"lane_used\":%zu,\"table_bytes\":%zu,\"runs_bytes\":%zu,\"groups\":%llu,"
			"\"group_ops\":%llu,\"leader_ns\":%llu,\"group_hist\":[%s],\"run_builds\":%llu,\"run_build_ns\":%llu,\"tail_avg\":%.1f,\"restaged\":%llu}",
			s.records, double(s.lane_cap + s.table_bytes + s.runs_bytes) / rec, double(s.lane_used + s.table_bytes + s.runs_bytes) / rec, s.lane_used, s.table_bytes,
			s.runs_bytes, (unsigned long long)s.groups, (unsigned long long)s.group_ops, (unsigned long long)s.leader_ns, h.c_str(), (unsigned long long)s.run_builds,
			(unsigned long long)s.run_build_ns, s.tail_n ? double(s.tail_sum) / double(s.tail_n) : 0.0, (unsigned long long)s.restaged);
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
				I ix;
				kvbench::values gen(5);
				// a flushed segment of N keys
				auto d = std::make_shared<seg_data>();
				{
					std::vector<std::pair<std::string, u64>> ks;
					ks.reserve(N);
					for (u64 i = 0; i < N; ++i) { ks.emplace_back(kvbench::key_of(i), i); }
					std::sort(ks.begin(), ks.end());
					for (auto& [k, id] : ks) {
						d->keys.push_back(k);
						d->values.emplace_back(gen.next(100));
						d->docid.push_back(id + 1);
						d->version.push_back(1);
					}
				}
				ix.add_segment(make_segment(1, d));
				const unsigned lane = ix.claim_lane();
				kvbench::rng r(11 + t);
				for (u64 i = 0; i < N; ++i) {
					const bool rp = r.below(100) < rep;
					const std::string k = kvbench::key_of(rp ? r.below(N) : N + i);
					op_in o{op_kind::put, k, gen.next(100), -1};
					write_out w;
					ix.write(lane, &o, 1, w);
				}
				const std::size_t pend = ix.active_buffer().pending.size();
				const auto t0 = kvbench::clock::now();
				ix.freeze();
				const double us = kvbench::since(t0) * 1e6;
				ix.release_lane(lane);
				std::printf("{\"impl\":\"%s\",\"buffered\":%llu,\"replace_pct\":%u,\"trial\":%u,\"pending_deletes\":%zu,\"freeze_us\":%.2f}\n", impl, (unsigned long long)N, rep, t, pend, us);
				std::fflush(stdout);
			}
		}
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

template <class I>
int check(const args& a) {
	const unsigned W = unsigned(a.num("--writers", 4)), Rd = unsigned(a.num("--readers", 3));
	const u64 T = a.num("--txns", 20000), K = a.num("--keys", 64);
	const bool chores = a.num("--chores", 1) != 0;
	I ix;
	std::atomic<unsigned> writers_done{0};
	std::vector<std::vector<logged_txn>> logs(W);
	std::vector<std::vector<read_obs>> robs(Rd);
	std::vector<std::vector<scan_obs>> sobs(Rd);
	std::vector<u64> vbad(Rd);
	std::atomic<u64> freezes{0}, flushes{0};
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
			const unsigned lane = ix.claim_lane();
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
				ix.write(lane, ops, n, out);
				logged_txn lt;
				lt.pos = out.pos;
				lt.ops = std::move(lops);
				lt.accepted = out.accepted;
				lt.out.assign(out.ops, out.ops + n);
				logs[w].push_back(std::move(lt));
				if (r.below(64) == 0) { std::this_thread::yield(); }
			}
			ix.release_lane(lane);
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
					// a held view: several reads at one position, across a pause
					epoch::guard g;
					const u64 v0 = ix.load_v();
					for (u64 y = r.below(40); y > 0; --y) { cpu_relax(); }  // a freeze or install may land between the two halves
					if (r.below(4) == 0) { std::this_thread::yield(); }
					const auto sc = ix.finish_scope(v0);
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
			u32 seg_id = 100;
			while (!stop.load(std::memory_order_relaxed)) {
				std::this_thread::sleep_for(std::chrono::microseconds(50 + r.below(400)));
				ix.freeze();
				freezes.fetch_add(1);
				segment s;
				while (ix.current_view()->frozen.size() > 1 || (r.below(2) == 0 && !ix.current_view()->frozen.empty())) {
					if (ix.build_flush_segment(s, ++seg_id)) {
						ix.install_segment(s);
						flushes.fetch_add(1);
					} else {
						break;
					}
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
	std::map<std::string, model_entry> cur;
	u64 docid = 0, bad = 0, accepted = 0;
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
			const u64 d = e.live ? e.docid : ++docid;
			e.docid = d;
			e.version = e.version + 1;
			e.live = o.kind == op_kind::put;
			e.value = o.kind == op_kind::put ? o.value : std::string();
			exp[k] = {true, d, e.version};
		}
		if (ok != t.accepted) {
			report("verdict", "pos " + std::to_string(t.pos) + " model " + (ok ? "accept" : "reject"));
		}
		if (ok) {
			++accepted;
			for (auto& [k, e] : tmp) {
				cur[k] = e;
				hist[k].push_back({t.pos, e});
			}
			for (std::size_t k = 0; k < t.ops.size(); ++k) {
				if (!t.accepted) { break; }
				if (t.out[k].docid != exp[k].docid || t.out[k].version != exp[k].version) {
					report("docid/version", "pos " + std::to_string(t.pos) + " op " + std::to_string(k));
				}
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
			if (!same) { report("scan", "from " + s.from + " at " + std::to_string(s.v) + " got " + std::to_string(s.items.size()) + " want " + std::to_string(want.size())); }
		}
	}
	for (unsigned i = 0; i < Rd; ++i) {
		if (vbad[i]) { report("V decreased", "reader " + std::to_string(i)); }
	}
	// T9: replay the log order on one thread against a fresh single-lane index.
	{
		buf_index<order_kind::runs, conc_kind::lease> rp;
		for (const logged_txn* t : all) {
			op_in ops[kMaxOps];
			for (std::size_t k = 0; k < t->ops.size(); ++k) { ops[k] = {t->ops[k].kind, t->ops[k].key, t->ops[k].value, t->ops[k].expected}; }
			write_out out;
			rp.write(0, ops, unsigned(t->ops.size()), out);
			if (out.accepted != t->accepted) {
				report("replay verdict", "pos " + std::to_string(t->pos));
				continue;
			}
			for (std::size_t k = 0; k < t->ops.size() && out.accepted; ++k) {
				if (out.ops[k].docid != t->out[k].docid || out.ops[k].version != t->out[k].version) { report("replay docid/version", "pos " + std::to_string(t->pos)); }
			}
		}
	}
	// the final state
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
	}
	const stats_t st = ix.stats();
	std::printf("{\"check\":\"%s\",\"txns\":%zu,\"accepted\":%llu,\"reads\":%llu,\"scans\":%llu,\"freezes\":%llu,\"flushes\":%llu,\"groups\":%llu,\"group_ops\":%llu,\"restaged\":%llu,\"mismatches\":%llu}\n",
		bad ? "FAIL" : "ok", all.size(), (unsigned long long)accepted, (unsigned long long)nreads, (unsigned long long)nscans, (unsigned long long)freezes.load(),
		(unsigned long long)flushes.load(), (unsigned long long)st.groups, (unsigned long long)st.group_ops, (unsigned long long)st.restaged, (unsigned long long)bad);
	return bad ? 1 : 0;
}

// ---- the architect's failure cases (docs/reviews/write-buffer-design.md WB-1, WB-2) --------
// Deterministic schedules on one thread; each prints one JSON line and returns whether it held.

template <class I>
segment seg_of(u32 id, std::initializer_list<std::pair<const char*, const char*>> kv, u64 first_docid = 1) {
	auto d = std::make_shared<seg_data>();
	u64 doc = first_docid;
	for (auto& [k, v] : kv) {
		d->keys.push_back(k);
		d->values.push_back(v);
		d->docid.push_back(doc++);
		d->version.push_back(1);
	}
	return make_segment(id, std::move(d));
}

// WB-1: a transfer of 10 from a to b (both in a segment, sum 100), then a freeze; a reader that
// loaded V before the transfer acquires the view after the freeze. It must see the transfer whole
// (or not at all), never the delete half.
template <class I>
bool case_wb1(const char* impl) {
	I ix;
	ix.add_segment(seg_of<I>(1, {{"a", "50"}, {"b", "50"}}));
	const unsigned lane = ix.claim_lane();
	const u64 v0 = ix.load_v();
	op_in ops[2] = {{op_kind::put, "a", "40", -1}, {op_kind::put, "b", "60", -1}};
	write_out w;
	ix.write(lane, ops, 2, w);
	ix.freeze();
	bool ok = true;
	std::string detail;
	auto read_sum = [&](const char* what) {
		epoch::guard g;
		const auto sc = ix.finish_scope(v0);
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
	ix.release_lane(lane);
	std::printf("{\"case\":\"WB-1 torn transfer\",\"impl\":\"%s\",\"result\":\"%s\",\"detail\":\"%s\"}\n", impl, ok ? "pass" : "FAIL", detail.c_str());
	return ok;
}

// WB-2: a delete pending against a merge's source is lost when the merge installs; the deleted
// key comes back, and two live copies of it exist.
template <class I>
bool case_wb2(const char* impl) {
	I ix;
	ix.set_drop_tombstones(true);
	ix.add_segment(seg_of<I>(1, {{"k", "40"}}));
	ix.add_segment(seg_of<I>(2, {{"j", "1"}}, 2));
	const unsigned lane = ix.claim_lane();
	write_out w;
	op_in put{op_kind::put, "k", "50", -1};
	ix.write(lane, &put, 1, w);  // replaces the segment copy: a pending delete on segment 1
	segment m;
	ix.build_merge_segment(m, 3, {1, 2});
	ix.merge_install(m, {1, 2});  // installs with the delete still pending
	ix.freeze();                  // applies (segment 1, ordinal 0) to a segment that is gone
	segment f;
	ix.build_flush_segment(f, 4);
	ix.install_segment(f);
	const std::size_t copies = ix.live_copies("k");
	op_in del{op_kind::del, "k", "", -1};
	ix.write(lane, &del, 1, w);
	ix.freeze();
	ix.build_flush_segment(f, 5);
	ix.install_segment(f);
	std::string v;
	const bool found = ix.get("k", &v).found;
	ix.release_lane(lane);
	const bool ok = copies == 1 && !found;
	std::printf("{\"case\":\"WB-2 delete resurrected across a merge install\",\"impl\":\"%s\",\"result\":\"%s\",\"live_copies_after_flush\":%zu,"
		"\"get_k_after_delete\":\"%s\",\"lost_pending_deletes\":%llu}\n",
		impl, ok ? "pass" : "FAIL", copies, found ? v.c_str() : "absent", (unsigned long long)ix.lost_pending());
	return ok;
}

template <class I>
int cases(const char* impl) {
	bool ok = case_wb1<I>(impl);
	ok = case_wb2<I>(impl) && ok;
	return ok ? 0 : 1;
}

template <class I>
int dispatch(const std::string& mode, const args& a, const char* impl) {
	if (mode == "check") { return check<I>(a); }
	if (mode == "cases") { return cases<I>(impl); }
	if (mode == "freeze") {
		freeze_sweep<I>(a, impl);
		return 0;
	}
	const bool timing = a.num("--leader-timing", 0) != 0;
	const std::string ms = mode;
	const unsigned K = ms == "requests" ? unsigned(a.num("--indexes", 1000)) : 1;
	auto head = [&](const std::string& body) { std::printf("{\"impl\":\"%s\",\"result\":%s}\n", impl, body.c_str()); };
	if (ms == "fill" || ms == "overwrite") {
		lb_engine<I> e(1, timing);
		head(lbwork::fill(e, a));
	} else if (ms == "rww") {
		lb_engine<I> e(1, timing);
		head(lbwork::rww(e, a));
	} else if (ms == "requests") {
		lb_engine<I> e(K, timing);
		lbwork::requests<lb_engine<I>> rq(e, a);
		const auto u0 = kvbench::sample();
		rq.preload(unsigned(a.num("--preload-threads", 4)));
		const auto u1 = kvbench::sample();
		std::printf("{\"impl\":\"%s\",\"preload\":{\"records\":%llu,\"rss_delta\":%llu,\"rss_per_index\":%.0f,\"engine\":%s}}\n", impl, (unsigned long long)rq.total_records(),
			(unsigned long long)(u1.rss - u0.rss), double(u1.rss - u0.rss) / K, e.stats_json().c_str());
		const double D = a.real("--seconds", 5);
		for (const auto& d : lbwork::split(a.str("--dists", "uniform,zipf"))) {
			for (const auto& m : lbwork::split(a.str("--mixes", "1w3r,3w1r"))) {
				for (const auto& r : lbwork::split(a.str("--rs", "1,4,8,16"))) { head(rq.run(unsigned(std::strtoul(r.c_str(), nullptr, 10)), d, m, D)); }
			}
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
