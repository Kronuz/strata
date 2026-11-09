// The manifest log's records (manifest-log D3): the layout byte by byte, the fixtures, the
// round trip over random pairs of states (T6's oracle at the codec), every replay check, the
// decoder's bounds, and the tracked snapshot size (the re-check's C7). The log file (D4, D6, D7):
// the reader's caller rules, crash images, bit flips, bounds on read, replay cost, allocation
// failure, and the appender.
#include "strata/core/faultfs.hh"
#include "strata/core/manifest_log.hh"

#include <atomic>
#include <chrono>
#include <new>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

// A global operator new that fails the nth allocation once armed (the allocation-failure stage).
static std::atomic<long> allocations_left{-1};
static std::atomic<long> allocations{0};
void* operator new(std::size_t n) {
	++allocations;
	if (const long left = allocations_left.load(); left >= 0) {
		allocations_left = left - 1;
		if (left == 0) { throw std::bad_alloc(); }
	}
	if (void* p = std::malloc(n ? n : 1)) { return p; }
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;
using namespace strata::files;
namespace fr = kronuz::journal::framing;

namespace {

using rng = std::mt19937_64;

template <typename E>
bool throws(const std::function<void()>& f) {
	try {
		f();
	} catch (const E&) {
		return true;
	} catch (...) {
		return false;
	}
	return false;
}
bool corrupt(const std::function<void()>& f) { return throws<corrupt_index>(f); }

field_declaration decl(std::string name, field_postings p = field_postings::docs, bool norms = false, field_role role = field_role::indexed) {
	return {std::move(name), role, p, norms};
}

// Sets the totals to the segments' sums, the versions to the segments', and the sizes.
void fix(log_state& s) {
	s.doc_count = s.live_count = s.live_length = 0;
	for (const auto& [id, seg] : s.segments) {
		s.doc_count += seg.doc_count;
		s.live_count += seg.live_count;
		s.live_length += seg.live_length;
	}
	if (s.segments.empty()) {
		s.min_version = s.max_version = 0;
	} else if (s.min_version == 0) {
		s.min_version = s.max_version = 13;
	}
	s.total_length = std::max(s.total_length, s.live_length);
	s.configured &= s.mask;
	recount(s);
}

void delete_some(log_segment& seg, rng& r, std::uint64_t k) {
	std::set<std::uint64_t> dead(seg.deleted.begin(), seg.deleted.end());
	for (std::uint64_t i = 0; i < k && dead.size() < seg.doc_count; ++i) {
		std::uint64_t o = r() % seg.doc_count;
		while (dead.count(o)) { o = (o + 1) % seg.doc_count; }
		dead.insert(o);
	}
	const std::uint64_t gone = dead.size() - seg.deleted.size();
	seg.deleted.assign(dead.begin(), dead.end());
	seg.live_count = seg.doc_count - seg.deleted.size();
	seg.live_length -= std::min(seg.live_length, gone * (r() % 4));
	seg.live_length = std::min(seg.live_length, seg.live_count * 20);
}

log_segment random_segment(rng& r, std::uint64_t id) {
	log_segment s;
	s.id = id;
	for (auto& b : s.identity) { b = std::uint8_t(r()); }
	s.size = 4096 + r() % 100000;
	s.footer_crc = std::uint32_t(r());
	switch (r() % 4) {
	case 0: s.doc_count = r() % 9; break;                 // tiny, empty included
	case 1: s.doc_count = 1 + r() % 3000; break;          // bitsets
	case 2: s.doc_count = 50000 + r() % 200000; break;    // sparse: deltas
	default: s.doc_count = 1 + r() % 200; break;
	}
	s.live_count = s.doc_count;
	s.live_length = s.doc_count * (1 + r() % 9);
	if (s.doc_count && r() % 3) { delete_some(s, r, s.doc_count < 4000 && r() % 2 ? s.doc_count / 3 : 1 + r() % 12); }
	return s;
}

const std::array<char, 16> uuid{'s', 't', 'r', 'a', 't', 'a', '-', 'm', 'l', 'o', 'g', '-', 't', 'e', 's', 't'};

log_state random_state(rng& r) {
	log_state s;
	s.uuid = uuid;
	s.generation = 1 + r() % 40;
	s.revision = r() % 1000;
	s.last_docid = r() % 100000;
	s.applied_index = r() % 50;
	s.applied_count = r() % 4;
	s.schema_version = r() % 3;
	s.mask = 15 | (r() % 2) << 7;
	s.configured = r() % 16;
	s.total_length = r() % 100000;
	for (std::uint64_t i = 0, n = r() % 6; i < n; ++i) {
		const std::uint64_t id = s.next_segment + r() % 3;
		s.segments.emplace(id, random_segment(r, id));
		s.next_segment = id + 1;
	}
	s.next_segment += r() % 3;
	for (std::uint64_t g = 1; g <= s.generation; ++g) {
		if (r() % 8 == 0) { s.pins.push_back(g); }
	}
	for (std::uint64_t i = 0, n = r() % 5; i < n; ++i) { s.metadata["k" + std::to_string(r() % 20)] = std::string(r() % 30, char('a' + r() % 26)); }
	for (std::uint64_t i = 0, n = r() % 4; i < n; ++i) { s.fields.append(decl("f" + std::to_string(i), field_postings(r() % 3), r() % 2)); }
	if (r() % 3 == 0) { s.fields.append(decl("key", field_postings::docs, false, field_role::key)); }
	fix(s);
	return s;
}

// A state one commit after d: some of a flush, a merge, deletes, metadata, declarations, pins,
// and scalar changes.
log_state successor(const log_state& d, rng& r) {
	log_state c = d;
	++c.generation;
	if (r() % 2) { ++c.revision; }
	if (r() % 3 == 0) { c.last_docid += r() % 100; }
	auto fresh_id = [&] {  // a gap below the next id now and then: a merge's id is its scheduling's
		for (std::uint64_t id = 1; id < c.next_segment; ++id) {
			if (!c.segments.count(id) && !d.segments.count(id) && r() % 4 == 0) { return id; }
		}
		return c.next_segment++;
	};
	for (std::uint64_t n = r() % 5; n; --n) {
		switch (r() % 10) {
		case 0: {  // flush
			const std::uint64_t id = fresh_id();
			c.segments.emplace(id, random_segment(r, id));
			break;
		}
		case 1:  // merge two
			if (c.segments.size() >= 2) {
				auto a = std::next(c.segments.begin(), std::ptrdiff_t(r() % c.segments.size()));
				c.segments.erase(a);
				auto b = std::next(c.segments.begin(), std::ptrdiff_t(r() % c.segments.size()));
				c.segments.erase(b);
				const std::uint64_t id = fresh_id();
				c.segments.emplace(id, random_segment(r, id));
			}
			break;
		case 2:
		case 3:  // deletes in a segment d holds
			if (!c.segments.empty()) {
				auto it = std::next(c.segments.begin(), std::ptrdiff_t(r() % c.segments.size()));
				if (d.segments.count(it->first) && it->second.live_count) { delete_some(it->second, r, 1 + r() % 20); }
			}
			break;
		case 4: c.metadata["k" + std::to_string(r() % 20)] = std::string(r() % 30, char('a' + r() % 26)); break;
		case 5:
			if (!c.metadata.empty()) { c.metadata.erase(std::next(c.metadata.begin(), std::ptrdiff_t(r() % c.metadata.size()))); }
			break;
		case 6: c.fields.append(decl("g" + std::to_string(c.generation) + "-" + std::to_string(c.fields.size()), field_postings(r() % 3), r() % 2)); break;
		case 7:
			if (r() % 2 && !c.pins.empty()) {
				c.pins.erase(c.pins.begin() + std::ptrdiff_t(r() % c.pins.size()));
			} else if (c.pins.empty() || c.pins.back() < c.generation) {
				c.pins.push_back(c.generation - r() % 2 * (c.pins.empty() || c.pins.back() < c.generation - 1));
			}
			break;
		case 8:
			c.applied_index += r() % 3;
			c.applied_count = r() % 4;
			c.schema_version += r() % 2;
			break;
		default:
			c.configured = r() % 16;
			c.total_length += r() % 1000;
			if (!c.segments.empty()) { c.max_version = 13 + r() % 2; }
			break;
		}
	}
	fix(c);
	return c;
}

std::uint64_t framed(const log_state& s) {
	fr::Emitter emitter(log_stream(s.uuid, s.generation), 0);
	emitter.record(encode_snapshot(s));
	return emitter.position();
}

// T6's oracle at the codec, with C7: replaying the encoded difference d -> c onto d gives c,
// their snapshots encode equal, and the size apply() keeps is the snapshot's, framing included.
void round_trip(std::uint64_t seed, int steps) {
	rng r(seed);
	log_state d = random_state(r);
	CHECK(decode_snapshot(encode_snapshot(d)) == d);
	for (int i = 0; i < steps; ++i) {
		const log_state c = successor(d, r);
		const log_edit e = diff(d, c);
		const std::string bytes = encode_edit(e);
		const log_edit back = decode_edit(bytes);
		CHECK(back == e);
		CHECK(record_header(bytes).kind == log_kind::edit && record_header(bytes).generation == c.generation);
		log_state x = d;
		apply(x, back);
		CHECK(x == c);
		const std::string snap = encode_snapshot(x);
		CHECK(snap == encode_snapshot(c));
		CHECK(decode_snapshot(snap) == c);
		CHECK(snapshot_size(x) == snap.size());  // C7, before the framing
		CHECK(snapshot_bytes(x) == framed(c));   // C7, framing included
		if (failures) { return; }
		d = c;
	}
}

// The layout, byte by byte, for a small snapshot and two edits.
void layout() {
	log_state s;
	s.generation = 1;
	recount(s);
	const std::string v = "\x87\x80\x80\x80\x08";  // files::version 0x8000'0007 in LEB128
	CHECK(version == 0x8000'0007);
	std::string want = std::string("\x01\x01", 2) + v + std::string(16, '\0');
	want += std::string("\x00\x00\x01", 3) + std::string(11, '\0');  // revision, last docid, next segment 1, the rest 0
	want += std::string(5, '\0');                                       // pins, segments, metadata, fields, attributes: none
	CHECK(encode_snapshot(s) == want);
	CHECK(snapshot_size(s) == want.size());
	CHECK(decode_snapshot(want) == s);

	log_edit e;
	e.generation = 2;
	e.scalars = 1u << log_scalar::revision;
	e.values[log_scalar::revision] = 5;
	CHECK(encode_edit(e) == std::string("\x02\x02\x01\x05\x00", 5));  // kind, generation, scalar mask, revision, no sections

	log_edit del;
	del.generation = 3;
	del.deletes.push_back({7, {3, 5, 300}, 40, 400});
	// kind, generation, no scalars, sections: deletes (bit 2); one segment: id 7, 3 ordinals as
	// 3, +2, +295 (LEB128 a7 02), live 40, live length 400 (90 03).
	CHECK(encode_edit(del) == std::string("\x02\x03\x00\x04\x01\x07\x03\x03\x02\xa7\x02\x28\x90\x03", 14));
	CHECK(decode_edit(encode_edit(del)) == del);
}

// The fixtures: a snapshot and one edit of each kind, bytes committed under test/fixtures/manifest_log.
log_state fixture_base() {
	log_state s;
	s.uuid = {'s', 't', 'r', 'a', 't', 'a', '-', 'f', 'i', 'x', 't', 'u', 'r', 'e', 's', '!'};
	s.generation = 3;
	s.revision = 9;
	s.last_docid = 120;
	s.next_segment = 8;
	s.applied_index = 4;
	s.applied_count = 1;
	s.schema_version = 2;
	s.configured = 15;
	s.mask = 15 | 1u << 7;
	s.min_version = s.max_version = 13;
	s.total_length = 900000;
	log_segment a{3, {}, 8192, 0x11223344, 100, 0, 700};
	for (std::uint64_t o = 0; o < 100; o += 3) { a.deleted.push_back(o); }  // dense: a bitset
	log_segment b{5, {}, 65536, 0x55667788, 100000, 0, 400000};
	b.deleted = {7, 4000, 99999};  // sparse: deltas
	log_segment c{6, {}, 4096, 0x99aabbcc, 40, 40, 160};
	for (auto* seg : {&a, &b, &c}) {
		for (std::size_t i = 0; i < 16; ++i) { seg->identity[i] = std::uint8_t(seg->id * 16 + i); }
		seg->live_count = seg->doc_count - seg->deleted.size();
		s.segments.emplace(seg->id, *seg);
	}
	s.pins = {2};
	s.metadata = {{"schema", "{\"type\": \"object\"}"}, {"version", "12"}};
	s.fields.append(decl("title", field_postings::positions, true));
	s.fields.append(decl("id", field_postings::docs, false, field_role::key));
	fix(s);
	return s;
}

struct fixture_log {
	log_state base;
	std::vector<std::pair<std::string, log_edit>> edits;  // name, edit
	log_state last;                                      // after every edit, as decoded
};

fixture_log fixture_edits() {
	fixture_log f;
	f.base = fixture_base();
	log_state d = f.base;
	auto step = [&](const char* name, const std::function<void(log_state&)>& change) {
		log_state c = d;
		++c.generation;
		change(c);
		fix(c);
		f.edits.emplace_back(name, diff(d, c));
		d = c;
	};
	step("scalars", [](log_state& c) {
		c.revision = 10;
		c.last_docid = 130;
	});
	step("pins", [](log_state& c) { c.pins = {4}; });
	step("flush", [](log_state& c) {
		log_segment s{8, {}, 12288, 0xdeadbeef, 50, 50, 250};
		for (std::size_t i = 0; i < 16; ++i) { s.identity[i] = std::uint8_t(0x80 + i); }
		c.segments.emplace(8, s);
		c.next_segment = 9;
		++c.revision;
		c.last_docid = 180;
		c.total_length += 250;
	});
	step("deletes", [](log_state& c) {
		auto& a = c.segments.at(3);
		a.deleted.insert(std::lower_bound(a.deleted.begin(), a.deleted.end(), 2), 2);
		a.deleted.insert(std::lower_bound(a.deleted.begin(), a.deleted.end(), 50), 50);
		a.live_count -= 2;
		a.live_length -= 14;
		auto& b = c.segments.at(5);
		b.deleted = {7, 8, 4000, 70000, 99999};
		b.live_count -= 2;
		b.live_length -= 9;
		++c.revision;
	});
	step("merge", [](log_state& c) {  // 3 and 6 into 7: an id below the flushed 8
		log_segment m{7, {}, 9000, 0x0badf00d, c.segments.at(3).live_count + 40, 0, c.segments.at(3).live_length + 160};
		for (std::size_t i = 0; i < 16; ++i) { m.identity[i] = std::uint8_t(0x70 + i); }
		m.live_count = m.doc_count;
		c.segments.erase(3);
		c.segments.erase(6);
		c.segments.emplace(7, m);
	});
	step("metadata", [](log_state& c) {
		c.metadata.erase("schema");
		c.metadata["version"] = "13";
		c.metadata["new"] = "x";
		++c.revision;
	});
	step("fields", [](log_state& c) {
		c.fields.append(decl("body", field_postings::positions, true));
		++c.revision;
	});
	// Attributes this binary lacks: a cache tag (even) it drops on reading.
	log_edit seg_attrs;
	seg_attrs.generation = d.generation + 1;
	seg_attrs.segment_attributes.push_back({5, {{2, "c"}}});
	f.edits.emplace_back("segment-attributes", seg_attrs);
	log_edit index_attrs;
	index_attrs.generation = d.generation + 2;
	index_attrs.attributes = std::vector<attribute>{{4, "r"}};
	f.edits.emplace_back("attributes", index_attrs);
	d.generation += 2;
	f.last = d;
	return f;
}

void fixtures() {
	const fixture_log f = fixture_edits();
	std::vector<std::pair<std::string, std::string>> files{{"snapshot.bin", encode_snapshot(f.base)}};
	for (const auto& [name, e] : f.edits) { files.emplace_back("edit-" + name + ".bin", encode_edit(e)); }
	const std::filesystem::path dir = std::filesystem::path(STRATA_FIXTURES) / "manifest_log";
	if (std::getenv("STRATA_WRITE_FIXTURES")) {
		std::filesystem::create_directories(dir);
		for (const auto& [name, bytes] : files) { std::ofstream(dir / name, std::ios::binary) << bytes; }
	}
	log_state s;
	for (const auto& [name, bytes] : files) {
		std::ifstream in(dir / name, std::ios::binary);
		const std::string golden((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(golden == bytes);
		if (name == "snapshot.bin") {
			s = decode_snapshot(golden);
			CHECK(s == f.base);
		} else {
			apply(s, decode_edit(golden));
		}
		CHECK(snapshot_size(s) == encode_snapshot(s).size());
	}
	CHECK(s == f.last);  // the attributes' tags are dropped: a binary that lacks them reads none
	CHECK((decode_edit(files[8].second).segment_attributes == std::vector<log_segment_attributes>{{5, {}}}));
	CHECK(decode_edit(files[9].second).attributes == std::vector<attribute>{});
	// The two deleted-set encodings: segment 3 is a bitset, segment 5 deltas.
	CHECK(log_detail::deleted_encoding(f.base.segments.at(3)).encoding == 1 && log_detail::deleted_encoding(f.base.segments.at(5)).encoding == 2);
	// Unknown required tags refuse: the index needs what this binary lacks.
	log_edit required;
	required.generation = 9;
	required.attributes = std::vector<attribute>{{3, "x"}};
	CHECK(throws<unsupported_index>([&] { decode_edit(encode_edit(required)); }));
	required.attributes.reset();
	required.segment_attributes.push_back({5, {{5, "y"}}});
	CHECK(throws<unsupported_index>([&] { decode_edit(encode_edit(required)); }));
}

// Each of D3's replay checks: a valid edit broken one way throws corrupt_index and leaves the
// state as it was.
void replay_checks() {
	const log_state base = fixture_base();
	log_state flushed = base;
	++flushed.generation;
	log_segment s{8, {}, 4096, 1, 10, 10, 30};
	s.identity.fill(0xee);
	flushed.segments.emplace(8, s);
	flushed.next_segment = 9;
	flushed.total_length += 30;
	flushed.pins.push_back(4);
	flushed.metadata["m"] = "v";
	flushed.fields.append(decl("body"));
	fix(flushed);
	const log_edit good = diff(base, flushed);
	{
		log_state x = base;
		apply(x, good);
		CHECK(x == flushed);
	}
	auto rejects = [&](const char* what, const std::function<void(log_edit&)>& change) {
		log_edit e = good;
		change(e);
		log_state x = base;
		const bool thrown = corrupt([&] { apply(x, e); });
		if (!thrown || !(x == base)) { std::fprintf(stderr, "replay check not enforced: %s\n", what); }
		CHECK(thrown);
		CHECK(x == base);
	};
	using sc = std::uint32_t;
	rejects("generations contiguous", [](log_edit& e) { ++e.generation; });
	rejects("a removed id present", [](log_edit& e) { e.segments_removed = {4}; });
	rejects("an added id absent", [](log_edit& e) { e.segments_added[0].id = 6; });
	rejects("an added id below the next segment id", [](log_edit& e) {
		e.segments_added[0].id = 9;
		e.scalars &= ~(sc(1) << log_scalar::next_segment);
	});
	rejects("an added id not zero", [](log_edit& e) { e.segments_added[0].id = 0; });
	// The next id never goes back (SR1): lowering it keeps every live id below it here, then
	// drops it under live ids, which would leave a state no snapshot decodes.
	auto next_id = [&](std::uint64_t to) {
		return [&base, to](log_edit& e) {
			e = log_edit{};
			e.generation = base.generation + 1;
			e.scalars = sc(1) << log_scalar::next_segment;
			e.values[log_scalar::next_segment] = to;
		};
	};
	rejects("the next segment id never lowered", next_id(base.next_segment - 1));
	rejects("the next segment id never below a live id", next_id(1));
	{  // raising it alone applies
		log_edit e;
		e.generation = base.generation + 1;
		e.scalars = sc(1) << log_scalar::next_segment;
		e.values[log_scalar::next_segment] = base.next_segment + 5;
		log_state x = base;
		apply(x, e);
		CHECK(x.next_segment == base.next_segment + 5 && decode_snapshot(encode_snapshot(x)) == x);
	}
	rejects("an added identity new", [&](log_edit& e) { e.segments_added[0].identity = base.segments.at(5).identity; });
	// Deletes with the totals carried to match, so only the check named fails.
	auto deletes = [](log_edit& e, log_deletes d, std::uint64_t dead, std::uint64_t length) {
		e.deletes = {std::move(d)};
		e.values[log_scalar::live_count] -= dead;
		e.values[log_scalar::live_length] -= length;
	};
	rejects("a new ordinal below the document count", [&](log_edit& e) { deletes(e, {6, {40}, 39, 150}, 1, 10); });
	rejects("a new ordinal not yet deleted", [&](log_edit& e) { deletes(e, {5, {4000}, 99996, 399990}, 1, 10); });
	rejects("deletes in a segment present", [&](log_edit& e) { deletes(e, {4, {1}, 0, 0}, 1, 0); });
	rejects("the segment's live count after", [&](log_edit& e) { deletes(e, {6, {1}, 40, 150}, 1, 10); });
	{  // and the same deletes, valid, apply
		log_edit e = good;
		deletes(e, {6, {1, 39}, 38, 150}, 2, 10);
		log_state x = base;
		apply(x, e);
		CHECK(x.segments.at(6).deleted == (std::vector<std::uint64_t>{1, 39}) && x.live_count == flushed.live_count - 2 && snapshot_size(x) == encode_snapshot(x).size());
	}
	rejects("a removed key present", [](log_edit& e) { e.metadata_removed = {"absent"}; });
	rejects("the table size extended is the table's", [](log_edit& e) { e.fields_from = 1; });
	rejects("a declared name new", [](log_edit& e) { e.fields[0].name = "title"; });
	rejects("one key field", [](log_edit& e) { e.fields[0] = decl("k2", field_postings::docs, false, field_role::key); });
	rejects("pins at most the generation", [](log_edit& e) { e.pins_added = {5}; });
	rejects("a removed pin present", [](log_edit& e) { e.pins_removed = {3}; });
	rejects("the document total a running sum", [](log_edit& e) { ++e.values[log_scalar::doc_count]; });
	rejects("the live total a running sum", [](log_edit& e) { ++e.values[log_scalar::live_count]; });
	rejects("the live-length total a running sum", [](log_edit& e) { ++e.values[log_scalar::live_length]; });
	rejects("live length within the total length", [](log_edit& e) {
		e.scalars |= sc(1) << log_scalar::total_length;
		e.values[log_scalar::total_length] = 1;
	});
	rejects("segment versions with segments", [](log_edit& e) {
		e.scalars |= sc(1) << log_scalar::min_version;
		e.values[log_scalar::min_version] = 0;
	});
	rejects("attributes of a segment present", [](log_edit& e) { e.segment_attributes = {{4, {}}}; });

	// The totals: removing a segment without carrying the new totals is caught by the sums.
	log_edit merge;
	merge.generation = base.generation + 1;
	merge.segments_removed = {6};
	log_state x = base;
	CHECK(corrupt([&] { apply(x, merge); }) && x == base);
	merge.scalars = sc(1) << log_scalar::doc_count | sc(1) << log_scalar::live_count | sc(1) << log_scalar::live_length;
	merge.values[log_scalar::doc_count] = base.doc_count - 40;
	merge.values[log_scalar::live_count] = base.live_count - 40;
	merge.values[log_scalar::live_length] = base.live_length - 160;
	apply(x, merge);
	CHECK(x.segments.size() == 2 && x.doc_count == base.doc_count - 40 && snapshot_size(x) == encode_snapshot(x).size());
}

// The decoder: every prefix, every masked bit, each canonical-form rule, and counts the bytes
// left cannot hold.
void decoder() {
	const fixture_log f = fixture_edits();
	const std::string snap = encode_snapshot(f.base);
	for (std::size_t n = 0; n < snap.size(); ++n) { CHECK(corrupt([&] { decode_snapshot(snap.substr(0, n)); })); }
	CHECK(corrupt([&] { decode_snapshot(snap + '\0'); }));
	for (const auto& [name, e] : f.edits) {
		const std::string bytes = encode_edit(e);
		for (std::size_t n = 0; n < bytes.size(); ++n) { CHECK(corrupt([&] { decode_edit(bytes.substr(0, n)); })); }
		CHECK(corrupt([&] { decode_edit(bytes + '\0'); }));
	}
	{  // segment id 0, in a snapshot and in an edit: ids start at 1
		log_state zero = f.base;
		log_segment s = zero.segments.begin()->second;
		zero.segments.erase(zero.segments.begin());
		s.id = 0;
		zero.segments.emplace(0, s);
		CHECK(corrupt([&] { decode_snapshot(encode_snapshot(zero)); }));
		log_edit e = f.edits[0].second;
		for (const auto& [name, x] : f.edits) {
			if (!x.segments_added.empty()) { e = x; }
		}
		CHECK(!e.segments_added.empty());
		e.segments_added[0].id = 0;
		CHECK(corrupt([&] { decode_edit(encode_edit(e)); }));
	}
	CHECK(corrupt([&] { decode_edit(snap); }) && corrupt([&] { decode_snapshot(encode_edit(f.edits[0].second)); }));
	// An overlong integer: generation 1 as 81 00.
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x81\x00\x00\x00", 5)); }));
	CHECK(decode_edit(std::string("\x02\x01\x00\x00", 4)).generation == 1);
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x00\x00\x00", 4)); }));                     // generation 0
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x01\x80\x80\x01\x00", 6)); }));             // scalar bit 14
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x01\x00\x80\x01", 5)); }));                 // section bit 7
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x01\x00\x01\x00\x00", 6)); }));             // an empty pins section
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x01\x00\x04\x01\x05\x00\x00\x00", 9)); })); // deletes with no ordinals
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x01\x00\x01\xff\xff\x03", 7)); }));         // a count past the bytes left
	CHECK(corrupt([&] { decode_edit(std::string("\x02\x01\x00\x01\x02\x05\x05\x00", 8)); }));     // pins not ascending
	// Readability first: another files version, a structure bit, or a segment version this binary lacks.
	{
		std::string other = snap;
		other[2] = char(0x88);  // version 0x8000'0008
		CHECK(throws<unsupported_index>([&] { decode_snapshot(other); }));
		CHECK(throws<unsupported_index>([&] { decode_snapshot(snap, readable_by{15}); }));
		CHECK(throws<unsupported_index>([&] { decode_snapshot(snap, readable_by{~std::uint64_t(0), 14, 20}); }));
		log_edit e;
		e.generation = 4;
		e.scalars = 1u << log_scalar::mask;
		e.values[log_scalar::mask] = 1u << 9;
		CHECK(throws<unsupported_index>([&] { decode_edit(encode_edit(e), readable_by{0xff}); }));
		e.scalars = 1u << log_scalar::max_version;
		e.values[log_scalar::mask] = 0;
		e.values[log_scalar::max_version] = 21;
		CHECK(throws<unsupported_index>([&] { decode_edit(encode_edit(e), readable_by{~std::uint64_t(0), 0, 20}); }));
		CHECK(decode_edit(encode_edit(e), readable_by{~std::uint64_t(0), 0, 21}) == e);
		e.values[log_scalar::max_version] = std::uint64_t(1) << 32;
		CHECK(corrupt([&] { decode_edit(encode_edit(e)); }));  // versions fit 32 bits
	}
	// One encoding per deleted set: none only without deletes, the smaller of bitset and deltas.
	{
		log_state s = f.base;
		log_edit e;
		e.generation = s.generation + 1;
		log_segment seg{9, {}, 1, 1, 64, 63, 1, {5}};
		seg.identity.fill(0x42);
		e.segments_added = {seg};
		std::string ok = encode_edit(e);
		CHECK(decode_edit(ok).segments_added[0] == seg);
		// The entry: id 09, 16 identity bytes, size 01, crc 01, 64 (40), 63 (3f), 1 (01), then
		// the encoding: deltas (02), one byte (01), 05; a bitset would take 8 bytes.
		const std::size_t at = ok.find(std::string(16, char(0x42))) + 16 + 5;
		CHECK(ok.substr(at, 3) == std::string("\x02\x01\x05", 3));
		std::string as_bitset = ok.substr(0, at) + std::string("\x01\x08\x20\x00\x00\x00\x00\x00\x00\x00", 10) + ok.substr(at + 3);
		CHECK(corrupt([&] { decode_edit(as_bitset); }));  // a valid bitset, but deltas are smaller
		std::string none = ok.substr(0, at) + std::string("\x00", 1) + ok.substr(at + 3);
		CHECK(corrupt([&] { decode_edit(none); }));
		std::string short_bitset = ok.substr(0, at) + std::string("\x01\x07\x20\x00\x00\x00\x00\x00\x00", 9) + ok.substr(at + 3);
		CHECK(corrupt([&] { decode_edit(short_bitset); }));  // 64 documents take 8 bytes
	}
	// A count no input can hold fails before anything is allocated for it.
	{
		std::string big("\x02\x01\x00\x02\x00", 5);
		char buf[10];
		big.append(buf, std::size_t(varint::leb128::encode(buf, std::uint64_t(1) << 60) - buf));
		CHECK(corrupt([&] { decode_edit(big); }));
		log_segment huge{9, {}, 1, 1, std::uint64_t(1) << 62, (std::uint64_t(1) << 62) - 1, 1, {12345}};
		log_edit e;
		e.generation = 2;
		e.segments_added = {huge};  // deltas: one byte per delete, whatever the document count
		CHECK(decode_edit(encode_edit(e)) == e);
		// The same entry claiming 2^62 deletes in its 2-byte body: refused before any reserve.
		std::string lie("\x02\x02\x00\x02\x00\x01\x09", 7);
		lie += std::string(16, '\0');
		lie += std::string("\x01\x01", 2);
		lie.append(buf, std::size_t(varint::leb128::encode(buf, std::uint64_t(1) << 62) - buf));
		lie += std::string("\x00\x01\x02\x02\xb9\x60\x00\x00", 8);  // live 0, live length 1, deltas of 2 bytes, 12345
		CHECK(corrupt([&] { decode_edit(lie); }));
		lie[lie.size() - 6] = 1;  // a 2-byte bitset for 2^62 documents
		CHECK(corrupt([&] { decode_edit(lie); }));
	}
}

// The tracked size must follow every edit: S from the last snapshot written is the mutant C7 names.
void tracked_size() {
	rng r(77);
	log_state d = random_state(r);
	const std::uint64_t written = snapshot_bytes(d);
	bool moved = false;
	for (int i = 0; i < 200; ++i) {
		const log_state c = successor(d, r);
		apply(d, diff(d, c));
		CHECK(snapshot_size(d) == encode_snapshot(d).size());
		CHECK(snapshot_bytes(d) == framed(d));
		moved |= snapshot_bytes(d) != written;
	}
	CHECK(moved);
}

// --- the log file (step 3): the reader's caller rules, crash images, bit flips, D7's bounds on
// read, replay cost in proportion to the edit, allocation failure, and the appender ---

// A log as the writer leaves it: a snapshot, then edits while D7's bounds hold.
struct built_log {
	std::string bytes;
	std::vector<log_state> states;     // states[k]: after k edits
	std::vector<std::uint64_t> ends;   // ends[k]: where record k ends
};
built_log build_log(log_state d, const std::vector<log_state>& next) {
	built_log b;
	fr::Emitter emitter(log_stream(d.uuid, d.generation), 0);
	emitter.record(encode_snapshot(d));
	b.states.push_back(d);
	b.ends.push_back(emitter.position());
	for (const auto& c : next) {
		const std::string record = encode_edit(diff(d, c));
		if (!log_within(c, fr::record_end(log_stream(d.uuid, b.states[0].generation), emitter.position(), record.size()), b.states.size(), 1)) { break; }
		emitter.record(record);
		b.states.push_back(c);
		b.ends.push_back(emitter.position());
		d = c;
	}
	b.bytes = emitter.bytes();
	return b;
}
built_log random_log(std::uint64_t seed, int edits) {
	rng r(seed);
	const log_state first = random_state(r);
	log_state d = first;
	std::vector<log_state> next;
	for (int i = 0; i < edits; ++i) { next.push_back(d = successor(d, r)); }
	return build_log(first, next);
}
built_log fixture_built() {
	const fixture_log f = fixture_edits();  // as decoded: reading drops unknown even attribute tags
	const log_state base = decode_snapshot(encode_snapshot(f.base));
	std::vector<log_state> next;
	log_state s = base;
	for (const auto& [name, e] : f.edits) {
		apply(s, decode_edit(encode_edit(e)));
		next.push_back(s);
	}
	return build_log(base, next);
}
// Small logs for the exhaustive checks: the fixtures and random ones under `cap` bytes.
std::vector<built_log> small_logs(std::size_t count, std::size_t cap, int edits) {
	std::vector<built_log> out{fixture_built()};
	for (std::uint64_t seed = 1; out.size() < count && seed < 1000; ++seed) {
		built_log b = random_log(seed, edits);
		if (b.bytes.size() <= cap && b.states.size() > 3) { out.push_back(std::move(b)); }
	}
	return out;
}

// D6.3's caller rules (A6): a scan that stops inside the first record is damage whatever the tail
// verdict; the snapshot names the log's g and the blocks' UUID; another build's manifest is
// unsupported.
void caller_rules() {
	rng r(5);
	log_state big = random_state(r);
	while (snapshot_bytes(big) < 1600) { big = random_state(r); }
	const std::string log = frame_snapshot(big);
	const std::uint64_t g = big.generation;
	CHECK(read_log(log, g).state == big);
	CHECK(read_log(log, g).end == log.size() && !read_log(log, g).dirty && read_log(log, g).edits == 0);
	// Every cut at a sector inside the snapshot reads as torn to the framing; the caller refuses it.
	int cuts = 0;
	for (std::uint64_t at = 512; at < log.size(); at += 512) {
		CHECK(fr::scan(std::string_view(log).substr(0, at), log_stream(big.uuid, g)).tail == fr::Tail::torn);
		CHECK(corrupt([&] { read_log(std::string_view(log).substr(0, at), g); }));
		++cuts;
	}
	CHECK(cuts >= 3);
	// The snapshot's last sector never written: torn by zeros to the framing, damage here.
	std::string zeroed = log;
	std::fill(zeroed.begin() + std::ptrdiff_t((log.size() - 1) / 512 * 512), zeroed.end(), '\0');
	CHECK(fr::scan(zeroed, log_stream(big.uuid, g)).tail == fr::Tail::torn);
	CHECK(corrupt([&] { read_log(zeroed, g); }));
	CHECK(corrupt([&] { read_log("", g); }));
	CHECK(corrupt([&] { read_log(std::string(512, '\0'), g); }));
	// The snapshot's generation is the log's: a log named g holding a snapshot of g + 1.
	{
		log_state other = big;
		++other.generation;
		fr::Emitter emitter(log_stream(big.uuid, g), 0);
		emitter.record(encode_snapshot(other));
		CHECK(fr::scan(emitter.bytes(), log_stream(big.uuid, g)).tail == fr::Tail::clean);
		CHECK(corrupt([&] { read_log(emitter.bytes(), g); }));
		CHECK(read_log(frame_snapshot(other), g + 1).state == other);
		CHECK(corrupt([&] { read_log(frame_snapshot(other), g); }));  // named g: its blocks say g + 1
	}
	// The snapshot's UUID is the blocks' identity.
	{
		log_state other = big;
		other.uuid[0] ^= 1;
		fr::Emitter emitter(log_stream(big.uuid, g), 0);
		emitter.record(encode_snapshot(other));
		CHECK(corrupt([&] { read_log(emitter.bytes(), g); }));
	}
	// The first record is a snapshot.
	{
		const fixture_log f = fixture_edits();
		fr::Emitter emitter(log_stream(f.base.uuid, f.base.generation + 1), 0);
		emitter.record(encode_edit(f.edits[0].second));
		CHECK(corrupt([&] { read_log(emitter.bytes(), f.base.generation + 1); }));
	}
	// A manifest of the retired format is another build's index (D11).
	CHECK(throws<unsupported_index>([&] { read_log(std::string(manifest_magic, 8) + std::string(100, '\x01'), g); }));
	// to: replays to it, refuses one past the log's end or before its snapshot.
	const built_log b = fixture_built();
	const std::uint64_t g0 = b.states[0].generation;
	for (std::size_t k = 0; k < b.states.size(); ++k) {
		const log_read lr = read_log(b.bytes, g0, {}, g0 + k);
		CHECK(lr.state == b.states[k] && lr.end == b.ends[k] && lr.edits == k && lr.damaged == 0);
	}
	CHECK(throws<generation_not_retained>([&] { read_log(b.bytes, g0, {}, g0 + b.states.size()); }));
	CHECK(throws<generation_not_retained>([&] { read_log(b.bytes, g0, {}, g0 - 1); }));
	// A torn tail reaches only the last whole generation; damage after `to` is reported, not fatal.
	const std::string torn = b.bytes.substr(0, b.ends[b.ends.size() - 2]) + std::string(b.ends.back() - b.ends[b.ends.size() - 2], '\0');
	CHECK(read_log(torn, g0).dirty && read_log(torn, g0).state == b.states[b.states.size() - 2]);
	CHECK(throws<generation_not_retained>([&] { read_log(torn, g0, {}, g0 + b.states.size() - 1); }));
	std::string damaged = b.bytes;
	damaged[fr::scan(b.bytes, log_stream(b.states[0].uuid, g0)).records[3].offset + fr::fragment_header] ^= 4;  // record 3's first payload byte
	CHECK(corrupt([&] { read_log(damaged, g0); }));
	const log_read before = read_log(damaged, g0, {}, g0 + 2);
	CHECK(before.state == b.states[2] && before.damaged == g0 + 3);
}

// Every honest crash image of every append (A7): each sector the append touches old or new, at
// every size a crash leaves (the old end, the new end, each sector boundary between). The log
// opens at the previous generation (torn or clean) or the new one; never damage, never another
// state.
void crash_images() {
	std::uint64_t images = 0, torn = 0, whole = 0, previous = 0, misread = 0;
	rng pick(99);
	std::vector<built_log> logs = small_logs(40, 64 << 10, 12);
	for (const auto& b : logs) {
		const std::uint64_t g = b.states[0].generation;
		for (std::size_t k = 1; k < b.states.size(); ++k) {
			const std::string_view old = std::string_view(b.bytes).substr(0, b.ends[k - 1]), now = std::string_view(b.bytes).substr(0, b.ends[k]);
			const std::uint64_t first = old.size() / 512, sectors = (now.size() + 511) / 512 - first;
			std::vector<std::uint64_t> sizes{old.size(), now.size()};
			for (std::uint64_t s = (old.size() / 512 + 1) * 512; s < now.size(); s += 512) { sizes.push_back(s); }
			std::vector<std::uint64_t> masks;
			if (sectors <= 10) {
				for (std::uint64_t m = 0; m < (std::uint64_t(1) << sectors); ++m) { masks.push_back(m); }
			} else {
				for (int i = 0; i < 256; ++i) { masks.push_back(pick()); }
			}
			for (std::uint64_t size : sizes) {
				for (std::uint64_t mask : masks) {
					std::string image(size, '\0');
					for (std::uint64_t at = 0; at < size; at += 512) {
						const std::uint64_t j = at / 512;
						const bool take_new = j < first || (j - first < 64 && (mask >> (j - first) & 1));
						const std::string_view from = take_new ? now : old;
						if (at < from.size()) { std::memcpy(image.data() + at, from.data() + at, std::min<std::uint64_t>({512, size - at, from.size() - at})); }
					}
					++images;
					try {
						const log_read lr = read_log(image, g);
						if (lr.state == b.states[k] && lr.end == b.ends[k] && !lr.dirty) {
							++whole;
						} else if (lr.state == b.states[k - 1] && lr.end == b.ends[k - 1]) {
							++(lr.dirty ? torn : previous);
						} else {
							++misread;
						}
					} catch (const std::exception&) {
						++misread;
					}
				}
			}
		}
	}
	std::printf("  crash images: %llu over %zu logs, %llu whole, %llu torn, %llu previous clean, %llu misread\n", (unsigned long long)images, logs.size(), (unsigned long long)whole,
		(unsigned long long)torn, (unsigned long long)previous, (unsigned long long)misread);
	CHECK(misread == 0);
	CHECK(torn > 0 && whole > 0 && previous > 0);
}

// Every single-bit flip in a synced log (A7): the open fails, or reads the same head (a flip in
// padding no reader reads); none reads another state or a shorter log silently.
void bit_flips() {
	std::uint64_t flips = 0, failed = 0, same = 0, silent = 0;
	for (const auto& b : small_logs(4, 3000, 8)) {
		const std::uint64_t g = b.states[0].generation;
		std::string bytes = b.bytes;
		for (std::size_t at = 0; at < bytes.size(); ++at) {
			for (int bit = 0; bit < 8; ++bit) {
				bytes[at] = char(bytes[at] ^ (1 << bit));
				++flips;
				try {
					const log_read lr = read_log(bytes, g);
					if (lr.state == b.states.back() && lr.end == b.ends.back() && !lr.dirty) {
						++same;
					} else {
						++silent;
					}
				} catch (const corrupt_index&) {
					++failed;
				} catch (const std::exception&) {
					++silent;  // anything but damage misreports a flip
				}
				bytes[at] = char(bytes[at] ^ (1 << bit));
			}
		}
	}
	std::printf("  bit flips: %llu, %llu fail the open, %llu in padding, %llu silent\n", (unsigned long long)flips, (unsigned long long)failed, (unsigned long long)same,
		(unsigned long long)silent);
	CHECK(silent == 0);
	CHECK(flips > 20000 && failed + same == flips);
}

// D7's bounds on read (A3, SR2-strata): a log reads while every record end is within twice the
// bounds the writer keeps, and is damage past them, by edits and by bytes.
void bounds_on_read() {
	auto check_prefixes = [](const log_state& base, const std::vector<log_edit>& edits) {
		fr::Emitter emitter(log_stream(base.uuid, base.generation), 0);
		emitter.record(encode_snapshot(base));
		log_state s = base;
		bool within = true, passed = false;
		std::uint64_t k = 0;
		for (const auto& e : edits) {
			emitter.record(encode_edit(e));
			apply(s, e);
			within = within && log_within(s, emitter.position(), ++k, 2);
			const bool reads = !corrupt([&] { read_log(emitter.bytes(), base.generation); });
			CHECK(reads == within);
			passed |= !within;
		}
		CHECK(passed);  // the log ran past the bound
	};
	const fixture_log f = fixture_edits();
	// Edits: a key set and removed in turn, so the state stays and R governs.
	std::vector<log_edit> toggles;
	log_state s = f.base;
	for (int i = 0; i < 2 * int(log_replay_floor) + 8; ++i) {
		log_state c = s;
		++c.generation;
		if (c.metadata.count("toggle")) {
			c.metadata.erase("toggle");
		} else {
			c.metadata["toggle"] = "x";
		}
		recount(c);
		toggles.push_back(diff(s, c));
		s = c;
	}
	check_prefixes(f.base, toggles);
	// Bytes: a 1,000-byte value set and removed in turn, so F governs before R.
	std::vector<log_edit> large;
	s = f.base;
	for (int i = 0; i < 80; ++i) {
		log_state c = s;
		++c.generation;
		if (c.metadata.count("large")) {
			c.metadata.erase("large");
		} else {
			c.metadata["large"] = std::string(1000, char('a' + i % 26));
		}
		recount(c);
		large.push_back(diff(s, c));
		s = c;
	}
	check_prefixes(f.base, large);
	CHECK(log_within(f.base, log_space_floor, log_replay_floor) && !log_within(f.base, log_space_floor + 1, 1) && !log_within(f.base, 1, log_replay_floor + 1));
}

// Replay costs time in proportion to the edit (A3, SR2-strata), measured as a ratio so the
// machine's speed cancels: ten times the declarations in one edit, and edits that each add a
// segment onto a state of a hundred times the segments.
double seconds(const std::function<void()>& f) {
	const auto t0 = std::chrono::steady_clock::now();
	f();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
void replay_cost() {
	auto declarations = [](std::size_t n) {
		log_state s;
		s.uuid = uuid;
		s.generation = 1;
		recount(s);
		log_edit e;
		e.generation = 2;
		for (std::size_t i = 0; i < n; ++i) { e.fields.push_back(decl("field-" + std::to_string(i))); }
		return seconds([&] { apply(s, e); });
	};
	const double fields_ratio = declarations(40000) / declarations(4000);
	auto additions = [](std::uint64_t segments) {
		log_state s;
		s.uuid = uuid;
		s.generation = 1;
		s.min_version = s.max_version = 13;
		for (std::uint64_t id = 1; id <= segments; ++id) {
			log_segment seg;
			seg.id = id;
			std::memcpy(seg.identity.data(), &id, 8);
			seg.doc_count = seg.live_count = 1;
			s.segments.emplace(id, seg);
		}
		s.next_segment = segments + 1;
		s.doc_count = s.live_count = segments;
		recount(s);
		std::vector<log_edit> edits;
		for (std::uint64_t i = 0; i < 2000; ++i) {
			log_edit e;
			e.generation = s.generation + 1 + i;
			const std::uint64_t id = segments + 1 + i;
			log_segment seg;
			seg.id = id;
			std::memcpy(seg.identity.data(), &id, 8);
			seg.doc_count = seg.live_count = 1;
			e.segments_added.push_back(seg);
			e.scalars = 1u << log_scalar::next_segment | 1u << log_scalar::doc_count | 1u << log_scalar::live_count;
			e.values[log_scalar::next_segment] = id + 1;
			e.values[log_scalar::doc_count] = e.values[log_scalar::live_count] = segments + 1 + i;
			edits.push_back(std::move(e));
		}
		return seconds([&] {
			for (const auto& e : edits) { apply(s, e); }
		});
	};
	const double segments_ratio = additions(100000) / additions(1000);
	std::printf("  replay cost: 10x declarations in one edit %.1fx the time; added segments on 100x the state %.1fx\n", fields_ratio, segments_ratio);
	CHECK(fields_ratio < 40);    // the per-name scan made it about 100x
	CHECK(segments_ratio < 20);  // the per-edit identity scan made it about 100x
}

// An allocation failure anywhere in a read drops the whole state (A5, SR4): read_log returns the
// head whole or throws bad_alloc, never a shorter or half-applied state and never damage.
void allocation_failure() {
	const built_log b = fixture_built();
	const std::uint64_t g = b.states[0].generation;
	allocations = 0;
	const log_read whole = read_log(b.bytes, g);
	const long total = allocations;
	CHECK(whole.state == b.states.back() && total > 10);
	long failed = 0, succeeded = 0, misreported = 0;
	for (long n = 0; n < total; ++n) {
		allocations_left = n;
		try {
			const log_read lr = read_log(b.bytes, g);
			allocations_left = -1;
			++(lr.state == b.states.back() && lr.end == b.ends.back() && !lr.dirty ? succeeded : misreported);
		} catch (const std::bad_alloc&) {
			allocations_left = -1;
			++failed;
		} catch (...) {
			allocations_left = -1;
			++misreported;
		}
	}
	std::printf("  allocation failure: %ld points, %ld throw bad_alloc, %ld read whole, %ld misreported\n", total, failed, succeeded, misreported);
	CHECK(misreported == 0 && failed > total / 2 && failed + succeeded == total);
}

// A File that counts and records its writes, over faultfs.
struct counting_file final : kronuz::journal::File {
	explicit counting_file(std::unique_ptr<kronuz::journal::File> f) : f_(std::move(f)) {}
	std::uint64_t size() override { return f_->size(); }
	std::size_t read_at(std::uint64_t o, std::span<char> b) override { return f_->read_at(o, b); }
	std::size_t write_at(std::uint64_t o, std::string_view b) override {
		writes.emplace_back(o, b.size());
		return f_->write_at(o, b);
	}
	void truncate(std::uint64_t s) override { f_->truncate(s); }
	void sync() override { f_->sync(); }
	void sync_data() override {
		++syncs;
		f_->sync_data();
	}
	std::vector<std::pair<std::uint64_t, std::size_t>> writes;
	int syncs = 0;
	std::unique_ptr<kronuz::journal::File> f_;
};

// The appender (D4): one write per append, one barrier; a short or failed write truncated back
// and synced, so the log stays clean and the retry appends at the old end; one record in flight.
void appender() {
	const built_log b = fixture_built();
	const log_state& base = b.states[0];
	const std::uint64_t g = base.generation;
	const std::string name = "manifest-" + std::to_string(g);
	std::vector<std::string> records;
	for (std::size_t k = 1; k < b.states.size(); ++k) { records.push_back(encode_edit(diff(b.states[k - 1], b.states[k]))); }
	auto fresh = [&](faultfs::filesystem& fs) {
		faultfs::io io(fs);
		auto f = io.create_exclusive(name);
		const std::string snap = frame_snapshot(base);
		f->write_at(0, snap);
		f->sync();
		io.sync_directory();
		return std::make_unique<counting_file>(std::move(f));
	};
	{  // one write and one sync_data per append; the bytes are the emitter's
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0]);
		for (std::size_t k = 0; k < records.size(); ++k) {
			const std::size_t writes = f->writes.size();
			CHECK(a.end_after(records[k].size()) == b.ends[k + 1]);
			a.append(records[k]);
			CHECK(f->writes.size() == writes + 1 && f->writes.back().first == b.ends[k]);
			CHECK(a.end() == b.ends[k + 1] && f->syncs == int(k + 1));
		}
		CHECK(std::string(fs.view(name)) == b.bytes);
		CHECK(read_log(fs.view(name), g).state == b.states.back());
	}
	{  // pieces: each ends on a sector boundary, and together they are the same bytes
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0], 600);  // 512-byte pieces
		log_state big = base;
		++big.generation;
		big.metadata["big"] = std::string(3000, 'q');
		recount(big);
		const std::string record = encode_edit(diff(base, big));
		a.append(record);
		CHECK(f->writes.size() > 5);
		for (std::size_t i = 0; i + 1 < f->writes.size(); ++i) { CHECK((f->writes[i].first + f->writes[i].second) % 512 == 0 && f->writes[i].second <= 512); }
		CHECK(read_log(fs.view(name), g).state == big);
	}
	{  // a short write: undone and synced, then the retry appends at the old end
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0]);
		a.append(records[0]);
		fs.short_writes(7);
		const int syncs = f->syncs;
		CHECK(throws<std::system_error>([&] { a.append(records[1]); }));
		fs.short_writes(0);
		CHECK(fs.view(name).size() == b.ends[1] && f->syncs == syncs + 1 && !a.broken() && !a.in_flight() && a.end() == b.ends[1]);
		fs.crash(3);  // the truncate-back is durable
		CHECK(fs.view(name).size() == b.ends[1]);
		faultfs::io io(fs);
		auto again = io.open_existing(name);
		log_appender retry(*again, base.uuid, g, b.ends[1]);
		retry.append(records[1]);
		CHECK(read_log(fs.view(name), g).state == b.states[2]);
	}
	{  // ENOSPC: the same, and the error is the caller's to report as space
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0]);
		fs.set_capacity(fs.used() + 10);
		bool space = false;
		try {
			a.append(records[0]);
		} catch (const std::system_error& e) {
			space = e.code().value() == ENOSPC;
		}
		CHECK(space && fs.view(name).size() == b.ends[0] && !a.broken());
		fs.set_capacity(~std::uint64_t(0));
		a.append(records[0]);
		CHECK(read_log(fs.view(name), g).state == b.states[1] && !read_log(fs.view(name), g).dirty);
	}
	{  // a failed truncate-back breaks the appender: no later append
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0]);
		fs.add_fault({faultfs::op::write});
		fs.add_fault({faultfs::op::truncate});
		CHECK(throws<std::system_error>([&] { a.append(records[0]); }));
		CHECK(a.broken());
		fs.clear_faults();
		CHECK(throws<std::logic_error>([&] { a.append(records[0]); }));
	}
	{  // a failed barrier breaks it too
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0]);
		fs.add_fault({faultfs::op::sync});
		CHECK(throws<std::system_error>([&] { a.append(records[0]); }));
		CHECK(a.broken() && a.end() == b.ends[0]);
		fs.clear_faults();
		CHECK(throws<std::logic_error>([&] { a.append(records[0]); }));
	}
	{  // one record in flight
		faultfs::filesystem fs;
		auto f = fresh(fs);
		log_appender a(*f, base.uuid, g, b.ends[0]);
		a.write(records[0]);
		CHECK(a.in_flight() && a.end() == b.ends[0]);
		CHECK(throws<std::logic_error>([&] { a.write(records[1]); }));
		a.sync();
		a.write(records[1]);
		a.sync();
		CHECK(read_log(fs.view(name), g).state == b.states[2]);
	}
	{  // a power loss after the write and before its barrier: the previous generation or the new one
		for (std::uint64_t seed = 0; seed < 64; ++seed) {
			faultfs::filesystem fs;
			auto f = fresh(fs);
			log_appender a(*f, base.uuid, g, b.ends[0]);
			a.append(records[0]);
			a.write(records[1]);
			fs.crash(seed);
			const log_read lr = read_log(fs.view(name), g);
			CHECK((lr.state == b.states[1] && lr.end == b.ends[1]) || (lr.state == b.states[2] && !lr.dirty));
		}
	}
}

