// Document order at flush (docs/research/indexing.md, "Reordering at flush"): the same
// documents written by segment_writer in several orders, and each order's cost and the
// postings and positions bytes it produces. Orders: the corpus's (curid order), a random
// shuffle, MinHash signatures over each document's terms (k minima, lexicographic), the
// same over terms neither unique nor ubiquitous, and strata's recursive graph bisection
// (the production P1 preset and one scaled to the sample).
//
//   research_order CORPUS.json [docs=0 (all)]

#include "stream.hh"

#include "strata/index.hh"
#include "hashes.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
using research::stream;

struct prepared {
	std::vector<strata::posting_input> in;
	std::vector<std::vector<std::uint32_t>> positions;
	std::vector<std::uint64_t> hashes;  // one per distinct term, aligned with `in`
};

std::vector<prepared> prepare(const stream& s) {
	std::vector<prepared> docs(s.docs.size());
	for (std::size_t n = 0; n < s.docs.size(); ++n) {
		const auto& d = s.docs[n];
		std::unordered_map<std::string_view, std::size_t> at;
		prepared& p = docs[n];
		for (std::uint32_t i = d.first; i < d.first + d.count; ++i) {
			const auto term = research::bytes_of(s.toks[i]);
			auto [it, inserted] = at.try_emplace(term, p.positions.size());
			if (inserted) {
				p.positions.emplace_back();
				p.hashes.push_back(::wordwise::hash(term.data(), term.size()));
				p.in.push_back({term, 0, {}});
			}
			p.positions[it->second].push_back(s.toks[i].pos);
		}
		for (std::size_t i = 0; i < p.in.size(); ++i) {
			p.in[i].wdf = std::uint32_t(p.positions[i].size());
			p.in[i].positions = p.positions[i];
		}
	}
	return docs;
}

struct sizes {
	std::uint64_t postings = 0, positions = 0, terms = 0, total = 0;
};

constexpr std::array<std::uint8_t, 16> segment_id{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

// Writes the documents in `order` ([new ordinal] = document) and measures the sections.
std::string write_in(const stream& s, const std::vector<prepared>& docs, const std::vector<std::uint32_t>& order, sizes& out) {
	strata::segment_writer w;
	for (std::size_t i = 0; i < order.size(); ++i) {
		const std::uint32_t n = order[i];
		w.add(i + 1, docs[n].in, s.docs[n].count, s.docs[n].id, 1, s.docs[n].key);
	}
	std::string file = w.flush(segment_id);
	const auto footer = strata::format::decode_footer(file);
	for (const auto& e : strata::format::decode_toc(file, footer)) {
		switch (e.kind) {
		case strata::format::section::postings: out.postings = e.length; break;
		case strata::format::section::positions: out.positions = e.length; break;
		case strata::format::section::terms: out.terms += e.length; break;
		case strata::format::section::term_index: out.terms += e.length; break;
		default: break;
		}
	}
	out.total = file.size();
	return file;
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t salt) {
	h ^= salt;
	h *= 0x9E3779B97F4A7C15ull;
	h ^= h >> 31;
	h *= 0xBF58476D1CE4E5B9ull;
	return h ^ (h >> 29);
}

// Sorts documents by k MinHash values over the terms `keep` accepts (lexicographic).
template <class Keep>
std::vector<std::uint32_t> minhash_order(const std::vector<prepared>& docs, unsigned k, Keep&& keep) {
	static constexpr std::uint64_t salts[8] = {0x1, 0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull, 0x8CB92BA72F3D8DD7ull, 0xA0761D6478BD642Full,
		0xE7037ED1A0B428DBull, 0x589965CC75374CC3ull, 0x1D8E4E27C47D124Full};
	std::vector<std::array<std::uint64_t, 8>> sig(docs.size());
	for (std::size_t n = 0; n < docs.size(); ++n) {
		auto& m = sig[n];
		m.fill(~std::uint64_t(0));
		for (std::size_t i = 0; i < docs[n].hashes.size(); ++i) {
			if (!keep(docs[n], i)) { continue; }
			for (unsigned j = 0; j < k; ++j) { m[j] = std::min(m[j], mix(docs[n].hashes[i], salts[j])); }
		}
	}
	std::vector<std::uint32_t> order(docs.size());
	std::iota(order.begin(), order.end(), 0u);
	std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
		for (unsigned j = 0; j < k; ++j) {
			if (sig[a][j] != sig[b][j]) { return sig[a][j] < sig[b][j]; }
		}
		return a < b;
	});
	return order;
}

