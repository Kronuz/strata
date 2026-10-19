// The query-gap design's microbenchmarks over a real segment (docs/design/query-gap.md):
//   qgbench skip SEG TERMS QUERIES   E6: locating a block by format 12's skip data against
//                                    F10's A1 to A4 at 128 and 256 postings per entry; probes
//                                    into FOR, bitset, and Elias-Fano blocks; M23's checks
//   qgbench codec SEG TERMS          H22: decode ns per posting, bitpack against SIMD-BP128
//   qgbench positions SEG TERMS      E7: positions under PFOR, FOR, Elias-Fano, split firsts
//   qgbench sizes SEG                bytes of A1 to A4, M23, and the positions candidates
//   qgbench m17 SEG OUT              E5: SEG rewritten with argmax-first frontiers into OUT
//   qgbench seeds SEG QUERIES OUT    E4b: dominance-pair seeds for QUERIES' terms and unions
// SEG holds segment.sx; TERMS one term per line; QUERIES the harness's JSON lines. One
// thread; every timing is the best of ROUNDS (default 7) passes over the same data.
#include "residency.hh"
#include "strata/search.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif
#ifdef QGBENCH_SIMDCOMP
extern "C" {
#include "simdcomp.h"
}
#endif

namespace {

using strata::ordinal_t;
using clk = std::chrono::steady_clock;

int rounds() {
	const char* r = std::getenv("ROUNDS");
	return r ? std::max(1, std::atoi(r)) : 7;
}

// The best of rounds() runs of f, in ns.
template <class F>
double best_ns(F&& f) {
	double best = 1e300;
	for (int r = 0; r < rounds(); ++r) {
		const auto t0 = clk::now();
		f();
		best = std::min(best, std::chrono::duration<double, std::nano>(clk::now() - t0).count());
	}
	return best;
}

volatile std::uint64_t sink;

struct segment_file {
	residency::file file;
	strata::segment seg;
	explicit segment_file(const std::string& dir) : file(residency::file::open(dir + "/segment.sx")), seg(view(file)) {}
	static std::string_view view(residency::file& f) {
		auto b = f.view({0, f.size()});
		return {reinterpret_cast<const char*>(b.data()), b.size()};
	}
	const strata::inverted_plugin::reader& inv() const { return seg.get<strata::inverted_plugin>(); }
};

std::vector<std::string> lines(const std::string& path) {
	std::vector<std::string> out;
	std::ifstream in(path);
	for (std::string l; std::getline(in, l);) {
		if (!l.empty()) { out.push_back(l); }
	}
	return out;
}

// A term's postings and skip data, as the format holds them.
struct term_data {
	std::string name;
	strata::format::term_info info;
	std::vector<ordinal_t> ords;
	std::vector<std::uint32_t> wdfs;
	std::vector<ordinal_t> block_last;           // per full block
	std::vector<std::uint32_t> block_bytes;      // ordinal and wdf bytes of each block
	std::vector<bool> block_bitset;
	std::vector<std::string> block_raw;          // each block's stored bytes
};

term_data load(const strata::inverted_plugin::reader& inv, const std::string& t) {
	term_data d;
	d.name = t;
	auto info = inv.term(t);
	if (!info) { return d; }
	d.info = *info;
	strata::postings_iterator it(inv, *info);
	for (ordinal_t o = it.next(); o != strata::no_more_docs; o = it.next()) {
		d.ords.push_back(o);
		d.wdfs.push_back(it.wdf());
	}
	strata::postings_iterator sk(inv, *info);
	for (std::size_t b = 0; b < sk.blocks(); ++b) {
		d.block_last.push_back(sk.block_last(b));
		const auto [bytes, bitset] = sk.block_data(b);
		d.block_bytes.push_back(std::uint32_t(bytes.size()));
		d.block_bitset.push_back(bitset);
		d.block_raw.emplace_back(bytes);
	}
	return d;
}

// --- E6: skip-entry layouts ------------------------------------------------------------------

// One term's advance entries in a layout, one entry per `per` blocks (1 or 2).
struct layout {
	int kind = 0;  // 1..4: A1..A4
	unsigned per = 1;
	std::vector<ordinal_t> group_last, group_base;  // level 1: each group's last and base
	std::vector<std::uint8_t> bytes;                 // the groups' entries, back to back
	std::vector<std::uint32_t> group_at;             // each group's entries offset in bytes
	std::vector<std::uint8_t> wide, wl, wo;          // A2/A4: u32 lasts; A3: widths
	std::size_t entries = 0;

	static constexpr std::size_t group_blocks = 32;

	void build(const term_data& t, int k, unsigned p) {
		kind = k;
		per = p;
		const std::size_t blocks = t.block_last.size();
		for (std::size_t g = 0; g * group_blocks < blocks; ++g) {
			const std::size_t b0 = g * group_blocks, b1 = std::min(blocks, b0 + group_blocks);
			const ordinal_t base = b0 ? t.block_last[b0 - 1] : 0;
			group_base.push_back(base);
			group_last.push_back(t.block_last[b1 - 1]);
			group_at.push_back(std::uint32_t(bytes.size()));
			std::vector<ordinal_t> lasts;
			std::vector<std::uint32_t> offs;
			std::uint32_t off = 0;
			for (std::size_t b = b0; b < b1; b += per) {
				lasts.push_back(t.block_last[std::min(b1, b + per) - 1] - base);
				offs.push_back(off);
				for (std::size_t j = b; j < std::min(b1, b + per); ++j) { off += t.block_bytes[j]; }
			}
			entries += lasts.size();
			const bool w = lasts.back() > 0xffff || kind == 1;
			wide.push_back(w);
			auto put = [&](std::uint64_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) { bytes.push_back(std::uint8_t(v >> (8 * i))); } };
			if (kind == 1 || kind == 2) {
				for (std::size_t i = 0; i < lasts.size(); ++i) {
					put(kind == 1 ? lasts[i] + base : lasts[i], w ? 4 : 2);
					put(offs[i], 2);
				}
			} else if (kind == 4) {
				for (auto v : lasts) { put(v, w ? 4 : 2); }
				for (auto v : offs) { put(v, 2); }
			} else {  // A3: lasts at width bl, offsets at width bo, one flag bit, bit-packed
				const unsigned bl = unsigned(std::bit_width(lasts.back())), bo = unsigned(std::bit_width(offs.back()));
				wl.push_back(std::uint8_t(bl));
				wo.push_back(std::uint8_t(bo));
				std::vector<std::uint8_t> bits((lasts.size() * (bl + bo + 1) + 7) / 8 + 8, 0);
				std::size_t at = 0;
				auto pack = [&](std::uint64_t v, unsigned n) {
					for (unsigned i = 0; i < n; ++i, ++at) { if (v >> i & 1) { bits[at / 8] |= std::uint8_t(1u << (at % 8)); } }
				};
				for (std::size_t i = 0; i < lasts.size(); ++i) { pack(lasts[i], bl); }
				for (std::size_t i = 0; i < lasts.size(); ++i) { pack(offs[i], bo); }
				at += lasts.size();  // the flags
				bytes.insert(bytes.end(), bits.begin(), bits.begin() + std::ptrdiff_t((at + 7) / 8));
				bytes.resize(bytes.size() + 8);  // slack for 64-bit loads
			}
		}
		bytes.resize(bytes.size() + 64);
	}

