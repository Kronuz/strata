// Format 14's step 1 (docs/design/format-14-kv.md, "First buildable step"): key-ordered
// document segments in 4 KiB codec-tagged blocks. The done criteria it proves (the ledger's K1
// to K3): fixtures pinned at the pre-freeze version and format 13's segments refused; check()
// refusing a segment out of key order; merges in key order through the ascending path with no
// key map. Also index_sort's reader rules, the stored codecs and block sizes, and the writer's
// refusals. Core headers only.
#include "kv_test.hh"
#include "segment_edit.hh"
#include "strata/core/check.hh"
#include "strata/core/merge.hh"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <span>

using segment = basic_segment<document_layout>;
using writer = basic_segment_writer<document_layout>;

static const auto uuid = uuid_of("format14");

static std::string read_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void write_file(const std::string& path, std::string_view bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), std::streamsize(bytes.size()));
}

enum class outcome { ok, corrupt, unsupported, other };
// Opens the bytes as a document-layout segment, checks it whole, and reads every record.
static outcome open_check(std::string_view bytes, std::string* what = nullptr) {
	try {
		const segment s(bytes);
		basic_check_report<document_layout> r;
		check_segment(s, nullptr, r);
		typename stored_plugin::reader::stored_frame frame;
		for (ordinal_t o = 0; o < s.core().doc_count(); ++o) { (void)s.get<stored_plugin>().stored(o, frame); }
		return outcome::ok;
	} catch (const corrupt_segment& e) {
		if (what) { *what = e.what(); }
		return outcome::corrupt;
	} catch (const unsupported_segment& e) {
		if (what) { *what = e.what(); }
		return outcome::unsupported;
	} catch (const std::exception& e) {
		if (what) { *what = e.what(); }
		return outcome::other;
	}
}
static bool corrupt_with(std::string_view bytes, const char* fragment) {
	std::string what;
	const outcome o = open_check(bytes, &what);
	if (o != outcome::corrupt || what.find(fragment) == std::string::npos) {
		std::fprintf(stderr, "  expected corrupt (%s), got %d: %s\n", fragment, int(o), what.c_str());
		return false;
	}
	return true;
}

// Each segment file of an index on faultfs, by name.
static std::map<std::string, std::string> segment_files(const faultfs::filesystem& fs) {
	std::map<std::string, std::string> out;
	for (const auto& n : fs.list()) {
		if (n.starts_with("seg-") && n.ends_with(".sx")) { out[n] = std::string(fs.view(n)); }
	}
	return out;
}

