// The write buffer's step 1, item 4 (docs/design/write-buffer.md D5, D6, D7, D10; WB-21 to
// WB-23): V and view positions, live reads from other threads at r = max(V, view position),
// the docid walk and check, the first-docid-only skip, docid maps built under the lease only,
// and map tables retired by view generation. T8's schedules at one writer mirror the
// architect's model (`.scratch/rev3/docid_model.py` on design/write-modes).
#include "kv_test.hh"
#include "strata/core/write_buffer.hh"

#include <atomic>
#include <random>
#include <thread>

namespace strata {
struct index_test_access {
	static void freeze(kv_index& ix) {
		const lease_scope l{ix.lease_};
		ix.freeze();
	}
	static std::uint64_t first_docid_only(const kv_index& ix) { return ix.wb_ ? ix.wb_->first_docid_only.load() : kv_index::buffer::none; }
	// D6's check and the docid walk on the active buffer, for the segment copy of `key`.
	static std::pair<bool, bool> check(const kv_index& ix, std::string_view key, std::uint64_t r) {
		for (const auto& s : ix.segments()) {
			const auto& k = s.seg->get<keys_plugin>();
			if (const auto o = k.find(key, live_bits{nullptr}, xxh64::hash(key))) {
				return {kv_index::superseded(ix.wb_.get(), s, *o, r), kv_index::docid_at(*ix.wb_, s.seg->core().docid(*o), r)};
			}
		}
		return {false, false};
	}
	struct rec {
		bool flag;
		wbuf::ref_t prev, docid_prev;
	};
	// The newest record of a key in the active buffer: its docid prev flag and fields (WB-21).
	static rec newest(const kv_index& ix, std::string_view key) {
		const auto& x = ix.wb_->lane.at(kv_index::find_key_in(*ix.wb_, key));
		return {x.has_docid_prev(), x.prev, x.docid_prev};
	}
	static std::size_t tables_waiting(const kv_index& ix) { return ix.pub_ ? ix.pub_->tables.size() : 0; }
};
}  // namespace strata

namespace {

kv_index::options with(reader_registry& reg) {
	kv_index::options o;
	o.key_prefix = "k";
	o.readers = &reg;
	return o;
}
operation keyless(const std::string& body) {
	operation o;
	o.type = operation::add;
	o.doc = doc("", body);
	return o;
}
operation erase_docid(std::uint64_t d) {
	operation o;
	o.type = operation::delete_docid;
	o.docid = d;
	return o;
}
operation replace_docid(std::uint64_t d, const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::replace_docid;
	o.docid = d;
	o.doc = doc(key, body);
	return o;
}
bool ok(const verdict& v) { return v.result == verdict::accepted; }

using state = std::map<std::string, std::string>;
// Every key's value through a reading's gets, and through its scan.
template <class R>
state read_all(const R& at, const std::vector<std::string>& keys) {
	state out;
	for (const auto& k : keys) {
		if (auto v = at.get(k)) { out.emplace(k, *v); }
	}
	return out;
}
template <class R>
state scan_all(const R& at) {
	state out;
	at.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace(std::string(k), std::string(v)); });
	return out;
}