	// As the reader's group_of: the last group found and its successor first, then a search.
	mutable std::size_t hint = 0;
	std::size_t group_of(ordinal_t t) const {
		const std::size_t n = group_last.size();
		if (hint < n && t <= group_last[hint] && (hint == 0 || t > group_last[hint - 1])) { return hint; }
		if (hint + 1 < n && t > group_last[hint] && t <= group_last[hint + 1]) { return ++hint; }
		return hint = std::size_t(std::lower_bound(group_last.begin(), group_last.end(), t) - group_last.begin());
	}

	// The entry (within the term) whose blocks hold t, and its byte offset: one read per probe.
	std::pair<std::size_t, std::uint32_t> locate(ordinal_t t) const {
		const std::size_t g = group_of(t);
		if (g == group_last.size()) { return {~std::size_t(0), 0}; }
		const std::uint8_t* p = bytes.data() + group_at[g];
		const std::size_t n = std::min<std::size_t>(group_blocks / per, (g + 1 == group_last.size() ? (entries - g * (group_blocks / per)) : group_blocks / per));
		const ordinal_t rel = t - group_base[g];
		std::size_t i = 0;
		std::uint32_t off = 0;
		auto u16 = [](const std::uint8_t* q) { std::uint16_t v; std::memcpy(&v, q, 2); return std::uint32_t(v); };
		auto u32 = [](const std::uint8_t* q) { std::uint32_t v; std::memcpy(&v, q, 4); return v; };
		if (kind == 1 || kind == 2) {
			const std::size_t stride = wide[g] ? 6 : 4;
			const ordinal_t key = kind == 1 ? t : rel;
			std::size_t lo = 0, len = n;
			while (len > 1) {
				const std::size_t half = len / 2;
				const ordinal_t v = wide[g] ? u32(p + (lo + half - 1) * stride) : u16(p + (lo + half - 1) * stride);
				lo += (v < key) * half;
				len -= half;
			}
			const ordinal_t v = wide[g] ? u32(p + lo * stride) : u16(p + lo * stride);
			i = lo + (v < key);
			off = u16(p + i * stride + (wide[g] ? 4 : 2));
		} else if (kind == 4) {
#if defined(__AVX2__)
			if (!wide[g]) {
				const __m256i bias = _mm256_set1_epi16(std::int16_t(0x8000)), key = _mm256_xor_si256(_mm256_set1_epi16(std::int16_t(rel)), bias);
				std::uint32_t below = 0;
				for (std::size_t j = 0; j < n; j += 16) {
					const __m256i v = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 2 * j)), bias);
					std::uint32_t m = std::uint32_t(_mm256_movemask_epi8(_mm256_cmpgt_epi16(key, v)));
					if (n - j < 16) { m &= (1u << (2 * (n - j))) - 1; }
					below += std::uint32_t(std::popcount(m)) / 2;
				}
				i = below;
				off = u16(p + 2 * n + 2 * i);
			} else
#endif
			{
				const std::size_t w = wide[g] ? 4 : 2;
				while (i < n && (w == 4 ? u32(p + i * 4) : u16(p + i * 2)) < rel) { ++i; }
				off = u16(p + w * n + 2 * i);
			}
		} else {  // A3: lane extracts
			const unsigned bl = wl[g], bo = wo[g];
			auto field = [&](std::size_t bit, unsigned w) {
				std::uint64_t v;
				std::memcpy(&v, p + bit / 8, 8);
				return std::uint32_t((v >> (bit % 8)) & ((std::uint64_t(1) << w) - 1));
			};
			std::size_t lo = 0, len = n;
			while (len > 1) {
				const std::size_t half = len / 2;
				lo += (field((lo + half - 1) * bl, bl) < rel) * half;
				len -= half;
			}
			i = lo + (field(lo * bl, bl) < rel);
			off = bo ? field(n * bl + i * bo, bo) : 0;
		}
		return {g * (group_blocks / per) + i, off};
	}
};

// Format 12's lookups through the reader itself (decode_group and its search).
struct f12 {
	const strata::inverted_plugin::reader* inv;
	strata::format::term_info info;
	std::unique_ptr<strata::postings_iterator> it;
	void reset() { it = std::make_unique<strata::postings_iterator>(*inv, info); }
	std::size_t locate(ordinal_t t) { return it->block_index_for(t); }
};

// Elias-Fano over one block's 128 ordinals in (base, last]: the successor of a target.
struct ef_block {
	ordinal_t base = 0;
	unsigned l = 0;
	std::vector<std::uint64_t> hi;
	std::vector<std::uint8_t> lo;  // bit-packed low parts
	std::size_t bytes() const { return hi.size() * 8 + lo.size(); }
	void build(const ordinal_t* o, ordinal_t b, ordinal_t last) {
		base = b;
		const std::uint64_t u = std::uint64_t(last - b) + 1;
		l = u > 128 ? unsigned(std::bit_width(u / 128) - 1) : 0;
		hi.assign((128 + (u >> l) + 64) / 64 + 1, 0);
		lo.assign((128 * l + 7) / 8 + 8, 0);
		for (std::size_t i = 0; i < 128; ++i) {
			const std::uint64_t v = o[i] - b;
			const std::uint64_t h = (v >> l) + i;
			hi[h / 64] |= std::uint64_t(1) << (h % 64);
			for (unsigned k = 0; k < l; ++k) { if (v >> k & 1) { lo[(i * l + k) / 8] |= std::uint8_t(1u << ((i * l + k) % 8)); } }
		}
	}
	std::uint64_t low(std::size_t i) const {
		if (!l) { return 0; }
		std::uint64_t v;
		std::memcpy(&v, lo.data() + (i * l) / 8, 8);
		return (v >> ((i * l) % 8)) & ((std::uint64_t(1) << l) - 1);
	}
	// The first value >= t (t in (base, last]): select0 of the bucket, then a scan.
	ordinal_t successor(ordinal_t t) const {
		const std::uint64_t v = t - base, bucket = v >> l;
		std::size_t p = 0;  // the first position of the bucket's run in hi
		if (bucket) {
			std::uint64_t seen = 0;
			for (std::size_t w = 0; w < hi.size(); ++w) {
				const std::uint64_t z = std::uint64_t(std::popcount(~hi[w]));
				if (seen + z >= bucket) {
					std::uint64_t word = ~hi[w];
					for (std::uint64_t k = seen + 1; k < bucket; ++k) { word &= word - 1; }
					p = w * 64 + std::size_t(std::countr_zero(word)) + 1;
					break;
				}
				seen += z;
			}
		}
		std::size_t i = p - std::size_t(bucket);  // elements before p
		while (i < 128) {
			while (!(hi[p / 64] >> (p % 64) & 1)) { ++p; }
			const std::uint64_t x = (std::uint64_t(p - i) << l) | low(i);
			if (x >= v) { return base + ordinal_t(x); }
			++p;
			++i;
		}
		return strata::no_more_docs;
	}
};