// A record of `size` bytes, half compressible as db_bench's values are, from r.
static std::string value_of(std::mt19937_64& r, std::size_t size) {
	std::string v(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { v[i] = i < size / 2 ? char('a' + r() % 26) : char('a' + i % 7); }
	return v;
}

// The stored frames' codec bytes in order.
static std::vector<std::uint8_t> codecs_of(std::string_view file) {
	const std::string st = test::section_bytes(file, format::section::stored);
	std::vector<std::uint8_t> out;
	if (st.size() < 4) { return out; }
	const std::uint32_t frames = format::get<std::uint32_t>(st.data() + st.size() - 4);
	const std::size_t index_at = st.size() - 4 - 16 * std::size_t(frames);
	for (std::uint32_t f = 0; f < frames; ++f) { out.push_back(std::uint8_t(st[format::get<std::uint64_t>(st.data() + index_at + 16 * f + 8)])); }
	return out;
}
// The stored frames' uncompressed sizes in order.
static std::vector<std::uint32_t> raw_sizes_of(std::string_view file) {
	const std::string st = test::section_bytes(file, format::section::stored);
	std::vector<std::uint32_t> out;
	const std::uint32_t frames = format::get<std::uint32_t>(st.data() + st.size() - 4);
	const std::size_t index_at = st.size() - 4 - 16 * std::size_t(frames);
	for (std::uint32_t f = 0; f < frames; ++f) { out.push_back(format::get<std::uint32_t>(st.data() + index_at + 16 * f + 4)); }
	return out;
}

// --- K1: fixtures at the pre-freeze version; format 13's segments refused ---------------------

// Three deterministic indexes' segments: a flush of shuffled keys with keyless documents and a
// record past the block target; their merge with deletes; the same merge under the disk preset.
static std::map<std::string, std::string> fixture_segments() {
	std::map<std::string, std::string> out;
	for (const bool preset : {false, true}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.key_prefix = "Q";
		o.identity_seed = 14;
		o.zstd_merged = preset;
		auto ix = kv_index::create(io, o, uuid);
		std::mt19937_64 r(14);
		std::vector<int> ids(600);
		for (int i = 0; i < 600; ++i) { ids[i] = i; }
		for (std::size_t i = ids.size() - 1; i > 0; --i) { std::swap(ids[i], ids[r() % (i + 1)]); }  // std::shuffle differs by library
		for (int s = 0; s < 2; ++s) {
			for (int j = s * 300; j < (s + 1) * 300; ++j) {
				char k[16];
				std::snprintf(k, sizeof k, "Q%05d", ids[j]);
				CHECK(ix.apply(tx({replace(k, value_of(r, j == 7 ? 9000 : 40 + r() % 300))}), {}).result == verdict::accepted);
				if (j % 50 == 0) {
					operation keyless;
					keyless.type = operation::add;
					keyless.doc = doc("", value_of(r, 64));
					CHECK(ix.apply(tx({keyless}), {}).result == verdict::accepted);
				}
			}
			ix.refresh();
		}
		ix.commit();
		if (!preset) {
			const auto flushed = segment_files(fs);
			CHECK(flushed.size() == 2);
			out["flushed.sx"] = flushed.begin()->second;
		}
		for (int j = 0; j < 600; j += 9) {
			char k[16];
			std::snprintf(k, sizeof k, "Q%05d", j);
			CHECK(ix.apply(tx({erase(k)}), {}).result == verdict::accepted);
		}
		ix.commit();
		ix.force_merge(1);
		ix.commit();
		CHECK(ix.segments().size() == 1);
		out[preset ? "merged-zstd1.sx" : "merged.sx"] = std::string(fs.view(index_detail::segment_name(ix.segments()[0].id)));
		ix.close();
	}
	return out;
}

static void fixtures() {
	const std::string dir = std::string(STRATA_FIXTURES) + "/format14";
	const auto now = fixture_segments();
	if (std::getenv("STRATA_WRITE_FIXTURES")) {
		std::filesystem::create_directories(dir);
		for (const auto& [name, bytes] : now) { write_file(dir + "/" + name, bytes); }
	}
	std::string all;
	for (const auto& [name, bytes] : now) {
		const std::string pinned = read_file(dir + "/" + name);
		CHECK(pinned == bytes);  // the bytes a build writes are the pinned ones, on every platform
		CHECK(format::read_version(pinned) == format::pre_freeze);
		CHECK(open_check(pinned) == outcome::ok);
		const segment s(pinned);
		CHECK(s.core().key_ordered() && s.core().structures() == (format::bit::keys | format::bit::stored | format::bit::index_sort));
		all += name + '\0' + std::to_string(pinned.size()) + '\0' + pinned;
	}
	// The codecs each fixture holds: LZ4 or raw in a flush and a default merge, zstd in the
	// preset's merge; the 9000-byte record takes a frame of its own.
	for (const auto& [name, bytes] : now) {
		std::set<std::uint8_t> seen;
		for (auto c : codecs_of(bytes)) { seen.insert(c); }
		CHECK(name == "merged-zstd1.sx" ? seen.count(std::uint8_t(format::codec::zstd)) && !seen.count(std::uint8_t(format::codec::lz4))
										: seen.count(std::uint8_t(format::codec::lz4)) && !seen.count(std::uint8_t(format::codec::zstd)));
	}
	CHECK(std::ranges::any_of(raw_sizes_of(now.at("flushed.sx")), [](std::uint32_t n) { return n > 9000; }) || std::ranges::any_of(raw_sizes_of(now.at("merged.sx")), [](std::uint32_t n) { return n > 9000; }));
	// C17: the fixtures' bytes change only with the pre-freeze version. pre_freeze.txt pins the
	// version beside a hash of every fixture, so a re-pin that keeps the version fails.
	{
		const std::string path = dir + "/pre_freeze.txt";
		unsigned long long was_version = 0, was_hash = 0;
		if (std::FILE* f = std::fopen(path.c_str(), "r")) {
			if (std::fscanf(f, "segments %llx %llx", &was_version, &was_hash) != 2) { was_version = was_hash = 0; }
			std::fclose(f);
		}
		const unsigned long long version = format::pre_freeze, hash = xxh64::hash(all);
		const bool bumped = hash == was_hash || version != was_version;
		if (!bumped) { std::fprintf(stderr, "pre_freeze.txt: the format-14 fixtures changed without a pre-freeze bump (C17)\n"); }
		CHECK(bumped);
		if (bumped && std::getenv("STRATA_WRITE_FIXTURES")) {
			char text[64];
			std::snprintf(text, sizeof text, "segments %08llx %016llx\n", version, hash);
			write_file(path, text);
		}
		char line[32];
		std::snprintf(line, sizeof line, "segments %08llx ", version);
		CHECK(read_file(path).starts_with(line));  // pinned at this version
	}
	// A segment format 13 wrote (test/fixtures/format13/document.sx, by main at 4d36aba) is
	// refused as unsupported, not damage, by the version alone; so are the version just
	// before this one and just after.
	{
		const std::string old = read_file(std::string(STRATA_FIXTURES) + "/format13/document.sx");
		CHECK(!old.empty() && format::get<std::uint32_t>(old.data() + 8) == 13);
		CHECK(open_check(old) == outcome::unsupported);
		CHECK(throws([&] { (void)format::read_version(old); }));
		const std::string good = now.at("flushed.sx");
		for (const std::uint32_t v : {format::pre_freeze - 1, format::pre_freeze + 1, std::uint32_t(13), std::uint32_t(14)}) {
			std::string bad = good;
			std::memcpy(bad.data() + 8, &v, 4);
			const std::uint32_t crc = ::kronuz::crc32c::crc32c(bad.data(), 12);
			std::memcpy(bad.data() + 12, &crc, 4);
			std::string what;
			CHECK(open_check(bad, &what) == outcome::unsupported);
		}
	}
	std::printf("  fixtures: %zu segments pinned at %08x\n", now.size(), unsigned(format::pre_freeze));
}

// --- index_sort's reader (F18 with the key component) -----------------------------------------

static std::string sort_section(std::initializer_list<std::tuple<std::uint32_t, int, int, int>> components, std::size_t count_override = ~std::size_t(0)) {
	std::string out;
	format::put_leb(out, count_override != ~std::size_t(0) ? count_override : components.size());
	for (const auto& [slot, direction, missing, selector] : components) {
		format::put<std::uint32_t>(out, slot);
		out.push_back(char(direction));
		out.push_back(char(missing));
		out.push_back(char(selector));
	}
	return out;
}

static void index_sort_rules() {
	const auto files = fixture_segments();
	const std::string good = files.at("flushed.sx");
	CHECK(test::section_bytes(good, format::section::index_sort) == format::encode_key_sort());
	CHECK(open_check(test::reseal(good, {})) == outcome::ok);  // the reseal itself changes nothing
	auto with_sort = [&](std::string bytes) { return test::reseal(good, {{format::section::index_sort, std::move(bytes)}}); };
	const std::uint32_t key = format::key_slot;
	CHECK(corrupt_with(with_sort(sort_section({}, 0)), "index_sort"));                                  // no component
	CHECK(corrupt_with(with_sort(sort_section({{key, 0, 1, 0}}) + "x"), "index_sort"));                 // trailing bytes
	CHECK(corrupt_with(with_sort(sort_section({{key, 0, 1, 0}}).substr(0, 5)), "index_sort"));          // truncated
	CHECK(corrupt_with(with_sort(sort_section({{key, 0, 1, 0}}, 2)), "index_sort"));                    // count past the bytes
	CHECK(corrupt_with(with_sort(sort_section({{key, 1, 1, 0}})), "key order"));                        // descending
	CHECK(corrupt_with(with_sort(sort_section({{key, 0, 0, 0}})), "key order"));                        // keyless first
	CHECK(corrupt_with(with_sort(sort_section({{key, 0, 1, 1}})), "key order"));                        // a selector
	CHECK(corrupt_with(with_sort(sort_section({{key, 2, 1, 0}})), "index_sort"));                       // a byte out of range
	CHECK(corrupt_with(with_sort(sort_section({{key, 0, 1, 0}, {key, 0, 1, 0}})), "key order"));        // not the only one
	CHECK(corrupt_with(with_sort(sort_section({{3, 0, 1, 0}})), "slot without values"));                // a slot, no values
	// The bit without the section, the section without the bit, and the bit without keys.
	const format::structure_bits bits = format::bit::keys | format::bit::stored | format::bit::index_sort;
	CHECK(corrupt_with(test::reseal(good, {{format::section::index_sort, std::nullopt}}), "missing section"));
	CHECK(corrupt_with(test::reseal(good, {}, bits & ~format::bit::index_sort), "section without its structure"));
	CHECK(corrupt_with(test::reseal(good, {{format::section::keys, std::nullopt}, {format::section::key_index, std::nullopt}, {format::section::versions, std::nullopt}},
						   format::bit::stored | format::bit::index_sort),
		"structure without its owner"));
	// A segment without index_sort is not key-ordered, and opens.
	{
		writer w;
		w.key_prefix = "Q";
		w.structures = format::bit::keys | format::bit::stored;
		document d;
		d.stored = "x";
		w.add({5, 1, "Qb"}, d, false);
		w.add({6, 1, "Qa"}, d, false);  // docid order, keys not: allowed without index_sort
		const std::string bytes = w.flush({});
		CHECK(open_check(bytes) == outcome::ok && !segment(bytes).core().key_ordered());
	}
	std::printf("  index_sort: its rules\n");
}

// --- K2 (T3): check() refuses a segment out of key order -------------------------------------

// A segment written in docid order with keys out of order, then claiming key order: what a
// writer that kept docid order (the mutant) would leave. check() refuses it; so do merges.
static void key_order_check() {
	auto unordered = [](bool keyless_disorder) {
		writer w;
		w.key_prefix = "Q";
		w.structures = format::bit::keys | format::bit::stored;  // no claim while writing
		w.permuted = keyless_disorder;
		document d;
		d.stored = "v";
		std::uint64_t docid = 100;
		for (int i = 0; i < 300; ++i) {
			char k[16];
			std::snprintf(k, sizeof k, "Q%05d", keyless_disorder ? i : (i * 37) % 300);
			w.add({docid++, 1, k}, d, false);
		}
		if (keyless_disorder) {
			for (std::uint64_t id : {900, 700, 800}) { w.add({id, 0, {}}, d, false); }
		}
		const std::string plain = w.flush({});
		CHECK(open_check(plain) == outcome::ok);
		return test::reseal(plain, {{format::section::index_sort, format::encode_key_sort()}}, format::bit::keys | format::bit::stored | format::bit::index_sort);
	};
	const std::string docid_ordered = unordered(false);
	CHECK(segment(docid_ordered).core().key_ordered());  // the claim opens: the walk is check()'s
	CHECK(corrupt_with(docid_ordered, "key order: an ordinal not its key's rank"));
	CHECK(corrupt_with(unordered(true), "key order: keyless documents out of docid order"));
	// A merge orders by the keys, not by the source's ordinals, so its output from such a
	// segment is key-ordered and passes check().
	{
		const segment s(docid_ordered);
		std::vector<basic_merge_source<document_layout>> sources{{&s, nullptr}};
		std::string out;
		string_output o(out);
		string_spill spill;
		(void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), {}, o, spill, {.key_prefix = "Q"});
		CHECK(open_check(out) == outcome::ok && segment(out).core().key_ordered());
	}
	// The writer refuses what would not be key order while it claims it.
	{
		document d;
		d.stored = "v";
		writer w;
		w.key_prefix = "Q";
		w.add({1, 1, "Qb"}, d, false);
		CHECK(throws([&] { w.add({2, 1, "Qa"}, d, false); }));  // a smaller key
		CHECK(throws([&] { w.add({3, 1, "Qb"}, d, false); }));  // the same key
		writer k;
		k.key_prefix = "Q";
		k.add({5, 0, {}}, d, false);
		CHECK(throws([&] { k.add({6, 1, "Qa"}, d, false); }));  // a key after a keyless document
		CHECK(throws([&] { k.add({4, 0, {}}, d, false); }));      // keyless out of docid order
		k.add({9, 0, {}}, d, false);
		writer n;
		n.key_prefix = "Q";
		n.structures = format::bit::stored | format::bit::index_sort;  // the key order without keys
		CHECK(throws([&] { n.add({1, 0, {}}, d, false); }));
	}
	std::printf("  check: segments out of key order refused\n");
}

