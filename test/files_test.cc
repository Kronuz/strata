// Manifests, live-docs, and metadata files: round trips, every single-byte change and
// every truncation detected, and well-formed but inconsistent contents rejected.
#include "strata/files.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace strata::files;

static bool rejects(const std::function<void()>& f) {
	try {
		f();
	} catch (const strata::corrupt_index&) {
		return true;
	}
	return false;
}

// Every byte flipped and every proper prefix must fail to decode.
template <class Decode>
static void damage(const std::string& good, Decode decode) {
	for (std::size_t i = 0; i < good.size(); ++i) {
		for (int bit : {0, 7}) {
			std::string bad = good;
			bad[i] = char(bad[i] ^ (1 << bit));
			CHECK(rejects([&] { decode(bad); }));
		}
	}
	for (std::size_t n = 0; n < good.size(); ++n) { CHECK(rejects([&] { decode(good.substr(0, n)); })); }
}

static manifest sample(std::mt19937_64& rng) {
	manifest m;
	for (auto& c : m.uuid) { c = char(rng()); }
	m.generation = 1 + rng() % 1000;
	m.revision = rng() % 1000;
	m.last_docid = rng() % 100000;
	m.next_segment = 100;
	m.next_live = 50;
	m.next_meta = 7;
	m.applied = {rng() % 1000, rng() % 4};
	m.schema_version = rng() % 5;
	m.key_prefix = "Q";
	m.meta = {6, 1234, std::uint32_t(rng())};
	const std::size_t n = rng() % 6;
	for (std::size_t i = 0; i < n; ++i) {
		segment_ref s;
		s.id = 1 + i * 10;
		s.size = 4096 + rng() % 100000;
		s.footer_crc = std::uint32_t(rng());
		s.doc_count = 1 + rng() % 1000;
		s.live_count = rng() % 2 ? s.doc_count : rng() % s.doc_count;
		s.live_length = s.live_count * 10;
		if (s.live_count != s.doc_count) { s.live = {1 + i, 100, std::uint32_t(rng())}; }
		m.doc_count += s.doc_count;
		m.total_length += s.doc_count * 10;
		m.live_count += s.live_count;
		m.live_length += s.live_length;
		m.segments.push_back(s);
	}
	if (m.generation > 2) { m.pinned = {m.generation - 1}; }
	m.configured = 15;
	m.mask = 15 | 128;
	if (!m.segments.empty()) { m.min_version = 13, m.max_version = 13; }
	return m;
}

