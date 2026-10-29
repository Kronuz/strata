// basic_index::scan (format-14-kv D3): the key-ordered scan over the buffer and the segments.
// A seeded workload of puts, deletes, flushes, and merges against a std::map, scanning from every
// key and between keys; then the behaviours one by one: newest copy wins, deletes hide older
// copies, the snapshot rule (a writer call from the callback throws and changes nothing),
// bounds, limits, and the boundaries of blocks and restarts.
#include "kv_test.hh"

#include <random>

namespace strata {
// Breaks invariants no public call can: a second live copy of a key, in a segment or a buffer.
struct index_test_access {
	static void revive_segment_copy(kv_index& ix, std::size_t segment, const std::string& key) {
		auto& s = *ix.segs_[segment];
		const auto ord = s.seg().get<keys_plugin>().find(key, live_bits{});
		CHECK(ord.has_value());
		s.deleted.reset(*ord);
		--s.dead;
	}
	// The buffer marks nothing dead: an older record stays in its runs, shadowed by the newer
	// one (write-buffer D6). This checks that the nth record of the key is there.
	static void revive_buffered_copy(kv_index& ix, const std::string& key, std::size_t nth) {
		const auto& b = *ix.wb_;
		for (std::size_t i = 0; i < b.order.size(); ++i) {
			if (b.lane.at(b.order[i].ref).doc().key == key && nth-- == 0) { return; }
		}
		CHECK(false);
	}
	// The entries in the active buffer's run levels (write-buffer D9).
	static std::size_t buffered_run_entries(const kv_index& ix) {
		const wbuf::run_set* rs = ix.wb_ ? ix.wb_->runs.load() : nullptr;
		std::size_t n = 0;
		for (std::size_t i = 0; rs && i < rs->levels.size(); ++i) { n += rs->levels[i]->size(); }
		return n;
	}
};
}  // namespace strata

// scan is offered to this layout (CR3's constraint: keys, stored, no inverter).
static_assert(requires(const kv_index& ix) { ix.scan(std::string_view(), [](std::string_view, std::string_view, std::uint64_t) {}); });

struct row {
	std::string key, value;
	std::uint64_t version;
	bool operator==(const row&) const = default;
};
static row R(std::string k, std::string v, std::uint64_t version) { return {std::move(k), std::move(v), version}; }
using model_t = std::map<std::string, std::pair<std::uint64_t, std::string>>;  // key -> (version, value)

static std::string key_of(std::uint64_t n) {
	char b[16];
	std::snprintf(b, sizeof b, "Q%06llu", (unsigned long long)n);
	return b;
}

// The helpers' n == 0 is the unlimited form, as the model's; scan's own n == 0 visits nothing.
static std::vector<row> run(const kv_index& ix, const std::string& from, std::size_t n) {
	std::vector<row> out;
	const auto f = [&](std::string_view k, std::string_view v, std::uint64_t version) {
		out.push_back({std::string(k), std::string(v), version});
		return true;
	};
	const std::size_t got = n ? ix.scan(from, n, f) : ix.scan(from, f);
	CHECK(got == out.size());
	return out;
}
static std::vector<row> run_to(const kv_index& ix, const std::string& from, const std::string& to, std::size_t n) {
	std::vector<row> out;
	const auto f = [&](std::string_view k, std::string_view v, std::uint64_t version) {
		out.push_back({std::string(k), std::string(v), version});
		return true;
	};
	const std::size_t got = n ? ix.scan(from, to, n, f) : ix.scan(from, to, f);
	CHECK(got == out.size());
	return out;
}
static std::vector<row> expect(const model_t& m, const std::string& from, std::size_t n, const std::string* to = nullptr) {
	std::vector<row> out;
	for (auto it = m.lower_bound(from); it != m.end() && (!n || out.size() < n); ++it) {
		if (to && it->first >= *to) { break; }
		out.push_back({it->first, it->second.second, it->second.first});
	}
	return out;
}

static const std::array<char, 16> uuid = {'s', 'c', 'a', 'n', '-', 't', 'e', 's', 't'};

struct store {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	kv_index ix;
	model_t model;
	explicit store(std::size_t flush_threads = 1) : ix(kv_index::create(io, options(flush_threads), uuid)) {}
	static kv_index::options options(std::size_t flush_threads) {
		kv_index::options o;
		o.keyed = true;
		o.flush_threads = flush_threads;
		return o;
	}
	void put(const std::string& key, const std::string& value) {
		CHECK(ix.apply(tx({replace(key, value)}), {}).result == verdict::accepted);
		model[key] = {model.count(key) ? model[key].first + 1 : 1, value};
	}
	void del(const std::string& key) {
		CHECK(ix.apply(tx({erase(key)}), {}).result == verdict::accepted);
		model.erase(key);
	}
	void check(const char* what) {
		const std::size_t before = failures;
		for (const std::string from : {std::string(), std::string("Q"), key_of(0), key_of(50), key_of(1000000)}) {
			for (const std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(7)}) {
				CHECK(run(ix, from, n) == expect(model, from, n));
			}
		}
		if (failures != int(before)) { std::fprintf(stderr, "  in %s\n", what); }
	}
};

