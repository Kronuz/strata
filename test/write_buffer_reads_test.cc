// The write buffer's step 1, item 4 (docs/design/write-buffer.md D5, D6, D7, D9, D10; WB-21 to
// WB-23): V and view positions, live reads from other threads at r = max(V, view position),
// the docid walk and check, the first-docid-only skip, docid maps built under the lease only,
// (C)'s scan runs, and map tables and run sets retired by view generation. T8's schedules at
// one writer mirror the architect's model (`.scratch/rev3/docid_model.py` on design/write-modes).
#include "kv_test.hh"
#include "strata/core/pool_executor.hh"
#include "strata/core/write_buffer.hh"

#include <atomic>
#include <cstring>
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
			const auto& k = s.seg()->get<keys_plugin>();
			if (const auto o = k.find(key, live_bits{nullptr}, xxh64::hash(key))) {
				return {kv_index::superseded(ix.wb_.get(), s, *o, r), kv_index::docid_at(*ix.wb_, s.seg()->core().docid(*o), r)};
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
		return {x.has_docid_prev(), x.prev, x.docid_prev()};
	}
	using record = kv_index::record;
	static const record& newest_record(const kv_index& ix, std::string_view key) { return ix.wb_->lane.at(kv_index::find_key_in(*ix.wb_, key)); }
	static const record& record_at(const kv_index& ix, wbuf::ref_t r) { return ix.wb_->lane.at(r); }
	static wbuf::ref_t newest_ref(const kv_index& ix, std::string_view key) { return kv_index::find_key_in(*ix.wb_, key); }
	static std::uint64_t buffered_bytes(const kv_index& ix) { return ix.buffered_bytes(); }
	static bool docids_built(const kv_index& ix) { return ix.wb_ && ix.wb_->docids.built(); }
	static void set_run_cap(kv_index& ix, std::size_t cap) { ix.run_cap_ = cap; }
	// A merge of every segment that leaves the pending deletes pending, as a background merge
	// scheduled before them meets them (CR5: force_merge applies them first).
	static void merge_leaving_pending(kv_index& ix) {
		const lease_scope l{ix.lease_};
		ix.run_merges([&](std::vector<merge_policy::candidate> c) { return ix.opts_.policy.find_forced_merges(std::move(c), 1); }, true, true);
	}
	// The active buffer's installed run set, its level sizes, and its cover (D9).
	static const wbuf::run_set* runs(const kv_index& ix) { return ix.wb_ ? ix.wb_->runs.load() : nullptr; }
	static std::vector<std::size_t> levels(const kv_index& ix) {
		std::vector<std::size_t> out;
		if (const auto* rs = runs(ix)) {
			for (const auto& l : rs->levels) { out.push_back(l->size()); }
		}
		return out;
	}
	static std::size_t tables_waiting(const kv_index& ix) { return ix.pub_ ? ix.pub_->tables.size() : 0; }
};
}  // namespace strata

namespace {

kv_index::options with(reader_registry& reg) {
	kv_index::options o;
	o.keyed = true;
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
// A scan's rows in the order it emits them: a duplicate or a misplaced key is a difference.
using rows = std::vector<std::pair<std::string, std::string>>;
rows ordered(const state& s) { return rows(s.begin(), s.end()); }
template <class R>
rows scan_all(const R& at) {
	rows out;
	at.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace_back(std::string(k), std::string(v)); });
	return out;
}

// A record's bytes at D3's offsets (I4-1, WB-21): the flags at 24, the docid prev field at 28 when
// its flag is set, the document after the fields; read from the raw bytes, not through accessors.
using access = index_test_access;
std::uint32_t word_at(const access::record& x, std::size_t off) {
	std::uint32_t w;
	std::memcpy(&w, reinterpret_cast<const unsigned char*>(&x) + off, 4);
	return w;
}
std::size_t doc_offset(const access::record& x) { return std::size_t(reinterpret_cast<const unsigned char*>(&x.doc()) - reinterpret_cast<const unsigned char*>(&x)); }
unsigned flags_byte(const access::record& x) { return reinterpret_cast<const unsigned char*>(&x)[24]; }

