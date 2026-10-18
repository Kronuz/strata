// The tuple buffer (the research note's inverter C) against segment_writer: the same
// documents give the same segment bytes, with hash collisions forced, long terms and far
// positions in side entries, terms given whole (an explicit wdf, positionless, keys), and
// documents left out and reordered at flush.
#include "strata/tuple_buffer.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

namespace {

struct entry {
	std::string term;
	std::uint32_t wdf;
	std::vector<std::uint32_t> positions;
};
struct doc {
	std::uint64_t docid = 0, version = 0;
	std::string text, stored, key;
	std::vector<strata::text_token> tokens;
	std::vector<entry> terms;
	std::uint32_t length = 0;
	std::vector<strata::posting_input> inputs() const {
		std::vector<strata::posting_input> in;
		for (const auto& e : terms) { in.push_back({e.term, e.wdf, e.positions}); }
		return in;
	}
};

doc random_doc(std::mt19937_64& r, std::uint64_t docid) {
	static const std::vector<std::string> words = {"a", "b", "the", "of", "rare", "octopus", "eightbyt", "ninebytes", std::string(254, 'm'),
		std::string(255, 'n'), std::string(300, 'l'), "x"};
	doc d;
	d.docid = docid;
	d.version = 1 + r() % 3;
	std::uint32_t pos = 0;
	for (int n = int(r() % 30); n >= 0; --n) {
		pos += 1 + std::uint32_t(r() % 3);
		if (r() % 40 == 0) { pos += std::uint32_t(1) << 24; }  // past a tuple's 24-bit position
		const std::string& w = words[r() % words.size()];
		d.text += r() % 4 ? " " : "  ";
		d.tokens.push_back({std::uint32_t(d.text.size()), std::uint32_t(w.size()), pos});
		d.text += w;
	}
	d.length = std::uint32_t(d.tokens.size());
	if (r() % 5) {
		d.key = "Q" + std::to_string(docid);
		d.terms.push_back({d.key, 0, {}});
	}
	if (r() % 3 == 0) { d.terms.push_back({"T" + std::to_string(r() % 4), 1, {}}); }   // positionless, wdf 1
	if (r() % 3 == 0) { d.terms.push_back({"W" + std::to_string(r() % 4), 3, {pos + 5}}); }  // wdf apart from its count
	if (r() % 7 == 0) { d.terms.push_back({std::string(400, 'Z'), 2, {pos + 7, pos + 9}}); }  // a long term given whole
	d.stored = std::string(std::size_t(r() % 9), 's') + std::to_string(docid);
	return d;
}

std::string reference(const std::vector<doc>& docs, const std::vector<std::size_t>& order) {
	strata::segment_writer w;
	for (std::size_t i : order) {
		const doc& d = docs[i];
		const auto in = d.inputs();
		w.add(d.docid, in, d.text, d.tokens, d.length, d.stored, d.version, d.key);
	}
	return w.flush({1, 2, 3});
}

std::string tuples(const std::vector<doc>& docs, std::span<const strata::ordinal_t> order, std::uint64_t mask, std::size_t threads = 1) {
	strata::tuple_buffer b;
	b.hash_mask = mask;
	for (const doc& d : docs) {
		const auto in = d.inputs();
		b.add(d.docid, in, d.text, d.tokens, d.length, d.stored, d.version, d.key);
	}
	strata::segment_writer w;
	return b.flush({1, 2, 3}, order, w, threads);
}

}  // namespace

int main() {
	for (std::uint64_t seed = 0; seed < 6; ++seed) {
		std::mt19937_64 r(seed);
		std::vector<doc> docs;
		for (std::uint64_t i = 0; i < 2000; ++i) { docs.push_back(random_doc(r, 10 + 3 * i)); }
		std::vector<std::size_t> all(docs.size());
		std::iota(all.begin(), all.end(), 0);
		const std::string expected = reference(docs, all);
		// Every document in order: the whole hash, then hashes narrowed to collide.
		for (std::uint64_t mask : {~std::uint64_t(0), std::uint64_t(0xFF00000000000003), std::uint64_t(0x0100000000000000), std::uint64_t(0)}) {
			for (std::size_t threads : {1u, 2u, 8u}) { CHECK(tuples(docs, {}, mask, threads) == expected); }  // flushes on several threads too (D18)
		}
		// Some left out and the docids shuffled: the flush takes the rest in docid order.
		std::vector<std::uint64_t> ids(docs.size());
		for (std::size_t i = 0; i < docs.size(); ++i) { ids[i] = docs[i].docid; }
		std::shuffle(ids.begin(), ids.end(), r);
		for (std::size_t i = 0; i < docs.size(); ++i) {
			docs[i].docid = ids[i];
			if (!docs[i].key.empty()) {
				docs[i].key = "Q" + std::to_string(ids[i]);
				docs[i].terms[0].term = docs[i].key;
			}
		}
		std::vector<std::size_t> live;
		for (std::size_t i = 0; i < docs.size(); ++i) {
			if (r() % 5) { live.push_back(i); }
		}
		std::sort(live.begin(), live.end(), [&](std::size_t a, std::size_t b) { return docs[a].docid < docs[b].docid; });
		std::vector<strata::ordinal_t> order(live.begin(), live.end());
		const std::string kept = reference(docs, live);
		for (std::uint64_t mask : {~std::uint64_t(0), std::uint64_t(0xFF00000000000003)}) {
			for (std::size_t threads : {1u, 3u}) { CHECK(tuples(docs, order, mask, threads) == kept); }
		}
		// Only some left out, the rest in arrival order (renumbered, never resorted).
		std::vector<std::size_t> in_order;
		for (std::size_t i = 0; i < docs.size(); ++i) {
			if (i % 4) { in_order.push_back(i); }
		}
		std::vector<doc> ordered = docs;
		for (std::size_t i = 0; i < ordered.size(); ++i) { ordered[i].docid = 100 + i; }
		std::vector<strata::ordinal_t> monotone(in_order.begin(), in_order.end());
		for (std::size_t threads : {1u, 4u}) { CHECK(tuples(ordered, monotone, ~std::uint64_t(0), threads) == reference(ordered, in_order)); }
	}
	// scan() reports every posting with its document's positions.
	{
		strata::tuple_buffer b;
		const std::string text = " a b a";
		const std::vector<strata::text_token> tokens = {{1, 1, 1}, {3, 1, 2}, {5, 1, 3}};
		const std::vector<strata::posting_input> terms = {{"Qk", 0, {}}};
		b.add(7, terms, text, tokens, 3, "s", 1, "Qk");
		std::vector<std::string> seen;
		b.scan([&](std::string_view term, strata::ordinal_t o, std::uint32_t wdf, std::span<const std::uint32_t> positions) {
			std::string s = std::string(term) + ":" + std::to_string(o) + ":" + std::to_string(wdf);
			for (auto p : positions) { s += "," + std::to_string(p); }
			seen.push_back(s);
		});
		std::sort(seen.begin(), seen.end());
		CHECK((seen == std::vector<std::string>{"Qk:0:0", "a:0:2,1,3", "b:0:1,2"}));
		CHECK(b.key(0) == "Qk" && b.stored(0) == "s" && b.docid(0) == 7 && b.version(0) == 1 && b.length(0) == 3);
	}
	std::puts(failures ? "tuple_buffer: FAILED" : "tuple_buffer: ok");
	return failures ? 1 : 0;
}
