// libFuzzer target for the root's record and command decoders (contract strata:placement-records,
// pre-release bytes under D30). The first byte picks the decoder, the rest is its input: it
// decodes or throws corrupt_record, never crashing, reading out of bounds or allocating past
// its input, and whatever decodes encodes back to the same bytes, so every record has one
// encoding. Its corpus (root_corpus/) replays as a test in every build.
#include "strata/placement/records.hh"

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

namespace sp = strata::placement;

void require(bool ok) {
	if (!ok) { std::abort(); }
}

template <class D>
void round_trip(D decode, std::string_view in) {
	try {
		require(sp::encode(decode(in)) == in);
	} catch (const sp::corrupt_record&) {}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size == 0) { return 0; }
	const std::string_view in(reinterpret_cast<const char*>(data) + 1, size - 1);
	switch (data[0] % 6) {
	case 0: round_trip(sp::decode_directory, in); break;
	case 1: round_trip(sp::decode_groups, in); break;
	case 2: round_trip(sp::decode_node, in); break;
	case 3: round_trip(sp::decode_load, in); break;
	case 4: round_trip(sp::decode_namespace, in); break;
	default:
		try {
			const auto c = sp::decode_command(in);
			require(sp::encode(c) == in);
			for (const auto& w : c.writes) { require(w.kind == sp::write::erase || sp::valid_record(w.key, w.value)); }
		} catch (const sp::corrupt_record&) {}
		break;
	}
	return 0;
}
