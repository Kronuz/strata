// D18 check 5's per-term rule (durability-modes revision 5; DMS-16, DM-R35 item 3): a term's
// dictionary value is rebuilt from the walk, never re-encoded from its own decoding.
//
// The value (format.hh:442-455, 562-597) carries df, cf, the flags no_wdf, no_positions, and
// counts_are_wdf, a single posting's ordinal and wdf, the postings and positions offsets and
// lengths, and for df >= 128 the whole-list competitive pairs term_scorer takes as its bound
// (search.hh:315-317). Decoding a planted value and encoding it again returns the planted bytes
// (the security reviewer's impacts_roundtrip.cc). Here the verifier builds the term_info from
// what the walk decoded, as the writer builds it (writer.hh:302-333), encodes it with
// encode_term_info, and compares bytes: the honest value passes, and each planted field fails.
//
// Build (headers at strata's pins): clang++ -std=c++20 -O1 -Wall -Wextra -Iinclude
//   -I<termdict> -I<fst> -I<varint> -I<hashes> term_value_check.cc -o term_value_check
#include "strata/format.hh"

#include <cstdio>
#include <string>
#include <vector>

using namespace strata;

namespace {

struct walked_posting {
	std::uint32_t ord, wdf, positions;  // positions: the count the walk decoded
	std::uint8_t norm;                  // the ordinal's norm byte, which check 5 verified
};

// What the walk saw for one term: its postings, and the bytes its decoders consumed.
struct walked_term {
	std::vector<walked_posting> postings;
	std::uint64_t postings_at = 0, postings_length = 0, positions_at = 0, positions_length = 0;
};

// The writer's term_info, from the walk alone (writer.hh:302-333).
format::term_info from_walk(const walked_term& w) {
	format::term_info t;
	t.df = std::uint32_t(w.postings.size());
	bool all_zero = true, any_positions = false, counts_are_wdf = true;
	std::vector<format::impact> pairs;
	for (const auto& p : w.postings) {
		t.cf += p.wdf;
		all_zero = all_zero && p.wdf == 0;
		any_positions = any_positions || p.positions != 0;
		counts_are_wdf = counts_are_wdf && p.positions == p.wdf;
		pairs.push_back({p.wdf, p.norm});
	}
	if (all_zero) { t.flags |= format::no_wdf; }
	if (!any_positions) { t.flags |= format::no_positions; }
	if (counts_are_wdf) { t.flags |= format::counts_are_wdf; }
	if (t.df >= format::block) { t.impacts = format::competitive(pairs); }
	if (any_positions) {
		t.positions = w.positions_at;
		t.positions_length = w.positions_length;
	}
	if (t.df == 1) {
		t.flags |= format::single_posting;
		t.postings = w.postings[0].ord;
		t.single_wdf = w.postings[0].wdf;
	} else {
		t.postings = w.postings_at;
		t.postings_length = w.postings_length;
	}
	return t;
}

std::string encode(const format::term_info& t) {
	format::term_run run;
	return format::encode_term_info(t, run, true);
}

// Check 5 as revision 4 wrote it: decode the received value, encode it again.
bool roundtrip_check(const std::string& received) {
	try {
		format::term_run run;
		const auto t = format::decode_term_info(received, run, true);
		return encode(t) == received;
	} catch (const std::exception&) {
		return false;  // a value the reader refuses
	}
}

// Check 5 as revision 5 writes it: the value rebuilt from the walk.
bool walk_check(const std::string& received, const walked_term& w) { return encode(from_walk(w)) == received; }

int failures = 0;

void row(const char* name, const std::string& received, const walked_term& w, bool honest) {
	const bool r4 = roundtrip_check(received), r5 = walk_check(received, w);
	const bool ok = r5 == honest;
	if (!ok) { ++failures; }
	std::printf("%-58s revision 4 %-6s revision 5 %-6s %s\n", name, r4 ? "pass" : "FAIL", r5 ? "pass" : "FAIL", ok ? "as expected" : "UNEXPECTED");
}

}  // namespace

int main() {
	// A term with skip blocks: 200 postings, longer documents holding more of it, so the
	// whole-list frontier has several pairs.
	walked_term big;
	for (std::uint32_t i = 0; i < 200; ++i) { big.postings.push_back({i * 3, 1 + i % 7, 1 + i % 7, std::uint8_t(20 + 2 * (i % 7) + i % 2)}); }
	big.postings_at = 4096;
	big.postings_length = 1234;
	big.positions_at = 8192;
	big.positions_length = 2345;
	const format::term_info honest = from_walk(big);
	const std::string honest_bytes = encode(honest);

	// A term below the block size whose counts differ from its wdf, and a single posting.
	walked_term small;
	for (std::uint32_t i = 0; i < 9; ++i) { small.postings.push_back({i * 5, 2, 1, std::uint8_t(30)}); }
	small.postings_at = 100;
	small.postings_length = 40;
	small.positions_at = 200;
	small.positions_length = 30;
	walked_term single;
	single.postings.push_back({17, 3, 3, std::uint8_t(12)});
	single.positions_at = 300;
	single.positions_length = 4;

	std::printf("A term's dictionary value, received against the walk (df %u, cf %llu, %zu whole-list pairs)\n", honest.df,
		(unsigned long long)honest.cf, honest.impacts.size());
	row("honest, df 200", honest_bytes, big, true);
	row("honest, df 9, counts differ from wdf", encode(from_walk(small)), small, true);
	row("honest, single posting", encode(from_walk(single)), single, true);

	auto planted = [&](const char* name, auto&& change, const walked_term& w) {
		format::term_info t = from_walk(w);
		change(t);
		row(name, encode(t), w, false);
	};
	planted("whole-list pairs lowered to (wdf 1, norm 20)", [](format::term_info& t) { t.impacts = {{1, 20}}; }, big);
	planted("whole-list pairs: the top pair dropped", [](format::term_info& t) { t.impacts.erase(t.impacts.begin()); }, big);
	planted("cf planted (999999)", [](format::term_info& t) { t.cf = 999999; }, big);
	planted("counts_are_wdf set where counts differ", [](format::term_info& t) { t.flags |= format::counts_are_wdf; }, small);
	planted("counts_are_wdf cleared where counts equal wdf", [](format::term_info& t) { t.flags &= std::uint8_t(~format::counts_are_wdf); }, big);
	planted("no_wdf set over nonzero wdf", [](format::term_info& t) { t.flags |= format::no_wdf; }, small);
	planted("df raised by one", [](format::term_info& t) { ++t.df; }, big);
	planted("single posting's wdf raised", [](format::term_info& t) { t.single_wdf = 9; t.cf = 9; }, single);
	planted("single posting's ordinal moved", [](format::term_info& t) { t.postings = 18; }, single);
	planted("postings length longer than the walk consumed", [](format::term_info& t) { t.postings_length += 16; }, big);
	planted("positions offset moved", [](format::term_info& t) { t.positions += 8; }, big);
	std::printf("%s\n", failures ? "RESULT: unexpected outcomes" : "RESULT: the honest values pass and every planted field fails the rebuilt value; the round trip passes every plant (revision 4's df rule caught df alone)");
	return failures ? 1 : 0;
}
