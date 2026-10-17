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

// sim_check HISTORY (simulator design D12): checks a history file (D9's format) for
// linearizability. Exits 0 when linearizable, 1 on a violation, 2 when inconclusive, and 3
// when the file cannot be read or is malformed.
#include "simulation/history.hh"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace kronuz::simulation;

int main(int argc, char** argv) {
	if (argc != 2) { std::fprintf(stderr, "usage: sim_check HISTORY\n"); return 3; }
	std::ifstream in(argv[1], std::ios::binary);
	if (!in) { std::fprintf(stderr, "sim_check: cannot open %s\n", argv[1]); return 3; }
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const auto parsed = check::read_history(text);
	if (!parsed.ok()) { std::fprintf(stderr, "%s:%zu: %s\n", argv[1], parsed.line, parsed.error.c_str()); return 3; }
	const auto r = check::check(parsed.h.ops);
	static constexpr const char* names[] = {"linearizable", "violation", "inconclusive"};
	std::printf("%s: %s; %llu operations, %llu keys, %llu segments, %llu search nodes",
		argv[1], names[std::size_t(r.result)], (unsigned long long)parsed.h.ops.size(), (unsigned long long)r.keys,
		(unsigned long long)r.segments, (unsigned long long)r.nodes);
	if (r.result != check::verdict::linearizable) { std::printf("; key %llu: %s", (unsigned long long)r.key, r.detail.c_str()); }
	std::printf("\n");
	return int(r.result);
}
