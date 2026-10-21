// The write buffer's two maps (key -> buffer index, docid -> buffer index) in 8-byte slots: a
// 32-bit hash beside a 32-bit value, up to 7/8 full. A 32-bit hash does not identify a key or a
// docid, so these tests force hash collisions in the table and in the index, and walk the
// 7/8 growth boundary and the buffer's entry cap.
#include "kv_test.hh"

#include <cstdio>
#include <map>
#include <random>
#include <vector>

using slots = strata::index_detail::hash_slots;

// Entries named by an id the table does not store: the test of the value compares ids.
struct ids {
	std::vector<std::uint64_t> of;  // value -> id
	std::uint32_t add(std::uint64_t id) {
		of.push_back(id);
		return std::uint32_t(of.size() - 1);
	}
	auto is(std::uint64_t id) const {
		return [this, id](std::uint32_t v) { return v < of.size() && of[v] == id; };
	}
};

static void table_collisions() {
	// 300 ids over three hashes: every probe sequence crosses entries with the same hash.
	slots t;
	ids n;
	std::map<std::uint64_t, std::uint32_t> model;
	const std::uint32_t hashes[3] = {0, 0xDEADBEEFu, 0xFFFFFFFFu};
	auto h_of = [&](std::uint64_t id) { return hashes[id % 3]; };
	for (std::uint64_t id = 0; id < 300; ++id) {
		CHECK(!t.find(h_of(id), n.is(id)));  // a hash match alone is not a hit
		model[id] = n.add(id);
		t.insert(h_of(id), model[id]);
	}
	for (std::uint64_t id = 0; id < 300; id += 2) {
		CHECK(t.erase(h_of(id), n.is(id)));
		CHECK(!t.erase(h_of(id), n.is(id)));
		model.erase(id);
	}
	for (std::uint64_t id = 0; id < 300; ++id) {
		const std::uint32_t* v = t.find(h_of(id), n.is(id));
		CHECK((v != nullptr) == model.count(id));
		if (v) { CHECK(*v == model[id]); }
	}
	CHECK(t.size() == 150);
}

static void table_random() {
	// Against std::map, with hashes from a handful of values up to all distinct, across growth
	// and the removals' shifts, including the wrap past the last slot.
	for (std::uint32_t spread : {1u, 5u, 97u, 0xFFFFFFFFu}) {
		std::mt19937_64 r(spread);
		slots t;
		ids n;
		std::map<std::uint64_t, std::uint32_t> model;
		auto h_of = [&](std::uint64_t id) { return spread == 0xFFFFFFFFu ? std::uint32_t((id * 0x9E3779B97F4A7C15ull) >> 32) : std::uint32_t(id % spread) * 0x1000193u; };
		for (int step = 0; step < 30000; ++step) {
			const std::uint64_t id = r() % 2500;
			const bool there = model.count(id) != 0;
			const std::uint32_t* v = t.find(h_of(id), n.is(id));
			CHECK((v != nullptr) == there);
			if (v) { CHECK(*v == model[id]); }
			if (r() % 3 == 0) {
				CHECK(t.erase(h_of(id), n.is(id)) == there);
				model.erase(id);
			} else if (!there) {
				model[id] = n.add(id);
				t.insert(h_of(id), model[id]);
			}
			CHECK(t.size() == model.size());
		}
		for (const auto& [id, v] : model) {
			const std::uint32_t* f = t.find(h_of(id), n.is(id));
			CHECK(f && *f == v);
		}
	}
}

static void table_growth_boundary() {
	// 16 slots hold 14 entries; the 15th doubles the table, as does the 29th (32 slots hold 28).
	slots t;
	ids n;
	CHECK(t.bytes() == 0);
	std::uint64_t next = 0;
	auto put = [&] {
		const std::uint64_t id = next++;
		t.insert(std::uint32_t(id * 0x9E3779B1u), n.add(id));
	};
	put();
	CHECK(t.bytes() == 16 * 8);
	while (t.size() < 14) { put(); }
	CHECK(t.bytes() == 16 * 8);
	put();
	CHECK(t.bytes() == 32 * 8);
	while (t.size() < 28) { put(); }
	CHECK(t.bytes() == 32 * 8);
	put();
	CHECK(t.bytes() == 64 * 8);
	for (std::uint64_t id = 0; id < next; ++id) { CHECK(t.find(std::uint32_t(id * 0x9E3779B1u), n.is(id))); }
	// Removals keep the table; a full table (the 28th entry) still ends every probe.
	slots full;
	ids m;
	for (std::uint64_t id = 0; id < 14; ++id) { full.insert(0, m.add(id)); }  // one cluster, 14 of 16
	CHECK(full.bytes() == 16 * 8);
	CHECK(!full.find(0, m.is(99)));
	CHECK(!full.find(1, m.is(99)));
	// The largest value a slot holds, and the smallest and largest hashes.
	slots e;
	ids big;
	big.of.assign(std::size_t(1) << 4, 0);
	e.insert(0, 0xFFFFFFFEu);
	e.insert(0xFFFFFFFFu, 3);
	std::uint32_t* a = e.find(0, [](std::uint32_t v) { return v == 0xFFFFFFFEu; });
	CHECK(a && *a == 0xFFFFFFFEu);
	CHECK(e.erase(0, [](std::uint32_t v) { return v == 0xFFFFFFFEu; }) && e.size() == 1);
}

