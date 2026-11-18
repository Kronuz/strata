// What the manifest log's records share (manifest_log.hh): the files family's version, the errors
// a decoder throws, a segment's reference and attributes, and the record reader, which reads
// LEB128 integers in their shortest form and bounds every length by the bytes left.
#pragma once

#include "varint.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

// A manifest log, or a record in it, that fails its checks.
struct corrupt_index : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// An index this binary cannot read: a well-formed log of another version, or a snapshot or edit
// naming structures or segment versions it lacks (F20). Not damage, so it never fences.
struct unsupported_index : std::runtime_error {
	using std::runtime_error::runtime_error;
};

namespace files {

// The file family's version (F20); the log's snapshot records it. 2 at format 13's checkpoint.
// Between checkpoints it is a pre-freeze number (the series starts at 0x8000'0006), bumped with
// each layout change and set to the family's next integer at the checkpoint, so another build's
// index is refused, never misparsed (C17). 0x8000'0006: the field table replaces the slot table
// (fields-and-keys F1). 0x8000'0007: the manifest drops the key prefix (F3). 0x8000'0008: the
// manifest log replaces the manifest, live-docs, metadata and field-table files (manifest-log D11).
inline constexpr std::uint64_t pre_freeze = 0x8000'0008;
inline constexpr std::uint64_t version = pre_freeze;
// The retired manifest file's magic: a `manifest-<g>` that starts with it is another build's
// index (unsupported_index, D11).
inline constexpr char manifest_magic[8] = {'S', 'T', 'R', 'M', 'A', 'N', '0', '1'};

// The writer's position in the replicated log: a log index and how many of that entry's
// transactions are applied (design R3). (0, 0) standalone.
struct position {
	std::uint64_t index = 0;
	std::uint64_t applied = 0;
	auto operator<=>(const position&) const = default;
};

// An attribute (F20, F13-R43, F13-R51): a segment's state readable without opening it, such
// as its retention slot's [min, max], or the index's per-replica state. A tag's low bit says
// what a binary that lacks it does: 1, refuse the index (unsupported_index); 0, a cache it
// drops at its next commit (PNG's critical and ancillary chunks). No tag is defined yet.
struct attribute {
	std::uint64_t tag = 0;
	std::string bytes;
	bool operator==(const attribute&) const = default;
};
// The attribute tags this binary reads, per segment and per index; none yet. A required tag
// outside them refuses.
inline constexpr bool known_attribute(std::uint64_t) noexcept { return false; }
inline constexpr bool known_index_attribute(std::uint64_t) noexcept { return false; }

struct segment_ref {
	std::uint64_t id = 0;  // the local number: its file's name
	// Its physical identity, the header's (F6, F13-R41): the producing term, then 8 bytes no
	// other producer repeats; kept by every copy, checked at open (I13).
	std::array<std::uint8_t, 16> identity{};
	std::uint64_t size = 0;
	std::uint32_t footer_crc = 0;
	std::uint64_t doc_count = 0;
	std::uint64_t live_count = 0;
	std::uint64_t live_length = 0;
	std::uint64_t values_generation = 0;  // 0 until field updates exist (workload-profiles.md item 4)
	std::vector<attribute> attributes;    // tags strictly increasing; the known ones only
	bool operator==(const segment_ref&) const = default;
};

// Metadata keys under this prefix are the engine's index options (F20, F13-R49): stage 1
// refuses a host's set_meta or delete_meta under it; decoding keeps unknown ones opaque.
inline constexpr std::string_view reserved_meta_prefix = "strata.";

namespace detail {

inline void put(std::string& out, std::uint64_t v) {
	char buf[varint::leb128::max_bytes];
	out.append(buf, varint::leb128::encode(buf, v));
}
inline void put_bytes(std::string& out, std::string_view b) {
	put(out, b.size());
	out += b;
}

// Reads one record inside the manifest log's framing, which checks its bytes: every integer in
// its shortest form, so a record decodes from exactly one encoding.
class reader {
public:
	reader(std::string_view body, const char* what) : what_(what), p_(body.data()), end_(body.data() + body.size()) {}
	std::uint64_t get() {
		std::uint64_t v = 0;
		const char* const at = p_;
		if (!varint::leb128::decode(&p_, end_, v)) { throw corrupt_index(std::string(what_) + ": truncated"); }
		if (p_ - at != (std::bit_width(v | 1) + 6) / 7) { throw corrupt_index(std::string(what_) + ": overlong integer"); }
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

// [count LEB128][per attribute: tag, length, bytes], tags strictly increasing (F20).
inline void put_attributes(std::string& out, const std::vector<attribute>& as) {
	put(out, as.size());
	for (const auto& a : as) {
		put(out, a.tag);
		put_bytes(out, a.bytes);
	}
}
// Keeps the tags `known` names, refuses an unknown required one, drops an unknown cache.
inline std::vector<attribute> get_attributes(reader& in, bool (*known)(std::uint64_t) noexcept, const char* what) {
	std::vector<attribute> out;
	std::uint64_t prev = 0;
	for (std::uint64_t k = in.count(2), i = 0; i < k; ++i) {
		attribute a;
		a.tag = in.get();
		a.bytes = std::string(in.sized());
		if (i && a.tag <= prev) { in.bad("attribute order"); }
		prev = a.tag;
		if (known(a.tag)) {
			out.push_back(std::move(a));
		} else if (a.tag & 1) {
			throw unsupported_index(std::string("strata: unsupported index: ") + what + " attribute " + std::to_string(a.tag));
		}
	}
	return out;
}

}  // namespace detail

// What a binary reads (F20): its structure bits and segment format versions; by default
// everything, for tools that only decode.
struct readable_by {
	std::uint64_t bits = ~std::uint64_t(0);
	std::uint32_t min_version = 0, max_version = ~std::uint32_t(0);
};

}  // namespace files
}  // namespace strata
