// libFuzzer target for the command codec (standalone-turn-log D3): a payload decodes as a batch
// whose transactions decode, as a checkpoint, or as a chunk (rocksdb-parity A3) whose slice decodes
// as a transaction when it is one whole, or it throws corrupt_index or unsupported_index, never
// crashing or reading out of bounds. Whatever decodes encodes back to the same bytes.
#include "strata/core/command.hh"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

void require(bool ok) {
	if (!ok) { std::abort(); }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	namespace command = strata::command;
	const std::string_view in(reinterpret_cast<const char*>(data), size);
	try {
		const std::uint8_t kind = command::kind_of(in);
		if (kind == command::checkpoint) {
			require(command::encode_checkpoint(command::decode_checkpoint(in)) == in);
			return 0;
		}
		if (kind == command::chunk) {
			const command::chunk_record c = command::decode_chunk(in);
			require(command::chunk_payload(c.place, c.bytes) == in);
			try {
				require(command::encode(command::decode<strata::document_layout>(c.bytes)) == c.bytes);
			} catch (const strata::corrupt_index&) {
			}
			return 0;
		}
		const auto transactions = command::decode_batch(in);
		std::string body;
		for (const std::string_view t : transactions) {
			command::add_transaction(body, t);
			try {
				require(command::encode(command::decode<strata::document_layout>(t)) == t);
			} catch (const strata::corrupt_index&) {
			}
		}
		require(command::batch_payload(transactions.size(), body) == in);
	} catch (const strata::corrupt_index&) {
	} catch (const strata::unsupported_index&) {
	}
	return 0;
}