// --- stored blocks and codecs (D1c, D1d) ------------------------------------------------------

static void blocks_and_codecs() {
	std::mt19937_64 r(5);
	auto build = [&](std::size_t block, format::stored_codec codec, auto&& value) {
		writer w;
		w.key_prefix = "Q";
		w.stored_block = block;
		w.stored_codec = codec;
		for (int i = 0; i < 2000; ++i) {
			char k[16];
			std::snprintf(k, sizeof k, "Q%05d", i);
			document d;
			d.stored = value(i);
			w.add({std::uint64_t(i + 1), 1, k}, d, false);
		}
		return w.flush({});
	};
	auto half = [&](int i) { return value_of(r, 100 + i % 50); };
	auto noise = [&](int) {
		std::string v(120, '\0');
		for (auto& c : v) { c = char(r()); }
		return v;
	};
	// The document layout's target is 4 KiB of records, search's 64 KiB.
	for (const std::size_t block : {std::size_t(4) << 10, std::size_t(64) << 10}) {
		const std::string bytes = build(block, format::lz4_codec, half);
		CHECK(open_check(bytes) == outcome::ok);
		const auto sizes = raw_sizes_of(bytes);
		for (std::size_t f = 0; f + 1 < sizes.size(); ++f) { CHECK(sizes[f] <= block + block / 32 && sizes[f] > block / 2); }  // records, and their lengths
		CHECK(sizes.size() >= 2000 * 125 / block);
	}
	// Each codec as asked; incompressible blocks stored raw (a codec saving under an eighth).
	for (const auto codec : {format::lz4_codec, format::zstd_codec(1), format::zstd_codec(3), format::stored_codec{format::codec::raw, 0}}) {
		const std::string bytes = build(4096, codec, half);
		CHECK(open_check(bytes) == outcome::ok);
		for (auto c : codecs_of(bytes)) { CHECK(c == std::uint8_t(codec.kind)); }
		const std::string raw = build(4096, codec, noise);
		CHECK(open_check(raw) == outcome::ok);
		for (auto c : codecs_of(raw)) { CHECK(c == std::uint8_t(format::codec::raw)); }
	}
	{
		writer w;
		CHECK(throws([&] { w.stored_block = 0; w.add({1, 1, "Qa"}, document{}, false); }));
		writer z;
		CHECK(throws([&] { z.stored_codec = format::zstd_codec(0); z.add({1, 1, "Qa"}, document{}, false); }));
		writer l;
		CHECK(throws([&] { l.stored_codec = {format::codec::lz4, 3}; l.add({1, 1, "Qa"}, document{}, false); }));
	}
	// Damage past the checksums: a codec byte unknown, a payload cut short, a record length
	// that overruns the frame or leaves bytes over, an LZ4 block flipped.
	const std::string good = build(4096, format::lz4_codec, half);
	const std::string st = test::section_bytes(good, format::section::stored);
	const std::uint32_t frames = format::get<std::uint32_t>(st.data() + st.size() - 4);
	const std::size_t index_at = st.size() - 4 - 16 * std::size_t(frames);
	auto frame_at = [&](std::uint32_t f) { return std::size_t(format::get<std::uint64_t>(st.data() + index_at + 16 * f + 8)); };
	auto with_stored = [&](std::string s) { return test::reseal(good, {{format::section::stored, std::move(s)}}); };
	// A stored section too small for the documents it serves (SR2): refused, not read as empty.
	// Every size under one frame and the count: 0 to 3 bytes, an empty index, and a lone byte of frame.
	for (const std::size_t n : {0, 1, 2, 3, 4, 5, 20}) { CHECK(corrupt_with(with_stored(st.substr(0, n)), "stored section")); }
	// A tail of the real section keeps its frame count but not its frames: refused too.
	CHECK(open_check(with_stored(st.substr(st.size() - 4))) == outcome::corrupt);
	// A segment without documents has no frames to hold and opens whole.
	{
		writer w;
		w.key_prefix = "Q";
		const std::string empty = w.flush({});
		CHECK(open_check(empty) == outcome::ok);
		const segment es(empty);
		CHECK(es.core().doc_count() == 0);
	}
	{
		std::string s = st;
		s[frame_at(3)] = char(3);
		CHECK(corrupt_with(with_stored(s), "stored codec"));
		s[frame_at(3)] = char(0xFF);
		CHECK(corrupt_with(with_stored(s), "stored codec"));
	}
	{
		std::string s = st;
		s[frame_at(3)] = char(format::codec::raw);  // an LZ4 payload taken as raw: the size differs
		CHECK(corrupt_with(with_stored(s), "stored frame"));
		s = st;
		s[frame_at(3)] = char(format::codec::zstd);
		CHECK(corrupt_with(with_stored(s), "stored frame"));
	}
	{
		std::string s = st;  // the frame's recorded size one larger
		const std::uint32_t raw = format::get<std::uint32_t>(s.data() + index_at + 16 * 3 + 4) + 1;
		std::memcpy(s.data() + index_at + 16 * 3 + 4, &raw, 4);
		CHECK(corrupt_with(with_stored(s), "stored frame"));
	}
	{
		int caught = 0;
		for (std::size_t at = frame_at(2) + 1; at < frame_at(3); at += 7) {
			std::string s = st;
			s[at] = char(s[at] ^ 0x5A);
			const outcome o = open_check(with_stored(s));
			CHECK(o == outcome::ok || o == outcome::corrupt);
			caught += o == outcome::corrupt;
		}
		CHECK(caught > 0);
	}
	// Frames in raw: lengths that overrun or leave bytes over.
	{
		writer w;
		w.key_prefix = "Q";
		w.stored_codec = {format::codec::raw, 0};
		document d;
		d.stored = "abcdef";
		w.add({1, 1, "Qa"}, d, false);
		w.add({2, 1, "Qb"}, d, false);
		const std::string bytes = w.flush({});
		const std::string s0 = test::section_bytes(bytes, format::section::stored);
		// [codec][count 2][len 6][len 6][12 bytes]: lengths at 2 and 3.
		CHECK(s0[0] == char(format::codec::raw) && s0[1] == 2 && s0[2] == 6 && s0[3] == 6);
		auto edited = [&](std::size_t at, char v) {
			std::string s = s0;
			s[at] = v;
			return test::reseal(bytes, {{format::section::stored, s}});
		};
		CHECK(corrupt_with(edited(3, 7), "stored record"));        // one byte past the frame
		CHECK(corrupt_with(edited(3, 5), "stored record bounds"));  // one byte left over
		CHECK(corrupt_with(edited(1, 3), "stored"));                // a third record that is not there
		CHECK(corrupt_with(edited(1, 0x7F), "stored frame count"));  // more records than bytes
	}
	std::printf("  blocks: 4 and 64 KiB targets, raw, LZ4, zstd 1 and 3, damage refused\n");
}