int skip_bench(const std::string& seg_dir, const std::string& terms_path, const std::string& queries_path) {
	segment_file sf(seg_dir);
	const auto& inv = sf.inv();
	std::map<std::string, term_data> terms;
	for (const auto& t : lines(terms_path)) {
		auto d = load(inv, t);
		if (d.block_last.size() >= 2) { terms.emplace(t, std::move(d)); }
	}
	// Traces per term: leapfrog targets (each intersection's lead postings sent to the other
	// terms), sequential block starts, and window bounds (4,096-ordinal windows).
	std::map<std::string, std::vector<ordinal_t>> leap;
	for (const auto& line : lines(queries_path)) {
		if (line.find("\"intersection\"") == std::string::npos) { continue; }
		const auto a = line.find("\"query\": \"") + 10;
		const std::string q = line.substr(a, line.find('"', a) - a);
		std::vector<std::string> ts;
		for (std::size_t i = 0; i < q.size();) {
			std::size_t j = q.find(' ', i);
			if (j == std::string::npos) { j = q.size(); }
			ts.push_back(q.substr(i + 1, j - i - 1));
			i = j + 1;
		}
		std::vector<term_data> ds;
		for (const auto& t : ts) { ds.push_back(load(inv, t)); }
		std::size_t lead = 0;
		for (std::size_t i = 1; i < ds.size(); ++i) { if (ds[i].ords.size() < ds[lead].ords.size()) { lead = i; } }
		for (std::size_t i = 0; i < ds.size(); ++i) {
			if (i == lead || !terms.count(ds[i].name)) { continue; }
			auto& v = leap[ds[i].name];
			v.insert(v.end(), ds[lead].ords.begin(), ds[lead].ords.end());
		}
	}
	std::printf("# E6 skip-entry layouts over %zu terms; ns per lookup (best of %d)\n", terms.size(), rounds());
	std::printf("trace\tlookups\tF12");
	const char* names[] = {"", "A1", "A2", "A3", "A4"};
	for (unsigned per : {1u, 2u}) { for (int k = 1; k <= 4; ++k) { std::printf("\t%s@%u", names[k], 128 * per); } }
	std::printf("\n");
	for (const char* trace : {"leapfrog", "sequential", "window"}) {
		std::size_t lookups = 0;
		double t12 = 0, tl[2][5] = {};
		for (auto& [name, d] : terms) {
			std::vector<ordinal_t> targets;
			if (std::string(trace) == "leapfrog") {
				auto it = leap.find(name);
				if (it == leap.end()) { continue; }
				targets = it->second;
				std::sort(targets.begin(), targets.end());
			} else if (std::string(trace) == "sequential") {
				for (std::size_t b = 0; b < d.block_last.size(); ++b) { targets.push_back(b ? d.block_last[b - 1] + 1 : 0); }
			} else {
				for (ordinal_t w = 0; w < d.block_last.back(); w += 4096) { targets.push_back(w); }
			}
			targets.erase(std::remove_if(targets.begin(), targets.end(), [&](ordinal_t t) { return t > d.block_last.back(); }), targets.end());
			if (targets.empty()) { continue; }
			lookups += targets.size();
			f12 old{&inv, d.info, nullptr};
			t12 += best_ns([&] {
				old.reset();
				std::uint64_t s = 0;
				for (auto t : targets) { s += old.locate(t); }
				sink = s;
			});
			for (unsigned per : {1u, 2u}) {
				for (int k = 1; k <= 4; ++k) {
					layout lay;
					lay.build(d, k, per);
					// Every layout agrees with the format on which block holds each target.
					for (auto t : targets) {
						const std::size_t want = std::size_t(std::lower_bound(d.block_last.begin(), d.block_last.end(), t) - d.block_last.begin()) / per;
						if (lay.locate(t).first != want) {
							std::fprintf(stderr, "qgbench: A%d@%u disagrees on %s at %u\n", k, 128 * per, name.c_str(), t);
							return 1;
						}
					}
					tl[per - 1][k] += best_ns([&] {
						std::uint64_t s = 0;
						for (auto t : targets) { s += lay.locate(t).first; }
						sink = s;
					});
				}
			}
		}
		std::printf("%s\t%zu\t%.2f", trace, lookups, t12 / double(lookups));
		for (unsigned per : {0u, 1u}) { for (int k = 1; k <= 4; ++k) { std::printf("\t%.2f", tl[per][k] / double(lookups)); } }
		std::printf("\n");
	}

	// Probes: one target per block into its 128 ordinals (a rare lead's candidates): FOR
	// decode and search, a bitset block's rank, and Elias-Fano's successor.
	std::printf("# probes, ns per probe (one target in each block)\n");
	std::printf("blocks\tFOR ns\tbitset ns (bitset blocks)\tEF ns\tstored ordinal bytes (FOR or bitset)\tEF bytes\n");
	std::size_t nblocks = 0, nbits = 0;
	double t_for = 0, t_bits = 0, t_ef = 0, b_for = 0, b_ef = 0, ef_saved = 0;
	std::size_t ef_smaller = 0;
	std::mt19937_64 rng(3);
	for (auto& [name, d] : terms) {
		std::vector<ef_block> efs(d.block_last.size());
		std::vector<ordinal_t> targets(d.block_last.size());
		for (std::size_t b = 0; b < d.block_last.size(); ++b) {
			const ordinal_t base = b ? d.block_last[b - 1] : 0;
			efs[b].build(d.ords.data() + 128 * b, base, d.block_last[b]);
			targets[b] = d.ords[128 * b + rng() % 128];
			b_ef += double(efs[b].bytes() - 8);  // the low bits' load slack is not stored
			const double stored = d.block_bitset[b] ? double((d.block_last[b] - (b ? d.block_last[b - 1] + 1 : 0) + 1 + 7) / 8)
			                                        : double(bitpack::encoded_size(reinterpret_cast<const std::uint8_t*>(d.block_raw[b].data()), d.block_raw[b].size()));
			if (double(efs[b].bytes() - 8) < stored) {
				++ef_smaller;
				ef_saved += stored - double(efs[b].bytes() - 8);
			}
			b_for += d.block_bitset[b] ? double((d.block_last[b] - (b ? d.block_last[b - 1] + 1 : 0) + 1 + 7) / 8)
			                           : double(bitpack::encoded_size(reinterpret_cast<const std::uint8_t*>(d.block_raw[b].data()), d.block_raw[b].size()));
		}
		nblocks += d.block_last.size();
		t_for += best_ns([&] {
			std::uint32_t buf[128];
			std::uint64_t s = 0;
			for (std::size_t b = 0; b < d.block_last.size(); ++b) {
				if (d.block_bitset[b]) {
					s += 1;
					continue;
				}
				const ordinal_t base = b ? d.block_last[b - 1] : std::numeric_limits<ordinal_t>::max();
				bitpack::decode_delta(reinterpret_cast<const std::uint8_t*>(d.block_raw[b].data()), d.block_raw[b].size(), base, buf);
				s += std::size_t(std::lower_bound(buf, buf + 128, targets[b]) - buf);
			}
			sink = s;
		});
		t_ef += best_ns([&] {
			std::uint64_t s = 0;
			for (std::size_t b = 0; b < d.block_last.size(); ++b) { s += efs[b].successor(targets[b]); }
			sink = s;
		});
		// Bitset rank as the reader does it (popcounts up to the target's word).
		t_bits += best_ns([&] {
			std::uint64_t s = 0;
			for (std::size_t b = 0; b < d.block_last.size(); ++b) {
				if (!d.block_bitset[b]) { continue; }
				const ordinal_t first = b ? d.block_last[b - 1] + 1 : 0;
				const auto* w = d.block_raw[b].data();
				const std::uint64_t at = targets[b] - first;
				std::size_t rank = 0;
				for (std::size_t i = 0; i < at / 64; ++i) {
					std::uint64_t word;
					std::memcpy(&word, w + 8 * i, 8);
					rank += std::size_t(std::popcount(word));
				}
				s += rank;
			}
			sink = s;
		});
		for (std::size_t b = 0; b < d.block_last.size(); ++b) { nbits += d.block_bitset[b]; }
	}
	const std::size_t nfor = nblocks - nbits;
	std::printf("%zu\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", nblocks, t_for / double(nfor), nbits ? t_bits / double(nbits) : 0.0, t_ef / double(nblocks),
		b_for / double(nblocks), b_ef / double(nblocks));
	std::printf("blocks where Elias-Fano is smaller than the stored codec\t%zu (%.1f%%), saving %.0f bytes\n", ef_smaller, 100.0 * double(ef_smaller) / double(nblocks), ef_saved);

	// M23: today's decode and check_increasing against gap - 1 decoding with a 64-bit sum
	// check, and with the header-width (<= 25 bits) form, on the FOR blocks.
	std::printf("# M23, ns per FOR block: decode_delta + check_increasing | gap-1 decode + 64-bit sum | gap-1 decode + width check\n");
	double c0 = 0, c1 = 0, c2 = 0;
	std::size_t nf = 0;
	std::vector<std::vector<std::uint8_t>> m23;  // the same blocks at gap - 1
	std::vector<ordinal_t> m23_base, m23_last;
	for (auto& [name, d] : terms) {
		for (std::size_t b = 0; b < d.block_last.size(); ++b) {
			if (d.block_bitset[b]) { continue; }
			const ordinal_t base = b ? d.block_last[b - 1] : ordinal_t(-1);
			std::uint32_t g[128];
			for (std::size_t i = 0; i < 128; ++i) { g[i] = d.ords[128 * b + i] - (i ? d.ords[128 * b + i - 1] : base) - 1; }
			std::vector<std::uint8_t> out(bitpack::max_encoded_bytes);
			out.resize(bitpack::encode_for(g, out.data()));
			m23.push_back(std::move(out));
			m23_base.push_back(base);
			m23_last.push_back(d.block_last[b]);
		}
	}
	for (auto& [name, d] : terms) {
		c0 += best_ns([&] {
			std::uint32_t buf[128];
			std::uint64_t s = 0;
			for (std::size_t b = 0; b < d.block_last.size(); ++b) {
				if (d.block_bitset[b]) { continue; }
				const ordinal_t base = b ? d.block_last[b - 1] : std::numeric_limits<ordinal_t>::max();
				bitpack::decode_delta(reinterpret_cast<const std::uint8_t*>(d.block_raw[b].data()), d.block_raw[b].size(), base, buf);
				std::uint32_t bad = base != std::numeric_limits<ordinal_t>::max() && buf[0] <= base;
				for (std::size_t i = 1; i < 128; ++i) { bad |= buf[i] <= buf[i - 1]; }
				s += bad + buf[127];
			}
			sink = s;
		});
		for (std::size_t b = 0; b < d.block_last.size(); ++b) { nf += !d.block_bitset[b]; }
	}
	c1 = best_ns([&] {
		std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < m23.size(); ++i) {
			const std::uint8_t* p = m23[i].data();
			const unsigned bits = p[0] & 0x3f;
			if (p[0] & bitpack::constant_flag) {
				std::uint32_t v;
				std::memcpy(&v, p + 1, 4);
				s += std::uint64_t(v) * 128 + 128 == std::uint64_t(m23_last[i] - m23_base[i]);
				continue;
			}
			bitpack::unpack(p + 1, bits, buf);
			std::uint64_t sum = 0;
			for (std::size_t j = 0; j < 128; ++j) { sum += buf[j]; }
			s += sum + 128 == std::uint64_t(ordinal_t(m23_last[i] - m23_base[i]));
			std::uint32_t at = m23_base[i];
			for (std::size_t j = 0; j < 128; ++j) { buf[j] = at += buf[j] + 1; }
			s += buf[127];
		}
		sink = s;
	});
	c2 = best_ns([&] {
		std::uint32_t buf[128];
		std::uint64_t s = 0;
		static const std::array<std::uint32_t, 128> iota = [] { std::array<std::uint32_t, 128> a{}; for (std::uint32_t j = 0; j < 128; ++j) { a[j] = j + 1; } return a; }();
		for (std::size_t i = 0; i < m23.size(); ++i) {
			const std::uint8_t* p = m23[i].data();
			if (p[0] & bitpack::constant_flag) { s += 1; continue; }
			const unsigned bits = p[0] & 0x3f;
			bitpack::unpack_delta(p + 1, bits, m23_base[i], buf);
			for (std::size_t j = 0; j < 128; ++j) { buf[j] += iota[j]; }
			s += bits <= 25 && buf[127] == m23_last[i];
		}
		sink = s;
	});
	std::printf("%zu\t%.1f\t%.1f\t%.1f\n", nf, c0 / double(nf), c1 / double(nf), c2 / double(nf));
	return 0;
}