void stage(const char* name, const std::function<void()>& f) {
	const int before = failures;
	try {
		f();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "%s: unexpected exception: %s\n", name, e.what());
		++failures;
	}
	std::printf("%s: %s\n", name, failures == before ? "ok" : "FAILED");
}

// The fuzz target's seeds: logs as [length LEB128][record]..., a snapshot first.
void write_seeds(const std::filesystem::path& dir) {
	std::filesystem::create_directories(dir);
	auto frame = [](std::string& out, const std::string& record) {
		char buf[10];
		out.append(buf, std::size_t(varint::leb128::encode(buf, record.size()) - buf));
		out += record;
	};
	int n = 0;
	auto save = [&](const std::string& bytes) { std::ofstream(dir / ("seed-" + std::to_string(n++)), std::ios::binary) << bytes; };
	const fixture_log f = fixture_edits();
	std::string log;
	frame(log, encode_snapshot(f.base));
	for (const auto& [name, e] : f.edits) {
		frame(log, encode_edit(e));
		save(log);
	}
	// One-scalar edits, each scalar lowered to 0 and raised by one, so mutation starts next to
	// the scalar checks (SR5).
	const log_detail::scalar_values base = log_detail::scalars_of(f.base);
	for (unsigned i = 0; i < log_scalar::count; ++i) {
		for (const std::uint64_t v : {std::uint64_t(0), base[i] + 1}) {
			log_edit e;
			e.generation = f.base.generation + 1;
			e.scalars = std::uint32_t(1) << i;
			e.values[i] = v;
			std::string one;
			frame(one, encode_snapshot(f.base));
			frame(one, encode_edit(e));
			save(one);
		}
	}
	for (std::uint64_t seed = 1; seed <= 24; ++seed) {
		rng r(seed);
		log_state d = random_state(r);
		std::string out;
		frame(out, encode_snapshot(d));
		for (int i = 0; i < 6; ++i) {
			const log_state c = successor(d, r);
			frame(out, encode_edit(diff(d, c)));
			d = c;
		}
		save(out);
	}
}

}  // namespace

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "--seeds") {
		write_seeds(argv[2]);
		return 0;
	}
	stage("layout", layout);
	stage("fixtures", fixtures);
	stage("round trip", [] {
		for (std::uint64_t seed = 1; seed <= 40; ++seed) { round_trip(seed, 60); }
	});
	stage("replay checks", replay_checks);
	stage("decoder", decoder);
	stage("tracked size", tracked_size);
	stage("caller rules", caller_rules);
	stage("crash images", crash_images);
	stage("bit flips", bit_flips);
	stage("bounds on read", bounds_on_read);
	stage("replay cost", replay_cost);
	stage("allocation failure", allocation_failure);
	stage("appender", appender);
	std::printf("manifest_log: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
