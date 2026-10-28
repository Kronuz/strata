// The field table's file (fields-and-keys D2): any bytes decode to a table or are refused, and a
// decoded table encodes back to a file that decodes to the same table and chain. The first byte
// picks the shape: raw bytes as the file, or the rest framed as one block, or as two blocks (a
// split at the second byte), the framing sealed with the CRC so the fuzzer reaches the declarations.
#include "strata/core/fields.hh"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace strata;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size == 0) { return 0; }
	const unsigned mode = data[0] % 3;
	std::string_view rest(reinterpret_cast<const char*>(data) + 1, size - 1);
	std::string file;
	if (mode == 0) {
		file.assign(rest);
	} else {
		file = files::fields_header();
		std::size_t cut = rest.size();
		if (mode == 2 && !rest.empty()) { cut = std::uint8_t(rest[0]) % (rest.size() + 1); }
		for (const std::string_view part : {rest.substr(0, cut), rest.substr(cut)}) {
			if (part.empty()) { continue; }
			std::string block(part);
			files::detail::seal(block);
			file += block;
		}
	}
	field_table t;
	try {
		files::decode_fields(file, t);
	} catch (const corrupt_index&) {
		return 0;
	} catch (const unsupported_index&) {
		return 0;
	}
	field_table back;
	files::decode_fields(files::encode_fields(t), back);
	if (!(back == t) || !(back.mark() == t.mark())) { std::abort(); }
	return 0;
}