// --- H22: decode per posting ------------------------------------------------------------------

int codec_bench(const std::string& seg_dir, const std::string& terms_path) {
	segment_file sf(seg_dir);
	const auto& inv = sf.inv();
	std::vector<term_data> terms;
	for (const auto& t : lines(terms_path)) {
		auto d = load(inv, t);
		if (d.block_last.size() >= 1) { terms.push_back(std::move(d)); }
	}
	std::size_t postings = 0, for_blocks = 0, bitset_blocks = 0, pfor_wdf = 0;
	for (const auto& d : terms) {
		postings += d.block_last.size() * 128;
		for (std::size_t b = 0; b < d.block_last.size(); ++b) {
			(d.block_bitset[b] ? bitset_blocks : for_blocks)++;
			const auto* p = reinterpret_cast<const std::uint8_t*>(d.block_raw[b].data());
			const std::size_t ob = d.block_bitset[b] ? 0 : bitpack::encoded_size(p, d.block_raw[b].size());
			if (!d.block_bitset[b] && ob < d.block_raw[b].size() && (p[ob] & bitpack::exceptions_flag)) { ++pfor_wdf; }
		}
	}
	std::printf("# H22 over %zu terms, %zu full blocks (%zu FOR, %zu bitset; %zu FOR blocks' wdfs with exceptions); ns per posting\n", terms.size(),
		for_blocks + bitset_blocks, for_blocks, bitset_blocks, pfor_wdf);
	// strata as stored: every block through the reader's own path (decode, checks, wdfs).
	double t_reader = 0;
	for (const auto& d : terms) {
		t_reader += best_ns([&] {
			strata::postings_iterator it(inv, d.info);
			std::uint64_t s = 0;
			const ordinal_t end = d.block_last.back() + 1;
			it.next();
			it.for_each_until(end, [&](ordinal_t o, std::uint32_t w) { s += o + w; });
			sink = s;
		});
	}
	// Kernels on the FOR blocks, re-encoded from the decoded postings: bitpack's integrated
	// delta decode and PFOR wdfs (strata), and SIMD-BP128 as PISA's simdbp uses it (unpack
	// gaps - 1 and freqs - 1, then a scalar prefix sum), and simdcomp's integrated d1 form.
	// Each codec's blocks back to back in one buffer, so every kernel streams only its bytes.
	struct stream {
		std::vector<std::uint8_t> bytes;
		std::vector<std::uint32_t> at;
		std::vector<std::uint8_t> bits;
		void add(const std::uint8_t* p, std::size_t n, unsigned b = 0) {
			at.push_back(std::uint32_t(bytes.size()));
			bits.push_back(std::uint8_t(b));
			bytes.insert(bytes.end(), p, p + n);
			while (bytes.size() % 16) { bytes.push_back(0); }  // SIMD-BP128 reads aligned 16-byte words
		}
	};
	stream s_ords, s_wdfs, p_docs, p_freqs, p_d1;
	std::vector<ordinal_t> bases;
	for (const auto& d : terms) {
		for (std::size_t b = 0; b < d.block_last.size(); ++b) {
			const ordinal_t base = b ? d.block_last[b - 1] : 0;
			std::uint32_t gaps[128], wd[128];
			alignas(16) std::uint32_t gm1[128], fm1[128], abs[128];
			for (std::size_t i = 0; i < 128; ++i) {
				const ordinal_t prev = i ? d.ords[128 * b + i - 1] : base;
				gaps[i] = d.ords[128 * b + i] - prev;
				gm1[i] = gaps[i] - (b || i ? 1 : 0);
				wd[i] = d.wdfs[128 * b + i];
				fm1[i] = wd[i] - 1;
				abs[i] = d.ords[128 * b + i];
			}
			bases.push_back(base);
			std::uint8_t out[bitpack::max_encoded_bytes];
			s_ords.add(out, bitpack::encode_for(gaps, out));
			s_wdfs.add(out, bitpack::encode_pfor(wd, out));
#ifdef QGBENCH_SIMDCOMP
			alignas(16) std::uint8_t pk[16 * 32];
			unsigned bd = maxbits(gm1);
			simdpackwithoutmask(gm1, reinterpret_cast<__m128i*>(pk), bd);
			p_docs.add(pk, 16 * bd, bd);
			unsigned bf = maxbits(fm1);
			simdpackwithoutmask(fm1, reinterpret_cast<__m128i*>(pk), bf);
			p_freqs.add(pk, 16 * bf, bf);
			unsigned b1 = simdmaxbitsd1(base, abs);
			simdpackwithoutmaskd1(base, abs, reinterpret_cast<__m128i*>(pk), b1);
			p_d1.add(pk, 16 * b1, b1);
#endif
		}
	}
	const std::size_t nb = bases.size();
	const double n = double(nb * 128);
	const double t_s_ords = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			bitpack::decode_delta(s_ords.bytes.data() + s_ords.at[i], s_ords.bytes.size() - s_ords.at[i], bases[i], buf);
			s += buf[127];
		}
		sink = s;
	});
	const double t_s_check = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			bitpack::decode_delta(s_ords.bytes.data() + s_ords.at[i], s_ords.bytes.size() - s_ords.at[i], bases[i], buf);
			std::uint32_t bad = 0;
			for (std::size_t j = 1; j < 128; ++j) { bad |= buf[j] <= buf[j - 1]; }
			s += buf[127] + bad;
		}
		sink = s;
	});
	const double t_s_wdfs = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			bitpack::decode(s_wdfs.bytes.data() + s_wdfs.at[i], s_wdfs.bytes.size() - s_wdfs.at[i], buf);
			s += buf[127];
		}
		sink = s;
	});
	// The same wdfs as FOR (no exceptions), through bitpack.
	stream s_wfor;
	for (std::size_t i = 0; i < nb; ++i) {
		alignas(16) std::uint32_t buf[128];
		bitpack::decode(s_wdfs.bytes.data() + s_wdfs.at[i], s_wdfs.bytes.size() - s_wdfs.at[i], buf);
		std::uint8_t out[bitpack::max_encoded_bytes];
		s_wfor.add(out, bitpack::encode_for(buf, out));
	}
	const double t_s_wfor = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			bitpack::decode(s_wfor.bytes.data() + s_wfor.at[i], s_wfor.bytes.size() - s_wfor.at[i], buf);
			s += buf[127];
		}
		sink = s;
	});
	std::printf("reader (as stored, all blocks, wdfs read)\t%.3f\n", t_reader / double(postings));
	std::printf("bitpack FOR ordinals, integrated delta\t%.3f\n", t_s_ords / n);
	std::printf("bitpack FOR ordinals + check_increasing\t%.3f\n", t_s_check / n);
	std::printf("bitpack PFOR wdfs (as the writer encodes them)\t%.3f\n", t_s_wdfs / n);
	std::printf("bitpack FOR wdfs\t%.3f\n", t_s_wfor / n);
	std::printf("bytes per posting: ordinals FOR %.3f, wdfs PFOR %.3f, wdfs FOR %.3f\n", double(s_ords.bytes.size()) / n, double(s_wdfs.bytes.size()) / n,
		double(s_wfor.bytes.size()) / n);
