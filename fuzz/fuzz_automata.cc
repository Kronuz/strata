// libFuzzer target for the automata (docs/design/automata.md, T3): an automaton and a term
// set from the input, matched through intersect_sorted, the matcher key by key, and a
// segment's dictionary, must equal brute force. data[0] picks the automaton (prefix, glob,
// Levenshtein) and data[1] its parameters; then a length byte and the pattern; then terms,
// each a length byte and its bytes. Any mismatch traps.
#include "strata/expand.hh"
#include "strata/writer.hh"

#include "../test/automata_reference.hh"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

namespace automata = strata::automata;

#define FUZZ_CHECK(cond) \
	do { \
		if (!(cond)) { __builtin_trap(); } \
	} while (0)

template <class A, class Ref>
void check(const A& a, const std::vector<std::string>& terms, const Ref& ref, bool segment) {
	std::vector<std::string> want, got;
	for (const auto& t : terms) {
		if (ref(t)) { want.push_back(t); }
	}
	automata::intersect_sorted(a, terms.begin(), terms.end(), [&](std::string_view k) {
		got.emplace_back(k);
		return true;
	});
	FUZZ_CHECK(got == want);
	if constexpr (!A::byte_range) {
		automata::matcher<A> m(a);
		for (const auto& t : terms) {
			const auto r = m(t);
			FUZZ_CHECK(r.match == ref(t));
			FUZZ_CHECK(!r.dead || (!r.match && r.dead <= t.size()));
		}
	}
	if (segment && !terms.empty()) {
		strata::segment_writer w;
		std::uint64_t docid = 1;
		for (const auto& t : terms) {
			const strata::posting_input in{t, 1, {}};
			w.add(docid++, std::span<const strata::posting_input>(&in, 1), 1);
		}
		const std::string file = w.flush({});
		const strata::segment seg(file);
		got.clear();
		strata::intersect(seg, a, [&](std::string_view k, const strata::format::term_info& info) {
			FUZZ_CHECK(info.df == 1);
			got.emplace_back(k);
			return true;
		}, nullptr, 1 + terms.size() % 64);
		FUZZ_CHECK(got == want);
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 3) { return 0; }
	const unsigned kind = data[0] % 3, params = data[1];
	std::size_t at = 2;
	const std::size_t plen = std::min<std::size_t>(data[at], size - at - 1);
	++at;
	const std::string pattern(reinterpret_cast<const char*>(data + at), plen);
	at += plen;
	std::set<std::string> set;
	while (at < size) {
		const std::size_t n = std::min<std::size_t>(data[at], size - at - 1);
		++at;
		set.emplace(reinterpret_cast<const char*>(data + at), n);
		at += n;
	}
	// The writer takes no empty term; the sorted walk does.
	const bool segment = (params & 0x80) && !set.count("");
	const std::vector<std::string> terms(set.begin(), set.end());
	if (kind == 0) {
		check(automata::prefix(pattern), terms, [&](std::string_view k) { return reference::prefix(pattern, k); }, segment);
	} else if (kind == 1) {
		const unsigned flags = params & 3;
		check(automata::glob(pattern, flags), terms, [&](std::string_view k) { return reference::glob(pattern, flags, k); }, segment);
	} else {
		const std::uint64_t k = params & 3;
		const std::size_t fixed = (params >> 2) & 7;
		const bool swaps = params & 0x20;
		check(automata::levenshtein(pattern, k, fixed, swaps), terms, [&](std::string_view key) { return reference::levenshtein(pattern, k, fixed, swaps, key); }, segment);
	}
	return 0;
}
