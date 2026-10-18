// Expansion (docs/design/automata.md, T1 and T2): each automaton intersected with real
// segments' dictionaries equals brute force over the segment's terms, with every matched
// term's info equal to scan_terms' and find_term's, through restart runs, blocks, and
// seeks to the matcher's targets; expansions over several segments equal a brute-force
// union of terms with a live document under every limit type; the `or` and constant
// combinations count the documents brute force does, with deletes; budgets, cancellation,
// pauses, and corrupt dictionaries.
#include "strata/expand.hh"
#include "strata/writer.hh"

#include "automata_reference.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

namespace automata = strata::automata;
using strata::format::term_info;

namespace {

struct built {
	std::string file;
	std::unique_ptr<strata::segment> seg;
	std::map<std::string, std::set<strata::ordinal_t>> docs;  // term -> ordinals
};

// One segment over a term pool: each document takes a few random terms, and the first two
// pool terms most documents (skip blocks and stored competitive pairs).
built build(const std::vector<std::string>& pool, std::size_t ndocs, std::uint64_t first_docid, std::mt19937_64& rng) {
	built b;
	strata::segment_writer w;
	std::vector<std::uint32_t> pos = {1, 2, 3, 4, 5, 6, 7, 8};
	for (std::size_t d = 0; d < ndocs; ++d) {
		std::set<std::string> picked;
		for (std::size_t i = 0, n = 1 + rng() % 12; i < n; ++i) { picked.insert(pool[rng() % pool.size()]); }
		if (pool.size() > 2 && rng() % 4) { picked.insert(pool[0]); }
		if (pool.size() > 2 && rng() % 4) { picked.insert(pool[1]); }
		std::vector<strata::posting_input> in;
		std::uint32_t length = 0;
		for (const auto& t : picked) {
			const std::uint32_t wdf = 1 + std::uint32_t(rng() % 3);
			const bool positions = rng() % 3 != 0;
			in.push_back({t, wdf, positions ? std::span<const std::uint32_t>(pos.data(), wdf) : std::span<const std::uint32_t>()});
			length += wdf;
			b.docs[t].insert(strata::ordinal_t(d));
		}
		w.add(first_docid + d, in, length);
	}
	b.file = w.flush({});
	b.seg = std::make_unique<strata::segment>(b.file);
	return b;
}

bool same(const term_info& x, const term_info& y) {
	if (x.flags != y.flags || x.df != y.df || x.cf != y.cf || x.postings != y.postings || x.positions != y.positions || x.single_wdf != y.single_wdf ||
		x.postings_length != y.postings_length || x.positions_length != y.positions_length || x.impacts.size() != y.impacts.size()) {
		return false;
	}
	for (std::size_t i = 0; i < x.impacts.size(); ++i) {
		if (x.impacts[i].wdf != y.impacts[i].wdf || x.impacts[i].length != y.impacts[i].length) { return false; }
	}
	return true;
}

std::vector<std::pair<std::string, term_info>> all_terms(const strata::segment& seg) {
	std::vector<std::pair<std::string, term_info>> out;
	seg.scan_terms([&](std::string_view t, const term_info& info) {
		out.emplace_back(std::string(t), info);
		return true;
	});
	return out;
}

std::uint64_t seeks = 0;

template <class A, class Ref>
void compare(const strata::segment& seg, const std::vector<std::pair<std::string, term_info>>& terms, const A& a, const Ref& ref) {
	std::vector<std::pair<std::string, term_info>> want, got;
	for (const auto& t : terms) {
		if (ref(t.first)) { want.push_back(t); }
	}
	strata::intersect_stats st;
	strata::intersect(seg, a, [&](std::string_view t, const term_info& info) {
		got.emplace_back(std::string(t), info);
		return true;
	}, {.stats = &st});
	seeks += st.seeks;
	// Seeking at every dead entry gives the same answers.
	std::vector<std::pair<std::string, term_info>> eager;
	strata::intersect(seg, a, [&](std::string_view t, const term_info& info) {
		eager.emplace_back(std::string(t), info);
		return true;
	}, {.seek_after = 1});
	CHECK(eager.size() == got.size());
	for (std::size_t i = 0; i < std::min(eager.size(), got.size()); ++i) { CHECK(eager[i].first == got[i].first && same(eager[i].second, got[i].second)); }
	CHECK(got.size() == want.size());
	for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
		CHECK(got[i].first == want[i].first);
		CHECK(same(got[i].second, want[i].second));
		const auto found = seg.find_term(got[i].first);
		CHECK(found && same(*found, got[i].second));
	}
	CHECK(st.matched == got.size() && st.visited >= st.matched);
	if (!want.empty()) {  // stopping after the first
		std::size_t n = 0;
		strata::intersect(seg, a, [&](std::string_view t, const term_info&) {
			CHECK(t == want[0].first);
			++n;
			return false;
		});
		CHECK(n == 1);
	}
}