#ifdef QGBENCH_SIMDCOMP
	const double t_p_docs = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			simdunpack(reinterpret_cast<const __m128i*>(p_docs.bytes.data() + p_docs.at[i]), buf, p_docs.bits[i]);
			buf[0] += bases[i];
			for (std::size_t j = 1; j < 128; ++j) { buf[j] += buf[j - 1] + 1; }
			s += buf[127];
		}
		sink = s;
	});
	const double t_p_d1 = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			simdunpackd1(bases[i], reinterpret_cast<const __m128i*>(p_d1.bytes.data() + p_d1.at[i]), buf, p_d1.bits[i]);
			s += buf[127];
		}
		sink = s;
	});
	const double t_p_freqs = best_ns([&] {
		alignas(16) std::uint32_t buf[128];
		std::uint64_t s = 0;
		for (std::size_t i = 0; i < nb; ++i) {
			simdunpack(reinterpret_cast<const __m128i*>(p_freqs.bytes.data() + p_freqs.at[i]), buf, p_freqs.bits[i]);
			s += buf[127] + 1;
		}
		sink = s;
	});
	std::printf("SIMD-BP128 docs as PISA's simdbp (unpack + scalar prefix)\t%.3f\n", t_p_docs / n);
	std::printf("SIMD-BP128 d1 (simdcomp integrated)\t%.3f\n", t_p_d1 / n);
	std::printf("SIMD-BP128 freqs (unpack)\t%.3f\n", t_p_freqs / n);
	double pb = 0;
	for (std::size_t i = 0; i < nb; ++i) { pb += 2 + 16.0 * (p_docs.bits[i] + p_freqs.bits[i]); }
	std::printf("bytes per posting: SIMD-BP128 docs+freqs %.3f (one width byte each)\n", pb / n);
