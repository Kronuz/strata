// What a merge costs today against what a copy merge cannot avoid (docs/research/indexing.md,
// "Merges as byte copies"): the sample split into two segments by document, merged by
// merge_segments (decode and re-encode every posting), against the floor of a copy merge:
// walking both dictionaries in order, building the merged dictionary, and copying the
// postings and positions bytes once. Five interleaved trials.
//
//   research_mergecost CORPUS.json [trials=5] [docs=0 (all)]

#include "stream.hh"

#include "strata/index.hh"
#include "hashes.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
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

std::string write_segment(const stream& s, const std::vector<prepared>& docs, std::size_t begin, std::size_t end, std::uint8_t id) {
	strata::segment_writer w;
	for (std::size_t n = begin; n < end; ++n) { w.add(n + 1, docs[n].in, s.docs[n].count, s.docs[n].id, 1, s.docs[n].key); }
	std::array<std::uint8_t, 16> sid{};
	sid[0] = id;
	return w.flush(sid);
}

std::uint64_t section_bytes(const strata::segment& seg, strata::format::section kind) {
	for (const auto& e : seg.sections()) {
		if (e.kind == kind) { return e.length; }
	}
	return 0;
}

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: research_mergecost CORPUS.json [trials] [docs]\n"); return 2; }
	const int trials = argc > 2 ? std::atoi(argv[2]) : 5;
	const std::size_t max_docs = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0;
	const stream s = research::load(argv[1], max_docs);
	const auto docs = prepare(s);
	const std::size_t N = docs.size(), half = N / 2;
	const std::string f1 = write_segment(s, docs, 0, half, 1), f2 = write_segment(s, docs, half, N, 2);
	strata::segment s1(f1), s2(f2);
	const std::uint64_t postings1 = section_bytes(s1, strata::format::section::postings) + section_bytes(s2, strata::format::section::postings);
	const std::uint64_t positions1 = section_bytes(s1, strata::format::section::positions) + section_bytes(s2, strata::format::section::positions);
	std::uint64_t postings = 0;  // (term, document) pairs over both segments
	for (const auto& d : docs) { postings += d.in.size(); }
	std::printf("two segments: %zu + %zu documents, %zu + %zu bytes; postings %llu, positions %llu bytes; %llu postings\n", half, N - half, f1.size(), f2.size(),
		static_cast<unsigned long long>(postings1), static_cast<unsigned long long>(positions1), static_cast<unsigned long long>(postings));
	std::vector<double> merge_ns, walk_ns, dict_ns, copy_ns;
	std::size_t merged_bytes = 0, terms = 0;
	for (int t = 0; t < trials; ++t) {
		// Today's merge.
		{
			const strata::merge_source src[2] = {{&s1, nullptr}, {&s2, nullptr}};
			std::string out;
			strata::string_output sink(out);
			strata::string_spill spill;
			std::array<std::uint8_t, 16> sid{};
			sid[0] = 3;
			const auto t0 = clock_type::now();
			strata::merge_segments(std::span<const strata::merge_source>(src, 2), sid, sink, spill, "Q");
			merge_ns.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - t0).count());
			merged_bytes = out.size();
		}
		// A copy merge's floor: the dictionaries walked in order (the union of terms),
		std::vector<std::string> keys;
		{
			const auto t0 = clock_type::now();
			strata::segment::term_cursor c1(s1), c2(s2);
			bool m1 = c1.next(), m2 = c2.next();
			std::uint64_t union_terms = 0, acc = 0;
			while (m1 || m2) {
				int which = !m2 ? 0 : !m1 ? 1 : c1.term() < c2.term() ? 0 : c1.term() > c2.term() ? 1 : 2;
				if (which == 0) { acc += c1.info().df; keys.emplace_back(c1.term()); m1 = c1.next(); }
				else if (which == 1) { acc += c2.info().df; keys.emplace_back(c2.term()); m2 = c2.next(); }
				else { acc += c1.info().df + c2.info().df; keys.emplace_back(c1.term()); m1 = c1.next(); m2 = c2.next(); }
				++union_terms;
			}
			walk_ns.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - t0).count() + double(acc == 1));
			terms = union_terms;
		}
		// the merged dictionary built over them with a small value each,
		{
			std::array<bool, 256> used{};
			for (const auto& k : keys) {
				for (char c : k) { used[std::uint8_t(c)] = true; }
			}
			std::string alphabet;
			for (unsigned c = 0; c < 256; ++c) {
				if (used[c]) { alphabet.push_back(char(c)); }
			}
			const auto t0 = clock_type::now();
			termdict::builder dict(alphabet);
			const char value[6] = {1, 2, 3, 4, 5, 6};
			for (const auto& k : keys) { dict.add(k, std::string_view(value, 6)); }
			auto tables = dict.finish();
			dict_ns.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - t0).count() + double(tables.blocks.empty()));
		}
		// and the postings and positions bytes copied once.
		{
			std::string out;
			out.reserve(postings1 + positions1);
			const auto t0 = clock_type::now();
			for (const strata::segment* seg : {&s1, &s2}) {
				for (const auto& e : seg->sections()) {
					if (e.kind == strata::format::section::postings || e.kind == strata::format::section::positions) {
						out.append((seg == &s1 ? f1 : f2).data() + e.offset, e.length);
					}
				}
			}
			copy_ns.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - t0).count() + double(out.size() == 1));
		}
		std::fprintf(stderr, "trial %d done\n", t + 1);
	}
	const double P = double(postings);
	std::printf("\n%-58s %10s %12s\n", "merge of the two segments (one thread, median)", "ms", "ns/posting");
	std::printf("%-58s %10.1f %12.1f\n", "merge_segments today (decode, re-encode, dictionary)", median(merge_ns) / 1e6, median(merge_ns) / P);
	std::printf("%-58s %10.1f %12.1f\n", "copy merge floor: walk both dictionaries in order", median(walk_ns) / 1e6, median(walk_ns) / P);
	std::printf("%-58s %10.1f %12.1f\n", "copy merge floor: build the merged dictionary", median(dict_ns) / 1e6, median(dict_ns) / P);
	std::printf("%-58s %10.1f %12.1f\n", "copy merge floor: copy postings and positions bytes", median(copy_ns) / 1e6, median(copy_ns) / P);
	std::printf("%-58s %10.1f %12.1f\n", "copy merge floor, in all", (median(walk_ns) + median(dict_ns) + median(copy_ns)) / 1e6,
		(median(walk_ns) + median(dict_ns) + median(copy_ns)) / P);
	std::printf("\nmerged segment %zu bytes; %zu terms in the union\n", merged_bytes, terms);
	return 0;
}
