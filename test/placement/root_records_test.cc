// The root's record bytes (quest placement A1; contract strata:placement-records, pre-release
// bytes under D30): each record and a command against fixed fixtures, both ways; every strict
// prefix and a trailing byte refused; each decoder rule refused by a record that breaks it alone;
// keys and paths, with key order path order, nested paths distinct and a subtree one key range
// right after its path.
#include "strata/placement/records.hh"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace sp = strata::placement;

int failures = 0;
bool check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 40) { std::cerr << "FAIL: " << what << '\n'; }
	return ok;
}
std::string bytes(std::initializer_list<int> b) {
	std::string out;
	for (int x : b) { out.push_back(char(x)); }
	return out;
}
std::string hex(std::string_view s) {
	static const char* d = "0123456789abcdef";
	std::string out;
	for (unsigned char c : s) {
		out.push_back(d[c >> 4]);
		out.push_back(d[c & 15]);
	}
	return out;
}
template <class F>
bool refuses(F decode, std::string_view in) {
	try {
		decode(in);
	} catch (const sp::corrupt_record&) {
		return true;
	} catch (const std::exception& e) {
		std::cerr << "  (threw " << e.what() << ", not corrupt_record)\n";
		return false;
	}
	return false;
}

// A fixture: the value, its bytes, and the decoder that reads them.
template <class R, class D>
void fixture(const char* name, const R& value, const std::string& expected, D decode) {
	const auto got = sp::encode(value);
	check(got == expected, std::string(name) + " encodes to its fixture: " + hex(got) + " vs " + hex(expected));
	try {
		check(decode(expected) == value, std::string(name) + " decodes from its fixture");
	} catch (const std::exception& e) {
		check(false, std::string(name) + " decodes: " + e.what());
	}
	for (std::size_t n = 0; n < expected.size(); ++n) { check(refuses(decode, expected.substr(0, n)), std::string(name) + " refuses its first " + std::to_string(n) + " bytes"); }
	check(refuses(decode, expected + '\0'), std::string(name) + " refuses a trailing byte");
}

const std::string key5_l = bytes({'L', 0, 0, 0, 0, 0, 0, 0, 5});
const std::string key5_n = bytes({'N', 0, 0, 0, 0, 0, 0, 0, 5});

void fixtures() {
	fixture("directory", sp::directory_record{3, "ab"}, bytes({1, 3, 2, 'a', 'b'}), sp::decode_directory);
	fixture("groups", sp::groups_record{300}, bytes({1, 0xac, 0x02}), sp::decode_groups);
	sp::node_record n{"n1", {"10.0.0.1:9880"}, "\xaa\xbb", sp::node_state::leaving, 7, ""};
	fixture("node", n, bytes({1, 2, 'n', '1', 1, 13, '1', '0', '.', '0', '.', '0', '.', '1', ':', '9', '8', '8', '0', 2, 0xaa, 0xbb, 1, 7, 0}), sp::decode_node);
	fixture("load", sp::load_record{5, 2}, bytes({1, 5, 2}), sp::decode_load);
	sp::namespace_record ns{3, 2, {{9, 1, {{4, sp::role::voter}, {2, sp::role::voter}, {7, sp::role::witness}}}}};
	fixture("namespace", ns, bytes({1, 3, 2, 1, 9, 1, 3, 4, 0, 2, 0, 7, 2}), sp::decode_namespace);
	sp::command c{{{sp::write::put, sp::load_key(5), std::nullopt, sp::encode(sp::load_record{1, 0})}, {sp::write::erase, sp::node_key(5), 3, ""}}};
	std::string cb = bytes({1, 2, 1, 9});
	cb += key5_l + bytes({0, 3, 1, 1, 0, 2, 9}) + key5_n + bytes({1, 3});
	fixture("command", c, cb, sp::decode_command);
	check(sp::node_key(258) == bytes({'N', 0, 0, 0, 0, 0, 0, 1, 2}) && sp::load_key(5) == key5_l, "node and load keys are the tag and the id big-endian");
	check(sp::directory_key() == "D" && sp::groups_key() == "G" && sp::namespace_key("twitter/user") == std::string("Ptwitter\0user", 13), "the directory, counter and namespace keys");
}