#endif
	return 0;
}

// --- E7: positions -------------------------------------------------------------------------

// A term's position stream as stored: per document its count of positions, the first
// absolute, the rest as gaps.
struct pos_stream {
	std::vector<std::uint32_t> values, firsts_at;  // firsts_at: index of each document's first
};

pos_stream positions_of(const strata::inverted_plugin::reader& inv, const strata::format::term_info& info) {
	pos_stream s;
	strata::postings_iterator it(inv, info);
	strata::positions_reader pr(inv, info);
	for (ordinal_t o = it.next(); o != strata::no_more_docs; o = it.next()) {
		const auto ps = pr.read(it);
		s.firsts_at.push_back(std::uint32_t(s.values.size()));
		std::uint32_t prev = 0;
		for (auto p : ps) {
			s.values.push_back(p - prev);
			prev = p;
		}
	}
	return s;
}

struct pos_tally {
	std::uint64_t positions = 0, blocks = 0, exc_blocks = 0, exceptions = 0;
	std::uint64_t b_pfor = 0, b_for = 0, b_ef = 0, b_split = 0;
	std::array<std::uint64_t, 33> width{};
	void add(const pos_stream& s) {
		const std::size_t full = s.values.size() / 128;
		positions += s.values.size();
		std::vector<bool> first(s.values.size(), false);
		for (auto f : s.firsts_at) { if (f < first.size()) { first[f] = true; } }
		std::uint8_t out[bitpack::max_encoded_bytes];
		std::vector<std::uint32_t> rest, firsts;
		for (std::size_t b = 0; b < full; ++b) {
			const std::uint32_t* v = s.values.data() + 128 * b;
			++blocks;
			const std::size_t np = bitpack::encode_pfor(v, out);
			b_pfor += np;
			if (out[0] & bitpack::exceptions_flag) {
				++exc_blocks;
				exceptions += out[1];
			}
			++width[out[0] & bitpack::constant_flag ? 0 : out[0] & 0x3f];
			b_for += bitpack::encode_for(v, out);
			// Elias-Fano over the block's prefix sums (positions restart per document, so the
			// sums are only monotone within the block's own stream).
			std::uint64_t u = 0;
			for (std::size_t i = 0; i < 128; ++i) { u += v[i]; }
			const unsigned l = u > 128 ? unsigned(std::bit_width(u / 128) - 1) : 0;
			b_ef += (128 * l + 7) / 8 + (128 + (u >> l) + 7) / 8;
			for (std::size_t i = 0; i < 128; ++i) { (first[128 * b + i] ? firsts : rest).push_back(v[i]); }
		}
		// Split firsts: documents' first positions in a stream of their own, gaps in another.
		auto blocks_of = [&](std::vector<std::uint32_t>& xs) {
			std::uint64_t bytes = 0;
			for (std::size_t i = 0; i + 128 <= xs.size(); i += 128) { bytes += bitpack::encode_pfor(xs.data() + i, out); }
			bytes += (xs.size() % 128) * 2;  // the remainder, about two LEB128 bytes each
			return bytes;
		};
		b_split += blocks_of(rest) + blocks_of(firsts);
	}
};