// Random puts and deletes with flushes and merges between, every scan equal to the map's.
static void against_the_map() {
	for (std::uint64_t seed = 1; seed <= 6; ++seed) {
		store s(seed % 3 ? 1 : 2);
		std::mt19937_64 rng(seed);
		for (int step = 0; step < 600; ++step) {
			const std::string key = key_of(rng() % 120);
			switch (rng() % 10) {
			case 0: case 1: case 2: case 3: case 4: s.put(key, std::string(1 + rng() % 200, char('a' + rng() % 26))); break;
			case 5: case 6: s.del(key); break;
			case 7: s.ix.refresh(); break;
			case 8:
				if (rng() % 4 == 0) { s.ix.force_merge(1 + rng() % 3); }
				break;
			default: {
				const std::string from = key_of(rng() % 130);
				const std::size_t n = rng() % 12;
				CHECK(run(s.ix, from, n) == expect(s.model, from, n));
				break;
			}
			}
			if (step % 50 == 0) { s.check("random"); }
		}
		s.check("random end");
		s.ix.commit();
		s.ix.close();
		s.ix = kv_index::open(s.io, store::options(1));
		for (const std::string from : {std::string(), key_of(60)}) { CHECK(run(s.ix, from, 0) == expect(s.model, from, 0)); }
	}
}

static void empty_index() {
	store s;
	CHECK(run(s.ix, "", 0).empty());
	CHECK(run(s.ix, "Q", 1).empty());
	CHECK(run_to(s.ix, "", "z", 0).empty());
	s.put("Qa", "1");
	s.del("Qa");
	CHECK(run(s.ix, "", 0).empty());  // buffered and dead
	s.put("Qa", "1");
	s.ix.refresh();
	s.del("Qa");
	CHECK(run(s.ix, "", 0).empty());  // in a segment and dead
	s.ix.refresh();
	CHECK(run(s.ix, "", 0).empty());
}

// Limits, bounds, and the callback's say.
static void limits_and_bounds() {
	store s;
	for (int i = 0; i < 10; ++i) { s.put(key_of(i * 2), "v" + std::to_string(i)); }
	s.ix.refresh();
	for (int i = 10; i < 14; ++i) { s.put(key_of(i * 2), "v" + std::to_string(i)); }  // the rest buffered
	CHECK(run(s.ix, "", 0).size() == 14);  // the forms without n run to the end
	// n == 0 visits nothing, whatever the range.
	std::size_t visited = 0;
	const auto count = [&](std::string_view, std::string_view, std::uint64_t) { ++visited; };
	CHECK(s.ix.scan("", std::size_t(0), count) == 0 && s.ix.scan("", "z", std::size_t(0), count) == 0 && visited == 0);
	CHECK(run(s.ix, "", 1).size() == 1 && run(s.ix, "", 1)[0].key == key_of(0));
	CHECK(run(s.ix, "", 14).size() == 14 && run(s.ix, "", 15).size() == 14);
	CHECK(run(s.ix, key_of(5), 2) == expect(s.model, key_of(5), 2));  // between keys
	CHECK(run(s.ix, key_of(6), 1)[0].key == key_of(6));               // on a key
	CHECK(run(s.ix, key_of(26), 0) == expect(s.model, key_of(26), 0) && run(s.ix, key_of(26), 0).size() == 1);  // the last, buffered
	CHECK(run(s.ix, key_of(27), 0).empty());                          // past the last key
	CHECK(run(s.ix, "R", 0).empty());                                 // past every prefix
	CHECK(run_to(s.ix, key_of(4), key_of(10), 0).size() == 3);        // [4, 10): 4, 6, 8
	CHECK(run_to(s.ix, key_of(4), key_of(9), 0).size() == 3);
	CHECK(run_to(s.ix, key_of(4), key_of(4), 0).empty());             // empty range
	CHECK(run_to(s.ix, key_of(8), key_of(4), 0).empty());             // inverted
	CHECK(run_to(s.ix, key_of(4), key_of(10), 2).size() == 2);        // limit inside the range
	CHECK(run_to(s.ix, "", "z", 0).size() == 14);
	// f stops the scan by returning false; the call that did counts.
	std::size_t seen = 0;
	CHECK(s.ix.scan("", [&](std::string_view, std::string_view, std::uint64_t) { return ++seen < 5; }) == 5 && seen == 5);
	// A void f runs to the end.
	seen = 0;
	CHECK(s.ix.scan("", [&](std::string_view, std::string_view, std::uint64_t) { ++seen; }) == 14 && seen == 14);
}