void record_layout() {
	reader_registry reg(32);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("layout"));
	CHECK(ok(ix.apply(tx({replace("k1", "a1"), replace("k2", "a2"), replace("k4", "a4")}), {})));  // docids 1, 2, 3
	ix.refresh();
	// A replace_docid reserves the field from its type; the docid has no record yet, so the field
	// holds no reference. The flags byte is the only flag set.
	CHECK(ok(ix.apply(tx({replace_docid(2, "k5", "b5")}), {})));
	const auto& first = access::newest_record(ix, "k5");
	CHECK(flags_byte(first) == 2 && first.prev == wbuf::no_ref && word_at(first, 28) == wbuf::no_ref && doc_offset(first) == 32);
	// A key-changing replace_docid of a buffered document: prev is the new key's (none) and the
	// field at 28 names the docid's record under the old key.
	CHECK(ok(ix.apply(tx({replace_docid(2, "k6", "d6")}), {})));
	const auto& moved = access::newest_record(ix, "k6");
	CHECK(flags_byte(moved) == 2 && moved.prev == wbuf::no_ref);
	CHECK(word_at(moved, 28) == access::newest_ref(ix, "k5") && moved.docid_prev() == word_at(moved, 28));
	CHECK(doc_offset(moved) == 32 && moved.doc().key == "k6");
	// A replace_docid that keeps its key: prev names both, and the field is empty.
	CHECK(ok(ix.apply(tx({replace_docid(2, "k6", "e6")}), {})));
	const auto& same = access::newest_record(ix, "k6");
	CHECK(flags_byte(same) == 2 && same.prev != wbuf::no_ref && word_at(same, 28) == wbuf::no_ref && same.docid_prev() == wbuf::no_ref);
	// A tombstone has no field; writing its docid again names it in the field.
	CHECK(ok(ix.apply(tx({erase_docid(1)}), {})));
	CHECK(ok(ix.apply(tx({replace_docid(1, "k1", "c1")}), {})));
	const auto& again = access::newest_record(ix, "k1");
	const auto& gone = access::record_at(ix, word_at(again, 28));
	CHECK(flags_byte(again) == 2 && again.prev == wbuf::no_ref && flags_byte(gone) == 1 && gone.tombstone() && !gone.has_docid_prev());
	CHECK(doc_offset(gone) == 32 && gone.stride() == (32 + sizeof(document) + 7) / 8 * 8);
	// Without the flag the document follows the header at once and nothing is reserved.
	CHECK(ok(ix.apply(tx({replace("k4", "b4")}), {})));
	const auto& plain = access::newest_record(ix, "k4");
	CHECK(flags_byte(plain) == 0 && !plain.has_docid_prev() && plain.docid_prev() == wbuf::no_ref && doc_offset(plain) == 32);
	CHECK(plain.stride() == (32 + sizeof(document) + 7) / 8 * 8 && plain.doc().key == "k4");
}

// Records of every shape in one lane: each stride follows its flags; a cut back and a re-append
// keep the survivors and destroy the rest (the documents hold heap strings: ASan sees a wrong
// stride as an overflow or a double free).
void record_strides() {
	using record = access::record;
	wbuf::lane<record> l;
	const std::string big(200, 'x');
	const std::uint8_t shapes[] = {0, record::docid_prev_flag, record::ordinal_flag, std::uint8_t(record::docid_prev_flag | record::ordinal_flag), record::tombstone_flag};
	std::vector<wbuf::ref_t> refs;
	wbuf::lane<record>::mark cut;
	for (std::uint32_t i = 0; i < 400; ++i) {
		if (i == 150) { cut = l.tell(); }
		document d;
		d.key = big + std::to_string(i);
		const std::uint8_t f = shapes[i % 5];
		refs.push_back(record::emplace(l, i + 1, i + 2, i + 3, 10 + i, f, 1000 + i, 2000 + i, std::move(d)));
	}
	const auto check = [&](std::uint32_t from, std::uint32_t to) {
		for (std::uint32_t i = from; i < to; ++i) {
			const record& x = l.at(refs[i]);
			const std::uint8_t f = shapes[i % 5];
			CHECK(flags_byte(x) == f && x.docid == i + 1 && x.position == i + 3 && x.prev == 10 + i);
			CHECK(x.doc().key == big + std::to_string(i));
			CHECK(doc_offset(x) == (f == 6 ? 40 : 32) && x.stride() == record::stride_for(f));
			if (f & record::docid_prev_flag) { CHECK(word_at(x, 28) == 1000 + i && x.docid_prev() == 1000 + i); }
			if (f & record::ordinal_flag) { CHECK(word_at(x, f & record::docid_prev_flag ? 32 : 28) == 2000 + i && x.ordinal() == 2000 + i); }
		}
	};
	check(0, 400);
	l.truncate(cut);
	refs.resize(150);
	for (std::uint32_t i = 150; i < 260; ++i) {
		document d;
		d.key = big + std::to_string(i);
		refs.push_back(record::emplace(l, i + 1, i + 2, i + 3, 10 + i, shapes[i % 5], 1000 + i, 2000 + i, std::move(d)));
	}
	check(0, 260);
	std::printf("record layout: flags at 24, docid prev at 28, strides follow the flags\n");
}