// Each rule refused by a record that breaks it alone.
void rules() {
	const auto dir = [](std::string_view s) { return sp::decode_directory(s); };
	const auto grp = [](std::string_view s) { return sp::decode_groups(s); };
	const auto node = [](std::string_view s) { return sp::decode_node(s); };
	const auto load = [](std::string_view s) { return sp::decode_load(s); };
	const auto nsd = [](std::string_view s) { return sp::decode_namespace(s); };
	const auto cmd = [](std::string_view s) { return sp::decode_command(s); };
	check(refuses(dir, sp::encode(sp::directory_record{0, ""})), "next node 0");
	check(refuses(grp, sp::encode(sp::groups_record{0})), "next group 0");
	check(refuses(dir, bytes({2, 3, 0})), "an unknown format");
	check(refuses(dir, bytes({1, 0x83, 0x00, 0})), "an overlong integer");
	check(refuses(dir, bytes({1, 0x83, 0x80, 0x00, 0})), "an overlong integer of three bytes");
	// The fuzzer's crash input: an overlong node id re-encodes to other bytes.
	check(refuses(nsd, bytes({1, 3, 2, 1, 9, 1, 3, 4, 0, 0xfa, 0, 0, 3, 2})), "an overlong node id");
	check(refuses(grp, bytes({1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02})), "an integer past 64 bits");
	check(refuses(node, sp::encode(sp::node_record{"", {}, "", sp::node_state::active, 0, ""})), "an empty name");
	check(refuses(node, sp::encode(sp::node_record{"n", {""}, "", sp::node_state::active, 0, ""})), "an empty address");
	check(refuses(node, bytes({1, 1, 'n', 0, 0, 3, 0, 0})), "an unknown node state");
	check(refuses(node, bytes({1, 1, 'n', 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x40, 0, 0, 0, 0})), "an address count of 2^62, past the bytes left, refused before any allocation");
	check(refuses(load, sp::encode(sp::load_record{1, 2})), "more leaderships than groups");
	const auto ns = [](std::uint32_t rf, std::vector<sp::shard> shards) { return sp::encode(sp::namespace_record{rf, 0, std::move(shards)}); };
	const sp::member v1{1, sp::role::voter}, v2{2, sp::role::voter}, l2{2, sp::role::learner};
	check(!refuses(nsd, ns(1, {{1, 1, {v1}}})), "a lone-voter shard is a record");
	check(refuses(nsd, ns(0, {{1, 1, {v1}}})), "rf 0");
	check(refuses(nsd, ns(1, {})), "no shards");
	check(refuses(nsd, ns(1, {{0, 1, {v1}}})), "group id 0");
	check(refuses(nsd, ns(1, {{1, 1, {v1}}, {1, 1, {v2}}})), "a group id twice");
	check(refuses(nsd, ns(1, {{1, 0, {v1}}})), "incarnation 0");
	check(refuses(nsd, ns(1, {{1, 1, {}}})), "a shard without members");
	check(refuses(nsd, ns(1, {{1, 1, {{0, sp::role::voter}}}})), "node 0");
	check(refuses(nsd, ns(2, {{1, 1, {v1, {1, sp::role::learner}}}})), "a node twice in a shard");
	check(refuses(nsd, ns(2, {{1, 1, {l2, v1}}})), "a preferred leader that is not a voter");
	check(refuses(nsd, bytes({1, 1, 0, 1, 1, 1, 1, 1, 3})), "an unknown role");
	check(refuses(nsd, bytes({1, 1, 0, 0xe8, 0x07, 1, 1, 1, 1, 0})), "a shard count past the bytes left");
	check(refuses(nsd, bytes({1, 1, 0, 1, 1, 1, 0xe8, 0x07, 1, 0})), "a member count past the bytes left");
	check(refuses(nsd, bytes({1, 0xff, 0xff, 0xff, 0xff, 0x1f, 0, 1, 1, 1, 1, 1, 0})), "an rf past 32 bits");

	// Commands.
	const auto put = [](std::string key, std::string value) { return sp::write{sp::write::put, std::move(key), std::nullopt, std::move(value)}; };
	const auto erase = [](std::string key) { return sp::write{sp::write::erase, std::move(key), 0, ""}; };
	const auto load0 = sp::encode(sp::load_record{});
	check(!refuses(cmd, sp::encode(sp::command{{put(sp::load_key(1), load0), erase(sp::namespace_key("a"))}})), "a put and an erase are a command");
	check(refuses(cmd, bytes({1, 0})), "a command without writes");
	check(refuses(cmd, bytes({2, 1, 2, 2, 'P', 'a', 0})), "an unknown command format");
	check(refuses(cmd, bytes({1, 1, 3, 2, 'P', 'a', 0})), "an unknown write kind");
	check(refuses(cmd, bytes({1, 1, 2, 2, 'P', 'a', 2})), "an unknown expectation flag");
	check(refuses(cmd, sp::encode(sp::command{{erase(sp::namespace_key("a")), erase(sp::namespace_key("a"))}})), "a key twice");
	check(refuses(cmd, sp::encode(sp::command{{erase(sp::directory_key())}})), "an erase of the directory");
	check(refuses(cmd, sp::encode(sp::command{{erase(sp::groups_key())}})), "an erase of the group counter");
	check(refuses(cmd, sp::encode(sp::command{{put(sp::node_key(1), load0)}})), "a put whose value its key does not accept");
	check(refuses(cmd, sp::encode(sp::command{{put("X", load0)}})), "a put of an unknown key");
	check(refuses(cmd, sp::encode(sp::command{{erase(std::string("N\0\0\0\0\0\0\0\0", 9))}})), "node id 0 is no key");
	check(refuses(cmd, sp::encode(sp::command{{erase("Nabc")}})), "a node key of the wrong length");
	check(refuses(cmd, sp::encode(sp::command{{erase("Pa//b")}})), "a namespace key of an invalid path");
}

