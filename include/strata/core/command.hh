// A transaction, and the command codec (standalone-turn-log D3, version 1): a transaction's bytes
// as the turn log's batch and chunk records carry them and the replicated path proposes them, and
// the checkpoint record's. Integers are LEB128 in their shortest form and every length is bounded by
// the bytes left (files::detail::reader), so a payload decodes from exactly one encoding.
#pragma once

#include "strata/core/fields.hh"
#include "strata/core/files.hh"
#include "strata/core/reader.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace strata {

template <class Layout>
struct basic_operation {
	enum kind : std::uint8_t { add, replace_key, delete_key, replace_docid, delete_docid, set_meta, delete_meta };
	kind type = add;
	std::uint64_t docid = 0;                       // replace_docid, delete_docid
	std::string key;                               // replace_key, delete_key: the key term
	std::optional<std::uint64_t> expected_version;  // 0: must be absent
	basic_document<Layout> doc;                    // add, replace_*
	std::string meta_key, meta_value;              // set_meta, delete_meta
};

template <class Layout>
struct basic_transaction {
	std::vector<basic_operation<Layout>> ops;
	std::optional<std::uint64_t> expected_schema;  // the schema version it was built against
	std::optional<std::uint64_t> set_schema;       // the schema version it sets
	// The fields its parts name by declaration (`field_ref::declared(i)`), applied in order before its
	// operations: a known name answers its id, an identical declaration too, a new name takes the next id.
	std::vector<field_declaration> fields;
	// The table its `field_ref::id(n)` references were read from; required by any `id` reference.
	std::optional<field_mark> expected_fields;
};