// CR1 (D4): a multi-operation transaction rejected after it pushed a record, beside readers of the
// record's key. With readers the rejected record stays in the lane (its slot is restored) and its
// bytes count toward the flush threshold, so the buffer stays bounded; without readers it is cut.
void rejection_beside_readers() {
	const std::string K = "kaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", X = "kxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
	const std::string good(100, 'v'), bad(100, 'w');
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.keyed = true;
		auto ix = kv_index::create(io, o, uuid_of("rej0"));
		CHECK(ok(ix.apply(tx({add(K, good), add(X, "x")}), {})));
		const auto before = access::buffered_bytes(ix);
		CHECK(!ok(ix.apply(tx({replace(K, bad), add(X, "y")}), {})));
		CHECK(access::buffered_bytes(ix) == before);  // no reader: the rejected record is cut
	}
	reader_registry reg(16);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget memory(1 << 30);
	auto o = with(reg);
	o.budget = &memory;
	o.buffer_share = 256 << 10;
	auto ix = kv_index::create(io, o, uuid_of("rej"));
	const auto h = ix.reader();
	CHECK(ok(ix.apply(tx({add(K, good), add(X, "x")}), {})));
	const auto before = access::buffered_bytes(ix);
	CHECK(!ok(ix.apply(tx({replace(K, bad), add(X, "y")}), {})));
	CHECK(access::buffered_bytes(ix) > before);  // with readers the rejected record stays, counted
	for (int i = 0; i < 20; ++i) { CHECK(!ok(ix.apply(tx({replace(K, std::string(2000, 'w')), add(X, "y")}), {}))); }
	CHECK(memory.used() >= access::buffered_bytes(ix));  // and charged to the budget
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> reads{0}, wrong{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 3; ++t) {
		threads.emplace_back([&] {
			while (!done.load()) {
				const auto live = h.live();
				const auto v = live.at.get(K);
				wrong += !v || *v != good;
				++reads;
			}
		});
	}
	std::uint64_t rejected = 0, peak = 0;
	for (int i = 0; i < 6000; ++i) {
		rejected += !ok(ix.apply(tx({replace(K, bad), add(X, "y")}), {}));
		if (i % 50 == 0) { CHECK(ok(ix.apply(tx({replace(K, good)}), {}))); }
		peak = std::max(peak, access::buffered_bytes(ix));
	}
	done = true;
	for (auto& t : threads) { t.join(); }
	CHECK(rejected == 6000 && wrong.load() == 0);
	CHECK(peak < (256 << 10) + (64 << 10));  // the rejected records' bytes reach the flush threshold
	// A buffer that only ever held rejected records is dropped at the next flush, not kept.
	faultfs::filesystem fs2;
	faultfs::io io2(fs2);
	auto small = with(reg);
	small.buffer_share = 64 << 10;
	auto only = kv_index::create(io2, small, uuid_of("rejonly"));
	const auto ho = only.reader();
	std::uint64_t peak_only = 0;
	for (int i = 0; i < 3000; ++i) {
		CHECK(!ok(only.apply(tx({add(X, std::string(100, 'a')), add(X, "b")}), {})));
		peak_only = std::max(peak_only, access::buffered_bytes(only));
	}
	CHECK(peak_only < (64 << 10) + (16 << 10) && only.buffered_count() == 0);
	std::printf("rejection beside readers: %llu rejected, %llu reads, %llu wrong, peak %llu B buffered\n", (unsigned long long)rejected, (unsigned long long)reads.load(),
	            (unsigned long long)wrong.load(), (unsigned long long)peak);
}

