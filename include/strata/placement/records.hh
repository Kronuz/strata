// The root's records and their bytes (contract strata:placement-records; cluster-release D5,
// cluster-membership M1 and M5; docs/mechanisms/placement-root.md). One strata index of the
// document layout holds them, each a key and a stored value:
//
//   "D"                  directory: the next node id, the application's attributes
//   "G"                  the next group id
//   "L" + id (8 B, BE)   a node's load: groups and preferred leaderships
//   "N" + id (8 B, BE)   a node: name, addresses, certificate pin, state, directory version, attributes
//   "P" + path           a namespace (one per index): rf, settings version, per shard its group
//                        id, incarnation and members (the first the preferred leader)
//
// A value is a format byte, then LEB128 integers in their shortest form and byte strings as a
// length and bytes; a decoder bounds every count by the bytes left, refuses trailing bytes and
// throws corrupt_record on any other input.
#pragma once

#include "strata/core/format.hh"

#include "varint.hh"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata::placement {

// A record or command that fails its checks: the root never stores one.
struct corrupt_record : std::runtime_error {
	using std::runtime_error::runtime_error;
};

using node_id = std::uint64_t;   // consensus's NodeId; 0 is no node
using group_id = std::uint64_t;  // consensus's GroupId; 0 is no group

inline constexpr std::uint8_t record_format = 1;

// --- keys --------------------------------------------------------------------------------

namespace tag {
inline constexpr char directory = 'D';
inline constexpr char groups = 'G';
inline constexpr char load = 'L';
inline constexpr char node = 'N';
inline constexpr char path = 'P';
}  // namespace tag

// The longest path: strata's key limit less the tag byte.
inline constexpr std::size_t max_path = format::max_term_bytes - 1;

// An index path: 1 to max_path bytes of '/'-separated names, none empty, no NUL. `.` names are
// records like any other; listing them is the scan's choice.
inline bool valid_path(std::string_view p) {
	if (p.empty() || p.size() > max_path || p.front() == '/' || p.back() == '/') { return false; }
	if (p.find('\0') != std::string_view::npos || p.find("//") != std::string_view::npos) { return false; }
	return true;
}

inline std::string directory_key() { return std::string(1, tag::directory); }
inline std::string groups_key() { return std::string(1, tag::groups); }

namespace detail {
inline std::string id_key(char t, std::uint64_t id) {
	std::string k(9, '\0');
	k[0] = t;
	for (int i = 0; i < 8; ++i) { k[std::size_t(1 + i)] = char((id >> (8 * (7 - i))) & 0xff); }
	return k;
}
inline std::optional<std::uint64_t> id_of(char t, std::string_view k) {
	if (k.size() != 9 || k[0] != t) { return std::nullopt; }
	std::uint64_t id = 0;
	for (std::size_t i = 1; i < 9; ++i) { id = (id << 8) | static_cast<unsigned char>(k[i]); }
	if (id == 0) { return std::nullopt; }
	return id;
}
}  // namespace detail

inline std::string node_key(node_id id) { return detail::id_key(tag::node, id); }
inline std::string load_key(node_id id) { return detail::id_key(tag::load, id); }
inline std::optional<node_id> node_of(std::string_view key) { return detail::id_of(tag::node, key); }
inline std::optional<node_id> load_of(std::string_view key) { return detail::id_of(tag::load, key); }

// A namespace's key: the tag, then the path's bytes, so keys sort as paths do and a subtree
// `p/...` is the range [namespace_key(p) + "/", namespace_key(p) + "0").
inline std::string namespace_key(std::string_view path) {
	if (!valid_path(path)) { throw std::invalid_argument("strata::placement: invalid path"); }
	std::string k(1, tag::path);
	k.append(path);
	return k;
}
// The path a namespace key names, if it is one.
inline std::optional<std::string_view> path_of(std::string_view key) {
	if (key.empty() || key[0] != tag::path || !valid_path(key.substr(1))) { return std::nullopt; }
	return key.substr(1);
}
// Every namespace key is in [namespaces_begin, namespaces_end).
inline std::string namespaces_begin() { return std::string(1, tag::path); }
inline std::string namespaces_end() { return std::string(1, char(tag::path + 1)); }
inline std::string nodes_begin() { return std::string(1, tag::node); }
inline std::string nodes_end() { return std::string(1, char(tag::node + 1)); }
inline std::string loads_begin() { return std::string(1, tag::load); }
inline std::string loads_end() { return std::string(1, char(tag::load + 1)); }

// --- records -----------------------------------------------------------------------------

enum class node_state : std::uint8_t { active = 0, leaving = 1, retired = 2 };
enum class role : std::uint8_t { voter = 0, learner = 1, witness = 2 };

struct directory_record {
	node_id next_node = 1;
	std::string attributes;  // the application's (Xapiand's cluster record, M1)
	bool operator==(const directory_record&) const = default;
};

struct groups_record {
	group_id next_group = 1;
	bool operator==(const groups_record&) const = default;
};

