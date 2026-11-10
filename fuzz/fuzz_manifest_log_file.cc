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

// libFuzzer target for the manifest log as a file (manifest-log D6, D11): journal's framing and
// tail rule, the caller's rules and the records together, through read_log. The log's g is the
// one block 0's header names, so mutations reach past the framing. Every input reads or throws
// corrupt_index, unsupported_index or generation_not_retained, never crashes or reads out of
// bounds. What reads is consistent: its end within the file, a fresh log of its state reads
// back as that state, and reading to its own generation gives the same state. `to` (from the
// input's last byte) reads every generation the log holds whole, damage past it reported, and
// none past them (D6.8).
#include "strata/core/manifest_log.hh"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using namespace strata::files;

void require(bool ok) {
	if (!ok) { std::abort(); }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	const std::string_view bytes(reinterpret_cast<const char*>(data), size);
	std::uint64_t g = 1;
	if (size >= 32) { std::memcpy(&g, data + 24, 8); }  // little-endian, as the framing writes it
	const std::uint64_t pick = size ? data[size - 1] : 0;
	auto not_retained = [&](std::uint64_t to) {
		try {
			(void)read_log(bytes, g, {}, to);
		} catch (const generation_not_retained& e) {
			return e.generation == to;
		}
		return false;
	};
	log_read lr;
	try {
		lr = read_log(bytes, g);
	} catch (const log_damage& e) {
		// Whole up to the damaged record: each generation before it reads with `to`.
		require(e.generation > g);
		const std::uint64_t to = g + pick % (e.generation - g);
		const log_read at = read_log(bytes, g, {}, to);
		require(at.state.generation == to && at.damaged == e.generation);
		require(not_retained(e.generation));
		return 0;
	} catch (const strata::corrupt_index&) {
		return 0;
	} catch (const strata::unsupported_index&) {
		return 0;
	}
	require(lr.end <= size && lr.state.generation == g + lr.edits);
	require(read_log(frame_snapshot(lr.state), lr.state.generation).state == lr.state);
	const log_read to = read_log(bytes, g, {}, lr.state.generation);
	require(to.state == lr.state && to.end == lr.end && to.edits == lr.edits);
	const std::uint64_t mid = g + pick % (lr.edits + 1);
	const log_read at = read_log(bytes, g, {}, mid);
	require(at.state.generation == mid && at.edits == mid - g && at.end <= lr.end && at.damaged == 0);
	require(not_retained(lr.state.generation + 1) && (g == 0 || not_retained(g - 1)));
	return 0;
}
