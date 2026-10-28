/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The field table (fields-and-keys D2): one append-only table of field declarations per index,
// replicated with the log. A declaration's id is the table's size when it applies, so every
// replica assigns the same ids. A transaction names the fields it declares by position
// (`declared(i)`) and the ones it read from the table by id under a mark (size and hash chain),
// so no host predicts an id and a lost tail cannot misattribute one.
//
// `fields-<gen>.kv` is append-only: a magic and version, then blocks [count][declarations][CRC32C of
// the block]. The manifest names the file with the prefix it may read (size, and the last
// block's CRC), so bytes past it (a torn append) are ignored and a commit that declares appends
// one block; an older manifest's prefix stays valid.

#pragma once

#include "hashes.hh"
#include "strata/core/files.hh"
#include "strata/core/format.hh"

#include <crc32c/crc32c.hh>

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

enum class field_role : std::uint8_t { indexed = 0, key = 1 };
// What an indexed field's postings hold (Lucene's IndexOptions).
enum class field_postings : std::uint8_t { docs = 0, freqs = 1, positions = 2 };

struct field_declaration {
	std::string name;  // host bytes, 1 to max_term_bytes, matched only exactly
	field_role role = field_role::indexed;
	field_postings postings = field_postings::docs;
	bool norms = false;
	bool operator==(const field_declaration&) const = default;
};

// The table as a transaction's host last read it: its size and the hash chain at that size.
struct field_mark {
	std::uint64_t size = 0;
	std::uint64_t chain = 0;
	bool operator==(const field_mark&) const = default;
};

// How a document part names its field: by an entry of the transaction's `fields`, or by an id
// read from the table under the transaction's mark.
struct field_ref {
	enum by : std::uint8_t { declaration, id_of };
	by type = id_of;
	std::uint32_t n = 0;
	static constexpr field_ref declared(std::uint32_t i) noexcept { return {declaration, i}; }
	static constexpr field_ref id(std::uint32_t n) noexcept { return {id_of, n}; }
	bool operator==(const field_ref&) const = default;
};

// Stage 1's resolution of a transaction's references (validate_context carries it).
struct field_resolution {
	std::span<const std::uint32_t> declared;  // the id of each entry of the transaction's fields
	std::uint64_t known = 0;                  // ids below it are the host's: expected_fields.size, or 0
	std::optional<std::uint32_t> key;         // the table's key field after this transaction's declarations
	std::optional<std::uint32_t> resolve(field_ref r) const noexcept {
		if (r.type == field_ref::declaration) {
			if (r.n >= declared.size()) { return std::nullopt; }
			return declared[r.n];
		}
		if (r.n >= known) { return std::nullopt; }
		return r.n;
	}
};

namespace fields_detail {
inline constexpr char magic[8] = {'S', 'T', 'R', 'F', 'L', 'D', '0', '1'};
// One declaration's bytes: [name length LEB][name][role][postings][flags]; flags bit 0 is norms.
inline void put_decl(std::string& out, const field_declaration& d) {
	files::detail::put_bytes(out, d.name);
	out.push_back(char(d.role));
	out.push_back(char(d.postings));
	out.push_back(char(d.norms ? 1 : 0));
}
}  // namespace fields_detail

// What a declaration may be (stage 1's rule and the decoder's): null when valid, else why not.
inline const char* field_problem(const field_declaration& d) noexcept {
	if (d.name.empty() || d.name.size() > format::max_term_bytes) { return "name length"; }
	if (std::uint8_t(d.role) > 1) { return "role"; }
	if (std::uint8_t(d.postings) > 2) { return "postings"; }
	if (d.role == field_role::key && (d.postings != field_postings::docs || d.norms)) { return "key field options"; }
	return nullptr;
}