namespace command {

// The largest command a logged index admits (D3): consensus's command ceiling, and the stored
// record's cap.
inline constexpr std::uint64_t max_bytes = 64ull << 20;
static_assert(max_bytes == format::max_stored_record);
// A payload's first two bytes: its kind, then its version.
inline constexpr std::uint8_t batch = 1, checkpoint = 2, chunk = 3, version = 1;

namespace detail {
// The transaction's presence mask, and an operation's flags.
enum : std::uint8_t { has_expected_schema = 1, has_set_schema = 2, has_fields = 4, has_expected_fields = 8, mask_bits = 15 };
enum : std::uint8_t { has_expected_version = 1 };

template <class E>
constexpr bool has_part_codec() {
	if constexpr (requires { typename E::part; }) {
		return requires(std::string& out, const typename E::part& p, files::detail::reader& in, typename E::part& q) {
			E::encode_part(out, p);
			E::decode_part(in, q);
		};
	} else {
		return true;
	}
}
inline void put_u8(std::string& out, std::uint8_t v) { out.push_back(char(v)); }
inline std::uint8_t get_u8(files::detail::reader& in) { return std::uint8_t(in.bytes(1)[0]); }
inline std::size_t leb_bytes(std::uint64_t v) { return std::size_t(std::bit_width(v | 1) + 6) / 7; }

// A declaration as fields_detail::put_decl writes it; its values are stage 1's to judge, so only
// the flags byte, a bool, is checked here.
inline field_declaration get_decl(files::detail::reader& in) {
	field_declaration d;
	d.name = std::string(in.sized());
	const std::string_view b = in.bytes(3);
	if (std::uint8_t(b[2]) > 1) { in.bad("field flags"); }
	d.role = field_role(std::uint8_t(b[0]));
	d.postings = field_postings(std::uint8_t(b[1]));
	d.norms = b[2] == 1;
	return d;
}
}  // namespace detail

// Whether every part of the layout has the codec's hook, E::encode_part and E::decode_part: only
// such a layout's index can be logged.
template <class Layout>
struct loggable_layout : std::false_type {};
template <class... Entries>
struct loggable_layout<layout<Entries...>> : std::bool_constant<(detail::has_part_codec<Entries>() && ...)> {};
template <class Layout>
inline constexpr bool loggable = loggable_layout<Layout>::value;

// A document: its core key, then each part in layout order through its plugin's hook.
template <class Layout>
void encode_document(std::string& out, const basic_document<Layout>& d) {
	files::detail::put_bytes(out, d.key);
	for_each_entry<Layout>([&](auto e) {
		using E = typename decltype(e)::type;
		if constexpr (requires { typename E::part; }) { E::encode_part(out, static_cast<const typename E::part&>(d)); }
	});
}
template <class Layout>
void decode_document(files::detail::reader& in, basic_document<Layout>& d) {
	d.key = std::string(in.sized());
	for_each_entry<Layout>([&](auto e) {
		using E = typename decltype(e)::type;
		if constexpr (requires { typename E::part; }) { E::decode_part(in, static_cast<typename E::part&>(d)); }
	});
}

// A transaction: its op count; per op its kind and the fields that kind reads; then the presence
// mask and the optional members it names. Throws std::invalid_argument on an unknown kind.
template <class Layout>
void encode(std::string& out, const basic_transaction<Layout>& t) {
	static_assert(loggable<Layout>, "strata: a logged layout's parts need encode_part and decode_part");
	using op = basic_operation<Layout>;
	files::detail::put(out, t.ops.size());
	for (const op& o : t.ops) {
		if (o.type > op::delete_meta) { throw std::invalid_argument("strata: an operation of unknown kind"); }
		detail::put_u8(out, o.type);
		if (o.type == op::set_meta || o.type == op::delete_meta) {
			files::detail::put_bytes(out, o.meta_key);
			if (o.type == op::set_meta) { files::detail::put_bytes(out, o.meta_value); }
			continue;
		}
		detail::put_u8(out, o.expected_version ? detail::has_expected_version : 0);
		if (o.expected_version) { files::detail::put(out, *o.expected_version); }
		if (o.type == op::replace_key || o.type == op::delete_key) { files::detail::put_bytes(out, o.key); }
		if (o.type == op::replace_docid || o.type == op::delete_docid) { files::detail::put(out, o.docid); }
		if (o.type == op::add || o.type == op::replace_key || o.type == op::replace_docid) { encode_document(out, o.doc); }
	}
	const std::uint8_t mask = (t.expected_schema ? detail::has_expected_schema : 0) | (t.set_schema ? detail::has_set_schema : 0) |
		(t.fields.empty() ? 0 : detail::has_fields) | (t.expected_fields ? detail::has_expected_fields : 0);
	detail::put_u8(out, mask);
	if (t.expected_schema) { files::detail::put(out, *t.expected_schema); }
	if (t.set_schema) { files::detail::put(out, *t.set_schema); }
	if (!t.fields.empty()) {
		files::detail::put(out, t.fields.size());
		for (const auto& d : t.fields) { fields_detail::put_decl(out, d); }
	}
	if (t.expected_fields) {
		files::detail::put(out, t.expected_fields->size);
		files::detail::put(out, t.expected_fields->chain);
	}
}
template <class Layout>
std::string encode(const basic_transaction<Layout>& t) {
	std::string out;
	encode(out, t);
	return out;
}

// The inverse of encode(); bytes no encoding produces throw corrupt_index.
template <class Layout>
basic_transaction<Layout> decode(std::string_view bytes) {
	static_assert(loggable<Layout>, "strata: a logged layout's parts need encode_part and decode_part");
	using op = basic_operation<Layout>;
	files::detail::reader in(bytes, "strata command");
	basic_transaction<Layout> t;
	for (std::uint64_t n = in.count(2); n; --n) {
		op& o = t.ops.emplace_back();
		const std::uint8_t kind = detail::get_u8(in);
		if (kind > op::delete_meta) { in.bad("operation kind"); }
		o.type = static_cast<typename op::kind>(kind);
		if (o.type == op::set_meta || o.type == op::delete_meta) {
			o.meta_key = std::string(in.sized());
			if (o.type == op::set_meta) { o.meta_value = std::string(in.sized()); }
			continue;
		}
		const std::uint8_t flags = detail::get_u8(in);
		if (flags & ~detail::has_expected_version) { in.bad("operation flags"); }
		if (flags) { o.expected_version = in.get(); }
		if (o.type == op::replace_key || o.type == op::delete_key) { o.key = std::string(in.sized()); }
		if (o.type == op::replace_docid || o.type == op::delete_docid) { o.docid = in.get(); }
		if (o.type == op::add || o.type == op::replace_key || o.type == op::replace_docid) { decode_document(in, o.doc); }
	}
	const std::uint8_t mask = detail::get_u8(in);
	if (mask & ~detail::mask_bits) { in.bad("transaction mask"); }
	if (mask & detail::has_expected_schema) { t.expected_schema = in.get(); }
	if (mask & detail::has_set_schema) { t.set_schema = in.get(); }
	if (mask & detail::has_fields) {
		const std::uint64_t n = in.count(4);
		if (n == 0) { in.bad("an empty field list"); }
		for (std::uint64_t i = 0; i < n; ++i) { t.fields.push_back(detail::get_decl(in)); }
	}
	if (mask & detail::has_expected_fields) {
		field_mark m;
		m.size = in.get();
		m.chain = in.get();
		t.expected_fields = m;
	}
	in.finish();
	return t;
}

// A payload's kind: batch or checkpoint. An unknown kind or version is another build's log
// (unsupported_index), never damage.
inline std::uint8_t kind_of(std::string_view payload) {
	if (payload.size() < 2) { throw corrupt_index("strata command: truncated"); }
	const std::uint8_t kind = std::uint8_t(payload[0]), v = std::uint8_t(payload[1]);
	if ((kind != batch && kind != checkpoint && kind != chunk) || v != version) { throw unsupported_index("strata command: unknown kind or version"); }
	return kind;
}

// A batch record's payload: the kind and version, the transaction count, then `body`, each
// transaction as its LEB128 length and bytes (add_transaction). Transaction i, counted from 1,
// of record s sits at position {s, i}.
inline void add_transaction(std::string& body, std::string_view transaction) { files::detail::put_bytes(body, transaction); }
inline std::uint64_t batch_bytes(std::uint64_t count, std::uint64_t body_bytes) { return 2 + detail::leb_bytes(count) + body_bytes; }
inline std::string batch_payload(std::uint64_t count, std::string_view body) {
	std::string out;
	out.reserve(batch_bytes(count, body.size()));
	detail::put_u8(out, batch);
	detail::put_u8(out, version);
	files::detail::put(out, count);
	out += body;
	return out;
}
// A batch's transactions, as views into `payload`; at least one.
inline std::vector<std::string_view> decode_batch(std::string_view payload) {
	if (kind_of(payload) != batch) { throw corrupt_index("strata command: not a batch"); }
	files::detail::reader in(payload.substr(2), "strata batch");
	std::vector<std::string_view> out;
	const std::uint64_t n = in.count(1);
	if (n == 0) { in.bad("an empty batch"); }
	out.reserve(n);
	for (std::uint64_t i = 0; i < n; ++i) { out.push_back(in.sized()); }
	in.finish();
	return out;
}

// A chunk record (rocksdb-parity A3): a slice of one transaction's encoding past the command
// limit, in a record of its own. Its place byte, then the slice, at least one byte, to the
// payload's end. A run is a first, any middles and a last; the last's record holds the
// transaction's position {s, 1}.
enum class chunk_place : std::uint8_t { first = 1, middle = 2, last = 3 };
inline constexpr std::uint64_t chunk_overhead = 3;
inline std::string chunk_payload(chunk_place p, std::string_view bytes) {
	std::string out;
	out.reserve(chunk_overhead + bytes.size());
	detail::put_u8(out, chunk);
	detail::put_u8(out, version);
	detail::put_u8(out, std::uint8_t(p));
	out += bytes;
	return out;
}
struct chunk_record {
	chunk_place place = chunk_place::first;
	std::string_view bytes;
};
inline chunk_record decode_chunk(std::string_view payload) {
	if (kind_of(payload) != chunk) { throw corrupt_index("strata command: not a chunk"); }
	if (payload.size() <= chunk_overhead) { throw corrupt_index("strata command: an empty chunk"); }
	const std::uint8_t p = std::uint8_t(payload[2]);
	if (p < 1 || p > 3) { throw corrupt_index("strata command: a chunk's place"); }
	return {chunk_place(p), payload.substr(chunk_overhead)};
}

// A checkpoint record (D5): the index, the manifest-log generation that holds its state, and
// the position that state is applied through.
struct checkpoint_record {
	std::array<char, 16> uuid{};
	std::uint64_t generation = 0;
	files::position at;
	bool operator==(const checkpoint_record&) const = default;
};
inline std::string encode_checkpoint(const checkpoint_record& c) {
	std::string out;
	detail::put_u8(out, checkpoint);
	detail::put_u8(out, version);
	out.append(c.uuid.data(), c.uuid.size());
	files::detail::put(out, c.generation);
	files::detail::put(out, c.at.index);
	files::detail::put(out, c.at.applied);
	return out;
}
inline checkpoint_record decode_checkpoint(std::string_view payload) {
	if (kind_of(payload) != checkpoint) { throw corrupt_index("strata command: not a checkpoint"); }
	files::detail::reader in(payload.substr(2), "strata checkpoint");
	checkpoint_record c;
	const std::string_view uuid = in.bytes(c.uuid.size());
	std::copy(uuid.begin(), uuid.end(), c.uuid.begin());
	c.generation = in.get();
	c.at.index = in.get();
	c.at.applied = in.get();
	in.finish();
	return c;
}

}  // namespace command
}  // namespace strata