int main() {
	std::mt19937_64 rng(42);
	for (int round = 0; round < 200; ++round) {
		const manifest m = sample(rng);
		const std::string bytes = encode(m);
		CHECK(decode_manifest(bytes) == m);
		if (round < 5) { damage(bytes, decode_manifest); }
	}
	// Inconsistent manifests, encoded as written and rejected on decode.
	{
		std::mt19937_64 r(1);
		manifest m = sample(r);
		while (m.segments.size() < 2) { m = sample(r); }
		auto rejected = [&](auto change) {
			manifest bad = m;
			change(bad);
			return rejects([&] { decode_manifest(encode(bad)); });
		};
		CHECK(rejected([](manifest& b) { b.generation = 0; }));
		CHECK(rejected([](manifest& b) { b.doc_count += 1; }));
		CHECK(rejected([](manifest& b) { b.segments[0].live_count = b.segments[0].doc_count + 1; }));
		CHECK(rejected([](manifest& b) { b.segments[1].id = b.segments[0].id; }));
		CHECK(rejected([](manifest& b) { b.segments[0].id = b.next_segment; }));
		CHECK(rejected([](manifest& b) { b.meta.generation = b.next_meta; }));
		CHECK(rejected([](manifest& b) { b.pinned = {b.generation + 1}; }));
		CHECK(rejected([](manifest& b) {
			b.segments[0].live = {};
			if (b.segments[0].live_count == b.segments[0].doc_count) {
				b.segments[0].live_count -= 1;
				b.live_count -= 1;
			}
		}));
	}
	// Live docs: both encodings round-trip, and each is chosen when smaller.
	for (std::uint64_t docs : {1u, 8u, 9u, 100u, 5000u}) {
		for (double rate : {0.0, 0.001, 0.5, 1.0}) {
			live_docs l;
			l.segment = 3;
			l.generation = 2;
			l.doc_count = docs;
			for (std::uint64_t d = 0; d < docs; ++d) {
				if (double(rng() % 1000) / 1000 < rate) { l.deleted.push_back(d); }
			}
			l.live_count = docs - l.deleted.size();
			l.live_length = l.live_count * 3;
			const std::string bytes = encode(l);
			CHECK(decode_live_docs(bytes) == l);
			if (docs == 100 && (rate == 0.5 || rate == 0.001)) { damage(bytes, decode_live_docs); }
		}
	}
	{
		live_docs l{3, 2, 10, 9, 0, {10}};  // an ordinal past the count
		CHECK(rejects([&] { decode_live_docs(encode(l)); }));
		live_docs m{3, 2, 10, 9, 0, {4}};
		m.live_count = 8;  // disagrees with the deleted set
		CHECK(rejects([&] { decode_live_docs(encode(m)); }));
		// A bitset sized for a document count near 2^64 must not wrap to an empty body.
		for (std::uint64_t below = 1; below <= 7; ++below) {
			using namespace strata::files::detail;
			std::string raw(strata::files::live_magic, 8);
			put(raw, strata::files::version);
			const std::uint64_t docs = ~std::uint64_t(0) - (below - 1);
			for (std::uint64_t v : {std::uint64_t(3), std::uint64_t(2), docs, docs, std::uint64_t(0)}) { put(raw, v); }
			put(raw, 0);  // bitset encoding
			put_bytes(raw, "");
			seal(raw);
			CHECK(rejects([&] { decode_live_docs(raw); }));
		}
		// A delta that wraps 2^64 lands below its predecessor, back on it, or past the count.
		for (std::uint64_t last : {3u, 5u, 9999u}) {
			live_docs w{3, 2, 10000, 9997, 0, {5, 9999, last}};
			CHECK(rejects([&] { decode_live_docs(encode(w)); }));
		}
	}
	// Metadata.
	{
		metadata m;
		m.generation = 4;
		m.entries = {{"", "empty key"}, {"schema", std::string(70000, 's')}, {"z", ""}};
		const std::string bytes = encode(m);
		CHECK(decode_metadata(bytes) == m);
		metadata small{1, {{"a", "b"}}};
		damage(encode(small), decode_metadata);
	}
	// Golden fixtures: deterministic files of each kind must keep their bytes (written
	// with STRATA_WRITE_FIXTURES set when a format changes on purpose).
	{
		std::mt19937_64 r(2026);
		manifest m = sample(r);
		while (m.segments.size() < 3) { m = sample(r); }  // segments with and without live docs
		live_docs l;
		l.segment = 11;
		l.generation = 3;
		l.doc_count = 5000;
		for (std::uint64_t o = 0; o < 5000; o += 1 + r() % 40) { l.deleted.push_back(o); }
		l.live_count = l.doc_count - l.deleted.size();
		l.live_length = l.live_count * 9;
		const metadata md{5, {{"schema", "{\"type\": \"object\"}"}, {"version", "12"}}};
		const std::pair<const char*, std::string> files[] = {{"manifest.bin", encode(m)}, {"live.bin", encode(l)}, {"meta.bin", encode(md)}};
		for (const auto& [name, bytes] : files) {
			const std::string path = std::string(STRATA_FIXTURES) + "/" + name;
			if (std::getenv("STRATA_WRITE_FIXTURES")) {
				std::FILE* f = std::fopen(path.c_str(), "wb");
				std::fwrite(bytes.data(), 1, bytes.size(), f);
				std::fclose(f);
			}
			std::string golden;
			if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
				for (int c; (c = std::fgetc(f)) != EOF;) { golden.push_back(char(c)); }
				std::fclose(f);
			}
			CHECK(golden == bytes);
		}
		CHECK(decode_manifest(files[0].second) == m && decode_live_docs(files[1].second) == l && decode_metadata(files[2].second) == md);
	}
	// F20: the family's version, a pre-freeze number until the freeze (C17). A well-formed file
	// of another version is unsupported, not damage; the real version-1 files and a version-2
	// manifest (7c-6's) are refused that way. The new manifest fields reject what they cannot hold.
	{
		auto unsupported = [](const std::function<void()>& f) {
			try {
				f();
			} catch (const strata::unsupported_index&) {
				return true;
			} catch (...) {
			}
			return false;
		};
		auto read = [](const char* name) {
			std::string out;
			if (std::FILE* f = std::fopen((std::string(STRATA_FIXTURES) + "/format12/" + name).c_str(), "rb")) {
				for (int c; (c = std::fgetc(f)) != EOF;) { out.push_back(char(c)); }
				std::fclose(f);
			}
			return out;
		};
		const std::string m1 = read("manifest.bin"), l1 = read("live.bin"), d1 = read("meta.bin");
		CHECK(!m1.empty() && !l1.empty() && !d1.empty());
		CHECK(unsupported([&] { (void)decode_manifest(m1); }));
		CHECK(unsupported([&] { (void)decode_live_docs(l1); }));
		CHECK(unsupported([&] { (void)decode_metadata(d1); }));
		std::mt19937_64 r(7);
		manifest m = sample(r);
		while (m.segments.empty()) { m = sample(r); }
		const std::string good = encode(m);
		{  // the same manifest at version 2, resealed: an earlier pre-freeze build's
			const std::size_t leb = 5;  // pre_freeze's LEB128 bytes
			static_assert(strata::files::version >> 28 && !(strata::files::version >> 35));
			std::string v2 = good.substr(0, 8) + '\x02' + good.substr(8 + leb, good.size() - 4 - 8 - leb);
			const std::uint32_t crc = hashes::crc32c(v2.data(), v2.size());
			v2.append(reinterpret_cast<const char*>(&crc), 4);
			CHECK(unsupported([&] { (void)decode_manifest(v2); }));
		}
		// A flipped version byte fails the CRC first: damage.
		std::string flipped = good;
		flipped[8] ^= 1;
		CHECK(rejects([&] { (void)decode_manifest(flipped); }));
		auto bad = [&](auto edit) {
			manifest x = m;
			edit(x);
			return rejects([&] { (void)decode_manifest(encode(x)); });
		};
		CHECK(bad([](manifest& x) { x.configured |= 64; }));  // configured outside the mask
		CHECK(bad([](manifest& x) { x.min_version = 14; }));  // min above max
		CHECK(bad([](manifest& x) { x.min_version = 0; }));   // segments without a version
		CHECK(bad([](manifest& x) { x.segments.back().values_generation = 1; }));
		CHECK(bad([](manifest& x) { x.slots = {1, 10, 0}; }));  // a slot table before D3 (C13)
		// Readability first (F13-R42): a later field under a mask bit the reader lacks is
		// unsupported; the same field without that bit is damage.
		{
			manifest x = m;
			x.mask |= std::uint64_t(1) << 20;
			x.segments.back().values_generation = 1;
			x.slots = {1, 10, 0};
			CHECK(unsupported([&] { (void)decode_manifest_for(encode(x), readable_by{~(std::uint64_t(1) << 20), 0, ~std::uint32_t(0)}); }));
			CHECK(rejects([&] { (void)decode_manifest_for(encode(x), readable_by{}); }));
		}
		CHECK(bad([](manifest& x) {
			x.segments.clear();
			x.doc_count = x.total_length = x.live_count = x.live_length = 0;
		}));  // no segments, a version range
	}
	std::printf("files: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
