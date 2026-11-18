// Builds termdict dictionaries from fieldsize.py's key files and prints their bytes per field
// (fields-and-keys revision 1, T1). Every dictionary has the alphabet its own keys use, as
// prospect's inverted builder computes it; values are `value_bytes` zero bytes each. Blocks
// are counted through the builder's sink, so "one block" is what the builder produced.
//
// Usage: fieldprobe DIR VALUE_BYTES POOL_THRESHOLD...
// Output: `flat blocks index`; per field `field id terms own_blocks own_nblocks own_index
// full_blocks full_nblocks` (own: own alphabet; full: 256 symbols, the one-block form keeps no
// alphabet); per threshold T `pool T fields terms blocks index` for the fields under T terms,
// pooled in one dictionary keyed by the field id's varint (D10's form) and the value.
#include "termdict.hh"

#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static std::vector<std::string> load(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	std::vector<std::string> keys;
	std::uint32_t n;
	while (in.read(reinterpret_cast<char*>(&n), 4)) {
		std::string k(n, '\0');
		in.read(k.data(), n);
		keys.push_back(std::move(k));
	}
	return keys;
}

struct built {
	std::size_t blocks = 0, nblocks = 0, index = 0;
};

static built build(const std::vector<std::string>& keys, std::size_t value_bytes, bool full_alphabet) {
	std::array<bool, 256> used{};
	if (full_alphabet) { used.fill(true); }
	for (const auto& k : keys) {
		for (char c : k) { used[std::uint8_t(c)] = true; }
	}
	std::string alphabet;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { alphabet.push_back(char(c)); }
	}
	built r;
	termdict::builder b(alphabet, [&](std::string_view block) { r.blocks += block.size(); ++r.nblocks; });
	const std::string value(value_bytes, '\0');
	for (const auto& k : keys) { b.add(k, value); }
	r.index = b.finish().index.size();
	r.nblocks /= 2;  // the sink takes a block's frame, then its entries (termdict.hh:194-196)
	return r;
}

static std::string field_varint(std::uint32_t n) {  // order-preserving, prefix-free (D10)
	if (n < 0xF0) { return std::string(1, char(n)); }
	std::string be;
	for (std::uint32_t v = n; v != 0; v >>= 8) { be.insert(be.begin(), char(v & 0xFF)); }
	return std::string(1, char(0xF0 + be.size())) + be;
}

int main(int argc, char** argv) {
	if (argc < 3) { return 2; }
	const std::string dir = argv[1];
	const std::size_t value_bytes = std::stoul(argv[2]);
	const auto flat = build(load(dir + "/flat.keys"), value_bytes, false);
	std::printf("flat %zu %zu\n", flat.blocks, flat.index);
	std::map<std::uint32_t, std::vector<std::string>> by_field;
	for (const auto& k : load(dir + "/field.keys")) {
		const std::uint32_t f = std::uint32_t(std::uint8_t(k[0])) << 24 | std::uint32_t(std::uint8_t(k[1])) << 16 | std::uint32_t(std::uint8_t(k[2])) << 8 | std::uint8_t(k[3]);
		by_field[f].push_back(k.substr(4));
	}
	for (const auto& [f, ks] : by_field) {
		const auto own = build(ks, value_bytes, false);
		const auto full = build(ks, value_bytes, true);
		std::printf("field %u %zu %zu %zu %zu %zu %zu\n", f, ks.size(), own.blocks, own.nblocks, own.index, full.blocks, full.nblocks);
	}
	for (int a = 3; a < argc; ++a) {
		const std::size_t t = std::stoul(argv[a]);
		std::vector<std::string> pooled;
		std::size_t fields = 0;
		for (const auto& [f, ks] : by_field) {
			if (ks.size() >= t) { continue; }
			++fields;
			for (const auto& k : ks) { pooled.push_back(field_varint(f) + k); }
		}
		const auto p = pooled.empty() ? built{} : build(pooled, value_bytes, false);
		std::printf("pool %zu %zu %zu %zu %zu\n", t, fields, pooled.size(), p.blocks, p.index);
	}
}
