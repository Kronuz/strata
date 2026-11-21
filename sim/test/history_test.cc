/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The history file, version 1 (simulator design D9, gate row T3): written histories read back
// equal; the reader rejects other versions, malformed records, malformed batches, and clients
// that overlap or continue after an unknown outcome, each naming its line.
#include "simulation/history.hh"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } } while (0)

using namespace kronuz::simulation;
using namespace kronuz::simulation::check;

namespace {

using C = consistency;
const std::string header = "strata-history 1 simulated\n";

read_result after_header(const std::string& body) { return read_history(header + body); }
bool accepts(const std::string& body) {
	const auto r = after_header(body);
	if (!r.ok()) { std::fprintf(stderr, "rejected at line %zu (%s): %s", r.line, r.error.c_str(), body.c_str()); }
	return r.ok();
}
bool rejects(const std::string& text, std::size_t line) {
	const auto r = read_history(text);
	if (r.ok() || r.line != line || !r.h.ops.empty()) { std::fprintf(stderr, "accepted or wrong line (%zu, %s): %s", r.line, r.error.c_str(), text.c_str()); return false; }
	return true;
}

}  // namespace

int main() {
	// Round trip: every operation kind, outcome, and mode, conditional and not, batches, both
	// time bases.
	{
		const std::uint64_t max = std::uint64_t(-1);
		history h;
		h.ops = {
			{42, kind::put, 1'000, 2'500, 9'001, {}, outcome::ok, 1, 7, 0, C::strong},
			{42, kind::get, 1'200, 1'900, 9'001, {}, outcome::ok, 1, 8, 0, C::strong},
			{42, kind::erase, 3'000, 3'500, 0, {}, outcome::ok, 0, 7, 0, C::all},
			{42, kind::put, 4'000, 4'500, 9'002, 0, outcome::ok, 1, 7, 0, C::local},
			{42, kind::put, 4'100, 4'400, 9'003, 0, outcome::conflict, 1, 8, 0, C::none},
			{42, kind::erase, 5'000, 5'100, 0, 3, outcome::conflict, 1, 7, 0, C::strong},
			{43, kind::erase, 5'000, 5'100, 0, {}, outcome::missing, 0, 9, 0, C::strong},
			{43, kind::get, 6'000, 6'100, 0, {}, outcome::missing, 0, 9, 0, C::session},
			{43, kind::get, 7'000, 7'100, 0, {}, outcome::missing, 0, 9, 0, C::nearest},
			{44, kind::put, 100, 0, 5, 2, outcome::unknown, 0, 10, 0, C::strong},
			{44, kind::get, 200, 0, 0, {}, outcome::unknown, 0, 11, 0, C::strong},
			{44, kind::erase, 300, 0, 0, {}, outcome::unknown, 0, 12, 0, C::strong},
			{45, kind::put, 8'000, 8'200, 6, {}, outcome::ok, 1, 13, 77, C::strong},
			{46, kind::erase, 8'000, 8'200, 0, {}, outcome::missing, 0, 13, 77, C::strong},
			{45, kind::erase_range, 8'000, 8'200, 50, {}, outcome::ok, 1, 13, 77, C::strong},
			{45, kind::put, 9'000, 9'200, 7, 0, outcome::conflict, 1, 13, 78, C::strong},
			{46, kind::put, 9'000, 9'200, 8, {}, outcome::aborted, 0, 13, 78, C::strong},
			{max, kind::get, max - 1, max, max, {}, outcome::ok, max, max, 0, C::strong},
		};
		for (const auto base : {time_base::simulated, time_base::monotonic}) {
			h.base = base;
			const auto text = write_history(h);
			const auto r = read_history(text);
			CHECK(r.ok() && r.line == 0 && r.h.base == base && r.h.ops == h.ops);
			if (!r.ok()) { std::fprintf(stderr, "line %zu: %s\n%s", r.line, r.error.c_str(), text.c_str()); }
		}
		const auto text = write_history(h);
		CHECK(text.find("\n7 42 put 1000 2500 ok 9001 - 1 0 quorum\n") != std::string::npos);
		CHECK(text.find("\n10 44 put 100 - unknown 5 2 0 0 quorum\n") != std::string::npos);
		CHECK(text.find("\n9 43 get 7000 7100 missing - - 0 0 nearest\n") != std::string::npos);
		CHECK(text.find("\n13 45 erase_range 8000 8200 ok 50 - 1 77 quorum\n") != std::string::npos);
	}
	// Comments, records in any order, and an empty history.
	{
		const auto r = after_header("# client key op invoked returned outcome value expected version batch mode\n2 1 get 50 60 missing - - 0 0 linearizable\n1 1 put 0 10 ok 3 - 1 0 quorum\n");
		CHECK(r.ok() && r.h.ops.size() == 2 && r.h.ops[0].client == 2 && r.h.ops[1].value == 3);
		CHECK(read_history(header).ok() && read_history(header).h.ops.empty());
		CHECK(read_history("strata-history 1 monotonic\n").h.base == time_base::monotonic);
	}
	// The header: another version, another base, another format.
	CHECK(rejects("", 1));
	CHECK(rejects("strata-history 2 simulated\n", 1));
	CHECK(rejects("strata-history 01 simulated\n", 1));
	CHECK(rejects("strata-history 1 wall\n", 1));
	CHECK(rejects("strata-history 1\n", 1));
	CHECK(rejects("strata-history 1 simulated \n", 1));
	CHECK(rejects("strata-histories 1 simulated\n", 1));
	CHECK(rejects("# strata-history 1 simulated\n", 1));
	// Lines: newline-terminated, at most 256 bytes, no blank lines.
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 quorum", 2));
	CHECK(rejects(header + "\n", 2));
	CHECK(rejects(header + "# " + std::string(255, 'x') + "\n", 2));
	CHECK(read_history(header + "#" + std::string(255, 'x') + "\n").ok());
	// Fields: eleven, single spaces, decimal without sign or leading zeros, within 64 bits.
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 quorum 9\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1  0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 quorum \n", 2));
	CHECK(rejects(header + " 1 1 put 0 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 00 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put +0 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put -0 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 18446744073709551616 put 0 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(accepts("1 18446744073709551615 put 0 10 ok 3 - 1 0 quorum\n"));
	CHECK(rejects(header + "1 1 PUT 0 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 done 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 quorum\r\n", 2));
	// Modes: a read's for gets, an acknowledgement for writes.
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 linearizable\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 session\n", 2));
	CHECK(rejects(header + "1 1 get 0 10 ok 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 get 0 10 ok 3 - 1 0 local\n", 2));
	CHECK(rejects(header + "1 1 get 0 10 ok 3 - 1 0 -\n", 2));
	CHECK(accepts("1 1 put 0 10 ok 3 - 1 0 none\n2 1 get 0 10 ok 3 - 1 0 nearest\n"));
	// D9's field rules.
	CHECK(rejects(header + "1 1 put 0 - ok 3 - 1 0 quorum\n", 2));          // returned - only when unknown
	CHECK(rejects(header + "1 1 put 0 10 unknown 3 - 0 0 quorum\n", 2));    // and always then
	CHECK(rejects(header + "1 1 put 0 10 missing 3 - 0 0 quorum\n", 2));    // a put is never missing
	CHECK(rejects(header + "1 1 put 0 10 ok - - 1 0 quorum\n", 2));         // a put has a value
	CHECK(rejects(header + "1 1 erase 0 10 ok 3 - 0 0 quorum\n", 2));       // an erase has none
	CHECK(rejects(header + "1 1 get 0 10 conflict - - 1 0 linearizable\n", 2));  // a get never conflicts
	CHECK(rejects(header + "1 1 get 0 10 ok 3 1 1 0 linearizable\n", 2));   // nor has an expected version
	CHECK(rejects(header + "1 1 get 0 10 ok - - 1 0 linearizable\n", 2));   // an ok get has a value
	CHECK(rejects(header + "1 1 get 0 10 missing 3 - 0 0 linearizable\n", 2));  // a missing one does not
	CHECK(rejects(header + "1 1 get 0 10 ok 3 - 1 5 linearizable\n", 2));   // nor is it in a batch
	CHECK(rejects(header + "1 1 erase 0 10 ok - - 2 0 quorum\n", 2));       // versions only where reported
	CHECK(rejects(header + "1 1 get 0 10 missing - - 1 0 linearizable\n", 2));
	CHECK(rejects(header + "1 1 put 0 - unknown 3 - 1 0 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 aborted 3 - 0 0 quorum\n", 2));    // aborts only in a batch
	CHECK(accepts("1 1 erase 0 10 conflict - 4 2 0 quorum\n"));
	CHECK(accepts("1 3 erase_range 0 10 ok 9 - 4 0 quorum\n"));
	CHECK(rejects(header + "1 3 erase_range 0 10 ok 3 - 4 0 quorum\n", 2));  // an end above its key
	CHECK(rejects(header + "1 3 erase_range 0 10 ok - - 4 0 quorum\n", 2));
	CHECK(rejects(header + "1 3 erase_range 0 10 ok 9 1 4 0 quorum\n", 2));  // no expected version
	CHECK(rejects(header + "1 3 erase_range 0 10 conflict 9 - 4 0 quorum\n", 2));
	CHECK(rejects(header + "1 3 erase_range 0 10 missing 9 - 0 0 quorum\n", 2));
	// Batches: contiguous records with one client, times, and acknowledgement; all accepted,
	// one conflict and the rest aborted, or all unknown. A batch is one client operation.
	CHECK(accepts("1 1 put 0 10 ok 3 - 1 7 quorum\n1 2 put 0 10 ok 4 - 1 7 quorum\n1 1 get 20 30 ok 3 - 1 0 linearizable\n"));
	CHECK(accepts("1 1 put 0 10 aborted 3 - 0 7 quorum\n1 2 put 0 10 conflict 4 5 1 7 quorum\n"));
	CHECK(accepts("1 1 put 0 - unknown 3 - 0 7 quorum\n1 2 put 0 - unknown 4 - 0 7 quorum\n"));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 7 quorum\n2 5 put 0 10 ok 3 - 1 0 quorum\n1 2 put 0 10 ok 4 - 1 7 quorum\n", 4));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 7 quorum\n2 2 put 0 10 ok 4 - 1 7 quorum\n", 3));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 7 quorum\n1 2 put 0 11 ok 4 - 1 7 quorum\n", 3));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 7 quorum\n1 2 put 0 10 ok 4 - 1 7 local\n", 3));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 7 quorum\n1 2 put 0 10 aborted 4 - 0 7 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 conflict 3 5 1 7 quorum\n1 2 put 0 10 conflict 4 5 1 7 quorum\n", 2));
	CHECK(rejects(header + "1 1 put 0 - unknown 3 - 0 7 quorum\n1 2 put 0 10 ok 4 - 1 7 quorum\n", 3));  // times differ first
	// Clients are sequential, and an unknown outcome is a client's last operation; the error
	// names the later operation's line.
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 0 quorum\n1 1 get 5 20 ok 3 - 1 0 linearizable\n", 3));
	CHECK(rejects(header + "1 1 get 20 30 ok 3 - 1 0 linearizable\n1 1 put 0 25 ok 3 - 1 0 quorum\n", 2));
	CHECK(accepts("1 1 put 0 10 ok 3 - 1 0 quorum\n1 1 get 10 20 ok 3 - 1 0 linearizable\n"));  // touching is sequential
	CHECK(rejects(header + "1 1 put 0 - unknown 3 - 0 0 quorum\n1 1 get 50 60 missing - - 0 0 linearizable\n", 3));
	CHECK(accepts("1 1 put 0 - unknown 3 - 0 0 quorum\n2 1 get 50 60 missing - - 0 0 linearizable\n"));
	// The reader feeds the checker.
	{
		const auto r = after_header("1 1 put 0 10 ok 3 - 1 0 quorum\n2 1 put 20 30 ok 4 - 2 0 quorum\n3 1 get 40 50 ok 3 - 1 0 linearizable\n");
		CHECK(r.ok() && check::check(r.h.ops).result == verdict::violation);
		const auto torn = after_header("1 1 put 0 10 ok 3 - 1 7 quorum\n1 2 put 0 10 ok 4 - 1 7 quorum\n2 1 get 20 30 ok 3 - 1 0 linearizable\n2 2 get 40 50 missing - - 0 0 linearizable\n");
		CHECK(torn.ok() && check::check(torn.h.ops).result == verdict::violation);
	}
	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