// CR2 (D9): a reader's private tail of T entries with no run installed, scanned for n results,
// compared as an ordered vector with the model: tails around the build threshold (64) and the
// select-32 boundary, with one record per key and with several.
void tail_select() {
	std::size_t cases = 0;
	for (const std::size_t T : {1, 31, 32, 33, 63, 64, 65, 66, 96, 97, 200, 700}) {
		for (const std::size_t n : {1, 5, 31, 32, 33, 40, 64, 65, 100000}) {
			for (int dup = 0; dup < 2; ++dup) {
				reader_registry reg(8);
				faultfs::filesystem fs;
				faultfs::io io(fs);
				auto ix = kv_index::create(io, with(reg), uuid_of("tailsel"));
				const auto h = ix.reader();
				std::mt19937 rng(unsigned(T * 1000 + n * 10 + dup));
				state want;
				for (std::size_t i = 0; i < T; ++i) {
					const std::string k = "kprefix_" + std::to_string(dup ? rng() % (T / 3 + 1) : rng() % 100000);
					const std::string v = "v" + std::to_string(i);
					want[k] = v;
					CHECK(ok(ix.apply(tx({replace(k, v)}), {})));
				}
				rows exp = ordered(want), got;
				if (n < exp.size()) { exp.resize(n); }
				h.live().at.scan("", n, [&](std::string_view k, std::string_view v, std::uint64_t) { got.emplace_back(std::string(k), std::string(v)); });
				CHECK(got == exp);
				++cases;
			}
		}
	}
	std::printf("tail select: %zu cases, ordered\n", cases);
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
	index_test_access::merge_leaving_pending(ix);
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
			wrong += scan_all(live.at) != ordered(expect);
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
			t.wrong += scan_all(at) != ordered(snaps[at.position()]);
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
				const bool bad = (rr() & 1) ? read_all(live.at, keys) != want : scan_all(live.at) != ordered(want);
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

// WB-23 (I4-13): has_docid() const builds no docid map; it walks a buffer without one.
void has_docid_builds_nothing() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("hasdocid"));
	CHECK(ok(ix.apply(tx({replace("k1", "a"), replace("k2", "b")}), {})));  // docids 1, 2
	CHECK(ok(ix.apply(tx({erase("k2")}), {})));
	CHECK(ix.has_docid(1) && !ix.has_docid(2) && !ix.has_docid(3));
	CHECK(!index_test_access::docids_built(ix));
	ix.refresh();
	CHECK(ok(ix.apply(tx({replace("k1", "c")}), {})));
	CHECK(ix.has_docid(1) && !ix.has_docid(2) && !index_test_access::docids_built(ix));
	std::printf("has_docid builds nothing: ok\n");
}

// find_docid() answers a docid's live version from the buffer, a flushed segment and after an
// erase, with no flush of its own.
void find_docid_reads_buffers() {
	reader_registry reg(4);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("finddocid"));
	CHECK(!ix.find_docid(1));
	CHECK(ok(ix.apply(tx({replace("k1", "a"), replace("k2", "b")}), {})));  // docids 1, 2 at version 1
	CHECK(ix.find_docid(1) == std::optional<std::uint64_t>(1) && ix.find_docid(2) == std::optional<std::uint64_t>(1) && !ix.find_docid(3));
	CHECK(ok(ix.apply(tx({replace("k1", "c")}), {})));
	CHECK(ix.find_docid(1) == std::optional<std::uint64_t>(2));
	CHECK(ix.segments().empty());  // nothing was flushed to answer
	ix.refresh();
	CHECK(ix.find_docid(1) == std::optional<std::uint64_t>(2) && ix.find_docid(2) == std::optional<std::uint64_t>(1));
	CHECK(ok(ix.apply(tx({erase("k2")}), {})));
	CHECK(ix.find_docid(1) && !ix.find_docid(2));
	std::printf("find_docid reads buffers: ok\n");
}