// --- SR1: records of zero bytes still close a block; SR3: a frame's cheap bounds first ---------

// N documents with empty values (and with one-byte values) land in frames that hold at most
// the block target's records, so a frame decodes to about twice the target whatever N, and
// a get's decode buffer is bounded by it (the reviewer's probe_empty_records, at a count that
// proves the bound: without the record cap one frame held every record).
static void empty_records() {
	for (const std::size_t block : {std::size_t(4) << 10, std::size_t(64) << 10}) {
		for (const std::size_t width : {std::size_t(0), std::size_t(1)}) {
			const std::size_t n = 3 * block + 123;
			writer w;
			w.key_prefix = "Q";
			w.stored_block = block;
			w.stored_codec = {format::codec::raw, 0};
			document d;
			d.stored.assign(width, 'x');
			for (std::size_t i = 0; i < n; ++i) {
				char k[24];
				std::snprintf(k, sizeof k, "Q%010zu", i);
				w.add({i + 1, 1, k}, d, false);
			}
			const std::string bytes = w.flush({});
			CHECK(open_check(bytes) == outcome::ok);
			const auto sizes = raw_sizes_of(bytes);
			CHECK(sizes.size() == 4);  // 3 full blocks of `block` records and the rest
			for (const std::uint32_t size : sizes) { CHECK(size <= 2 * block + 8); }
			const segment s(bytes);
			typename stored_plugin::reader::stored_frame frame;
			CHECK(s.get<stored_plugin>().stored(ordinal_t(n / 2), frame).size() == width);
			CHECK(frame.count == block && frame.buf.capacity() <= 4 * (2 * block + 8));
		}
	}
	std::printf("  empty records: frames close at the block's record count\n");
}

