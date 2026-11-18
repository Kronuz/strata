// Builds termdict dictionaries from terms.py's key files and reports their bytes and
// lookup times (fields-and-keys, T1). One dictionary per file, or one per field for
// field.keys ([field id u32 BE] + value), each with the alphabet its own keys use, as
// prospect's inverted builder computes it. Values are `value_bytes` zero bytes each.
//
// Usage: dictprobe DIR VALUE_BYTES LOOKUPS
#include "termdict.hh"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
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

struct dict {
	std::string alphabet;
	termdict::tables t;
};

static dict build(const std::vector<std::string>& keys, std::size_t value_bytes, bool full_alphabet = false) {
	std::array<bool, 256> used{};
	if (full_alphabet) { used.fill(true); }
	for (const auto& k : keys) {
		for (char c : k) { used[std::uint8_t(c)] = true; }
	}
	dict d;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { d.alphabet.push_back(char(c)); }
	}
	termdict::builder b(d.alphabet);
	const std::string value(value_bytes, '\0');
	for (const auto& k : keys) { b.add(k, value); }
	d.t = b.finish();
	return d;
}

template <class Find>
static double time_finds(const std::vector<std::string>& probes, Find&& find) {
	std::size_t hits = 0;
	const auto t0 = std::chrono::steady_clock::now();
	for (const auto& p : probes) { hits += find(p) ? 1 : 0; }
	const auto t1 = std::chrono::steady_clock::now();
	if (hits != probes.size()) { std::fprintf(stderr, "missed %zu\n", probes.size() - hits); }
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(probes.size());
}

int main(int argc, char** argv) {
	if (argc < 4) { return 2; }
	const std::string dir = argv[1];
	const std::size_t value_bytes = std::stoul(argv[2]);
	const std::size_t lookups = std::stoul(argv[3]);
	std::mt19937_64 rng(42);
	for (const char* variant : {"flat", "fid", "field", "field1"}) {
		const std::string name = std::string(variant) == "field1" ? "field" : variant;
		// field1: a field whose terms fit one block (<= 64) keeps no alphabet and no index;
		// its directory entry would locate the block itself.
		const bool one_block = std::string(variant) == "field1";
		const auto keys = load(dir + "/" + name + ".keys");
		std::vector<std::size_t> pick(lookups);
		for (auto& p : pick) { p = rng() % keys.size(); }
		std::size_t blocks = 0, index = 0, fields = 1, alpha = 0;
		double ns = 0;
		if (name != "field") {
			const dict d = build(keys, value_bytes);
			blocks = d.t.blocks.size();
			index = d.t.index.size();
			alpha = d.alphabet.size();
			termdict::reader r(d.t.blocks, d.t.index);
			std::vector<std::string> probes;
			for (auto p : pick) { probes.push_back(keys[p]); }
			for (int round = 0; round < 3; ++round) { ns = time_finds(probes, [&](const std::string& k) { return r.find(k).has_value(); }); }
		} else {
			std::map<std::uint32_t, std::vector<std::string>> by_field;
			for (const auto& k : keys) {
				const std::uint32_t f = std::uint32_t(std::uint8_t(k[0])) << 24 | std::uint32_t(std::uint8_t(k[1])) << 16 | std::uint32_t(std::uint8_t(k[2])) << 8 | std::uint8_t(k[3]);
				by_field[f].push_back(k.substr(4));
			}
			fields = by_field.size();
			std::vector<dict> dicts(by_field.rbegin()->first + 1);
			for (const auto& [f, ks] : by_field) {
				const bool small = one_block && ks.size() <= 64;
				dicts[f] = build(ks, value_bytes, small);
				blocks += dicts[f].t.blocks.size();
				if (!small) { index += dicts[f].t.index.size(); }
				alpha = std::max(alpha, dicts[f].alphabet.size());
			}
			std::vector<std::optional<termdict::reader>> readers(dicts.size());
			for (std::size_t f = 0; f < dicts.size(); ++f) {
				if (!dicts[f].t.index.empty()) { readers[f].emplace(dicts[f].t.blocks, dicts[f].t.index); }
			}
			std::vector<std::string> probes;
			for (auto p : pick) { probes.push_back(keys[p]); }
			for (int round = 0; round < 3; ++round) {
				ns = time_finds(probes, [&](const std::string& k) {
					const std::uint32_t f = std::uint32_t(std::uint8_t(k[0])) << 24 | std::uint32_t(std::uint8_t(k[1])) << 16 | std::uint32_t(std::uint8_t(k[2])) << 8 | std::uint8_t(k[3]);
					return readers[f]->find(std::string_view(k).substr(4)).has_value();
				});
			}
		}
		std::printf("%-7s keys %9zu fields %4zu alphabet %3zu blocks %10zu index %8zu total %10zu  find %.0f ns\n", variant, keys.size(), fields, alpha, blocks, index, blocks + index, ns);
	}
}