// The newest copy of a key wins whichever source it is in, and a delete hides the older copies.
static void newest_wins() {
	store s;
	s.put("Qk", "one");
	s.ix.refresh();
	s.put("Qk", "two");
	s.ix.refresh();
	s.put("Qk", "three");  // the buffer, over two segments
	s.put("Qj", "j");
	s.put("Ql", "l");
	CHECK(s.ix.segments().size() == 2);
	auto got = run(s.ix, "", 0);
	CHECK(got.size() == 3 && got[1] == R("Qk", "three", 3));
	s.ix.refresh();
	got = run(s.ix, "", 0);
	CHECK(got.size() == 3 && got[1] == R("Qk", "three", 3));
	// A delete in the buffer of a key live in a segment hides it.
	s.del("Qk");
	got = run(s.ix, "", 0);
	CHECK(got.size() == 2 && got[0].key == "Qj" && got[1].key == "Ql");
	CHECK(run(s.ix, "Qk", 0).size() == 1);
	// A replace over a segment's copy, then a delete of the replacement.
	s.put("Qj", "j2");
	CHECK(run(s.ix, "", 1)[0] == R("Qj", "j2", 2));
	s.del("Qj");
	got = run(s.ix, "", 0);
	CHECK(got.size() == 1 && got[0].key == "Ql");
	// After merging the dead copies away nothing reappears.
	s.ix.refresh();
	s.ix.force_merge(1);
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
}

// The tie rule, with the invariant (one live copy per key) broken by test access: the buffer's
// copy beats a segment's, a newer segment's an older one's, and every copy is passed.
static void ties() {
	{
		store s;
		s.put("Qa", "a");
		s.put("Qk", "old");
		s.ix.refresh();
		s.put("Qk", "mid");
		s.put("Qz", "z");
		s.ix.refresh();
		index_test_access::revive_segment_copy(s.ix, 0, "Qk");  // live in both segments
		const auto got = run(s.ix, "", 0);
		CHECK(got.size() == 3 && got[0].key == "Qa" && got[1] == R("Qk", "mid", 2) && got[2].key == "Qz");
		s.put("Qk", "new");  // the buffer's copy too: it beats both
		index_test_access::revive_segment_copy(s.ix, 1, "Qk");
		const auto top = run(s.ix, "", 0);
		CHECK(top.size() == 3 && top[1] == R("Qk", "new", 3));
		CHECK(run(s.ix, "Qk", 1)[0].value == "new" && run(s.ix, "Ql", 1)[0].key == "Qz");
		std::size_t calls = 0;
		s.ix.scan("", [&](std::string_view, std::string_view, std::uint64_t) { return ++calls, true; });
		CHECK(calls == 3);
	}
	{  // a run level and the private tail both hold the key: the later record wins
		store s;
		for (int i = 0; i < 5000; ++i) { s.put(key_of(i), "v" + std::to_string(i)); }
		CHECK(run(s.ix, "", 1).size() == 1);  // builds a level of 5000 (D9)
		CHECK(index_test_access::buffered_run_entries(s.ix) == 5000);
		s.put(key_of(77), "again");
		CHECK(run(s.ix, key_of(77), 1)[0].value == "again");  // in the tail, the old copy in the level
		index_test_access::revive_buffered_copy(s.ix, key_of(77), 0);
		const auto got = run(s.ix, key_of(76), 3);
		CHECK(got.size() == 3 && got[1] == R(key_of(77), "again", 2) && got[2].key == key_of(78));
	}
}