// A stored section of one hand-made frame in a 5000-document segment, decoded directly.
static std::string one_frame_segment(std::uint8_t codec, std::string_view payload, std::uint32_t raw_size) {
	writer w;
	w.key_prefix = "Q";
	w.stored_codec = {format::codec::raw, 0};
	document d;
	d.stored = "v";
	for (int i = 0; i < 5000; ++i) {
		char k[16];
		std::snprintf(k, sizeof k, "Q%05d", i);
		w.add({std::uint64_t(i + 1), 1, k}, d, false);
	}
	const std::string good = w.flush({});
	std::string st;
	st.push_back(char(codec));
	st.append(payload);
	format::put<std::uint32_t>(st, 0);         // first ordinal
	format::put<std::uint32_t>(st, raw_size);  // recorded size
	format::put<std::uint64_t>(st, 0);         // offset
	format::put<std::uint32_t>(st, 1);         // frames
	return test::reseal(good, {{format::section::stored, st}});
}

// The frame is refused with the decode buffer still small: nothing grew to the recorded size.
static bool refused_small(std::string_view bytes, std::size_t limit, const char* what) {
	const segment s(bytes);
	typename stored_plugin::reader::stored_frame frame;
	try {
		s.get<stored_plugin>().decode_stored_frame(0, frame);
	} catch (const corrupt_segment&) {
		if (frame.buf.capacity() <= limit) { return true; }
		std::fprintf(stderr, "  %s: refused, but the buffer grew to %zu\n", what, frame.buf.capacity());
		return false;
	}
	std::fprintf(stderr, "  %s: not refused\n", what);
	return false;
}

