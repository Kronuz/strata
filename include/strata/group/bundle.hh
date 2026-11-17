// The checkpoint bundle (contract strata:checkpoint-bundle; cluster-release D16): consensus's
// application image for a group. It binds the log to strata by position P_b and carries the
// application's file set and its opaque state. Little-endian, in the order of D16's table:
//
//   magic and version                      8
//   P_b, its term                          8 + 8
//   file count, then per file              4 + per file 2 + name + 8 + 1 + 4
//     name, length at P_b, immutable flag, CRC32C (0 unless immutable)
//   application state: length, bytes      4 + its own
//   CRC32C of the above                    4
#pragma once

#include <crc32c/crc32c.hh>

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace strata::group {

// A bundle that fails its checks: recovery fails closed on it.
struct corrupt_bundle : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// A file of the set as of P_b: its length there, and its CRC32C once rotated (immutable).
struct file_entry {
	std::uint64_t length = 0;
	std::optional<std::uint32_t> crc;
	bool operator==(const file_entry&) const = default;
};
using file_map = std::map<std::string, file_entry, std::less<>>;

struct bundle {
	std::uint64_t position = 0, term = 0;  // P_b and its term: the capture's through and term
	file_map files;
	std::string state;  // the application's opaque bytes
	bool operator==(const bundle&) const = default;
};

namespace bundle_detail {

inline constexpr char magic[8] = {'S', 'T', 'G', 'B', 'N', 'D', 0, 1};
inline constexpr std::size_t max_name = 255;

// A name of the set: one path component, so recovery never reaches outside its directory.
inline bool valid_name(std::string_view name) {
	return !name.empty() && name.size() <= max_name && name != "." && name != ".." && name.find('/') == std::string_view::npos &&
		name.find('\0') == std::string_view::npos;
}

inline void put(std::string& out, std::uint64_t v, int bytes) {
	for (int i = 0; i < bytes; ++i) { out.push_back(char((v >> (8 * i)) & 0xff)); }
}

struct reader {
	std::string_view in;
	std::size_t at = 0;
	std::string_view take(std::size_t n) {
		if (n > in.size() - at) { throw corrupt_bundle("strata::group: bundle truncated"); }
		auto out = in.substr(at, n);
		at += n;
		return out;
	}
	std::uint64_t get(int bytes) {
		auto b = take(std::size_t(bytes));
		std::uint64_t v = 0;
		for (int i = 0; i < bytes; ++i) { v |= std::uint64_t(static_cast<unsigned char>(b[std::size_t(i)])) << (8 * i); }
		return v;
	}
};

}  // namespace bundle_detail

inline std::string encode(const bundle& b) {
	namespace d = bundle_detail;
	if (b.state.size() > 0xffffffffu || b.files.size() > 0xffffffffu) { throw std::length_error("strata::group: bundle too large"); }
	std::string out(d::magic, sizeof d::magic);
	d::put(out, b.position, 8);
	d::put(out, b.term, 8);
	d::put(out, b.files.size(), 4);
	for (const auto& [name, f] : b.files) {
		if (!d::valid_name(name)) { throw std::invalid_argument("strata::group: invalid file-set name"); }
		d::put(out, name.size(), 2);
		out += name;
		d::put(out, f.length, 8);
		d::put(out, f.crc ? 1 : 0, 1);
		d::put(out, f.crc.value_or(0), 4);
	}
	d::put(out, b.state.size(), 4);
	out += b.state;
	d::put(out, kronuz::crc32c::crc32c(out), 4);
	return out;
}

// Decodes a bundle; `bytes` may run past its end (an image padded to its reservation).
inline bundle decode(std::string_view bytes) {
	namespace d = bundle_detail;
	d::reader r{bytes};
	if (std::memcmp(r.take(sizeof d::magic).data(), d::magic, sizeof d::magic) != 0) { throw corrupt_bundle("strata::group: bundle magic or version"); }
	bundle b;
	b.position = r.get(8);
	b.term = r.get(8);
	const auto count = r.get(4);
	for (std::uint64_t i = 0; i < count; ++i) {
		const std::string name(r.take(std::size_t(r.get(2))));
		file_entry f;
		f.length = r.get(8);
		const auto flag = r.get(1);
		const auto crc = std::uint32_t(r.get(4));
		if (flag > 1 || (!flag && crc)) { throw corrupt_bundle("strata::group: bundle file flag"); }
		if (flag) { f.crc = crc; }
		if (!d::valid_name(name) || !b.files.emplace(name, f).second) { throw corrupt_bundle("strata::group: bundle file name"); }
	}
	b.state = std::string(r.take(std::size_t(r.get(4))));
	const auto body = r.at;
	if (std::uint32_t(r.get(4)) != kronuz::crc32c::crc32c(bytes.substr(0, body))) { throw corrupt_bundle("strata::group: bundle CRC32C"); }
	return b;
}

// The encoded size of a bundle, for consensus's application_cap.
inline std::size_t encoded_size(const bundle& b) {
	std::size_t n = 8 + 16 + 4 + 4 + b.state.size() + 4;
	for (const auto& [name, f] : b.files) { n += 2 + name.size() + 8 + 1 + 4; }
	return n;
}

}  // namespace strata::group