// The snapshot rule: the scan sees the index as it began, a writer call from f throws and
// changes nothing, a get from f works, and the next scan sees what came between.
static void snapshot() {
	store s;
	for (int i = 0; i < 6; ++i) { s.put(key_of(i * 10), "v" + std::to_string(i)); }
	s.ix.refresh();
	s.put(key_of(55), "late");
	const auto before = run(s.ix, "", 0);
	const std::uint64_t digest = s.ix.digest();
	std::size_t calls = 0, throws_seen = 0;
	std::vector<row> during;
	s.ix.scan("", [&](std::string_view k, std::string_view v, std::uint64_t version) {
		during.push_back({std::string(k), std::string(v), version});
		if (++calls == 2) {  // a mid-scan insert, update, delete, flush, merge, commit
			throws_seen += throws([&] { s.ix.apply(tx({replace(key_of(15), "new")}), {}); });
			throws_seen += throws([&] { s.ix.apply(tx({replace(key_of(30), "changed")}), {}); });
			throws_seen += throws([&] { s.ix.apply(tx({erase(key_of(40))}), {}); });
			throws_seen += throws([&] { s.ix.refresh(); });
			throws_seen += throws([&] { s.ix.force_merge(); });
			throws_seen += throws([&] { s.ix.commit(); });
			throws_seen += throws([&] { s.ix.wait_merges(); });
			throws_seen += throws([&] { s.ix.close(); });
			throws_seen += throws([&] { s.ix.checkpoint(); });
			throws_seen += throws([&] { s.ix.reclaim(); });
			throws_seen += throws([&] { s.ix.heal(); });
			throws_seen += throws([&] { s.ix.maybe_merge(); });
			throws_seen += throws([&] { s.ix.force_merge_deletes(); });
			throws_seen += throws([&] { s.ix.merge({}); });
			std::string got;
			CHECK(s.ix.get(key_of(30), [&](std::string_view x) { got = x; }) && got == "v3");
			CHECK(run(s.ix, key_of(30), 1)[0].value == "v3");  // a scan inside a scan
		}
		return true;
	});
	CHECK(throws_seen == 14);
	CHECK(during == before);
	CHECK(s.ix.digest() == digest && run(s.ix, "", 0) == before);
	// An exception from f leaves the index usable.
	CHECK(throws([&] { s.ix.scan("", [&](std::string_view, std::string_view, std::uint64_t) -> bool { throw std::runtime_error("f"); }); }));
	// Between scans, inserts, updates, and deletes are seen.
	s.put(key_of(15), "inserted");
	s.put(key_of(30), "updated");
	s.del(key_of(40));
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
	CHECK(run(s.ix, key_of(15), 1)[0] == R(key_of(15), "inserted", 1));
	CHECK(run(s.ix, key_of(30), 1)[0] == R(key_of(30), "updated", 2));
	s.ix.refresh();
	s.ix.force_merge(1);
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
}

// Every key of a multi-block segment, and the places between, as the scan's start, with the
// records after it also in a second segment and the buffer: block and restart boundaries fall
// wherever the formats put them.
static void boundaries() {
	store s;
	for (int i = 0; i < 3000; ++i) { s.put(key_of(i * 3), std::string(60 + i % 90, char('a' + i % 26))); }
	s.ix.refresh();
	for (int i = 0; i < 400; ++i) { s.put(key_of(i * 7 + 1), "second" + std::to_string(i)); }
	s.ix.refresh();
	for (int i = 0; i < 100; ++i) { s.put(key_of(i * 90 + 2), "buffer" + std::to_string(i)); }
	CHECK(s.ix.segments().size() == 2 && s.ix.buffered_count() == 100);
	for (std::uint64_t n = 0; n < 9010; ++n) {
		const std::string from = key_of(n);
		CHECK(run(s.ix, from, 4) == expect(s.model, from, 4));
	}
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
	const std::string a = key_of(2999), b = key_of(5001);
	CHECK(run_to(s.ix, a, b, 0) == expect(s.model, a, 0, &b));
}

// Hundreds of records appended between scans, replaced and deleted keys among them: the runs
// build, merge as a binary counter, keep superseded records, and every scan equals the map's.
static void buffer_view() {
	store s;
	std::mt19937_64 rng(7);
	for (int round = 0; round < 12; ++round) {
		const int puts = round % 3 == 2 ? 1 : 900;
		for (int i = 0; i < puts; ++i) {
			const std::string key = key_of(rng() % 2500);
			if (rng() % 5 == 0) {
				s.del(key);
			} else {
				s.put(key, std::string(1 + rng() % 40, char('a' + rng() % 26)));
			}
		}
		CHECK(s.ix.segments().empty());
		CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
		CHECK(run(s.ix, key_of(1234), 5) == expect(s.model, key_of(1234), 5));
	}
	s.ix.refresh();  // the view empties with the buffer
	for (int i = 0; i < 20; ++i) { s.put(key_of(5000 + i), "x"); }
	CHECK(run(s.ix, "", 0).size() == s.model.size());  // a view of 20 records
	s.ix.refresh();
	for (int i = 0; i < 30; ++i) { s.put(key_of(6000 + i), "y"); }  // more than the view covered
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
	s.ix.refresh();
	CHECK(index_test_access::buffered_run_entries(s.ix) == 0);
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
	s.put(key_of(3), "after");
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
}

// A rejected transaction leaves no trace in the scan.
static void rejected() {
	store s;
	s.put("Qa", "a");
	CHECK(run(s.ix, "", 0).size() == 1);
	kv_index::operation bad = replace("Qb", "b", 9);  // a version that does not match
	CHECK(s.ix.apply(tx({replace("Qc", "c"), bad}), {}).result != verdict::accepted);
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
	s.put("Qc", "c");
	CHECK(run(s.ix, "", 0) == expect(s.model, "", 0));
}

int main() {
	against_the_map();
	empty_index();
	limits_and_bounds();
	newest_wins();
	ties();
	snapshot();
	boundaries();
	buffer_view();
	rejected();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("scan: ok");
	return 0;
}
