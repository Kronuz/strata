// libFuzzer target for the index's files: manifests, live docs, and metadata must decode
// or throw corrupt_index, never crash or read out of bounds. data[0] picks the file kind;
// data[1] even decodes the rest raw, odd builds a valid file seeded by the input, applies
// (position, xor) pairs from the rest, and reseals its CRC so the changes reach the field
// checks. A file that decodes must survive re-encoding unchanged, and its dead ordinals
// strictly ascend.
#include "strata/files.hh"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace strata::files;

std::string valid(unsigned kind, std::uint64_t seed) {
	std::mt19937_64 r(seed);
	if (kind == 0) {
		manifest m;
		for (auto& c : m.uuid) { c = char(r()); }
		m.generation = 1 + r() % 50;
		m.next_segment = 40;
		m.next_live = 20;
		m.next_meta = 5;
		m.key_prefix = "Q";
		m.meta = {1 + r() % 4, r() % 1000, std::uint32_t(r())};
		m.applied = {r() % 9, r() % 3};
		for (std::uint64_t i = 0, n = r() % 5; i < n; ++i) {
			segment_ref s{1 + i * 7, 4096, std::uint32_t(r()), 1 + r() % 300};
			s.live_count = r() % 2 ? s.doc_count : r() % s.doc_count;
			s.live_length = s.live_count;
			if (s.live_count != s.doc_count) { s.live = {1 + i, 64, std::uint32_t(r())}; }
			m.doc_count += s.doc_count;
			m.total_length += s.doc_count;
			m.live_count += s.live_count;
			m.live_length += s.live_length;
			m.segments.push_back(s);
		}
		if (m.generation > 1) { m.pinned.push_back(m.generation - 1); }
		return encode(m);
	}
	if (kind == 1) {
		live_docs l{1 + r() % 9, 1 + r() % 9, r() % 400};
		if (r() % 8 == 0) {
			l.doc_count = ~std::uint64_t(0) - r() % 16;  // counts near 2^64, none deleted
		} else {
			for (std::uint64_t d = 0; d < l.doc_count; ++d) {
				if (r() % 7 == 0) { l.deleted.push_back(d); }
			}
		}
		l.live_count = l.doc_count - l.deleted.size();
		return encode(l);
	}
	metadata m{1 + r() % 9};
	for (std::uint64_t i = 0, n = r() % 6; i < n; ++i) { m.entries["k" + std::to_string(r() % 20)] = std::string(r() % 30, 'v'); }
	return encode(m);
}

void reseal(std::string& f) {
	if (f.size() < 4) { return; }
	const std::uint32_t crc = hashes::crc32c(f.data(), f.size() - 4);
	std::memcpy(f.data() + f.size() - 4, &crc, 4);
}

template <class T, class Decode>
void check(const std::string& bytes, Decode decode) {
	T first;
	try {
		first = decode(bytes);
	} catch (const strata::corrupt_index&) {
		return;
	}
	if (!(decode(encode(first)) == first)) { __builtin_trap(); }
	if constexpr (std::is_same_v<T, live_docs>) {
		for (std::size_t i = 1; i < first.deleted.size(); ++i) {
			if (first.deleted[i] <= first.deleted[i - 1]) { __builtin_trap(); }
		}
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 2) { return 0; }
	const unsigned kind = data[0] % 3;
	std::string bytes;
	if (data[1] % 2 == 0) {
		bytes.assign(reinterpret_cast<const char*>(data + 2), size - 2);
	} else {
		std::uint64_t seed = 0;
		for (std::size_t i = 2; i < std::min<std::size_t>(size, 10); ++i) { seed = seed * 131 + data[i]; }
		bytes = valid(kind, seed);
		for (std::size_t i = 10; i + 2 < size; i += 3) {
			const std::size_t at = (std::size_t(data[i]) << 8 | data[i + 1]) % bytes.size();
			bytes[at] = char(bytes[at] ^ (data[i + 2] | 1));
		}
		reseal(bytes);
	}
	switch (kind) {
	case 0: check<manifest>(bytes, decode_manifest); break;
	case 1: check<live_docs>(bytes, decode_live_docs); break;
	default: check<metadata>(bytes, decode_metadata); break;
	}
	return 0;
}