int positions_bench(const std::string& seg_dir, const std::string& terms_path) {
	segment_file sf(seg_dir);
	const auto& inv = sf.inv();
	pos_tally all;
	std::vector<std::vector<std::uint8_t>> pf, fo;
	std::vector<std::uint32_t> raw;
	for (const auto& t : lines(terms_path)) {
		auto info = inv.term(t);
		if (!info || (info->flags & strata::format::no_positions)) { continue; }
		const auto s = positions_of(inv, *info);
		all.add(s);
		for (std::size_t b = 0; b + 128 <= s.values.size(); b += 128) {
			std::vector<std::uint8_t> o(bitpack::max_encoded_bytes);
			o.resize(bitpack::encode_pfor(s.values.data() + b, o.data()));
			pf.push_back(std::move(o));
			std::vector<std::uint8_t> f(bitpack::max_encoded_bytes);
			f.resize(bitpack::encode_for(s.values.data() + b, f.data()));
			fo.push_back(std::move(f));
		}
	}
	const double n = double(pf.size() * 128);
	auto time_decode = [&](const std::vector<std::vector<std::uint8_t>>& bs) {
		return best_ns([&] {
			std::uint32_t buf[128];
			std::uint64_t s = 0;
			for (const auto& b : bs) {
				bitpack::decode(b.data(), b.size(), buf);
				std::uint32_t at = 0;
				for (std::size_t i = 0; i < 128; ++i) { s += at += buf[i]; }
			}
			sink = s;
		});
	};
	const double t_pfor = time_decode(pf), t_for = time_decode(fo);
	// Stepping over block headers, as decode_block does to reach block j.
	const double t_step = best_ns([&] {
		std::uint64_t s = 0;
		for (const auto& b : pf) { s += bitpack::encoded_size(b.data(), b.size()); }
		sink = s;
	});
	std::printf("# E7 over the terms' positions: %llu positions, %llu full blocks\n", (unsigned long long)all.positions, (unsigned long long)all.blocks);
	std::printf("blocks with exceptions\t%.3f\texceptions per such block\t%.2f\n", double(all.exc_blocks) / double(all.blocks),
		all.exc_blocks ? double(all.exceptions) / double(all.exc_blocks) : 0.0);
	std::printf("bytes per position: PFOR %.3f, FOR %.3f, EF %.3f, split firsts %.3f\n", double(all.b_pfor) / double(all.blocks * 128),
		double(all.b_for) / double(all.blocks * 128), double(all.b_ef) / double(all.blocks * 128), double(all.b_split) / double(all.blocks * 128));
	std::printf("decode ns per position (with the prefix sum): PFOR %.3f, FOR %.3f; header step ns per block %.2f\n", t_pfor / n, t_for / n, t_step / double(pf.size()));
	std::printf("widths:");
	for (unsigned w = 0; w <= 32; ++w) { if (all.width[w]) { std::printf(" %u:%.3f", w, double(all.width[w]) / double(all.blocks)); } }
	std::printf("\n");
	return 0;
}

// --- sizes over the whole segment ---------------------------------------------------------------

int sizes(const std::string& seg_dir) {
	segment_file sf(seg_dir);
	const auto& inv = sf.inv();
	std::uint64_t terms = 0, blocks = 0, groups = 0, a[3][5] = {}, m23_for = 0, gap_for = 0, flags_bits = 0, f12_fields = 0;
	pos_tally pos;
	const bool with_positions = std::getenv("POSITIONS") != nullptr;
	inv.scan_terms([&](std::string_view key, const strata::format::term_info& info) {
		if (with_positions && !(info.flags & strata::format::no_positions) && info.df) { pos.add(positions_of(inv, info)); }
		if (info.df < 128) { return true; }
		++terms;
		const auto d = load(inv, std::string(key));
		blocks += d.block_last.size();
		groups += (d.block_last.size() + 31) / 32;
		auto leb = [](std::uint64_t v) { std::uint64_t n = 1; while (v >= 128) { v >>= 7; ++n; } return n; };
		for (std::size_t b = 0; b < d.block_last.size(); ++b) {  // format 12's level-0 last and length fields
			f12_fields += leb(d.block_last[b] - (b ? d.block_last[b - 1] : 0)) + leb(std::uint64_t(d.block_bytes[b]) << 1 | std::uint64_t(d.block_bitset[b]));
		}
		flags_bits += d.block_last.size();
		for (unsigned per : {1u, 2u}) {
			for (int k = 1; k <= 4; ++k) {
				layout lay;
				lay.build(d, k, per);
				// A3's slack for 64-bit loads is not stored.
				a[per][k] += lay.bytes.size() - 64 - (k == 3 ? 8 * lay.group_last.size() : 0);
			}
		}
		std::uint8_t out[bitpack::max_encoded_bytes];
		for (std::size_t b = 0; b < d.block_last.size(); ++b) {
			if (d.block_bitset[b]) { continue; }
			const ordinal_t base = b ? d.block_last[b - 1] : ordinal_t(-1);
			std::uint32_t g[128], g1[128];
			for (std::size_t i = 0; i < 128; ++i) {
				const ordinal_t prev = i ? d.ords[128 * b + i - 1] : base;
				g[i] = d.ords[128 * b + i] - (b || i ? prev : 0);
				g1[i] = d.ords[128 * b + i] - prev - 1;
			}
			gap_for += bitpack::encode_for(g, out);
			m23_for += bitpack::encode_for(g1, out);
		}
		return true;
	});
	std::printf("# sizes: %llu terms with skip blocks, %llu blocks, %llu groups\n", (unsigned long long)terms, (unsigned long long)blocks, (unsigned long long)groups);
	const char* names[] = {"", "A1", "A2", "A3", "A4"};
	for (unsigned per : {1u, 2u}) {
		for (int k = 1; k <= 4; ++k) { std::printf("%s@%u\t%.3f MB (entries only; flags below)\n", names[k], 128 * per, double(a[per][k]) / 1e6); }
	}
	std::printf("flags at a bit per block\t%.3f MB\n", double(flags_bits) / 8e6);
	std::printf("format 12's level-0 last and length fields (LEB128)\t%.3f MB\n", double(f12_fields) / 1e6);
	std::printf("M23: FOR ordinal blocks at gap - 1 %.3f MB against gap %.3f MB (%+.3f MB)\n", double(m23_for) / 1e6, double(gap_for) / 1e6,
		(double(m23_for) - double(gap_for)) / 1e6);
	if (with_positions) {
		std::printf("positions: %llu, PFOR %.3f MB, FOR %.3f MB, EF %.3f MB, split firsts %.3f MB; blocks with exceptions %.3f\n", (unsigned long long)pos.positions,
			double(pos.b_pfor) / 1e6, double(pos.b_for) / 1e6, double(pos.b_ef) / 1e6, double(pos.b_split) / 1e6, double(pos.exc_blocks) / double(std::max<std::uint64_t>(1, pos.blocks)));
	}
	return 0;
}

// --- E5: argmax-first frontiers in format-12 bytes ----------------------------------------------