// live[s]: the terms segment s holds in a live document.
template <class A, class Ref>
void compare_expansion(std::span<const strata::segment_snapshot> segs, const std::vector<std::vector<std::pair<std::string, term_info>>>& terms,
	const std::vector<std::set<std::string>>& live, const A& a, const Ref& ref, strata::expansion_limit limit) {
	std::map<std::string, std::uint64_t> union_df;
	std::set<std::string> any_live;
	for (std::size_t s = 0; s < terms.size(); ++s) {
		for (const auto& [t, info] : terms[s]) {
			if (ref(t)) { union_df[t] += info.df; }
			if (ref(t) && live[s].count(t)) { any_live.insert(t); }
		}
	}
	std::vector<std::pair<std::string, std::uint64_t>> want;
	for (const auto& [t, df] : union_df) {
		if (any_live.count(t)) { want.emplace_back(t, df); }  // a term live nowhere is never selected
	}
	bool want_error = false;
	if (limit.max && want.size() > limit.max) {
		if (limit.type == strata::limit_type::error) {
			want_error = true;
		} else if (limit.type == strata::limit_type::first) {
			want.resize(limit.max);
		} else {
			std::stable_sort(want.begin(), want.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
			want.resize(limit.max);
			std::sort(want.begin(), want.end());
		}
	}
	strata::expansion e;
	bool threw = false;
	try {
		e = strata::expand(segs, a, limit);
	} catch (const strata::expansion_error& err) {
		threw = true;
		CHECK(err.limit == limit.max);
	}
	CHECK(threw == want_error);
	if (threw) { return; }
	CHECK(e.terms.size() == want.size() && e.df.size() == want.size() && e.segments.size() == segs.size());
	for (std::size_t i = 0; i < std::min(e.terms.size(), want.size()); ++i) { CHECK(e.terms[i] == want[i].first && e.df[i] == want[i].second); }
	for (std::size_t s = 0; s < segs.size(); ++s) {
		std::size_t at = 0;
		for (const auto& en : e.segments[s]) {
			CHECK(en.term < e.terms.size());
			const auto info = segs[s].seg->find_term(e.terms[en.term]);
			CHECK(info && same(*info, en.info));
			CHECK(en.live == (live[s].count(e.terms[en.term]) != 0));
			CHECK(at == 0 || e.segments[s][at - 1].term < en.term);
			++at;
		}
		std::size_t present = 0;
		for (const auto& t : e.terms) { present += segs[s].seg->find_term(t) ? 1 : 0; }
		CHECK(present == e.segments[s].size());
	}
}

std::string random_string(std::mt19937_64& rng, const std::vector<std::string>& pieces, std::size_t max_pieces) {
	std::string s;
	for (std::size_t i = 0, n = rng() % (max_pieces + 1); i < n; ++i) { s += pieces[rng() % pieces.size()]; }
	return s;
}

const std::vector<std::string> pieces = {"a", "b", "c", "o", "m", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9D\x84\x9E", "\xE2", "\x82", "\xC3\xA2", "\xF0", "\xFF",
	std::string(1, '\0')};

// Recomputes every section's checksum and the table's, so a changed term value reaches
// the decoder's own checks.
void refresh_checksums(std::string& file) {
	const auto footer = strata::format::decode_footer(file);
	auto toc = strata::format::decode_toc(file, footer);
	for (auto& e : toc) { e = strata::format::describe(e.kind, e.offset, std::string_view(file).substr(e.offset, e.length), e.region_shift); }
	const std::string toc_bytes = strata::format::encode_toc(toc);
	file.replace(footer.toc_offset, toc_bytes.size(), toc_bytes);
	auto f = footer;
	f.toc_crc = hashes::crc32c(toc_bytes.data(), toc_bytes.size());
	const std::string footer_bytes = strata::format::encode_footer(f);
	file.replace(file.size() - footer_bytes.size(), footer_bytes.size(), footer_bytes);
}

strata::format::section_entry terms_section(const std::string& file) {
	const auto footer = strata::format::decode_footer(file);
	for (const auto& e : strata::format::decode_toc(file, footer)) {
		if (e.kind == strata::format::section::terms) { return e; }
	}
	std::abort();
}

}  // namespace