// D10 (I4-3): a grown table waits for every view at or below its stamp, so a reader that loaded
// it before the growth keeps a live table; the scan after the release frees it.
void tables_wait_for_views() {
	reader_registry reg(8);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("tables"));
	const auto h = ix.reader();
	CHECK(ok(ix.apply(tx({replace("k0", "a")}), {})));
	std::optional<kv_index::reader_handle::live_view> held(h.live());
	const std::uint64_t g = h.generation();
	for (int i = 1; i < 40; ++i) { CHECK(ok(ix.apply(tx({replace("k" + std::to_string(i), "a")}), {}))); }
	CHECK(h.generation() > g);  // each replacement publishes a view (D10)
	ix.commit();
	const std::size_t waiting = index_test_access::tables_waiting(ix);
	CHECK(waiting > 0 && held->at.get("k0") == std::optional<std::string>("a") && !held->at.get("k39"));
	held.reset();
	ix.commit();
	CHECK(index_test_access::tables_waiting(ix) == 0 && h.live().at.get("k39") == std::optional<std::string>("a"));
	std::printf("tables wait for views: %zu waited: ok\n", waiting);
}

// --- (C)'s scan (D9) -----------------------------------------------------------------------

// A bounded scan's expected result from a state.
state expect_scan(const state& s, const std::string& from, const std::string* to, std::size_t n) {
	state out;
	for (auto it = s.lower_bound(from); it != s.end() && out.size() < n && (!to || it->first < *to); ++it) { out.insert(*it); }
	return out;
}
template <class R>
rows scan_range(const R& at, const std::string& from, const std::string* to, std::size_t n) {
	rows out;
	const auto f = [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace_back(std::string(k), std::string(v)); };
	if (to) {
		at.scan(from, *to, n, f);
	} else {
		at.scan(from, n, f);
	}
	return out;
}
// Keys whose first 8 bytes tie (record reads in sorts and merges), short keys, and zero bytes.
std::vector<std::string> scan_keys(std::size_t n) {
	std::vector<std::string> keys;
	for (std::size_t i = 0; keys.size() < n; ++i) {
		switch (i % 4) {
		case 0: keys.push_back("kprefix_" + std::to_string(i)); break;
		case 1: keys.push_back("k" + std::to_string(i)); break;
		case 2: keys.push_back(std::string("k\0", 2) + std::to_string(i)); break;
		default: keys.push_back("kprefix_" + std::string(i % 3, '\0') + "x" + std::to_string(i)); break;
		}
	}
	return keys;
}

// WB-17 and the binary counter (I4-8): writes alone build nothing; a scan whose tail passes
// tail_build asks, and the lease holder's next group builds (inline without an executor). The
// levels hold only keyed records, each sorted, and a level past the cap is merged by
// compaction. A view held across replacements reads its set; the replaced sets wait (D10).
void runs_by_counter() {
	reader_registry reg(8);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto ix = kv_index::create(io, with(reg), uuid_of("runs"));
	index_test_access::set_run_cap(ix, 128);
	const auto h = ix.reader();
	const auto keys = scan_keys(400);
	state want;
	std::size_t at = 0;
	const auto write = [&](std::size_t n) {
		for (std::size_t i = 0; i < n; ++i, ++at) {
			const std::string& k = keys[(at * 7) % keys.size()];
			want[k] = "v" + std::to_string(at);
			CHECK(ok(ix.apply(tx({replace(k, want[k]), keyless("x")}), {})));
		}
	};
	write(100);
	CHECK(!index_test_access::runs(ix));  // writes alone build nothing
	std::optional<kv_index::reader_handle::live_view> held(h.live());
	const state at_held = want;
	CHECK(scan_all(held->at) == ordered(want));  // a tail of 200 entries asks for a build
	write(1);
	const auto* first = index_test_access::runs(ix);
	CHECK(first && first->cover == 202 && index_test_access::levels(ix) == std::vector<std::size_t>{101});
	std::size_t replaced = 0, compacted = 0;
	for (int round = 0; round < 40; ++round) {
		write(40);
		CHECK(scan_all(h.live().at) == ordered(want));
		const auto* before = index_test_access::runs(ix);
		write(1);
		replaced += index_test_access::runs(ix) != before;
		std::size_t sum = 0;
		const auto lv = index_test_access::levels(ix);
		for (const std::size_t n : lv) { sum += n; }
		CHECK(sum == index_test_access::runs(ix)->cover / 2);  // one keyed record in two
		compacted += lv.size() > 1 && lv[0] > 128;
	}
	CHECK(replaced >= 39 && compacted > 0);
	CHECK(index_test_access::tables_waiting(ix) > 0);  // the replaced sets wait for the held view
	CHECK(scan_all(held->at) == ordered(at_held));
	held.reset();
	std::printf("runs by counter: %zu replaced, %zu compacted, levels", replaced, compacted);
	for (const std::size_t n : index_test_access::levels(ix)) { std::printf(" %zu", n); }
	std::printf(": ok\n");
	ix.commit();
	ix.commit();
	CHECK(index_test_access::tables_waiting(ix) == 0 && scan_all(ix) == ordered(want));
}