// Rewrites a segment with every block and group frontier argmax-first under bm25 at the
// segment's own clamped average, the rest in competitive()'s order (ties: the higher wdf,
// which competitive() lists first). Pair encodings do not depend on their order, so every
// offset holds; the postings section's checksums, the table of contents, and the footer are
// rewritten. Term-level pairs (in the dictionary) keep their order.
int m17(const std::string& seg_dir, const std::string& out_path) {
	segment_file sf(seg_dir);
	const auto& inv = sf.inv();
	const std::string_view file = segment_file::view(sf.file);
	std::string buf(file);
	const auto w = strata::weight::bm25(inv, 1);
	auto order = [&](std::vector<strata::format::impact>& ps) {
		std::size_t best = 0;
		double top = -1;
		for (std::size_t i = 0; i < ps.size(); ++i) {
			const double v = w(ps[i].wdf, strata::format::decode_length(ps[i].length));
			if (v > top) { top = v; best = i; }
		}
		std::rotate(ps.begin(), ps.begin() + std::ptrdiff_t(best), ps.begin() + std::ptrdiff_t(best) + 1);
		return best != 0;
	};
	auto put = [&](std::string_view at, const std::string& bytes) {
		if (bytes.size() != at.size()) { std::fprintf(stderr, "qgbench m17: a frontier changed size\n"); std::exit(1); }
		std::memcpy(buf.data() + (at.data() - file.data()), bytes.data(), bytes.size());
	};
	std::uint64_t lists = 0, moved = 0;
	inv.scan_terms([&](std::string_view, const strata::format::term_info& info) {
		if (info.df < strata::format::block) { return true; }
		strata::postings_iterator it(inv, info);
		for (std::size_t g = 0; g < it.groups(); ++g) {
			const std::string_view at = it.group_frontier(g);
			const char* p = at.data();
			std::vector<strata::format::impact> ps;
			strata::format::decode_impacts(&p, at.data() + at.size(), ps);
			moved += order(ps);
			++lists;
			std::string bytes;
			strata::format::encode_impacts(bytes, ps);
			put(at, bytes);
		}
		for (std::size_t b = 0; b < it.blocks(); ++b) {
			const std::string_view at = it.block_frontier(b);
			std::vector<strata::format::impact> ps;
			strata::format::any_sized_impact(at, [&](const strata::format::impact& i) { ps.push_back(i); return false; });
			moved += order(ps);
			++lists;
			std::string bytes;
			strata::format::encode_sized_impacts(bytes, ps);
			put(at, bytes);
		}
		return true;
	});
	const auto f = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, f);
	for (auto& e : toc) {
		if (e.kind == strata::format::section::postings) {
			e = strata::format::describe(e.kind, e.offset, std::string_view(buf).substr(e.offset, e.length), e.region_shift);
		}
	}
	const std::string t = strata::format::encode_toc(toc);
	if (t.size() != f.toc_length) { std::fprintf(stderr, "qgbench m17: table of contents changed size\n"); return 1; }
	std::memcpy(buf.data() + f.toc_offset, t.data(), t.size());
	auto nf = f;
	nf.toc_crc = hashes::crc32c(t.data(), t.size());
	const std::string foot = strata::format::encode_footer(nf);
	std::memcpy(buf.data() + buf.size() - foot.size(), foot.data(), foot.size());
	std::ofstream out(out_path, std::ios::binary);
	out.write(buf.data(), std::streamsize(buf.size()));
	std::printf("%llu frontiers, %llu reordered (argmax not first before), average %.3f\n", (unsigned long long)lists, (unsigned long long)moved, inv.average_length());
	return out ? 0 : 1;
}

// --- E4b: dominance-pair seeds -----------------------------------------------------------------

// For each term and k, the largest f(w, l) over every dominance pair (at least k postings with
// wdf >= w and exact length <= l), under each preset at the segment's statistics: what F13's
// seeds can reach at best (F13 stores at most three pairs per k). Written for each term and
// union query of QUERIES (seeds do not apply to conjunctions or phrases) as `dump` lines whose
// k-th hit carries the seed, for STRATA_SEED_FILE.
int seeds(const std::string& seg_dir, const std::string& queries, const std::string& out_path) {
	segment_file sf(seg_dir);
	const auto& inv = sf.inv();
	std::map<std::tuple<std::string, std::size_t, int>, double> best;  // (term, k, scheme)
	auto term_seeds = [&](const std::string& t) {
		if (best.count({t, 10, 0})) { return; }
		auto info = inv.term(t);
		std::vector<std::pair<std::uint32_t, std::uint32_t>> ps;  // (wdf, length)
		if (info) {
			strata::postings_iterator it(inv, *info);
			for (ordinal_t o = it.next(); o != strata::no_more_docs; o = it.next()) { ps.push_back({it.wdf(), inv.length(o)}); }
		}
		std::sort(ps.begin(), ps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (const std::size_t k : {10u, 100u, 1000u}) {
			for (int sch = 0; sch < 2; ++sch) {
				const auto w = sch ? strata::weight::bm25_xapian(inv, info ? info->df : 0) : strata::weight::bm25(inv, info ? info->df : 0);
				double top = 0;
				std::priority_queue<std::uint32_t> lens;  // the k shortest lengths among wdf >= the current w
				for (std::size_t i = 0; i < ps.size();) {
					const std::uint32_t wdf = ps[i].first;
					for (; i < ps.size() && ps[i].first == wdf; ++i) {
						lens.push(ps[i].second);
						if (lens.size() > k) { lens.pop(); }
					}
					// Every one of those k postings has wdf >= wdf and a length (so a length code) at
					// most the k-th shortest's: each scores at least this.
					if (lens.size() == k) { top = std::max(top, sch ? w(wdf, lens.top()) : w.by_norm(wdf, strata::format::encode_length(lens.top()))); }
				}
				best[{t, k, sch}] = top;
			}
		}
	};
	std::ofstream out(out_path);
	for (const auto& line : lines(queries)) {
		const auto a = line.find("\"query\": \"") + 10;
		const std::string q = line.substr(a, line.find('"', a) - a);
		if (q.find_first_of("+-\"") != std::string::npos) { continue; }
		std::vector<std::string> ts;
		for (std::size_t i = 0; i < q.size();) {
			std::size_t j = q.find(' ', i);
			if (j == std::string::npos) { j = q.size(); }
			if (j > i) { ts.push_back(q.substr(i, j - i)); }
			i = j + 1;
		}
		for (const auto& t : ts) { term_seeds(t); }
		for (int sch = 0; sch < 2; ++sch) {
			for (const std::size_t k : {10u, 100u, 1000u}) {
				double seed = 0;
				for (const auto& t : ts) { seed = std::max(seed, best[{t, k, sch}]); }
				if (seed <= 0) { continue; }
				out << q << '\t' << sch << '\t' << k << "\t0";
				char buf[64];
				std::snprintf(buf, sizeof buf, " 0:%a", seed);
				for (std::size_t i = 0; i < k; ++i) { out << buf; }
				out << '\n';
			}
		}
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "skip" && argc == 5) { return skip_bench(argv[2], argv[3], argv[4]); }
	if (mode == "codec" && argc == 4) { return codec_bench(argv[2], argv[3]); }
	if (mode == "positions" && argc == 4) { return positions_bench(argv[2], argv[3]); }
	if (mode == "sizes" && argc == 3) { return sizes(argv[2]); }
	if (mode == "m17" && argc == 4) { return m17(argv[2], argv[3]); }
	if (mode == "seeds" && argc == 5) { return seeds(argv[2], argv[3], argv[4]); }
	std::fprintf(stderr, "usage: qgbench skip SEG TERMS QUERIES | codec SEG TERMS | positions SEG TERMS | sizes SEG\n");
	return 2;
}