int main() {
	std::mt19937_64 rng(13);

	// Every {a, b} string up to 10 characters: restart runs, blocks, and long dead runs.
	{
		std::vector<std::string> pool = {"a", "b"};
		for (std::size_t i = 0; i < pool.size() && pool.size() < 2046; ++i) {
			if (pool[i].size() < 10) {
				pool.push_back(pool[i] + "a");
				pool.push_back(pool[i] + "b");
			}
		}
		std::sort(pool.begin(), pool.end());
		built b = build(pool, 800, 1, rng);
		const auto terms = all_terms(*b.seg);
		CHECK(terms.size() > 1500);
		for (std::uint64_t k = 0; k <= 2; ++k) {
			for (const char* t : {"bbbbbbbbbb", "aaaaaaaaaa", "abababab", "ba"}) {
				for (bool swaps : {false, true}) {
					compare(*b.seg, terms, automata::levenshtein(t, k, 0, swaps), [&](std::string_view key) { return reference::levenshtein(t, k, 0, swaps, key); });
					compare(*b.seg, terms, automata::levenshtein(t, k, 2, swaps), [&](std::string_view key) { return reference::levenshtein(t, k, 2, swaps, key); });
				}
			}
		}
		for (const char* g : {"*", "a*", "b?a*", "*a*a*a*b", "?????", "b*b", "", "abba"}) {
			compare(*b.seg, terms, automata::glob(g), [&](std::string_view key) { return reference::glob(g, 3, key); });
		}
		for (const char* p : {"", "a", "abab", "bbbbbbbbbb", "c"}) {
			compare(*b.seg, terms, automata::prefix(p), [&](std::string_view key) { return reference::prefix(p, key); });
		}
		CHECK(seeks > 0);  // the dead runs under "aa..." for "bbbbbbbbbb" pass seek_after

		// Seek targets (D6): "?a" visits a handful of the 2,046 entries when it seeks at every
		// dead one, and fewer than the default walk.
		{
			strata::intersect_stats eager, lazy;
			std::size_t n = 0;
			const automata::glob g("?a");
			strata::intersect(*b.seg, g, [&](std::string_view, const term_info&) { return ++n > 0; }, {.stats = &eager, .seek_after = 1});
			CHECK(n == 2);
			strata::intersect(*b.seg, g, [&](std::string_view, const term_info&) { return true; }, {.stats = &lazy});
			CHECK(eager.visited <= 16 && eager.visited < lazy.visited);
		}

		// Cancellation: a whole-dictionary walk stops within check_every entries of the request.
		{
			task::cancel_source src;
			const task::cancel_token tok = src.token();
			std::size_t n = 0;
			bool cancelled = false;
			try {
				strata::intersect(*b.seg, automata::glob("*"), [&](std::string_view, const term_info&) {
					if (++n == 5) { src.request(); }
					return true;
				}, {.cancel = &tok, .check_every = 16});
			} catch (const task::cancelled&) {
				cancelled = true;
			}
			CHECK(cancelled && n >= 5 && n <= 5 + 16);
		}

		// Pauses: a walk paused every 7 entries resumes to the same answer.
		{
			std::vector<std::string> whole, paused;
			const automata::levenshtein l("abab", 1);
			strata::intersect(*b.seg, l, [&](std::string_view t, const term_info&) {
				whole.emplace_back(t);
				return true;
			});
			std::optional<std::string> at = std::string();
			std::size_t calls = 0;
			while (at) {
				const std::string from = *at;
				at = strata::intersect(*b.seg, l, [&](std::string_view t, const term_info&) {
					paused.emplace_back(t);
					return true;
				}, {.from = from, .pause = [] { return true; }, .check_every = 7});
				++calls;
				CHECK(!at || *at > from || from.empty());
			}
			CHECK(paused == whole && calls > 10);
		}

		// The budget: the matcher's states and the expansion's terms draw on a permit.
		{
			const strata::segment_snapshot one[] = {{b.seg.get(), nullptr}};
			strata::byte_budget small(256), ample(std::uint64_t(1) << 30);
			auto p1 = small.open(), p2 = ample.open();
			bool refused = false;
			try {
				(void)strata::expand(std::span<const strata::segment_snapshot>(one), automata::glob("*"), {}, {.permit = &p1});
			} catch (const strata::query_budget_error&) {
				refused = true;
			}
			CHECK(refused);
			const auto e = strata::expand(std::span<const strata::segment_snapshot>(one), automata::glob("*"), {}, {.permit = &p2});
			CHECK(e.terms.size() == terms.size() && p2.bytes() > e.terms.size() * 8);
		}
	}

	// Terms held only by deleted documents are never selected (D8).
	{
		strata::segment_writer w;
		const strata::posting_input dead[] = {{"obamaa", 1, {}}};
		const strata::posting_input live[] = {{"obamab", 1, {}}};
		w.add(1, std::span<const strata::posting_input>(dead), 1);
		w.add(2, std::span<const strata::posting_input>(live), 1);
		const std::string file = w.flush({});
		const strata::segment seg(file);
		const std::vector<std::uint64_t> deleted = {1};  // ordinal 0, docid 1
		const strata::segment_snapshot snap[] = {{&seg, &deleted, 1}};
		const std::span<const strata::segment_snapshot> segs(snap);
		const automata::prefix p("obama");
		for (auto type : {strata::limit_type::first, strata::limit_type::most_frequent, strata::limit_type::error}) {
			const auto e = strata::expand(segs, p, {1, type});
			CHECK(e.terms == std::vector<std::string>{"obamab"} && e.df == std::vector<std::uint64_t>{1});
		}
		const auto all = strata::expand(segs, p);
		CHECK(all.terms == std::vector<std::string>{"obamab"});
		CHECK(all.segments[0].size() == 1 && all.segments[0][0].live);
	}

	// Corrupt dictionaries: a changed block fails its region check, and changed term values
	// either decode or throw corrupt_segment, a df past the document count among them.
	{
		std::vector<std::string> pool;
		for (int i = 0; i < 120; ++i) { pool.push_back("t" + std::to_string(1000 + i)); }
		std::mt19937_64 r2(3);
		built b = build(pool, 60, 1, r2);
		const auto sec = terms_section(b.file);
		{
			std::string bad = b.file;
			bad[sec.offset + sec.length / 2] ^= 0x10;
			const strata::segment seg(bad);
			bool threw = false;
			try {
				strata::intersect(seg, automata::prefix(""), [](std::string_view, const term_info&) { return true; });
			} catch (const strata::corrupt_segment&) {
				threw = true;
			}
			CHECK(threw);
		}
		std::size_t df_errors = 0, other = 0;
		for (std::uint64_t at = 0; at < sec.length; ++at) {
			for (char x : {char(0x01), char(0x80), char(0xFF)}) {
				std::string bad = b.file;
				bad[sec.offset + at] ^= x;
				refresh_checksums(bad);
				try {
					const strata::segment seg(bad);
					strata::intersect(seg, automata::glob("t1*"), [](std::string_view, const term_info&) { return true; });
				} catch (const strata::corrupt_segment& e) {
					if (std::string_view(e.what()).find("document frequency") != std::string_view::npos) { ++df_errors; }
				} catch (...) {
					++other;
				}
			}
		}
		CHECK(df_errors > 0 && other == 0);
	}

	// Random pools, Unicode and invalid bytes included; several segments sharing terms.
	for (int round = 0; round < 12; ++round) {
		std::set<std::string> s;
		for (std::size_t i = 0, n = 50 + rng() % 1500; i < n; ++i) { s.insert(random_string(rng, pieces, 1 + rng() % 8)); }
		s.erase("");
		if (round % 4 == 0) {  // 2 KiB terms, past one block's 1 KiB
			const std::string base = random_string(rng, pieces, 600) + std::string(2048, 'o');
			for (int j = 0; j < 3; ++j) { s.insert(base + random_string(rng, pieces, 3)); }
		}
		const std::vector<std::string> pool(s.begin(), s.end());
		std::vector<built> segs;
		for (std::size_t i = 0, n = 1 + rng() % 4; i < n; ++i) { segs.push_back(build(pool, 50 + rng() % 400, 1 + i * 100000, rng)); }
		std::vector<std::vector<std::pair<std::string, term_info>>> terms;
		std::vector<std::vector<std::uint64_t>> deleted;
		for (auto& b : segs) {
			terms.push_back(all_terms(*b.seg));
			deleted.emplace_back((b.seg->doc_count() + 63) / 64, 0);
			for (std::uint64_t o = 0; o < b.seg->doc_count(); ++o) {
				if (rng() % 5 == 0) { deleted.back()[o / 64] |= std::uint64_t(1) << (o % 64); }
			}
		}
		std::vector<strata::segment_snapshot> snap;
		for (std::size_t i = 0; i < segs.size(); ++i) { snap.push_back({segs[i].seg.get(), &deleted[i]}); }
		std::vector<std::set<std::string>> live(segs.size());
		for (std::size_t i = 0; i < segs.size(); ++i) {
			for (const auto& [t, ords] : segs[i].docs) {
				for (auto o : ords) {
					if (!((deleted[i][o / 64] >> (o % 64)) & 1)) {
						live[i].insert(t);
						break;
					}
				}
			}
		}

		for (int i = 0; i < 10; ++i) {
			std::string near;  // a short term: the references are quadratic in pattern length
			do { near = pool[rng() % pool.size()]; } while (near.size() > 64);
			const std::string p = near.substr(0, rng() % 3);
			std::string g;
			for (const auto& c : automata::decode(near)) { g += rng() % 4 == 0 ? (rng() % 2 ? "*" : "?") : std::string(c.bytes); }
			const std::uint64_t k = rng() % 3;
			const std::size_t fixed = rng() % 3;
			const bool swaps = rng() % 2;
			const automata::prefix pa(p);
			const automata::glob ga(g);
			const automata::levenshtein la(near, k, fixed, swaps);
			auto pr = [&](std::string_view key) { return reference::prefix(p, key); };
			auto gr = [&](std::string_view key) { return reference::glob(g, 3, key); };
			auto lr = [&](std::string_view key) { return reference::levenshtein(near, k, fixed, swaps, key); };
			for (std::size_t si = 0; si < segs.size(); ++si) {
				compare(*segs[si].seg, terms[si], pa, pr);
				compare(*segs[si].seg, terms[si], ga, gr);
				compare(*segs[si].seg, terms[si], la, lr);
			}
			for (auto type : {strata::limit_type::error, strata::limit_type::first, strata::limit_type::most_frequent}) {
				for (std::uint64_t max : {0, 1, 3, 20}) {
					compare_expansion(snap, terms, live, pa, pr, {max, type});
					compare_expansion(snap, terms, live, ga, gr, {max, type});
					compare_expansion(snap, terms, live, la, lr, {max, type});
				}
			}
			// The `or` combination counts the live documents holding any expanded term.
			const auto e = strata::expand(std::span<const strata::segment_snapshot>(snap), la);
			std::uint64_t want = 0;
			for (std::size_t si = 0; si < segs.size(); ++si) {
				std::set<strata::ordinal_t> any;
				for (const auto& t : e.terms) {
					auto it = segs[si].docs.find(t);
					if (it != segs[si].docs.end()) { any.insert(it->second.begin(), it->second.end()); }
				}
				for (auto o : any) { want += (deleted[si][o / 64] >> (o % 64) & 1) ? 0 : 1; }
			}
			auto builder = [&](const strata::segment& seg) -> strata::scorer_ptr {
				for (std::size_t si = 0; si < snap.size(); ++si) {
					if (snap[si].seg == &seg) { return strata::expansion_or(e, snap, si); }
				}
				return nullptr;
			};
			CHECK(strata::search_count(snap, builder) == want);
			std::uint64_t total = 0;
			(void)strata::search(snap, builder, 10, &total);
			CHECK(total == want);
			// Constant score: the same documents, each scoring the constant.
			auto constant = [&](const strata::segment& seg) -> strata::scorer_ptr {
				for (std::size_t si = 0; si < snap.size(); ++si) {
					if (snap[si].seg == &seg) { return strata::expansion_constant(e, snap, si, 2.5); }
				}
				return nullptr;
			};
			CHECK(strata::search_count(snap, constant) == want);
			const auto hits = strata::search(snap, constant, 10);
			CHECK(hits.size() == std::min<std::uint64_t>(want, 10));
			for (const auto& h : hits) { CHECK(h.score == 2.5); }
		}
	}

	std::printf("expand: %llu seeks, %d failures\n", (unsigned long long)seeks, failures);
	return failures ? 1 : 0;
}