// strata's own reordering (reorder.hh) over the docid-order segment just written.
std::vector<std::uint32_t> bisection_order(const std::string& file, std::uint32_t min_df) {
	strata::segment seg(file);
	strata::merge_source src{&seg, nullptr};
	strata::string_scratch scratch;
	strata::reorder::settings set;
	set.min_df = min_df;
	set.bisection.threads = 1;
	const auto refs = strata::reorder::order(std::span<const strata::merge_source>(&src, 1), scratch, set);
	std::vector<std::uint32_t> order(seg.doc_count());
	if (refs.empty()) {
		std::iota(order.begin(), order.end(), 0u);
	} else {
		for (std::size_t i = 0; i < refs.size(); ++i) { order[i] = refs[i].ord; }
	}
	return order;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: research_order CORPUS.json [docs]\n"); return 2; }
	const std::size_t max_docs = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
	const stream s = research::load(argv[1], max_docs);
	const auto docs = prepare(s);
	const std::uint32_t N = std::uint32_t(docs.size());
	// Term document frequencies, for the filtered MinHash.
	std::unordered_map<std::uint64_t, std::uint32_t> df;
	for (const auto& d : docs) {
		for (auto h : d.hashes) { ++df[h]; }
	}
	std::printf("documents %u, tokens %zu\n", N, s.toks.size());
	std::printf("%-44s %9s %13s %13s %11s %13s %8s\n", "order", "order ms", "postings", "positions", "terms", "segment", "ratio");
	sizes base{};
	std::string identity_file;
	auto report = [&](const char* name, double order_ms, const std::vector<std::uint32_t>& order, bool keep_file = false) {
		sizes sz{};
		std::string file = write_in(s, docs, order, sz);
		if (keep_file) { identity_file = std::move(file); base = sz; }
		const double ratio = base.postings ? double(sz.postings + sz.positions) / double(base.postings + base.positions) : 1.0;
		std::printf("%-44s %9.0f %13llu %13llu %11llu %13llu %8.3f\n", name, order_ms, static_cast<unsigned long long>(sz.postings),
			static_cast<unsigned long long>(sz.positions), static_cast<unsigned long long>(sz.terms), static_cast<unsigned long long>(sz.total), ratio);
		std::fflush(stdout);
	};
	std::vector<std::uint32_t> identity(N);
	std::iota(identity.begin(), identity.end(), 0u);
	report("corpus order (curid)", 0, identity, true);
	{
		auto order = identity;
		std::shuffle(order.begin(), order.end(), std::mt19937_64(7));
		report("random shuffle", 0, order);
	}
	auto all = [](const prepared&, std::size_t) { return true; };
	auto informative = [&](const prepared& d, std::size_t i) {
		const auto f = df.find(d.hashes[i])->second;
		return f >= 2 && f <= N / 10;
	};
	for (unsigned k : {1u, 2u, 4u}) {
		auto t0 = clock_type::now();
		auto order = minhash_order(docs, k, all);
		const double ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
		char name[64];
		std::snprintf(name, sizeof name, "MinHash k=%u, every term", k);
		report(name, ms, order);
		t0 = clock_type::now();
		order = minhash_order(docs, k, informative);
		const double ms2 = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
		std::snprintf(name, sizeof name, "MinHash k=%u, 2 <= df <= N/10", k);
		report(name, ms2, order);
	}
	for (std::uint32_t min_df : {4096u, 82u}) {
		const auto t0 = clock_type::now();
		auto order = bisection_order(identity_file, min_df);
		const double ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
		char name[64];
		std::snprintf(name, sizeof name, "bisection (P1), min_df %u, 1 thread", min_df);
		report(name, ms, order);
	}
	return 0;
}
