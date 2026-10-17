// Merges through the plugins (layers.md L6, step 5) write the same bytes and plan as the
// merge before the split (merge_oracle.hh): random segments, with and without deletes and
// keys, in docid order, reordered, and from a single permuted source, with and without a
// record spill; bad orders and duplicate docids fail the same way.
#include "merge_oracle.hh"
#include "strata/merge.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <typeinfo>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using namespace strata;

namespace {

const std::array<std::uint8_t, 16> id = {'o', 'r', 'a', 'c', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 0, 5};
const char* words[] = {"a", "b", "c", "d", "e", "rare", "Bool", "\xff\x01"};

// A random segment of `docs` documents with docids from `ids`, keys "Q<docid>" when keyed.
std::string build(std::mt19937_64& r, const std::vector<std::uint64_t>& ids, bool keyed, bool wide, std::uint8_t shift) {
	segment_writer w;
	w.wide_skip_offsets = wide;
	w.region_shift = shift;
	std::vector<std::vector<std::uint32_t>> pos(std::size(words));
	std::vector<posting_input> in;
	std::string key;
	for (const std::uint64_t docid : ids) {
		in.clear();
		std::uint32_t length = 0;
		for (std::size_t k = 0; k < std::size(words); ++k) {
			pos[k].clear();
			if (r() % (k == 5 ? 40 : 3) != 0) { continue; }
			const std::uint32_t wdf = std::string_view(words[k]) == "Bool" ? 0 : 1 + std::uint32_t(r() % 4);
			std::uint32_t p = 0;
			if (r() % 6) {
				for (std::uint32_t i = 0; i < std::max<std::uint32_t>(wdf, 1); ++i) { pos[k].push_back(p += 1 + std::uint32_t(r() % 3)); }
			}
			in.push_back({words[k], wdf, pos[k]});
			length += wdf;
		}
		if (keyed) {
			key = "Q" + std::to_string(docid);
			in.push_back({key, 0, {}});
		}
		std::string stored;
		if (r() % 3 == 0) { stored.assign(r() % 3000, char('a' + r() % 26)); }
		w.add(docid, in, length, stored, r() % 4 == 0 ? r() >> 2 : r() % 9, keyed ? std::string_view(key) : std::string_view());
	}
	return w.flush(id);
}

struct result {
	std::string bytes;
	merge_plan plan;
	std::string error;  // the exception's type, when one was thrown
};

template <class Merge>
result run(Merge&& merge) {
	result out;
	try {
		string_output o(out.bytes);
		string_spill spill;
		out.plan = merge(o, spill);
	} catch (const std::exception& e) {
		out.error = typeid(e).name();
		out.bytes.clear();
	}
	return out;
}

void same(const result& a, const result& b, const char* what, std::uint64_t seed) {
	const bool ok = a.error == b.error && a.bytes == b.bytes && a.plan.doc_count == b.plan.doc_count && a.plan.ordinals == b.plan.ordinals;
	CHECK(ok);
	if (!ok) {
		std::fprintf(stderr, "  seed %llu, %s: %zu bytes (%s) against the oracle's %zu (%s)\n", (unsigned long long)seed, what, a.bytes.size(), a.error.c_str(),
			b.bytes.size(), b.error.c_str());
	}
}

}  // namespace