static void frame_bounds_first() {
	const std::uint32_t big = 60'000'000;
	const std::string few(8, '\x01');
	CHECK(refused_small(one_frame_segment(std::uint8_t(format::codec::lz4), few, big), 1 << 16, "lz4 frame claiming 60 MB from 8 bytes"));
	CHECK(refused_small(one_frame_segment(std::uint8_t(format::codec::zstd), few, big), 1 << 16, "zstd frame claiming 60 MB from 8 bytes"));
	CHECK(refused_small(one_frame_segment(std::uint8_t(format::codec::raw), few, big), 1 << 16, "raw frame claiming 60 MB from 8 bytes"));
	CHECK(refused_small(one_frame_segment(0x7F, few, big), 1 << 16, "unknown codec claiming 60 MB"));
	// A real zstd frame whose recorded size differs from its own: refused before any growth.
	{
		std::string raw(4000, 'a'), packed(zstd_frame_bound(raw.size()), '\0');
		packed.resize(compress_zstd_into(raw, packed.data(), packed.size(), 1));
		CHECK(refused_small(one_frame_segment(std::uint8_t(format::codec::zstd), packed, 4001), 4100, "zstd frame of another size"));
	}
	// A count of 1999 in a 2000-byte raw frame names more records than bytes follow it: refused
	// before the offset table (four bytes a record) is allocated.
	{
		std::string payload;
		format::put_leb(payload, 1999);
		payload.resize(2000, '\0');
		CHECK(refused_small(one_frame_segment(std::uint8_t(format::codec::raw), payload, 2000), 4000, "count past the bytes after it"));
	}
	std::printf("  frames: the cheap bounds come before any buffer grows\n");
}