static std::uint32_t key_hash_of(const std::string& k) { return std::uint32_t(::wordwise::hash(k.data(), k.size()) >> 32); }
static std::uint32_t docid_hash_of(std::uint64_t d) { return std::uint32_t(d ^ (d >> 32)); }

static operation replace_docid(std::uint64_t docid, const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::replace_docid;
	o.docid = docid;
	o.doc = doc(key, body);
	return o;
}
static operation erase_docid(std::uint64_t docid) {
	operation o;
	o.type = operation::delete_docid;
	o.docid = docid;
	return o;
}

static void index_key_collisions() {
	// Keys whose 32-bit hashes are equal (found by search over "Q<n>"): the buffered version map
	// must tell them apart by the key.
	const std::string a = "Q5698", b = "Q814401", c = "Q5697", d = "Q814501";
	CHECK(key_hash_of(a) == key_hash_of(b) && key_hash_of(c) == key_hash_of(d) && key_hash_of(a) != key_hash_of(c));
	kv_index::options keyed;
	keyed.key_prefix = "Q";
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, keyed, uuid_of("slots-keys"));
	CHECK(ix.apply(tx({add(a, "A1")}), {}).result == verdict::accepted);
	CHECK(!ix.find_key(b) && !ix.get(b) && ix.get(a) == "A1");  // b shares a's hash and is absent
	CHECK(ix.apply(tx({add(b, "B1"), add(c, "C1")}), {}).result == verdict::accepted);
	CHECK(ix.get(a) == "A1" && ix.get(b) == "B1" && ix.get(c) == "C1" && !ix.get(d));
	CHECK(ix.find_key(a)->second == 1 && ix.find_key(b)->second == 1 && ix.find_key(a)->first != ix.find_key(b)->first);
	// A replace of one leaves the other; a delete of one leaves the other and its version.
	CHECK(ix.apply(tx({replace(a, "A2", 1)}), {}).result == verdict::accepted);
	CHECK(ix.get(a) == "A2" && ix.get(b) == "B1" && ix.find_key(b)->second == 1);
	CHECK(ix.apply(tx({replace(b, "B-wrong-version", 7)}), {}).result == verdict::version_conflict);
	CHECK(ix.apply(tx({erase(a)}), {}).result == verdict::accepted);
	CHECK(!ix.get(a) && !ix.find_key(a) && ix.get(b) == "B1");
	// A transaction rejected after it changed b's entry rolls back to the old table.
	CHECK(ix.apply(tx({replace(b, "B2"), erase(d), add(c, "C-dup")}), {}).result == verdict::version_conflict);
	CHECK(ix.get(b) == "B1" && ix.find_key(b)->second == 1 && ix.get(c) == "C1" && !ix.get(a) && !ix.get(d));
	CHECK(ix.apply(tx({add(a, "A3"), add(d, "D1"), erase(b)}), {}).result == verdict::accepted);
	CHECK(ix.get(a) == "A3" && ix.get(c) == "C1" && ix.get(d) == "D1" && !ix.get(b));
	// The same after a flush and a reopen.
	ix.commit();
	ix.close();
	ix = kv_index::open(io, keyed);
	CHECK(ix.get(a) == "A3" && ix.get(c) == "C1" && ix.get(d) == "D1" && !ix.get(b));
}

static void index_docid_collisions() {
	// Docids whose folded halves are equal: 7, 2^32 + 6, and 2 * 2^32 + 5.
	const std::uint64_t x = 7, y = (std::uint64_t(1) << 32) | 6, z = (std::uint64_t(2) << 32) | 5;
	CHECK(docid_hash_of(x) == docid_hash_of(y) && docid_hash_of(y) == docid_hash_of(z));
	kv_index::options keyed;
	keyed.key_prefix = "Q";
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, keyed, uuid_of("slots-docs"));
	CHECK(ix.apply(tx({replace_docid(x, "", "X1")}), {}).result == verdict::accepted);
	CHECK(ix.has_docid(x) && !ix.has_docid(y) && !ix.has_docid(z));  // y shares x's hash and is absent
	CHECK(ix.apply(tx({replace_docid(y, "", "Y1"), replace_docid(z, "", "Z1")}), {}).result == verdict::accepted);
	CHECK(ix.has_docid(x) && ix.has_docid(y) && ix.has_docid(z));
	// A delete or a replace of one leaves the others.
	CHECK(ix.apply(tx({erase_docid(y)}), {}).result == verdict::accepted);
	CHECK(ix.has_docid(x) && !ix.has_docid(y) && ix.has_docid(z));
	CHECK(ix.apply(tx({replace_docid(x, "", "X2")}), {}).result == verdict::accepted);
	CHECK(ix.has_docid(x) && !ix.has_docid(y) && ix.has_docid(z));
	// A rejection after the buffer pushed y again and erased z unwinds both.
	CHECK(ix.apply(tx({replace_docid(y, "", "Y2"), erase_docid(z), replace_docid(9, "Qx", "no-version"), add("Qk", "k"), add("Qk", "k-again")}), {}).result == verdict::version_conflict);
	CHECK(ix.has_docid(x) && !ix.has_docid(y) && ix.has_docid(z) && !ix.has_docid(9));
	CHECK(ix.apply(tx({replace_docid(y, "", "Y3")}), {}).result == verdict::accepted);
	ix.commit();
	const auto c = contents(ix);
	CHECK(c.size() == 3 && c.at(x).second == "X2" && c.at(y).second == "Y3" && c.at(z).second == "Z1");
}

