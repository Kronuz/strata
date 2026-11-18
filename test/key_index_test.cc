// A segment whose key_index (the termdict's FST) is damaged and resealed: the lazy cursor
// from "" lands on a later key than the checked walk starts at (fuzz_stored's crash), so
// check() must reject the segment, and past check() the cursor and the walk agree.
#include "../fuzz/fuzz_stored.cc"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

int main() {
	// fuzz_stored's input 653f0000000000000000 2c7dc6: seed 0, key_index, one byte flipped.
	const std::uint8_t in[] = {0x65, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0, 0x2c, 0x7d, 0xc6};
	const std::string good = valid(0);
	std::string bytes = test::section_bytes(good, format::section::key_index);
	const std::size_t at = (std::size_t(in[10]) << 8 | in[11]) % bytes.size();
	bytes[at] = char(bytes[at] ^ (in[12] | 1));
	const std::string bad = test::reseal(good, {{format::section::key_index, std::move(bytes)}});
	bool rejected = false;
	try {
		const segment s(bad);
		s.verify_all();
		basic_check_report<document_layout> r;
		check_segment(s, nullptr, r);
	} catch (const corrupt_segment& e) {
		std::fprintf(stderr, "rejected: %s\n", e.what());
		rejected = true;
	}
	CHECK(rejected);
	CHECK(LLVMFuzzerTestOneInput(in, sizeof in) == 0);  // traps if cursor and walk disagree past check()
	return failures ? 1 : 0;
}