// T8's D6 schedule (I4-5, I4-7): a delete_docid and a key-changing replace_docid of keyed segment
// copies, a replace_docid writing the deleted docid again, and a key-changing replace_docid of a
// buffered document, each read by key from another thread at positions on both sides; then
// across a merge install carrying a pending delete, and after the freeze.
void d6_schedule() {
	reader_registry reg(32);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("d6"));
	CHECK(ok(ix.apply(tx({replace("k1", "a1"), replace("k2", "a2"), replace("k3", "a3")}), {})));  // docids 1-3
	ix.refresh();
	CHECK(ok(ix.apply(tx({replace("k4", "a4")}), {})));  // docid 4
	ix.refresh();
	const auto h = ix.reader();
	const std::vector<std::string> keys{"k1", "k2", "k3", "k4", "k5", "k6", "k7"};
	std::vector<std::pair<kv_index::reader_handle::live_view, state>> held;
	state want{{"k1", "a1"}, {"k2", "a2"}, {"k3", "a3"}, {"k4", "a4"}};
	const auto hold = [&] { held.emplace_back(h.live(), want); };
	hold();
	CHECK(ok(ix.apply(tx({erase_docid(1)}), {})));  // k1's segment copy, found by docid alone
	want.erase("k1");
	hold();
	CHECK(ok(ix.apply(tx({replace_docid(2, "k5", "b5")}), {})));  // k2's copy moves to k5
	want.erase("k2");
	want["k5"] = "b5";
	hold();
	CHECK(ok(ix.apply(tx({replace_docid(1, "k1", "c1")}), {})));  // the deleted docid again
	want["k1"] = "c1";
	hold();
	CHECK(ok(ix.apply(tx({replace_docid(2, "k6", "d6")}), {})));  // docid 2, buffered under k5, to k6
	want.erase("k5");
	want["k6"] = "d6";
	hold();
	// WB-21: the replace_docid record reserves the field and names the old key's tombstone.
	const auto x = index_test_access::newest(ix, "k6");
	CHECK(x.flag && x.docid_prev != wbuf::no_ref && x.docid_prev != x.prev);
	const auto y = index_test_access::newest(ix, "k1");  // k1 had no record; docid 1 its tombstone
	CHECK(y.flag && y.prev == wbuf::no_ref && y.docid_prev != wbuf::no_ref);
	CHECK(ok(ix.apply(tx({replace("k4", "b4")}), {})));
	CHECK(!index_test_access::newest(ix, "k4").flag);  // only a replace_docid reserves it
	CHECK(ok(ix.apply(tx({replace_docid(4, "k4", "c4")}), {})));
	const auto z = index_test_access::newest(ix, "k4");  // both predecessors are k4's record
	CHECK(z.flag && z.prev != wbuf::no_ref && z.docid_prev == wbuf::no_ref);
	want["k4"] = "c4";
	hold();
	// A merge carries the pending delete on k3's segment; new keys grow the key map, so a view
	// lists the merge's output while the delete is still pending.
	CHECK(ok(ix.apply(tx({erase_docid(3)}), {})));
	want.erase("k3");
	hold();
	ix.force_merge(1);
	for (int i = 0; i < 40; ++i) {
		CHECK(ok(ix.apply(tx({replace("k7", "e" + std::to_string(i))}), {})));
		CHECK(ok(ix.apply(tx({keyless("x" + std::to_string(i))}), {})));
	}
	want["k7"] = "e39";
	hold();
	CHECK(held.back().first.guard->segments().size() == 1);
	ix.refresh();  // the freeze applies the pending deletes; the flush installs
	hold();
	std::size_t wrong = 0, reads = 0;
	std::thread reader([&] {
		for (const auto& [live, expect] : held) {
			reads += 2;
			wrong += read_all(live.at, keys) != expect;
			wrong += scan_all(live.at) != expect;
		}
	});
	reader.join();
	CHECK(wrong == 0);
	std::printf("d6 schedule: %zu reads at %zu positions, %zu wrong\n", reads, held.size(), wrong);
}

// WB-22 (I4-7): the first docid-only supersession is set at its position and restored on a
// rejection; a reader below it skips the check, though the docid walk would find a record.
void first_docid_only() {
	reader_registry reg(8);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("fdo"));
	CHECK(ok(ix.apply(tx({replace("k1", "a1"), replace("k2", "a2")}), {})));  // docids 1, 2
	ix.refresh();
	const auto h = ix.reader();
	CHECK(index_test_access::first_docid_only(ix) == ~std::uint64_t(0));
	CHECK(ok(ix.apply(tx({replace("k1", "b1")}), {})));  // by key: docid 1 at position q
	const std::uint64_t q = h.visible();
	// A rejected transaction that found a segment copy by docid alone leaves no trace.
	CHECK(!ok(ix.apply(tx({erase_docid(2), add("k1", "dup")}), {})));
	CHECK(index_test_access::first_docid_only(ix) == ~std::uint64_t(0));
	CHECK(ok(ix.apply(tx({erase_docid(2)}), {})));  // builds the docid map; position p
	const std::uint64_t p = h.visible();
	CHECK(index_test_access::first_docid_only(ix) == p && p == q + 1);
	// Below p: docid 1's record at q is found by the walk, and the check is skipped.
	const auto [skip_gone, walk_found] = index_test_access::check(ix, "k1", q);
	CHECK(walk_found && !skip_gone);
	CHECK(index_test_access::check(ix, "k2", q).first == false);
	CHECK(index_test_access::check(ix, "k2", p).first == true);
	std::printf("first docid-only: q %llu, p %llu: ok\n", (unsigned long long)q, (unsigned long long)p);
}