// --- K3: merges in key order, no records scratch, no key map ----------------------------------

// faultfs's io, recording every file the index creates.
class recording_io final : public kronuz::journal::IO {
public:
	explicit recording_io(faultfs::io& inner) : inner_(&inner) {}
	std::vector<std::string> created;
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return inner_->acquire_owner(create); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_->acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override { return inner_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return inner_->directory_trust(); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override { return inner_->open_existing(name); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		created.emplace_back(name);
		return inner_->create_exclusive(name);
	}
	void replace(std::string_view a, std::string_view b) override { inner_->replace(a, b); }
	void remove(std::string_view name) override { inner_->remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_->scan_directory(); }
	void sync_directory() override { inner_->sync_directory(); }
	void sync_files(std::span<kronuz::journal::File* const> files) override { inner_->sync_files(files); }

private:
	faultfs::io* inner_;
};

// The peak a merge of `n` live keys of `length` bytes charges to its budget, the merge run
// directly over two flushed segments.
static std::uint64_t merge_peak(std::size_t n, std::size_t length) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "Q";
	auto ix = kv_index::create(io, o, uuid);
	std::mt19937_64 r(n + length);
	for (std::size_t s = 0; s < 2; ++s) {
		for (std::size_t i = s; i < 2 * n; i += 2) {
			std::string k = "Q" + std::string(length - 9, 'k');
			char tail[9];
			std::snprintf(tail, sizeof tail, "%08zu", i);
			k += tail;
			for (std::size_t j = 1; j + 8 < k.size(); j += 13) { k[j] = char('a' + (i * 31 + j) % 26); }  // keys that share little
			CHECK(ix.apply(tx({replace(k, "v")}), {}).result == verdict::accepted);
		}
		ix.refresh();
	}
	std::vector<basic_merge_source<document_layout>> sources;
	for (const auto& v : ix.segments()) { sources.push_back({v.seg(), nullptr}); }
	byte_budget budget(1ull << 40);
	flush_charge charge(&budget);
	std::string out;
	string_output sink(out);
	string_spill spill;
	(void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), {}, sink, spill, {.memory = &charge, .key_prefix = "Q"});
	CHECK(open_check(out) == outcome::ok && segment(out).core().key_ordered() && segment(out).get<keys_plugin>().key_count() == 2 * n);
	return budget.peak();
}