// A stale splice (its consumed levels gone) is dropped; a fresh one moves the cover (D9).
void stale_splice_dropped() {
	const auto lvl = [](std::size_t n) { return std::make_shared<const std::vector<wbuf::pos_entry>>(n); };
	wbuf::run_set cur;
	cur.levels = {lvl(64), lvl(8)};
	cur.cover = 72;
	wbuf::splice sp;
	sp.consumed = {lvl(8)};  // not the installed level
	sp.produced = lvl(9);
	sp.fresh = true;
	sp.cover = 73;
	CHECK(!wbuf::apply_splice(&cur, sp));
	sp.consumed = {cur.levels[1]};
	const auto next = wbuf::apply_splice(&cur, sp);
	CHECK(next && next->levels.size() == 2 && next->levels[0] == cur.levels[0] && next->levels[1] == sp.produced && next->cover == 73);
	std::printf("stale splice dropped: ok\n");
}

// T8's scan schedule at one writer (I4-8): the model check's operations over keys with tied
// prefixes, with run builds on a pool, a lowered cap so compactions run, readers on other
// threads scanning ranges and bounded prefixes at their r, views held across splices and read
// later, and the lease holder's own scans.
struct scan_totals {
	std::uint64_t accepted = 0, reads = 0, wrong = 0, installs = 0, compacted = 0, owner = 0;
};
void scan_schedule(unsigned seed, std::size_t steps, unsigned readers, scan_totals& sum) {
	ThreadPool<> pool("wbrscan{}", 2);
	pool_executor<> ex(pool);
	reader_registry reg(64);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	auto o = with(reg);
	o.background = &ex;
	auto ix = kv_index::create(io, o, uuid_of("scansched"));
	index_test_access::set_run_cap(ix, 64);
	const auto h = ix.reader();
	const auto keys = scan_keys(48);
	std::vector<state> snaps(steps + 1);
	model m;
	std::mt19937 rng(seed);
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> wrong{0}, reads{0};
	const auto one_scan = [&keys](std::mt19937& rr, const auto& at, const state& want) {
		const std::string from = rr() % 4 ? keys[rr() % keys.size()] : std::string();
		const std::string to = keys[rr() % keys.size()];
		const std::size_t n = rr() % 3 ? 1 + rr() % 6 : ~std::size_t(0);
		const std::string* top = rr() % 2 ? &to : nullptr;
		return scan_range(at, from, top, n) == ordered(expect_scan(want, from, top, n));
	};
	std::vector<std::thread> threads;
	for (unsigned t = 0; t < readers; ++t) {
		threads.emplace_back([&, t] {
			std::mt19937 rr(seed * 131 + t);
			while (!done.load(std::memory_order_relaxed)) {
				const auto live = h.live();
				const bool bad = !one_scan(rr, live.at, snaps[live.at.position()]);
				wrong += bad;
				reads += 1;
				if (bad && wrong == 1) { std::fprintf(stderr, "scan seed %u: wrong scan at r %llu\n", seed, (unsigned long long)live.at.position()); }
			}
		});
	}
	std::vector<std::pair<kv_index::reader_handle::live_view, std::uint64_t>> held;
	const auto check_held = [&] {
		std::thread([&] {
			std::mt19937 rr(seed);
			for (const auto& [live, r] : held) {
				const auto at = live.guard->at(r);
				wrong += scan_all(at) != ordered(snaps[at.position()]);
				wrong += !one_scan(rr, at, snaps[at.position()]);
				reads += 2;
			}
		}).join();
		held.clear();
	};
	scan_totals t;
	const wbuf::run_set* last = nullptr;
	const auto val = [&] { return "v" + std::to_string(rng() % 1000); };
	for (std::size_t step = 0; step < steps; ++step) {
		if (rng() % 8 == 0) {
			auto live = h.live();
			const std::uint64_t lo = live.guard->visible(), hi = live.at.position();
			held.emplace_back(std::move(live), lo + rng() % (hi - lo + 1));
			if (held.size() >= 32) { check_held(); }
		}
		const unsigned c = rng() % 1000;
		if (c < 2) {
			ix.refresh();
			continue;
		}
		if (c < 4) {
			index_test_access::freeze(ix);
			continue;
		}
		if (c < 5) {
			ix.force_merge(1);
			continue;
		}
		if (c < 25) {  // the lease holder's own scan
			std::mt19937 rr(seed + unsigned(step));
			t.owner += 1;
			wrong += !one_scan(rr, ix, m.snapshot());
			continue;
		}
		const std::string& k = keys[rng() % keys.size()];
		const std::uint64_t d = 1 + rng() % (m.last + 2);
		operation op;
		switch (rng() % 12) {
		case 0: op = add(k, val()); break;
		case 1: op = erase(k); break;
		case 2: op = replace_docid(d, k, val()); break;
		case 3: op = erase_docid(d); break;
		case 4: op = keyless(val()); break;
		default: op = replace(k, val()); break;
		}
		model next = m;
		const bool want = next.apply(op);
		if (want) { snaps[h.visible() + 1] = next.snapshot(); }
		const bool got = ok(ix.apply(tx({op}), {}));
		wrong += got != want;
		if (got && want) {
			m = std::move(next);
			++t.accepted;
		}
		if (const auto* rs = index_test_access::runs(ix); rs != last) {
			last = rs;
			t.installs += rs != nullptr;
			const auto lv = index_test_access::levels(ix);
			t.compacted += lv.size() > 1 && lv[0] > 64 && lv[1] > 64;
		}
	}
	done = true;
	for (auto& th : threads) { th.join(); }
	check_held();
	CHECK(scan_all(ix) == ordered(m.snapshot()));
	CHECK(wrong == 0);
	if (wrong) { std::printf("scan schedule seed %u: %llu wrong\n", seed, (unsigned long long)wrong.load()); }
	sum.accepted += t.accepted;
	sum.reads += reads + t.owner;
	sum.wrong += wrong;
	sum.installs += t.installs;
	sum.compacted += t.compacted;
	sum.owner += t.owner;
}

}  // namespace

int main() {
	record_layout();
	record_strides();
	tail_select();
	rejection_beside_readers();
	d6_schedule();
	first_docid_only();
	tables_wait_for_views();
	has_docid_builds_nothing();
	find_docid_reads_buffers();
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
	runs_by_counter();
	stale_splice_dropped();
	{
		scan_totals t;
		for (unsigned seed = 0; seed < 6; ++seed) { scan_schedule(seed, 3000, 3, t); }
		std::printf("scan schedule: seeds 0-5, %llu accepted, %llu reads (%llu by the lease holder), %llu run sets installed, %llu with compacted levels, %llu wrong\n",
		            (unsigned long long)t.accepted, (unsigned long long)t.reads, (unsigned long long)t.owner, (unsigned long long)t.installs, (unsigned long long)t.compacted,
		            (unsigned long long)t.wrong);
	}
	if (failures) {
		std::printf("write_buffer_reads_test: %d failures\n", failures);
		return 1;
	}
	std::printf("write_buffer_reads_test: ok\n");
	return 0;
}