// T8 at one writer with readers on other threads (I4-4, I4-8): a seeded schedule of keyed and
// keyless adds, replaces and deletes by key and by docid, rejections, freezes without a flush
// (a frozen buffer's docid map then built by stage 1), flushes, and merges. Each accepted
// transaction's state is recorded before V passes it; readers check every key and a full scan
// at their r against it.
struct model {
	std::map<std::uint64_t, std::pair<std::string, std::string>> docs;  // docid: key, value
	std::map<std::string, std::uint64_t> keys;
	std::uint64_t last = 0;
	// The sequential semantics (the model's `reference`): whether the operation applies.
	bool apply(const operation& o) {
		const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
		std::string key = o.type == operation::delete_key ? o.key : writes ? o.doc.key : std::string();
		std::optional<std::uint64_t> by_key, by_docid;
		if (!key.empty() && keys.count(key)) { by_key = keys[key]; }
		if (o.type == operation::replace_docid || o.type == operation::delete_docid) {
			if (docs.count(o.docid)) { by_docid = o.docid; }
			if (by_key && (!by_docid || *by_key != o.docid)) { return false; }
		}
		const auto target = by_key ? by_key : by_docid;
		if (o.type == operation::add && target) { return false; }
		const auto drop = [&](std::uint64_t d) {
			if (!docs[d].first.empty()) { keys.erase(docs[d].first); }
			docs.erase(d);
		};
		if (o.type == operation::delete_key || o.type == operation::delete_docid) {
			if (target) { drop(*target); }
			return true;
		}
		const std::uint64_t d = o.type == operation::replace_docid ? o.docid : target ? *target : last + 1;
		if (target) { drop(*target); }
		docs[d] = {key, std::string(static_cast<const stored_plugin::part&>(o.doc).stored)};
		if (!key.empty()) { keys[key] = d; }
		last = std::max(last, d);
		return true;
	}
	state snapshot() const {
		state s;
		for (const auto& [k, d] : keys) { s.emplace(k, docs.at(d).second); }
		return s;
	}
};

struct totals {
	std::uint64_t accepted = 0, rejected = 0, reads = 0, wrong = 0, verdicts = 0;
};
// Reads each held view at a position drawn from [its position, V when held], on another thread.
template <class Held>
void check_held(Held& held, const std::vector<state>& snaps, const std::vector<std::string>& keys, totals& t) {
	std::thread([&] {
		for (const auto& [live, r] : held) {
			const auto at = live.guard->at(r);
			t.wrong += read_all(at, keys) != snaps[at.position()];
			t.wrong += scan_all(at) != snaps[at.position()];
			t.reads += 2;
		}
	}).join();
	held.clear();
}

void model_check(unsigned seed, std::size_t steps, unsigned readers, totals& sum) {
	reader_registry reg(64);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("model"));
	const auto h = ix.reader();
	std::vector<std::string> keys;
	for (int i = 0; i < 6; ++i) { keys.push_back("k" + std::to_string(i)); }
	std::vector<state> snaps(steps + 1);  // by V; written before V passes it
	model m;
	std::mt19937 rng(seed);
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> wrong{0}, reads{0};
	std::vector<std::thread> threads;
	for (unsigned t = 0; t < readers; ++t) {
		threads.emplace_back([&, t] {
			std::mt19937 rr(seed * 31 + t);
			while (!done.load(std::memory_order_relaxed)) {
				const auto live = h.live();
				const state& want = snaps[live.at.position()];
				const bool bad = (rr() & 1) ? read_all(live.at, keys) != want : scan_all(live.at) != want;
				wrong += bad;
				reads += 1;
				if (bad && wrong == 1) { std::fprintf(stderr, "seed %u: wrong read at r %llu\n", seed, (unsigned long long)live.at.position()); }
			}
		});
	}
	std::size_t accepted = 0, rejected = 0, mismatched = 0;
	totals held_t;
	std::vector<std::pair<kv_index::reader_handle::live_view, std::uint64_t>> held;
	const auto val = [&] { return "v" + std::to_string(rng() % 1000); };
	for (std::size_t step = 0; step < steps; ++step) {
		if (rng() % 3 == 0) {  // a reader's view held across later groups (the model's reads)
			auto live = h.live();
			const std::uint64_t lo = live.guard->visible(), hi = live.at.position();
			held.emplace_back(std::move(live), lo + rng() % (hi - lo + 1));
			if (held.size() >= 48) { check_held(held, snaps, keys, held_t); }
		}
		const unsigned c = rng() % 100;
		if (c < 4) {
			ix.refresh();
			continue;
		}
		if (c < 8) {
			index_test_access::freeze(ix);
			continue;
		}
		if (c < 10) {
			ix.force_merge(1);
			continue;
		}
		const std::string& k = keys[rng() % keys.size()];
		const std::uint64_t d = 1 + rng() % (m.last + 2);
		operation o;
		switch (rng() % 7) {
		case 0: o = add(k, val()); break;
		case 1: o = replace(k, val()); break;
		case 2: o = erase(k); break;
		case 3: o = replace_docid(d, (rng() % 4) ? k : std::string(), val()); break;
		case 4: o = erase_docid(d); break;
		case 5: o = keyless(val()); break;
		default: o = replace_docid(d, k, val()); break;
		}
		model next = m;
		const bool want = next.apply(o);
		const std::uint64_t v = h.visible();
		if (want) { snaps[v + 1] = next.snapshot(); }
		const bool got = ok(ix.apply(tx({o}), {}));
		if (got != want) {
			++mismatched;
			if (mismatched == 1) { std::fprintf(stderr, "seed %u step %zu: verdict %d, model %d (op %d)\n", seed, step, got, want, int(o.type)); }
		}
		if (got && want) {
			m = std::move(next);
			++accepted;
		} else {
			++rejected;
		}
	}
	done = true;
	for (auto& t : threads) { t.join(); }
	check_held(held, snaps, keys, held_t);
	// The lease holder's own reads agree at the end.
	CHECK(read_all(ix, keys) == m.snapshot());
	CHECK(wrong == 0 && held_t.wrong == 0 && mismatched == 0);
	if (wrong || held_t.wrong || mismatched) { std::printf("model check seed %u: %llu wrong reads, %zu verdicts wrong\n", seed, (unsigned long long)(wrong + held_t.wrong), mismatched); }
	sum.accepted += accepted;
	sum.rejected += rejected;
	sum.reads += reads + held_t.reads;
	sum.wrong += wrong + held_t.wrong;
	sum.verdicts += mismatched;
}

