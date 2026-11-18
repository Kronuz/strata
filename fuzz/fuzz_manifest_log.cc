// libFuzzer target for the manifest log's records (manifest-log D3): a log as [length LEB128]
// [record]..., a snapshot first, then edits replayed onto it. Every record must decode or throw
// corrupt_index or unsupported_index, never crash or read out of bounds. What decodes must
// decode the same after re-encoding; a replay that throws leaves the state as it was; every
// state replay reaches is a snapshot the decoder accepts back unchanged, and its tracked size
// is its encoding's (C7).
#include "strata/core/manifest_log.hh"

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

using namespace strata::files;

void require(bool ok) {
	if (!ok) { std::abort(); }
}

bool next_record(std::string_view& in, std::string_view& record) {
	const char* p = in.data();
	std::uint64_t n = 0;
	if (!varint::leb128::decode(&p, in.data() + in.size(), n) || n > std::uint64_t(in.data() + in.size() - p)) { return false; }
	record = std::string_view(p, n);
	in.remove_prefix(std::size_t(p - in.data()) + n);
	return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	std::string_view in(reinterpret_cast<const char*>(data), size), record;
	if (!next_record(in, record)) { return 0; }
	log_state state;
	try {
		state = decode_snapshot(record);
	} catch (const strata::corrupt_index&) {
		return 0;
	} catch (const strata::unsupported_index&) {
		return 0;
	}
	require(decode_snapshot(encode_snapshot(state)) == state);
	require(snapshot_size(state) == encode_snapshot(state).size());
	while (next_record(in, record)) {
		log_edit edit;
		try {
			edit = decode_edit(record);
		} catch (const strata::corrupt_index&) {
			continue;
		} catch (const strata::unsupported_index&) {
			continue;
		}
		require(decode_edit(encode_edit(edit)) == edit);
		const log_state before = state;
		try {
			apply(state, edit);
		} catch (const strata::corrupt_index&) {
			require(state == before);
			continue;
		}
		const std::string snapshot = encode_snapshot(state);
		require(snapshot_size(state) == snapshot.size());
		require(decode_snapshot(snapshot) == state);
	}
	return 0;
}