struct node_record {
	std::string name;
	std::vector<std::string> addresses;
	std::string pin;  // the certificate's key pin (M5)
	node_state state = node_state::active;
	std::uint64_t directory_version = 0;  // the directory record's version this change wrote
	std::string attributes;               // the application's (M1's admission fields)
	bool operator==(const node_record&) const = default;
};

struct load_record {
	std::uint64_t groups = 0;   // shard memberships, witnesses included
	std::uint64_t leaders = 0;  // preferred leaderships, at most groups
	bool operator==(const load_record&) const = default;
};

struct member {
	node_id node = 0;
	placement::role role = placement::role::voter;
	bool operator==(const member&) const = default;
};

struct shard {
	group_id group = 0;
	std::uint64_t incarnation = 1;
	std::vector<member> members;  // distinct nodes; the first, a voter, is the preferred leader
	bool operator==(const shard&) const = default;
};

struct namespace_record {
	std::uint32_t rf = 1;  // data replicas wanted; a shard runs short when nodes are fewer (D7)
	std::uint64_t settings_version = 0;
	std::vector<shard> shards;
	bool operator==(const namespace_record&) const = default;
};

// --- the codec ---------------------------------------------------------------------------

namespace codec {

inline void put(std::string& out, std::uint64_t v) {
	char buf[varint::leb128::max_bytes];
	out.append(buf, std::size_t(varint::leb128::encode(buf, v) - buf));
}
inline void put(std::string& out, std::string_view bytes) {
	put(out, std::uint64_t(bytes.size()));
	out.append(bytes);
}

struct reader {
	std::string_view in;
	const char* what;
	std::size_t at = 0;

	[[noreturn]] void fail(const char* why) const { throw corrupt_record(std::string("strata::placement: ") + what + ": " + why); }
	std::size_t left() const { return in.size() - at; }
	std::uint8_t byte() {
		if (!left()) { fail("truncated"); }
		return static_cast<std::uint8_t>(in[at++]);
	}
	template <class T = std::uint64_t>
	T number() {
		const char* p = in.data() + at;
		T v{};
		if (!varint::leb128::decode(&p, in.data() + in.size(), v)) { fail("truncated or overlong integer"); }
		at = std::size_t(p - in.data());
		return v;
	}
	std::string_view bytes() {
		const auto n = number();
		if (n > left()) { fail("truncated"); }
		auto out = in.substr(at, std::size_t(n));
		at += std::size_t(n);
		return out;
	}
	// A count of items each at least `least` bytes: bounded by what is left, before any reserve.
	std::size_t count(std::size_t least) {
		const auto n = number();
		if (n > left() / least) { fail("count past the bytes left"); }
		return std::size_t(n);
	}
	void format() {
		if (byte() != record_format) { fail("unknown format"); }
	}
	void end() const {
		if (left()) { fail("trailing bytes"); }
	}
};

}  // namespace codec

inline std::string encode(const directory_record& r) {
	std::string out(1, char(record_format));
	codec::put(out, r.next_node);
	codec::put(out, r.attributes);
	return out;
}
inline directory_record decode_directory(std::string_view in) {
	codec::reader r{in, "directory record"};
	r.format();
	directory_record d;
	d.next_node = r.number();
	if (d.next_node == 0) { r.fail("next node 0"); }
	d.attributes = r.bytes();
	r.end();
	return d;
}

inline std::string encode(const groups_record& g) {
	std::string out(1, char(record_format));
	codec::put(out, g.next_group);
	return out;
}
inline groups_record decode_groups(std::string_view in) {
	codec::reader r{in, "groups record"};
	r.format();
	groups_record g;
	g.next_group = r.number();
	if (g.next_group == 0) { r.fail("next group 0"); }
	r.end();
	return g;
}

inline std::string encode(const node_record& n) {
	std::string out(1, char(record_format));
	codec::put(out, n.name);
	codec::put(out, std::uint64_t(n.addresses.size()));
	for (const auto& a : n.addresses) { codec::put(out, a); }
	codec::put(out, n.pin);
	out.push_back(char(n.state));
	codec::put(out, n.directory_version);
	codec::put(out, n.attributes);
	return out;
}
inline node_record decode_node(std::string_view in) {
	codec::reader r{in, "node record"};
	r.format();
	node_record n;
	n.name = r.bytes();
	if (n.name.empty()) { r.fail("empty name"); }
	n.addresses.resize(r.count(1));
	for (auto& a : n.addresses) {
		a = r.bytes();
		if (a.empty()) { r.fail("empty address"); }
	}
	n.pin = r.bytes();
	const auto s = r.byte();
	if (s > std::uint8_t(node_state::retired)) { r.fail("unknown state"); }
	n.state = node_state(s);
	n.directory_version = r.number();
	n.attributes = r.bytes();
	r.end();
	return n;
}

inline std::string encode(const load_record& l) {
	std::string out(1, char(record_format));
	codec::put(out, l.groups);
	codec::put(out, l.leaders);
	return out;
}
inline load_record decode_load(std::string_view in) {
	codec::reader r{in, "load record"};
	r.format();
	load_record l;
	l.groups = r.number();
	l.leaders = r.number();
	if (l.leaders > l.groups) { r.fail("more leaderships than groups"); }
	r.end();
	return l;
}

