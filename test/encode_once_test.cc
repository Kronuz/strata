// The encode-once flush (format 13's bit 14 prototype) against today's: every list equal
// through the test decoder (encode_once_check.hh) with batches of 1 to a million tuples,
// hash collisions, long terms, far positions, terms given whole, and pieces re-encoded for a
// term's wdf or count blocks; the paths that decode the runs (documents left out, the
// key-range steps, the digest's scan) give exactly today's results.
#include "encode_once_check.hh"
#include "strata/tuple_buffer.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>
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
};

// tuple_buffer_test's documents, plus "mixw" (given with wdf 0, later a word) and "mixc"
// (given with a wdf apart from its count, later a word) to force re-encoded pieces.
doc random_doc(std::mt19937_64& r, std::uint64_t docid, bool late) {
	static const std::vector<std::string> words = {"a", "b", "the", "of", "rare", "octopus", "eightbyt", "ninebytes", std::string(254, 'm'),
		std::string(255, 'n'), std::string(300, 'l'), "x"};
	doc d;
	d.docid = docid;
	d.version = 1 + r() % 3;
	std::uint32_t pos = 0;
	auto word = [&](const std::string& w) {
		pos += 1 + std::uint32_t(r() % 3);
		if (r() % 40 == 0) { pos += std::uint32_t(1) << 24; }  // past a tuple's 24-bit position
		d.text += r() % 4 ? " " : "  ";
		d.tokens.push_back({std::uint32_t(d.text.size()), std::uint32_t(w.size()), pos});
		d.text += w;
	};
	for (int n = int(r() % 30); n >= 0; --n) { word(words[r() % words.size()]); }
	if (late) {
		word("mixw");
		if (r() % 2) { word("mixc"); }
	}
	d.length = std::uint32_t(d.tokens.size());
	if (r() % 5) {
		d.key = "Q" + std::to_string(docid);
		d.terms.push_back({d.key, 0, {}});
	}
	if (r() % 3 == 0) { d.terms.push_back({"T" + std::to_string(r() % 4), 1, {}}); }
	if (r() % 3 == 0) { d.terms.push_back({"W" + std::to_string(r() % 4), 3, {pos + 5}}); }
	if (r() % 7 == 0) { d.terms.push_back({std::string(400, 'Z'), 2, {pos + 7, pos + 9}}); }
	if (!late) {
		d.terms.push_back({"mixw", 0, {}});
		if (r() % 2) { d.terms.push_back({"mixc", 2, {pos + 11}}); }
	}
	d.stored = std::string(std::size_t(r() % 9), 's') + std::to_string(docid);
	return d;
}

void fill(strata::tuple_buffer& b, const std::vector<doc>& docs) {
	for (const doc& d : docs) {
		std::vector<strata::posting_input> in;
		for (const auto& e : d.terms) { in.push_back({e.term, e.wdf, e.positions}); }
		b.add(d.docid, in, d.text, d.tokens, d.length, d.stored, d.version, d.key);
	}
}

std::string flush(const std::vector<doc>& docs, bool once, std::size_t batch, std::uint64_t mask, std::span<const strata::ordinal_t> order,
	std::uint64_t* redone = nullptr) {
	strata::tuple_buffer b;
	b.encode_once = once;
	b.batch_tuples = batch;
	b.hash_mask = mask;
	fill(b, docs);
	strata::segment_writer w;
	std::string file;
	strata::string_output out(file);
	strata::string_spill positions;
	b.flush({1, 2, 3}, order, w, out, positions);
	if (redone) { *redone = b.redone_pieces(); }
	return file;
}

using posting = std::tuple<std::string, strata::ordinal_t, std::uint32_t, std::vector<std::uint32_t>>;

}  // namespace

int main() {
	std::uint64_t redone_total = 0, shorts = 0;
	for (std::uint64_t seed = 0; seed < 3; ++seed) {
		std::mt19937_64 r(seed);
		std::vector<doc> docs;
		for (std::uint64_t i = 0; i < 2000; ++i) { docs.push_back(random_doc(r, 10 + 3 * i, i >= 1000)); }
		for (std::size_t batch : {std::size_t(1) << 20, std::size_t(3000), std::size_t(500), std::size_t(37), std::size_t(1)}) {
			for (std::uint64_t mask : {~std::uint64_t(0), std::uint64_t(0xFF00000000000003)}) {
				std::uint64_t redone = 0;
				const std::string today = flush(docs, false, batch, mask, {});
				const std::string once = flush(docs, true, batch, mask, {}, &redone);
				redone_total += redone;
				try {
					shorts += encode_once_check::compare(today, once, false).shorts;
				} catch (const std::exception& e) {
					std::fprintf(stderr, "seed %llu batch %zu: %s\n", (unsigned long long)seed, batch, e.what());
					CHECK(false);
				}
			}
		}
		// Documents left out: the runs decoded, today's segment byte for byte.
		std::vector<strata::ordinal_t> some;
		for (strata::ordinal_t o = 0; o < docs.size(); ++o) {
			if (r() % 5) { some.push_back(o); }
		}
		for (std::size_t batch : {std::size_t(1) << 20, std::size_t(300), std::size_t(1)}) {
			CHECK(flush(docs, true, batch, ~std::uint64_t(0), some) == flush(docs, false, batch, ~std::uint64_t(0), some));
		}
		// The key-range steps and the digest's scan read the runs alike.
		for (std::size_t batch : {std::size_t(3000), std::size_t(7)}) {
			strata::tuple_buffer a, b;
			a.batch_tuples = b.batch_tuples = batch;
			b.encode_once = true;
			fill(a, docs);
			fill(b, docs);
			std::vector<posting> sa, sb;
			auto into = [](std::vector<posting>& v) {
				return [&v](std::string_view t, strata::ordinal_t o, std::uint32_t w, std::span<const std::uint32_t> p) {
					v.emplace_back(std::string(t), o, w, std::vector<std::uint32_t>(p.begin(), p.end()));
				};
			};
			a.scan(into(sa));
			b.scan(into(sb));
			std::sort(sa.begin(), sa.end());
			std::sort(sb.begin(), sb.end());
			CHECK(sa == sb);
			a.drain_for_flush(nullptr);
			b.drain_for_flush(nullptr);
			for (const auto& order : {std::vector<strata::ordinal_t>{}, some}) {
				auto rows = [&](const strata::tuple_buffer& buf) {
					std::vector<std::string> out;
					buf.each_term({}, {}, buf.plan(order), [&](std::string_view term, strata::term_postings& tp) {
						std::string row(term);
						for (std::size_t i = 0; i < tp.ords.size(); ++i) { row += ":" + std::to_string(tp.ords[i]) + "," + std::to_string(tp.wdfs[i]) + "," + std::to_string(tp.counts[i]); }
						for (auto p : tp.positions) { row += "/" + std::to_string(p); }
						out.push_back(row);
					});
					return out;
				};
				CHECK(rows(a) == rows(b));
			}
		}
	}
	CHECK(redone_total > 0);  // the mixed terms forced re-encoded pieces
	CHECK(shorts > 0);
	std::printf("encode_once_test: %d failures; %llu pieces re-encoded, %llu short blocks\n", failures, (unsigned long long)redone_total, (unsigned long long)shorts);
	return failures ? 1 : 0;
}