class field_table {
public:
	std::size_t size() const noexcept { return decls_.size(); }
	const field_declaration& operator[](std::size_t id) const { return decls_[id]; }
	std::optional<std::uint32_t> find(std::string_view name) const {
		const auto it = ids_.find(name);
		return it == ids_.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
	}
	// The id of the key field, if one is declared.
	std::optional<std::uint32_t> key_field() const noexcept { return key_; }
	field_mark mark() const noexcept { return {size(), chain_.back()}; }
	// The chain at a size not above the table's.
	std::optional<std::uint64_t> chain_at(std::uint64_t n) const noexcept {
		if (n >= chain_.size()) { return std::nullopt; }
		return chain_[n];
	}
	// Appends a valid declaration whose name is absent, and returns its id.
	std::uint32_t append(field_declaration d) {
		const std::uint32_t id = std::uint32_t(decls_.size());
		std::string in(8, '\0');
		const std::uint64_t c = chain_.back();
		std::memcpy(in.data(), &c, 8);
		fields_detail::put_decl(in, d);
		const std::uint64_t next = xxh64::hash(in);
		// Everything that can throw comes first, so a failure leaves the table as it was.
		decls_.reserve(decls_.size() + 1);
		chain_.reserve(chain_.size() + 1);
		ids_.emplace(d.name, id);
		if (d.role == field_role::key) { key_ = id; }
		name_bytes_ += d.name.size();
		decls_.push_back(std::move(d));
		chain_.push_back(next);
		return id;
	}
	// Drops the declarations from id `n` on (a rejected transaction's).
	void truncate(std::size_t n) {
		while (decls_.size() > n) {
			ids_.erase(decls_.back().name);
			name_bytes_ -= decls_.back().name.size();
			if (key_ == decls_.size() - 1) { key_.reset(); }
			decls_.pop_back();
			chain_.pop_back();
		}
	}
	// The bytes of all the names, for sizing the table's file.
	std::uint64_t name_bytes() const noexcept { return name_bytes_; }
	bool operator==(const field_table& o) const { return decls_ == o.decls_; }

private:
	std::vector<field_declaration> decls_;
	std::vector<std::uint64_t> chain_{0};
	std::map<std::string, std::uint32_t, std::less<>> ids_;
	std::optional<std::uint32_t> key_;
	std::uint64_t name_bytes_ = 0;
};

namespace files {

// The file's start: magic and version.
inline std::string fields_header() {
	std::string out(fields_detail::magic, 8);
	detail::put(out, version);
	return out;
}
// Declarations [from, to) of the table as one block: [count][declarations][CRC32C of both].
inline std::string encode_fields_block(const field_table& t, std::size_t from, std::size_t to) {
	std::string out;
	detail::put(out, to - from);
	for (std::size_t i = from; i < to; ++i) { fields_detail::put_decl(out, t[i]); }
	detail::seal(out);
	return out;
}
// The whole table as a file (a snapshot): the header and one block, or the header alone when empty.
inline std::string encode_fields(const field_table& t) { return fields_header() + (t.size() ? encode_fields_block(t, 0, t.size()) : std::string()); }

// Decodes a file's blocks into `out` (which starts empty). Returns the last block's CRC (0 for
// none). Every length is bounded by the bytes left; a name twice, a second key field, or a
// declaration `field_problem` refuses is damage.
inline std::uint32_t decode_fields(std::string_view file, field_table& out) {
	auto bad = [](const char* why) { throw corrupt_index(std::string("fields: ") + why); };
	if (file.size() < 9 || std::memcmp(file.data(), fields_detail::magic, 8) != 0) { bad("magic"); }
	const char* p = file.data() + 8;
	const char* const end = file.data() + file.size();
	std::uint64_t v = 0;
	if (!varint::leb128::decode(&p, end, v)) { bad("truncated"); }
	if (v != version) { throw unsupported_index("fields: version " + std::to_string(v)); }
	std::uint32_t last = 0;
	while (p != end) {
		const char* const start = p;
		std::uint64_t count = 0;
		if (!varint::leb128::decode(&p, end, count) || count == 0) { bad("block count"); }
		if (count > std::uint64_t(end - p) / 5) { bad("block count"); }  // a declaration is 5 bytes at least
		if (out.size() + count > 0xffffffffu) { bad("too many fields"); }
		for (std::uint64_t i = 0; i < count; ++i) {
			std::uint64_t len = 0;
			if (!varint::leb128::decode(&p, end, len)) { bad("truncated"); }
			if (len == 0 || len > format::max_term_bytes || len + 3 > std::uint64_t(end - p)) { bad("name length"); }
			field_declaration d;
			d.name.assign(p, len);
			p += len;
			const std::uint8_t role = std::uint8_t(p[0]), postings = std::uint8_t(p[1]), flags = std::uint8_t(p[2]);
			p += 3;
			if (flags > 1) { bad("flags"); }
			d.role = field_role(role);
			d.postings = field_postings(postings);
			d.norms = flags == 1;
			if (field_problem(d)) { bad(field_problem(d)); }
			if (out.find(d.name)) { bad("a name twice"); }
			if (d.role == field_role::key && out.key_field()) { bad("two key fields"); }
			out.append(std::move(d));
		}
		if (end - p < 4) { bad("truncated"); }
		std::memcpy(&last, p, 4);
		if (last != ::kronuz::crc32c::crc32c(start, p - start)) { bad("checksum"); }
		p += 4;
	}
	return last;
}

}  // namespace files
}  // namespace strata
