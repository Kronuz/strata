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
// equal; the reader rejects other versions, malformed records, and clients that overlap or
// continue after an unknown outcome, each naming its line.
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

const std::string header = "strata-history 1 simulated\n";

// The error and line of reading `body` after a valid header.
read_result after_header(const std::string& body) { return read_history(header + body); }
bool rejects(const std::string& text, std::size_t line) {
	const auto r = read_history(text);
	if (r.ok() || r.line != line || !r.h.ops.empty()) { std::fprintf(stderr, "accepted or wrong line (%zu, %s): %s", r.line, r.error.c_str(), text.c_str()); return false; }
	return true;
}

}  // namespace

int main() {
	// Round trip: every operation kind and outcome, conditional and not, both time bases.
	{
		history h;
		h.ops = {
			{42, kind::put, 1'000, 2'500, 9'001, {}, outcome::ok, 1, 7},
			{42, kind::get, 1'200, 1'900, 9'001, {}, outcome::ok, 1, 8},
			{42, kind::erase, 3'000, 3'500, 0, {}, outcome::ok, 0, 7},
			{42, kind::put, 4'000, 4'500, 9'002, 0, outcome::ok, 1, 7},
			{42, kind::put, 4'100, 4'400, 9'003, 0, outcome::conflict, 1, 8},
			{42, kind::erase, 5'000, 5'100, 0, 3, outcome::conflict, 1, 7},
			{43, kind::erase, 5'000, 5'100, 0, {}, outcome::missing, 0, 9},
			{43, kind::get, 6'000, 6'100, 0, {}, outcome::missing, 0, 9},
			{44, kind::put, 100, 0, 5, 2, outcome::unknown, 0, 10},
			{44, kind::get, 200, 0, 0, {}, outcome::unknown, 0, 11},
			{44, kind::erase, 300, 0, 0, {}, outcome::unknown, 0, 12},
			{std::uint64_t(-1), kind::get, std::uint64_t(-2), std::uint64_t(-1), std::uint64_t(-1), {}, outcome::ok, std::uint64_t(-1), std::uint64_t(-1)},
		};
		for (const auto base : {time_base::simulated, time_base::monotonic}) {
			h.base = base;
			const auto text = write_history(h);
			const auto r = read_history(text);
			CHECK(r.ok() && r.line == 0 && r.h.base == base && r.h.ops == h.ops);
			if (!r.ok()) { std::fprintf(stderr, "line %zu: %s\n%s", r.line, r.error.c_str(), text.c_str()); }
		}
		CHECK(write_history(h).find("\n7 42 put 1000 2500 ok 9001 - 1\n") != std::string::npos);
		CHECK(write_history(h).find("\n10 44 put 100 - unknown 5 2 0\n") != std::string::npos);
	}
	// Comments, records in any order, and an empty history.
	{
		const auto r = after_header("# client key op invoked returned outcome value expected version\n2 1 get 50 60 missing - - 0\n1 1 put 0 10 ok 3 - 1\n");
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
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1", 2));
	CHECK(rejects(header + "\n", 2));
	CHECK(rejects(header + "# " + std::string(255, 'x') + "\n", 2));
	CHECK(read_history(header + "#" + std::string(255, 'x') + "\n").ok());
	// Fields: nine, single spaces, decimal without sign or leading zeros, within 64 bits.
	CHECK(rejects(header + "1 1 put 0 10 ok 3 -\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 9\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 -  1\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1 \n", 2));
	CHECK(rejects(header + " 1 1 put 0 10 ok 3 - 1\n", 2));
	CHECK(rejects(header + "1 1 put 00 10 ok 3 - 1\n", 2));
	CHECK(rejects(header + "1 1 put +0 10 ok 3 - 1\n", 2));
	CHECK(rejects(header + "1 1 put -0 10 ok 3 - 1\n", 2));
	CHECK(rejects(header + "1 18446744073709551616 put 0 10 ok 3 - 1\n", 2));
	CHECK(read_history(header + "1 18446744073709551615 put 0 10 ok 3 - 1\n").ok());
	CHECK(rejects(header + "1 1 PUT 0 10 ok 3 - 1\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 done 3 - 1\n", 2));
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1\r\n", 2));
	// D9's field rules.
	CHECK(rejects(header + "1 1 put 0 - ok 3 - 1\n", 2));          // returned - only when unknown
	CHECK(rejects(header + "1 1 put 0 10 unknown 3 - 0\n", 2));    // and always then
	CHECK(rejects(header + "1 1 put 0 10 missing 3 - 0\n", 2));    // a put is never missing
	CHECK(rejects(header + "1 1 put 0 10 ok - - 1\n", 2));         // a put has a value
	CHECK(rejects(header + "1 1 erase 0 10 ok 3 - 0\n", 2));       // an erase has none
	CHECK(rejects(header + "1 1 get 0 10 conflict - - 1\n", 2));   // a get never conflicts
	CHECK(rejects(header + "1 1 get 0 10 ok 3 1 1\n", 2));         // nor has an expected version
	CHECK(rejects(header + "1 1 get 0 10 ok - - 1\n", 2));         // an ok get has a value
	CHECK(rejects(header + "1 1 get 0 10 missing 3 - 0\n", 2));    // a missing one does not
	CHECK(rejects(header + "1 1 erase 0 10 ok - - 2\n", 2));       // versions only where reported
	CHECK(rejects(header + "1 1 get 0 10 missing - - 1\n", 2));
	CHECK(rejects(header + "1 1 put 0 - unknown 3 - 1\n", 2));
	CHECK(read_history(header + "1 1 erase 0 10 conflict - 4 2\n").ok());
	// Clients are sequential, and an unknown outcome is a client's last operation; the error
	// names the later operation's line.
	CHECK(rejects(header + "1 1 put 0 10 ok 3 - 1\n1 1 get 5 20 ok 3 - 1\n", 3));
	CHECK(rejects(header + "1 1 get 20 30 ok 3 - 1\n1 1 put 0 25 ok 3 - 1\n", 2));
	CHECK(read_history(header + "1 1 put 0 10 ok 3 - 1\n1 1 get 10 20 ok 3 - 1\n").ok());  // touching is sequential
	CHECK(rejects(header + "1 1 put 0 - unknown 3 - 0\n1 1 get 50 60 missing - - 0\n", 3));
	CHECK(read_history(header + "1 1 put 0 - unknown 3 - 0\n2 1 get 50 60 missing - - 0\n").ok());
	// The reader feeds the checker.
	{
		const auto r = after_header("1 1 put 0 10 ok 3 - 1\n2 1 put 20 30 ok 4 - 2\n3 1 get 40 50 ok 3 - 1\n");
		CHECK(r.ok() && check::check(r.h.ops).result == verdict::violation);
	}
	std::printf("%s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
