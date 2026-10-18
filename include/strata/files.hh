// The index's own files besides segments (Phase 3 design, "Files"): manifests, live-docs
// generations, and metadata generations. Each starts with an 8-byte magic and a format
// version, holds LEB128 integers unless noted, and ends in a CRC-32C of every preceding
// byte. Decoders check the CRC first, then every length against the bytes left, and
// reject unknown versions and trailing bytes.
#pragma once

#include "crc32c.hh"
#include "varint.hh"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

// A manifest, live-docs, or metadata file that fails its checks.
struct corrupt_index : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// An index this binary cannot read: a well-formed file of another version, or a manifest
// naming structures or segment versions it lacks (F20). Not damage, so it never fences.
struct unsupported_index : std::runtime_error {
	using std::runtime_error::runtime_error;
};

namespace files {

// The file family's version (F20); its frame (magic, LEB128 version, trailing CRC) is frozen.
inline constexpr std::uint64_t version = 2;
inline constexpr char manifest_magic[8] = {'S', 'T', 'R', 'M', 'A', 'N', '0', '1'};
inline constexpr char live_magic[8] = {'S', 'T', 'R', 'L', 'I', 'V', '0', '1'};
inline constexpr char meta_magic[8] = {'S', 'T', 'R', 'M', 'E', 'T', '0', '1'};

// The writer's position in the replicated log: a log index and how many of that entry's
// transactions are applied (design R3). (0, 0) standalone.
struct position {
	std::uint64_t index = 0;
	std::uint64_t applied = 0;
	auto operator<=>(const position&) const = default;
};

// A file named by a manifest, with the size and CRC it must have.
struct file_ref {
	std::uint64_t generation = 0;  // 0: none
	std::uint64_t size = 0;
	std::uint32_t crc = 0;
	bool operator==(const file_ref&) const = default;
};

struct segment_ref {
	std::uint64_t id = 0;
	std::uint64_t size = 0;
	std::uint32_t footer_crc = 0;
	std::uint64_t doc_count = 0;
	std::uint64_t live_count = 0;
	std::uint64_t live_length = 0;
	file_ref live;  // the live-docs generation; none while the segment has no deletes
	std::uint64_t values_generation = 0;  // 0 until field updates exist (workload-profiles.md item 4)
	bool operator==(const segment_ref&) const = default;
};

struct manifest {
	std::array<char, 16> uuid{};
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	std::uint64_t last_docid = 0;
	std::uint64_t next_segment = 1, next_live = 1, next_meta = 1;
	position applied;
	std::uint64_t schema_version = 0;
	std::string key_prefix;
	file_ref slots;  // the slot table's generation file, slots-<gen>.kv (D3, N1); none yet
	// The structures the index is configured with, set by a replicated operation (F13-R28),
	// and the mask: those together with every bit a segment sets, for readability only.
	std::uint32_t configured = 0, mask = 0;
	// The lowest and highest segment format version (S2); 0 and 0 without segments.
	std::uint32_t min_version = 0, max_version = 0;
	file_ref meta;
	// Scoring statistics include deleted documents (D13); live figures are reported.
	std::uint64_t doc_count = 0, total_length = 0, live_count = 0, live_length = 0;
	std::vector<std::uint64_t> pinned;  // checkpoint manifests' generations (D4)
	std::vector<segment_ref> segments;
	bool operator==(const manifest&) const = default;
};

// Deleted ordinals of one segment at one generation.
struct live_docs {
	std::uint64_t segment = 0, generation = 0, doc_count = 0, live_count = 0, live_length = 0;
	std::vector<std::uint64_t> deleted;  // ascending ordinals
	bool operator==(const live_docs&) const = default;
};

struct metadata {
	std::uint64_t generation = 0;
	std::map<std::string, std::string> entries;
	bool operator==(const metadata&) const = default;
};

namespace detail {

inline void put(std::string& out, std::uint64_t v) {
	char buf[varint::leb128::max_bytes];
	out.append(buf, varint::leb128::encode(buf, v));
}
inline void put_bytes(std::string& out, std::string_view b) {
	put(out, b.size());
	out += b;
}
inline void seal(std::string& out) {
	const std::uint32_t crc = hashes::crc32c(out.data(), out.size());
	char b[4];
	std::memcpy(b, &crc, 4);  // little-endian hosts only, as the segment format
	out.append(b, 4);
}

// Reads one file's body after its magic, CRC checked up front.
class reader {
public:
	reader(std::string_view file, const char (&magic)[8], const char* what) : what_(what) {
		if (file.size() < 12 || std::memcmp(file.data(), magic, 8) != 0) { throw corrupt_index(std::string(what) + ": magic"); }
		std::uint32_t crc;
		std::memcpy(&crc, file.data() + file.size() - 4, 4);
		if (crc != hashes::crc32c(file.data(), file.size() - 4)) { throw corrupt_index(std::string(what) + ": checksum"); }
		p_ = file.data() + 8;
		end_ = file.data() + file.size() - 4;
		// The CRC first, then the version: a flipped version is damage, a written one is not.
		if (const std::uint64_t v = get(); v != version) { throw unsupported_index(std::string(what) + ": version " + std::to_string(v)); }
	}
	std::uint64_t get() {
		std::uint64_t v = 0;
		if (!varint::leb128::decode(&p_, end_, v)) { throw corrupt_index(std::string(what_) + ": truncated"); }
		return v;
	}
	// A count of items each at least `min_bytes` long, bounded by the bytes left.
	std::uint64_t count(std::size_t min_bytes = 1) {
		const std::uint64_t n = get();
		if (n > std::uint64_t(end_ - p_) / min_bytes) { throw corrupt_index(std::string(what_) + ": count"); }
		return n;
	}
	std::string_view bytes(std::uint64_t n) {
		if (n > std::uint64_t(end_ - p_)) { throw corrupt_index(std::string(what_) + ": length"); }
		std::string_view out(p_, n);
		p_ += n;
		return out;
	}
	std::string_view sized() { return bytes(get()); }
	std::uint32_t u32() {
		const std::uint64_t v = get();
		if (v > 0xffffffffu) { throw corrupt_index(std::string(what_) + ": field range"); }
		return std::uint32_t(v);
	}
	void finish() const {
		if (p_ != end_) { throw corrupt_index(std::string(what_) + ": trailing bytes"); }
	}
	[[noreturn]] void bad(const char* why) const { throw corrupt_index(std::string(what_) + ": " + why); }

private:
	const char* what_;
	const char* p_;
	const char* end_;
};

inline void put_ref(std::string& out, const file_ref& r) {
	put(out, r.generation);
	put(out, r.size);
	put(out, r.crc);
}
inline file_ref get_ref(reader& in) {
	file_ref r;
	r.generation = in.get();
	r.size = in.get();
	r.crc = in.u32();
	if (!r.generation && (r.size || r.crc)) { in.bad("absent file with a size"); }
	return r;
}

}  // namespace detail

inline std::string encode(const manifest& m) {
	using namespace detail;
	std::string out(manifest_magic, 8);
	put(out, version);
	out.append(m.uuid.data(), 16);
	for (std::uint64_t v : {m.generation, m.revision, m.last_docid, m.next_segment, m.next_live, m.next_meta, m.applied.index, m.applied.applied, m.schema_version}) { put(out, v); }
	put_bytes(out, m.key_prefix);
	put_ref(out, m.slots);
	for (std::uint64_t v : {std::uint64_t(m.configured), std::uint64_t(m.mask), std::uint64_t(m.min_version), std::uint64_t(m.max_version)}) { put(out, v); }
	put_ref(out, m.meta);
	for (std::uint64_t v : {m.doc_count, m.total_length, m.live_count, m.live_length}) { put(out, v); }
	put(out, m.pinned.size());
	for (std::uint64_t g : m.pinned) { put(out, g); }
	put(out, m.segments.size());
	for (const auto& s : m.segments) {
		for (std::uint64_t v : {s.id, s.size, std::uint64_t(s.footer_crc), s.doc_count, s.live_count, s.live_length}) { put(out, v); }
		put_ref(out, s.live);
		put(out, s.values_generation);
	}
	seal(out);
	return out;
}

// Decodes and checks a manifest's internal consistency: counters above every name they
// cover, live figures within the totals, segments sorted by id.
inline manifest decode_manifest(std::string_view file) {
	detail::reader in(file, manifest_magic, "manifest");
	manifest m;
	std::memcpy(m.uuid.data(), in.bytes(16).data(), 16);
	m.generation = in.get();
	m.revision = in.get();
	m.last_docid = in.get();
	m.next_segment = in.get();
	m.next_live = in.get();
	m.next_meta = in.get();
	m.applied.index = in.get();
	m.applied.applied = in.get();
	m.schema_version = in.get();
	m.key_prefix = std::string(in.sized());
	m.slots = detail::get_ref(in);
	if (m.slots.generation) { in.bad("slots"); }  // no slot table yet (D3; C13)
	m.configured = in.u32();
	m.mask = in.u32();
	m.min_version = in.u32();
	m.max_version = in.u32();
	if ((m.configured & ~m.mask) || m.min_version > m.max_version) { in.bad("structures or versions"); }
	m.meta = detail::get_ref(in);
	m.doc_count = in.get();
	m.total_length = in.get();
	m.live_count = in.get();
	m.live_length = in.get();
	for (std::uint64_t n = in.count(); n; --n) { m.pinned.push_back(in.get()); }
	std::uint64_t docs = 0, live = 0, live_length = 0;
	for (std::uint64_t n = in.count(9); n; --n) {
		segment_ref s;
		s.id = in.get();
		s.size = in.get();
		s.footer_crc = in.u32();
		s.doc_count = in.get();
		s.live_count = in.get();
		s.live_length = in.get();
		s.live = detail::get_ref(in);
		s.values_generation = in.get();
		if (s.values_generation != 0) { in.bad("values generation"); }
		if (s.live_count > s.doc_count || (s.live.generation == 0) != (s.live_count == s.doc_count)) { in.bad("segment live count"); }
		if (s.id >= m.next_segment || s.live.generation >= m.next_live || (!m.segments.empty() && s.id <= m.segments.back().id)) { in.bad("segment id"); }
		docs += s.doc_count;
		live += s.live_count;
		live_length += s.live_length;
		m.segments.push_back(s);
	}
	in.finish();
	if (m.generation == 0 || m.meta.generation >= m.next_meta) { in.bad("generations"); }
	if (m.segments.empty() ? (m.min_version || m.max_version) : m.min_version == 0) { in.bad("segment versions"); }
	if (docs != m.doc_count || live != m.live_count || live_length != m.live_length || m.live_length > m.total_length) { in.bad("totals"); }
	for (std::uint64_t g : m.pinned) {
		if (g == 0 || g > m.generation) { in.bad("pinned generation"); }
	}
	return m;
}

// Dead ordinals as a bitset (encoding 0) or ascending LEB128 deltas (1), whichever is
// smaller.
inline std::string encode(const live_docs& l) {
	using namespace detail;
	std::string out(live_magic, 8);
	put(out, version);
	for (std::uint64_t v : {l.segment, l.generation, l.doc_count, l.live_count, l.live_length}) { put(out, v); }
	std::string deltas;
	std::uint64_t prev = 0;
	for (std::size_t i = 0; i < l.deleted.size(); ++i) {
		put(deltas, i ? l.deleted[i] - prev : l.deleted[i]);
		prev = l.deleted[i];
	}
	const std::uint64_t bitset_bytes = l.doc_count / 8 + (l.doc_count % 8 != 0);  // no wrap near 2^64
	if (bitset_bytes <= deltas.size()) {
		std::string bits(bitset_bytes, '\0');
		for (std::uint64_t d : l.deleted) { bits[d >> 3] = char(std::uint8_t(bits[d >> 3]) | (1u << (d & 7))); }
		put(out, 0);
		put_bytes(out, bits);
	} else {
		put(out, 1);
		put_bytes(out, deltas);
	}
	seal(out);
	return out;
}

inline live_docs decode_live_docs(std::string_view file) {
	detail::reader in(file, live_magic, "live docs");
	live_docs l;
	l.segment = in.get();
	l.generation = in.get();
	l.doc_count = in.get();
	l.live_count = in.get();
	l.live_length = in.get();
	if (l.live_count > l.doc_count || l.generation == 0) { in.bad("counts"); }
	const std::uint64_t encoding = in.get();
	const std::string_view body = in.sized();
	in.finish();
	if (encoding == 0) {
		if (body.size() != l.doc_count / 8 + (l.doc_count % 8 != 0)) { in.bad("bitset size"); }
		for (std::uint64_t d = 0; d < l.doc_count; ++d) {
			if (std::uint8_t(body[d >> 3]) >> (d & 7) & 1) { l.deleted.push_back(d); }
		}
		if (l.doc_count % 8 && std::uint8_t(body.back()) >> (l.doc_count % 8)) { in.bad("bits past the document count"); }
	} else if (encoding == 1) {
		const char* p = body.data();
		const char* end = body.data() + body.size();
		std::uint64_t prev = 0;
		while (p != end) {
			std::uint64_t v = 0;
			if (!varint::leb128::decode(&p, end, v)) { in.bad("truncated delta"); }
			if (!l.deleted.empty() && v == 0) { in.bad("repeated ordinal"); }
			// Compared against the room left, so a delta cannot wrap past 2^64.
			if (v >= l.doc_count - prev) { in.bad("ordinal past the document count"); }
			prev = l.deleted.empty() ? v : prev + v;
			l.deleted.push_back(prev);
		}
	} else {
		in.bad("encoding");
	}
	if (l.doc_count - l.deleted.size() != l.live_count) { in.bad("live count"); }
	return l;
}

inline std::string encode(const metadata& m) {
	using namespace detail;
	std::string out(meta_magic, 8);
	put(out, version);
	put(out, m.generation);
	put(out, m.entries.size());
	for (const auto& [k, v] : m.entries) {
		put_bytes(out, k);
		put_bytes(out, v);
	}
	seal(out);
	return out;
}

inline metadata decode_metadata(std::string_view file) {
	detail::reader in(file, meta_magic, "metadata");
	metadata m;
	m.generation = in.get();
	if (m.generation == 0) { in.bad("generation"); }
	std::string prev;
	for (std::uint64_t n = in.count(2); n; --n) {
		std::string k(in.sized());
		std::string v(in.sized());
		if (!m.entries.empty() && k <= prev) { in.bad("keys not ascending"); }
		prev = k;
		m.entries.emplace(std::move(k), std::move(v));
	}
	in.finish();
	return m;
}

// The CRC a file_ref records: the file's trailing CRC-32C.
inline std::uint32_t trailing_crc(std::string_view file) {
	std::uint32_t crc = 0;
	if (file.size() >= 4) { std::memcpy(&crc, file.data() + file.size() - 4, 4); }
	return crc;
}

}  // namespace files
}  // namespace strata