void paths() {
	for (std::string p : {"a", "twitter", "twitter/user", ".x", "a/.b/c", "a-b", "\xc3\xa9t\xc3\xa9"}) { check(sp::valid_path(p), "valid path " + p); }
	for (const std::string& p : std::vector<std::string>{"", "/", "/a", "a/", "a//b", std::string("a\0b", 3)}) { check(!sp::valid_path(p), "invalid path " + hex(p)); }
	check(sp::max_path == strata::format::max_term_bytes - 1, "a path is strata's key limit less the tag");
	// Key order is path order, name by name: a path's subtree sorts right after it.
	const std::vector<std::string> sorted = {"twitte", "twitter", "twitter/user", "twitter/user/a", "twitter-x", "twitter0"};
	for (std::size_t i = 1; i < sorted.size(); ++i) { check(sp::namespace_key(sorted[i - 1]) < sp::namespace_key(sorted[i]), sorted[i - 1] + " sorts before " + sorted[i]); }
	check(sp::namespace_key("twitter/user") == std::string("Ptwitter\0user", 13), "a key stores each / as byte 0");
	check(sp::namespace_key("twitter") != sp::namespace_key("twitter/user"), "nested paths are distinct keys");
	check(sp::subtree_begin("twitter") == std::string("Ptwitter\0", 9) && sp::subtree_end("twitter") == std::string("Ptwitter\1", 9), "a subtree is [key \\0, key \\1)");
	check(sp::path_of(sp::namespace_key("twitter/user")) == std::optional<std::string>("twitter/user"), "path_of reads a key back");
	check(!sp::path_of("Pa/b") && !sp::path_of(std::string("Pa\0\0b", 5)) && !sp::path_of(sp::node_key(1)), "path_of refuses a raw /, an empty name and other keys");
	check(sp::namespaces_begin() <= sp::namespace_key("\x01") && sp::namespace_key("\xff\xff") < sp::namespaces_end(), "every namespace key is in the namespace range");
	// Over random paths: key order equals path order, and a subtree's range holds exactly its
	// descendants, right after the path's own key.
	const auto names = [](const std::string& p) {
		std::vector<std::string> out(1);
		for (char c : p) {
			if (c == '/') {
				out.emplace_back();
			} else {
				out.back().push_back(c);
			}
		}
		return out;
	};
	std::mt19937_64 rng(7);
	const std::string alphabet = std::string("ab-./0z\x01\x7f\xff", 10);
	std::vector<std::string> ps;
	while (ps.size() < 400) {
		std::string p;
		for (std::size_t n = 1 + rng() % 7; n; --n) { p.push_back(alphabet[rng() % alphabet.size()]); }
		if (sp::valid_path(p)) { ps.push_back(p); }
	}
	std::size_t inside = 0;
	for (const auto& p : ps) {
		const auto lo = sp::subtree_begin(p), hi = sp::subtree_end(p), own = sp::namespace_key(p);
		check(sp::path_of(own) == p, "path_of reads " + hex(p) + " back");
		for (const auto& q : ps) {
			const auto k = sp::namespace_key(q);
			const auto np = names(p), nq = names(q);
			const bool path_less = std::lexicographical_compare(np.begin(), np.end(), nq.begin(), nq.end(), [](const std::string& x, const std::string& y) {
				return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(), [](char c, char d) { return static_cast<unsigned char>(c) < static_cast<unsigned char>(d); });
			});
			if (!check(path_less == (own < k), hex(p) + " and " + hex(q) + ": key order is path order")) { return; }
			const bool below = q.size() > p.size() + 1 && q.compare(0, p.size(), p) == 0 && q[p.size()] == '/';
			const bool in_range = lo <= k && k < hi;
			inside += below;
			if (!check(below == in_range, hex(q) + (below ? " is" : " is not") + " under " + hex(p) + ", so its key is" + (below ? "" : " not") + " in the subtree's range")) { return; }
			check(!(own < k && k < lo), "no key sorts between " + hex(p) + " and its subtree");
		}
	}
	check(inside > 0, "the random paths nest");
}

}  // namespace

int main() {
	fixtures();
	rules();
	paths();
	std::printf("root records: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
