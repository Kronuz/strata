// The manifest log's records (manifest-log D3): the layout byte by byte, the fixtures, the
// round trip over random pairs of states (T6's oracle at the codec), every replay check, the
// decoder's bounds, and the tracked snapshot size (the re-check's C7).
#include "strata/core/manifest_log.hh"

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
	std::printf("manifest_log: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