static void index_random_collisions() {
	// Random operations on keys and docids drawn from colliding families against a model;
	// every step checks every name, buffered, then flushed.
	const std::vector<std::string> keys = {"Q5698", "Q814401", "Q5697", "Q814501", "Q5699", "Q814701", "Qplain", "Qother"};
	const std::vector<std::uint64_t> docids = {7, (std::uint64_t(1) << 32) | 6, (std::uint64_t(2) << 32) | 5, 8, (std::uint64_t(1) << 32) | 7, 1000};
	for (std::uint64_t seed = 1; seed <= 4; ++seed) {
		kv_index::options keyed;
		keyed.key_prefix = "Q";
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = kv_index::create(io, keyed, uuid_of("slots-random"));
		std::mt19937_64 r(seed);
		std::map<std::string, std::string> by_key;
		std::map<std::uint64_t, std::string> by_docid;  // keyless documents only
		for (int step = 0; step < 600; ++step) {
			const std::string& k = keys[r() % keys.size()];
			const std::uint64_t d = docids[r() % docids.size()];
			const std::string body = "v" + std::to_string(step);
			switch (r() % 5) {
				case 0: CHECK(ix.apply(tx({replace(k, body)}), {}).result == verdict::accepted); by_key[k] = body; break;
				case 1: CHECK(ix.apply(tx({erase(k)}), {}).result == verdict::accepted); by_key.erase(k); break;
				case 2: {
					// A keyless document at a docid no keyed document holds.
					bool taken = false;
					for (const auto& [kk, bb] : by_key) { taken = taken || (ix.find_key(kk) && ix.find_key(kk)->first == d); }
					if (!taken) { CHECK(ix.apply(tx({replace_docid(d, "", body)}), {}).result == verdict::accepted); by_docid[d] = body; }
					break;
				}
				case 3: {
					if (by_docid.count(d)) { CHECK(ix.apply(tx({erase_docid(d)}), {}).result == verdict::accepted); by_docid.erase(d); }
					break;
				}
				default: if (r() % 7 == 0) { ix.refresh(); }
			}
			for (const auto& name : keys) {
				const auto it = by_key.find(name);
				CHECK(ix.get(name) == (it == by_key.end() ? std::optional<std::string>() : std::optional<std::string>(it->second)));
			}
			for (const auto& id : docids) {
				const bool keyed_here = [&] {
					for (const auto& [kk, bb] : by_key) { if (ix.find_key(kk) && ix.find_key(kk)->first == id) { return true; } }
					return false;
				}();
				CHECK(ix.has_docid(id) == (by_docid.count(id) || keyed_here));
			}
		}
	}
}

static void buffer_cap() {
	// The buffer flushes before it passes its cap of entries, and a transaction larger than the
	// cap is invalid (nothing could hold it).
	kv_index::options o;
	o.key_prefix = "Q";
	o.buffered_docs_cap = 5;
	o.auto_merge = false;
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, o, uuid_of("slots-cap"));
	std::size_t most = 0;
	for (int i = 0; i < 40; ++i) {
		CHECK(ix.apply(tx({add("Qa" + std::to_string(i), "x"), add("Qb" + std::to_string(i), "y")}), {}).result == verdict::accepted);
		most = std::max(most, ix.buffered_count());
		CHECK(ix.buffered_count() <= 5);
	}
	CHECK(most == 4 && ix.segments().size() >= 15);
	CHECK(ix.apply(tx({add("Q1", "1"), add("Q2", "2"), add("Q3", "3"), add("Q4", "4"), add("Q5", "5"), add("Q6", "6")}), {}).result == verdict::invalid);
	CHECK(!ix.get("Q1"));
	CHECK(ix.apply(tx({add("Q1", "1"), add("Q2", "2"), add("Q3", "3"), add("Q4", "4"), add("Q5", "5")}), {}).result == verdict::accepted);
	CHECK(ix.buffered_count() == 5 && ix.get("Q5") == "5" && ix.get("Qa0") == "x");
	for (int i = 0; i < 40; ++i) { CHECK(ix.get("Qa" + std::to_string(i)) == "x" && ix.get("Qb" + std::to_string(i)) == "y"); }
}

int main() {
	table_collisions();
	table_random();
	table_growth_boundary();
	index_key_collisions();
	index_docid_collisions();
	index_random_collisions();
	buffer_cap();
	if (failures) {
		std::fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	std::puts("ok");
}