static void merges() {
	// Through the index: segments of shuffled keys, deletes and overwrites across them, keyless
	// documents, then one merge. Its output is key-ordered with every live record once, its
	// digest its inputs', and it created no scratch file: the ascending path.
	for (const bool preset : {false, true}) {
		faultfs::filesystem fs;
		faultfs::io inner(fs);
		recording_io io(inner);
		kv_index::options o;
		o.key_prefix = "Q";
		o.zstd_merged = preset;
		auto ix = kv_index::create(io, o, uuid);
		std::mt19937_64 r(preset ? 31 : 30);
		std::map<std::string, std::string> model;
		std::map<std::uint64_t, std::string> keyless;
		for (int s = 0; s < 5; ++s) {
			for (int i = 0; i < 400; ++i) {
				char k[16];
				std::snprintf(k, sizeof k, "Q%05d", int(r() % 1500));
				const std::string v = value_of(r, 30 + r() % 200);
				CHECK(ix.apply(tx({replace(k, v)}), {}).result == verdict::accepted);
				model[k] = v;
				if (r() % 7 == 0) {
					std::snprintf(k, sizeof k, "Q%05d", int(r() % 1500));
					if (model.count(k)) {
						CHECK(ix.apply(tx({erase(k)}), {}).result == verdict::accepted);
						model.erase(k);
					}
				}
				if (r() % 40 == 0) {
					operation op;
					op.type = operation::add;
					op.doc = doc("", value_of(r, 50));
					const auto v2 = ix.apply(tx({op}), {});
					CHECK(v2.result == verdict::accepted && v2.docids.size() == 1);
					if (!v2.docids.empty()) { keyless[v2.docids[0]] = op.doc.stored; }
				}
			}
			ix.refresh();
			ix.commit();
		}
		CHECK(ix.segments().size() == 5);
		const std::uint64_t before = ix.digest();
		io.created.clear();
		ix.force_merge(1);
		ix.commit();
		CHECK(ix.segments().size() == 1);
		CHECK(ix.digest() == before);
		for (const auto& n : io.created) { CHECK(!n.ends_with(".rec") && !n.ends_with(".fwd") && !n.ends_with(".spill")); }
		const auto& seg = *ix.segments()[0].seg();
		CHECK(seg.core().key_ordered());
		(void)ix.check();
		std::size_t i = 0;
		seg.get<keys_plugin>().each_key([&](std::string_view key, ordinal_t o) {
			CHECK(o == i++);
			const auto it = model.find(std::string(key));
			CHECK(it != model.end() && seg.get<stored_plugin>().stored(o) == it->second);
		});
		CHECK(i == model.size() && seg.core().doc_count() == model.size() + keyless.size());
		std::uint64_t prev = 0;
		for (ordinal_t o = ordinal_t(i); o < seg.core().doc_count(); ++o) {
			const std::uint64_t d = seg.core().docid(o);
			CHECK(d > prev && keyless.count(d) && seg.get<stored_plugin>().stored(o) == keyless[d]);
			prev = d;
		}
		for (const auto& [k, v] : model) { CHECK(ix.get(k) == v); }
		std::set<std::uint8_t> codecs;
		for (auto c : codecs_of(fs.view(index_detail::segment_name(ix.segments()[0].id)))) { codecs.insert(c); }
		CHECK(codecs.count(std::uint8_t(preset ? format::codec::zstd : format::codec::lz4)) && !codecs.count(std::uint8_t(preset ? format::codec::lz4 : format::codec::zstd)));
		// One source, its docids out of order: still no scratch (a docid-ordered layout's
		// single permuted source spills its records).
		CHECK(!seg.core().docid_ordered());
		for (int j = 0; j < 1500; j += 11) {
			char k[16];
			std::snprintf(k, sizeof k, "Q%05d", j);
			if (model.erase(k)) { CHECK(ix.apply(tx({erase(k)}), {}).result == verdict::accepted); }
		}
		ix.commit();
		io.created.clear();
		ix.force_merge(1);
		ix.commit();
		CHECK(ix.segments().size() == 1 && ix.segments()[0].dead == 0);
		for (const auto& n : io.created) { CHECK(!n.ends_with(".rec") && !n.ends_with(".fwd") && !n.ends_with(".spill")); }
		for (const auto& [k, v] : model) { CHECK(ix.get(k) == v); }
		(void)ix.check();
		ix.close();
	}
	// No key map: a map of every live key holds each key byte at the merge's peak (64 B a key
	// besides); streamed, the keys are held nowhere but in the output's dictionary, which
	// shares their long runs. The peak's growth per added key byte stays under half a byte.
	{
		const std::size_t n = 4000;
		const std::uint64_t short_keys = merge_peak(n, 16), long_keys = merge_peak(n, 1016);
		const double per_byte = double(long_keys - short_keys) / double(n * 1000);
		std::printf("  merge peak: %llu B at 16-byte keys, %llu B at 1016-byte keys, %.2f B per key byte\n", (unsigned long long)short_keys,
			(unsigned long long)long_keys, per_byte);
		CHECK(per_byte < 0.5);
	}
	// A key-ordered merge takes no order from its caller.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.key_prefix = "Q";
		auto ix = kv_index::create(io, o, uuid);
		CHECK(ix.apply(tx({replace("Qa", "1"), replace("Qb", "2")}), {}).result == verdict::accepted);
		ix.refresh();
		std::vector<basic_merge_source<document_layout>> sources{{ix.segments()[0].seg(), nullptr}};
		const merge_ref order[2] = {{0, 1}, {0, 0}};
		std::string out;
		string_output sink(out);
		string_spill spill;
		CHECK(throws([&] { (void)detail::merge_layout(std::span<const basic_merge_source<document_layout>>(sources), {}, sink, spill, {.order = order, .key_prefix = "Q"}); }));
	}
	std::printf("  merges: key order, the ascending path, no key map\n");
}

int main() {
	fixtures();
	index_sort_rules();
	key_order_check();
	blocks_and_codecs();
	empty_records();
	frame_bounds_first();
	merges();
	std::printf("format14: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