inline std::string encode(const namespace_record& ns) {
	std::string out(1, char(record_format));
	codec::put(out, std::uint64_t(ns.rf));
	codec::put(out, ns.settings_version);
	codec::put(out, std::uint64_t(ns.shards.size()));
	for (const auto& s : ns.shards) {
		codec::put(out, s.group);
		codec::put(out, s.incarnation);
		codec::put(out, std::uint64_t(s.members.size()));
		for (const auto& m : s.members) {
			codec::put(out, m.node);
			out.push_back(char(m.role));
		}
	}
	return out;
}
inline namespace_record decode_namespace(std::string_view in) {
	codec::reader r{in, "namespace record"};
	r.format();
	namespace_record ns;
	ns.rf = r.number<std::uint32_t>();
	if (ns.rf == 0) { r.fail("rf 0"); }
	ns.settings_version = r.number();
	ns.shards.resize(r.count(3));  // a group id, an incarnation and a member count
	if (ns.shards.empty()) { r.fail("no shards"); }
	std::set<group_id> groups;
	for (auto& s : ns.shards) {
		s.group = r.number();
		if (s.group == 0 || !groups.insert(s.group).second) { r.fail("group id 0 or repeated"); }
		s.incarnation = r.number();
		if (s.incarnation == 0) { r.fail("incarnation 0"); }
		s.members.resize(r.count(2));  // a node id and a role
		if (s.members.empty()) { r.fail("a shard without members"); }
		for (auto& m : s.members) {
			m.node = r.number();
			const auto ro = r.byte();
			if (m.node == 0) { r.fail("node 0"); }
			if (ro > std::uint8_t(role::witness)) { r.fail("unknown role"); }
			m.role = role(ro);
		}
		if (s.members.front().role != role::voter) { r.fail("the preferred leader is not a voter"); }
		for (std::size_t i = 0; i < s.members.size(); ++i) {
			for (std::size_t k = 0; k < i; ++k) {
				if (s.members[i].node == s.members[k].node) { r.fail("a node twice in a shard"); }
			}
		}
	}
	r.end();
	return ns;
}

// Whether `value` is a well-formed record for `key`: the root stores nothing else.
inline bool valid_record(std::string_view key, std::string_view value) {
	try {
		if (key == directory_key()) {
			decode_directory(value);
		} else if (key == groups_key()) {
			decode_groups(value);
		} else if (node_of(key)) {
			decode_node(value);
		} else if (load_of(key)) {
			decode_load(value);
		} else if (path_of(key)) {
			decode_namespace(value);
		} else {
			return false;
		}
	} catch (const corrupt_record&) {
		return false;
	}
	return true;
}

// --- commands ----------------------------------------------------------------------------

// One record write of a root command: a put or an erase, conditional on the record's strata
// version (0: absent) when `expected` holds one.
struct write {
	enum kind_type : std::uint8_t { put = 1, erase = 2 };
	kind_type kind = put;
	std::string key;
	std::optional<std::uint64_t> expected;
	std::string value;  // a put's
	bool operator==(const write&) const = default;
};

// A root command: writes applied as one strata transaction, all or none. The leader prepares
// it from the records it read (root.hh); apply only checks and writes it.
struct command {
	std::vector<write> writes;
	bool operator==(const command&) const = default;
};

inline constexpr std::uint8_t command_format = 1;

inline std::string encode(const command& c) {
	std::string out(1, char(command_format));
	codec::put(out, std::uint64_t(c.writes.size()));
	for (const auto& w : c.writes) {
		out.push_back(char(w.kind));
		codec::put(out, w.key);
		out.push_back(char(w.expected ? 1 : 0));
		if (w.expected) { codec::put(out, *w.expected); }
		if (w.kind == write::put) { codec::put(out, w.value); }
	}
	return out;
}
// Checks every write: a known kind, a root key, distinct keys, a put's value a record its key's
// decoder accepts, and no erase of a counter.
inline command decode_command(std::string_view in) {
	codec::reader r{in, "command"};
	if (r.byte() != command_format) { r.fail("unknown format"); }
	command c;
	c.writes.resize(r.count(4));  // a kind, a key length and byte, a flag
	if (c.writes.empty()) { r.fail("no writes"); }
	std::set<std::string_view> keys;
	for (auto& w : c.writes) {
		const auto k = r.byte();
		if (k != write::put && k != write::erase) { r.fail("unknown write"); }
		w.kind = write::kind_type(k);
		const auto key = r.bytes();
		if (!keys.insert(key).second) { r.fail("a key twice"); }
		w.key = key;
		const auto flag = r.byte();
		if (flag > 1) { r.fail("unknown expectation flag"); }
		if (flag) { w.expected = r.number(); }
		if (w.kind == write::put) {
			w.value = r.bytes();
			if (!valid_record(w.key, w.value)) { r.fail("a record its key does not accept"); }
		} else if (!(node_of(w.key) || load_of(w.key) || path_of(w.key))) {
			r.fail("an erase of a counter or not a root key");  // ids are never reused (M1)
		}
	}
	r.end();
	return c;
}

}  // namespace strata::placement