int main() {
	std::size_t compared = 0;
	for (std::uint64_t seed = 0; seed < 60; ++seed) {
		std::mt19937_64 r(seed);
		const std::size_t n = 1 + r() % 4;
		const bool keyed = r() % 4 != 0, deletes = r() % 3 != 0, wide = r() % 5 == 0;
		const std::uint8_t shift = r() % 3 == 0 ? format::min_region_shift + 2 : format::default_region_shift;
		// Disjoint docids across sources, interleaved; some sources large enough for skip blocks.
		std::vector<std::vector<std::uint64_t>> ids(n);
		std::uint64_t docid = 1 + r() % 50;
		const std::size_t total = 1 + r() % (seed % 4 == 0 ? 3000 : 400);
		for (std::size_t i = 0; i < total; ++i) {
			ids[r() % n].push_back(docid);
			docid += 1 + (r() % 8 == 0 ? r() % 1000 : 0);
		}
		std::vector<std::string> files;
		std::vector<std::vector<std::uint64_t>> deleted(n);
		for (std::size_t s = 0; s < n; ++s) {
			files.push_back(build(r, ids[s], keyed, wide, shift));
			deleted[s].assign((ids[s].size() + 63) / 64, 0);
			if (deletes) {
				for (std::size_t o = 0; o < ids[s].size(); ++o) {
					if (r() % 4 == 0) { deleted[s][o >> 6] |= std::uint64_t(1) << (o & 63); }
				}
			}
		}
		std::vector<segment> segs;
		segs.reserve(n);
		for (const auto& f : files) { segs.emplace_back(f); }
		std::vector<merge_source> sources;
		for (std::size_t s = 0; s < n; ++s) { sources.push_back({&segs[s], deletes ? &deleted[s] : nullptr}); }
		const std::string prefix = keyed && r() % 5 ? "Q" : "";

		// Docid order.
		auto both = [&](const char* what, std::span<const merge_source> src, std::span<const merge_ref> order, bool spill_records) {
			const auto mine = run([&](output& o, spill& sp) {
				string_scratch records;
				return merge_segments(src, id, o, sp, prefix, shift, nullptr, nullptr, order, spill_records ? &records : nullptr);
			});
			const auto theirs = run([&](output& o, spill& sp) {
				string_scratch records;
				return oracle::merge_segments(src, id, o, sp, prefix, shift, nullptr, nullptr, order, spill_records ? &records : nullptr);
			});
			same(mine, theirs, what, seed);
			++compared;
			return mine;
		};
		const auto docid_merge = both("docid order", sources, {}, false);
		(void)both("docid order, records given", sources, {}, true);
		CHECK(docid_merge.error.empty());

		// Reordered: a shuffle of the live documents.
		std::vector<merge_ref> order;
		for (std::uint32_t s = 0; s < n; ++s) {
			for (ordinal_t o = 0; o < segs[s].doc_count(); ++o) {
				if (sources[s].live(o)) { order.push_back({s, o}); }
			}
		}
		std::shuffle(order.begin(), order.end(), r);
		const auto permuted = both("reordered", sources, order, false);
		(void)both("reordered, records spilled", sources, order, true);

		// A single permuted source keeps its order; merged back into docid order.
		if (permuted.error.empty() && !permuted.bytes.empty()) {
			const segment ps(permuted.bytes);
			const merge_source one[] = {{&ps, nullptr}};
			(void)both("single permuted source", one, {}, false);
			(void)both("single permuted source, records spilled", one, {}, true);
			std::vector<std::uint64_t> dead((ps.doc_count() + 63) / 64, 0);
			for (ordinal_t o = 0; o < ps.doc_count(); o += 3) { dead[o >> 6] |= std::uint64_t(1) << (o & 63); }
			const merge_source one_deleted[] = {{&ps, &dead}};
			(void)both("single permuted source with deletes", one_deleted, {}, true);
		}

		// Bad orders: one missing, one repeated.
		if (order.size() > 1) {
			auto missing = order;
			missing.pop_back();
			(void)both("order missing a document", sources, missing, false);
			auto repeated = order;
			repeated.back() = repeated.front();
			(void)both("order repeating a document", sources, repeated, false);
		}
		// Two live copies of a docid.
		if (n == 1 && !ids[0].empty()) {
			const merge_source twice[] = {{&segs[0], nullptr}, {&segs[0], nullptr}};
			const auto dup = both("duplicate docids", twice, {}, false);
			CHECK(!dup.error.empty());
		}
	}
	std::printf("%zu merges compared against the oracle\n", compared);
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("merge_oracle_test: ok");
	return 0;
}