// WB-23 and D10 under readers (I4-4): first builds and growths of the active buffer's docid map,
// and key-map growths, while readers run the docid check on every get; the old tables wait for
// the readers' views and are freed after them.
void growth_under_readers() {
	reader_registry reg(16);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("growth"));
	CHECK(ok(ix.apply(tx({replace("k0", "s0"), replace("k1", "s1")}), {})));  // docids 1, 2
	ix.refresh();
	const auto h = ix.reader();
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> wrong{0}, reads{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&] {
			while (!done.load(std::memory_order_relaxed)) {
				const auto live = h.live();
				const auto k0 = live.at.get("k0"), k1 = live.at.get("k1");
				wrong += k1 != std::optional<std::string>("s1") || (k0 && *k0 != "s0");
				reads += 2;
			}
		});
	}
	std::size_t rounds = 0;
	for (int round = 0; round < 40; ++round, ++rounds) {
		// k0's segment copy deleted by docid and written back: the check runs on every get.
		CHECK(ok(ix.apply(tx({erase_docid(1)}), {})));
		CHECK(ok(ix.apply(tx({replace_docid(1, "k0", "s0")}), {})));
		for (int i = 0; i < 300; ++i) {
			CHECK(ok(ix.apply(tx({keyless(std::to_string(i)), replace("kx" + std::to_string(i), "v")}), {})));
		}
		if (round % 3 == 0) { index_test_access::freeze(ix); }
		if (round % 4 == 0) { ix.refresh(); }
	}
	done = true;
	for (auto& t : threads) { t.join(); }
	ix.commit();
	CHECK(wrong == 0 && *ix.get("k1") == "s1");
	CHECK(index_test_access::tables_waiting(ix) == 0);
	std::printf("growth under readers: %zu rounds, %llu reads, %llu wrong\n", rounds, (unsigned long long)reads.load(), (unsigned long long)wrong.load());
}

}  // namespace

int main() {
	d6_schedule();
	first_docid_only();
	const auto run = [](const char* what, unsigned from, unsigned to, unsigned readers) {
		totals t;
		for (unsigned seed = from; seed < to; ++seed) { model_check(seed, 400, readers, t); }
		std::printf("model check, %s: seeds %u-%u, %llu accepted, %llu rejected, %llu reads, %llu wrong, %llu verdicts wrong\n", what, from, to - 1,
		            (unsigned long long)t.accepted, (unsigned long long)t.rejected, (unsigned long long)t.reads, (unsigned long long)t.wrong,
		            (unsigned long long)t.verdicts);
	};
	run("held views", 0, 200, 0);
	run("3 reader threads", 1000, 1008, 3);
	growth_under_readers();
	if (failures) {
		std::printf("write_buffer_reads_test: %d failures\n", failures);
		return 1;
	}
	std::printf("write_buffer_reads_test: ok\n");
	return 0;
}
