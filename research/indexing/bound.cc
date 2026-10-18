// The hardware bound for indexing (docs/research/indexing.md, "The bound"): the per-stage
// cost of turning the corpus sample into postings, each stage alone, on one thread, over
// five interleaved trials. Stages are the minimal work the pipeline needs (read, find the
// fields, split tokens, hash, resolve terms, append postings, encode, write) in both the
// current shape (one big term table, LEB128 streams) and a cache-resident shape (radix
// partition by term hash, per-partition tables, counting sort). Nothing here is the
// engine; the numbers bound what any design can reach.
//
//   research_bound CORPUS.json [trials=5]

#include "bitpack.hh"
#include "hashes.hh"
#include "varint.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {

using clock_type = std::chrono::steady_clock;

struct doc_span {
	std::uint32_t id_off, id_len, text_off, text_len;
};

struct corpus {
	std::string file;
	std::vector<doc_span> docs;
	std::uint64_t text_bytes = 0, tokens = 0;
};

// Finds the "id" and "text" string values of each JSON line. The corpus's strings hold
// no escaped quotes, so the value ends at the next quote.
corpus load(const char* path) {
	corpus c;
	std::ifstream in(path, std::ios::binary);
	if (!in) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
	in.seekg(0, std::ios::end);
	c.file.resize(std::size_t(in.tellg()));
	in.seekg(0);
	in.read(c.file.data(), std::streamsize(c.file.size()));
	std::string_view all(c.file);
	for (std::size_t at = 0; at < all.size();) {
		std::size_t nl = all.find('\n', at);
		if (nl == std::string_view::npos) { nl = all.size(); }
		std::string_view line = all.substr(at, nl - at);
		doc_span d{};
		if (auto i = line.find("\"id\": \""); i != std::string_view::npos) {
			i += 7;
			d.id_off = std::uint32_t(at + i);
			d.id_len = std::uint32_t(line.find('"', i) - i);
		}
		if (auto t = line.find("\"text\": \""); t != std::string_view::npos) {
			t += 9;
			d.text_off = std::uint32_t(at + t);
			d.text_len = std::uint32_t(line.find('"', t) - t);
		}
		if (d.text_len) {
			c.docs.push_back(d);
			c.text_bytes += d.text_len;
		}
		at = nl + 1;
	}
	return c;
}

inline std::string_view text_of(const corpus& c, const doc_span& d) { return std::string_view(c.file).substr(d.text_off, d.text_len); }

// Calls f(token) for each space-separated token of text.
template <class F>
inline void for_tokens(std::string_view text, F&& f) {
	const char* p = text.data();
	const char* end = p + text.size();
	while (p < end) {
		const char* q = static_cast<const char*>(std::memchr(p, ' ', std::size_t(end - p)));
		if (!q) { q = end; }
		if (q > p) { f(std::string_view(p, std::size_t(q - p))); }
		p = q + 1;
	}
}

// --- the big term table (strata's buffer_for shape: hash, pointer into an arena, length) ---

struct big_table {
	struct slot {
		std::uint64_t hash = 0;
		const char* key = nullptr;
		std::uint32_t length = 0;
		std::uint32_t id = 0;
	};
	std::vector<slot> slots;
	std::deque<std::string> arena;
	std::uint32_t count = 0;

	explicit big_table(std::size_t capacity) : slots(std::bit_ceil(capacity * 2)) {}

	// Returns the term's id, inserting it when absent.
	std::uint32_t intern(std::string_view term, std::uint64_t h) {
		const std::size_t mask = slots.size() - 1;
		for (std::size_t i = h & mask;; i = (i + 1) & mask) {
			slot& s = slots[i];
			if (!s.key) {
				if (arena.empty() || arena.back().size() + term.size() > arena.back().capacity()) {
					arena.emplace_back();
					arena.back().reserve(1 << 20);
				}
				std::string& chunk = arena.back();
				const char* at = chunk.data() + chunk.size();
				chunk.append(term);
				s = {h, at, std::uint32_t(term.size()), count++};
				return s.id;
			}
			if (s.hash == h && s.length == term.size() && std::memcmp(s.key, term.data(), term.size()) == 0) { return s.id; }
		}
	}
	// Finds a present term's id (every token has been interned).
	std::uint32_t find(std::string_view term, std::uint64_t h) const {
		const std::size_t mask = slots.size() - 1;
		for (std::size_t i = h & mask;; i = (i + 1) & mask) {
			const slot& s = slots[i];
			if (s.hash == h && s.length == term.size() && std::memcmp(s.key, term.data(), term.size()) == 0) { return s.id; }
			if (!s.key) { return ~0u; }
		}
	}
	const slot* slot_of(std::uint64_t h) const { return &slots[h & (slots.size() - 1)]; }
};

// --- the token stream as tuples: (term hash, ordinal, position) -----------------------------

// 20 bytes: the term's hash, the document's ordinal, the token's text offset (so a term's
// identity is verified against its bytes, not trusted to the hash) and its position and
// length (positions of 2^24 and lengths of 255 are escapes in a real design).
struct __attribute__((packed)) tuple {
	std::uint64_t hash;
	std::uint32_t ord, off, pos_len;
	std::uint32_t pos() const noexcept { return pos_len >> 8; }
	std::uint32_t len() const noexcept { return pos_len & 255; }
};
static_assert(sizeof(tuple) == 20);
inline constexpr std::size_t wc_tuples = 16;  // one write-combining flush: 320 bytes, five lines

// Partition by the hash's top byte into 256 buckets with 64-byte write-combining buffers
// (Polychroniou and Ross, SIGMOD 2014): the scatter writes whole cache lines.
struct partitioned {
	std::vector<tuple> out;            // bucket b at [start[b], start[b] + count[b]); padding between buckets
	std::array<std::uint64_t, 257> start{};
	std::array<std::uint64_t, 256> count{};
};

inline unsigned bucket_of(std::uint64_t h) { return unsigned(h >> 56); }

void partition(const std::vector<tuple>& in, partitioned& p) {
	auto& count = p.count;
	count.fill(0);
	for (const auto& t : in) { ++count[bucket_of(t.hash)]; }
	std::uint64_t at = 0;
	for (unsigned b = 0; b < 256; ++b) {
		at = (at + wc_tuples - 1) / wc_tuples * wc_tuples;  // each bucket starts on a flush boundary
		p.start[b] = at;
		at += count[b];
	}
	p.start[256] = at;
	p.out.resize(at + wc_tuples);
	alignas(64) static std::array<std::array<tuple, wc_tuples>, 256> wc;
	std::array<std::uint8_t, 256> fill{};
	std::array<std::uint64_t, 256> cursor;
	for (unsigned b = 0; b < 256; ++b) { cursor[b] = p.start[b]; }
	for (const auto& t : in) {
		const unsigned b = bucket_of(t.hash);
		wc[b][fill[b]++] = t;
		if (fill[b] == wc_tuples) {
			std::memcpy(&p.out[cursor[b]], wc[b].data(), sizeof(tuple) * wc_tuples);
			cursor[b] += wc_tuples;
			fill[b] = 0;
		}
	}
	for (unsigned b = 0; b < 256; ++b) {
		for (unsigned i = 0; i < fill[b]; ++i) { p.out[cursor[b]++] = wc[b][i]; }
	}
}

// A partition's term table: hash-keyed, small enough for L2.
struct small_table {
	std::vector<std::uint64_t> hashes;
	std::vector<std::uint32_t> ids;
	std::uint32_t count = 0;
	explicit small_table(std::size_t capacity) : hashes(std::bit_ceil(std::max<std::size_t>(capacity * 2, 16))), ids(hashes.size()) {}
	std::uint32_t intern(std::uint64_t h) {
		const std::size_t mask = hashes.size() - 1;
		for (std::size_t i = (h >> 8) & mask;; i = (i + 1) & mask) {
			if (hashes[i] == h) { return ids[i]; }
			if (hashes[i] == 0) {
				hashes[i] = h;
				ids[i] = count++;
				return ids[i];
			}
		}
	}
};

// Per-term LEB128 streams (strata's term_buffer shape).
struct leb_streams {
	struct term {
		std::string ords, positions;
		std::uint32_t last = 0, open_prev = 0;
		bool open = false;
	};
	std::vector<term> terms;
	explicit leb_streams(std::size_t n) : terms(n) {}
	static void put(std::string& s, std::uint64_t v) {
		char b[varint::leb128::max_bytes];
		s.append(b, std::size_t(varint::leb128::encode(b, v) - b));
	}
	void add(std::uint32_t id, std::uint32_t ord, std::uint32_t pos) {
		term& t = terms[id];
		if (!t.open || t.last != ord) {
			put(t.ords, t.open ? ord - t.last : ord + 1);
			t.last = ord;
			t.open = true;
			t.open_prev = 0;
		}
		put(t.positions, pos - t.open_prev);
		t.open_prev = pos;
	}
	std::uint64_t bytes() const {
		std::uint64_t n = 0;
		for (const auto& t : terms) { n += t.ords.capacity() + t.positions.capacity() + sizeof(term); }
		return n;
	}
};

// Per-term byte slices in one arena (Lucene's ByteBlockPool, Tantivy's expull): a slice
// ends with a pointer to the next, twice as large, up to 1 KiB.
struct slice_streams {
	struct term {
		std::uint32_t head = ~0u, tail = ~0u;  // arena offsets of the first and current slice
		std::uint32_t fill = 0, size = 0;       // bytes used in the current slice and its size
		std::uint32_t last = 0, open_prev = 0;
		bool open = false;
	};
	std::vector<term> terms;
	std::vector<std::uint8_t> arena;
	explicit slice_streams(std::size_t n) : terms(n) { arena.reserve(64 << 20); }
	std::uint32_t allocate(std::uint32_t size) {
		const std::uint32_t at = std::uint32_t(arena.size());
		arena.resize(arena.size() + size);
		return at;
	}
	void put(term& t, std::uint64_t v) {
		std::uint8_t b[varint::leb128::max_bytes];
		const auto n = std::size_t(varint::leb128::encode(reinterpret_cast<char*>(b), v) - reinterpret_cast<char*>(b));
		for (std::size_t i = 0; i < n; ++i) {
			if (t.tail == ~0u || t.fill + 4 >= t.size) {  // 4 bytes of a full slice link the next
				const std::uint32_t size = t.tail == ~0u ? 16 : std::min<std::uint32_t>(t.size * 2, 1024);
				const std::uint32_t next = allocate(size);
				if (t.tail == ~0u) {
					t.head = next;
				} else {
					std::memcpy(&arena[t.tail + t.fill], &next, 4);
				}
				t.tail = next;
				t.size = size;
				t.fill = 0;
			}
			arena[t.tail + t.fill++] = b[i];
		}
	}
	void add(std::uint32_t id, std::uint32_t ord, std::uint32_t pos) {
		term& t = terms[id];
		if (!t.open || t.last != ord) {
			put(t, t.open ? ord - t.last : ord + 1);
			t.last = ord;
			t.open = true;
			t.open_prev = 0;
		}
		put(t, pos - t.open_prev);
		t.open_prev = pos;
	}
	std::uint64_t bytes() const { return arena.size() + terms.size() * sizeof(term); }
};

// --- stages -------------------------------------------------------------------------------

struct result {
	std::string name;
	std::vector<double> ns;  // per trial
	std::uint64_t units = 0; // tokens, bytes, or postings the stage processed
	std::string unit;
	std::string note;
};

struct state {
	const corpus& c;
	std::string copy_buffer;            // st_copy's target, allocated once (an engine reuses its arenas)
	std::vector<std::uint64_t> hashes;  // one per token, in order
	std::vector<tuple> tuples;
	std::unique_ptr<big_table> table;
	std::vector<std::uint32_t> ids;     // the big table's id per token
	partitioned parts;
	std::uint64_t postings = 0, vocab = 0;
	std::uint64_t encoded_bytes = 0;
	std::string encoded;
	explicit state(const corpus& cc) : c(cc) {}
};

using stage_fn = std::function<std::uint64_t(state&)>;

struct stage {
	const char* name;
	const char* unit;  // what `units` counts
	stage_fn run;
	std::function<std::uint64_t(const state&)> units;
};

std::uint64_t st_copy(state& s) {
	std::string& buf = s.copy_buffer;
	buf.resize(s.c.text_bytes);
	std::size_t at = 0;
	for (const auto& d : s.c.docs) {
		std::memcpy(buf.data() + at, s.c.file.data() + d.text_off, d.text_len);
		at += d.text_len;
	}
	return std::uint64_t(buf[at / 2]) + at;
}

// The JSON work this corpus needs: each line's end and its "id" and "text" values.
std::uint64_t st_parse(state& s) {
	std::string_view all(s.c.file);
	std::uint64_t docs = 0, text = 0;
	for (std::size_t at = 0; at < all.size();) {
		const char* nl = static_cast<const char*>(std::memchr(all.data() + at, '\n', all.size() - at));
		const std::size_t end = nl ? std::size_t(nl - all.data()) : all.size();
		std::string_view line = all.substr(at, end - at);
		const auto i = line.find("\"id\": \"");
		const auto t = line.find("\"text\": \"");
		if (i != std::string_view::npos && t != std::string_view::npos) {
			const std::size_t t0 = t + 9;
			const std::size_t t1 = line.find('"', t0);
			text += t1 - t0;
			++docs;
		}
		at = end + 1;
	}
	return docs * 1000003 + text;
}

std::uint64_t st_scan_memchr(state& s) {
	std::uint64_t n = 0, bytes = 0;
	for (const auto& d : s.c.docs) {
		for_tokens(text_of(s.c, d), [&](std::string_view t) { ++n; bytes += t.size(); });
	}
	return n * 1000003 + bytes;
}

#if defined(__SSE2__)
// 16 bytes at a time: a mask of spaces, token boundaries from its edges.
std::uint64_t st_scan_sse2(state& s) {
	std::uint64_t n = 0, bytes = 0;
	const __m128i space = _mm_set1_epi8(' ');
	for (const auto& d : s.c.docs) {
		const std::string_view t = text_of(s.c, d);
		const char* p = t.data();
		const std::size_t len = t.size();
		std::uint32_t prev = 1;  // the byte before the text counts as a space
		std::size_t i = 0;
		for (; i + 16 <= len; i += 16) {
			const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
			std::uint32_t m = std::uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, space)));
			// Token starts: a non-space whose previous byte is a space.
			const std::uint32_t starts = ~m & ((m << 1) | prev) & 0xffffu;
			n += std::uint32_t(std::popcount(starts));
			bytes += 16 - std::uint32_t(std::popcount(m));
			prev = m >> 15;
		}
		for (; i < len; ++i) {
			const bool sp = p[i] == ' ';
			if (!sp && prev) { ++n; }
			bytes += !sp;
			prev = sp;
		}
	}
	return n * 1000003 + bytes;
}
#endif

template <class H>
std::uint64_t hash_all(state& s, bool keep) {
	std::uint64_t acc = 0, n = 0;
	if (keep) { s.hashes.clear(); s.hashes.reserve(s.c.tokens); }
	for (const auto& d : s.c.docs) {
		for_tokens(text_of(s.c, d), [&](std::string_view t) {
			const std::uint64_t h = H::hash(t.data(), t.size());
			acc ^= h + n++;
			if (keep) { s.hashes.push_back(h); }
		});
	}
	return acc;
}

std::uint64_t st_hash_wordwise(state& s) { return hash_all<::wordwise>(s, true); }

// One 8-byte load, masked to the token's length, mixed with a multiply and an xorshift;
// longer tokens fall back to wordwise. The split is the SSE2 mask.
inline std::uint64_t hash8(const char* p, std::size_t len) {
	if (len <= 8) {
		std::uint64_t w;
		std::memcpy(&w, p, 8);  // the corpus keeps a byte past every token (a space or quote)
		w &= len == 8 ? ~std::uint64_t(0) : (std::uint64_t(1) << (8 * len)) - 1;
		w ^= std::uint64_t(len) << 59;
		w *= 0x9E3779B97F4A7C15ull;
		return w ^ (w >> 29);
	}
	return ::wordwise::hash(p, len);
}

#if defined(__SSE2__)
std::uint64_t st_hash8_sse2(state& s) {
	std::uint64_t acc = 0, n = 0;
	const __m128i space = _mm_set1_epi8(' ');
	for (const auto& d : s.c.docs) {
		const std::string_view t = text_of(s.c, d);
		const char* p = t.data();
		const std::size_t len = t.size();
		std::size_t start = std::string_view::npos;  // the current token's start
		std::size_t i = 0;
		auto emit = [&](std::size_t end) {
			acc ^= hash8(p + start, end - start) + n++;
			start = std::string_view::npos;
		};
		for (; i + 16 <= len; i += 16) {
			const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
			std::uint32_t m = std::uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, space))) & 0xffffu;
			// Each space ends a token (if one is open) and the byte after it starts one.
			while (m) {
				const unsigned k = unsigned(std::countr_zero(m));
				if (start != std::string_view::npos) { emit(i + k); }
				start = i + k + 1;
				m &= m - 1;
			}
			if (start == std::string_view::npos) { start = i; }  // a token spans the block
		}
		for (; i < len; ++i) {
			if (p[i] == ' ') {
				if (start != std::string_view::npos && start < i) { emit(i); }
				start = i + 1;
			}
		}
		if (start != std::string_view::npos && start < len) { emit(len); }
	}
	return acc;
}
#endif
std::uint64_t st_hash_xxh64(state& s) { return hash_all<::xxh64>(s, false); }

// Builds the one big table over every token (the first flush's shape: every term new once,
// then hits), then returns the table's distinct count.
std::uint64_t st_table_build(state& s) {
	s.table = std::make_unique<big_table>(400000);
	s.ids.clear();
	s.ids.reserve(s.c.tokens);
	std::size_t k = 0;
	for (const auto& d : s.c.docs) {
		for_tokens(text_of(s.c, d), [&](std::string_view t) { s.ids.push_back(s.table->intern(t, s.hashes[k++])); });
	}
	s.vocab = s.table->count;
	return s.table->count;
}

// Every token looked up in the built table, each probe depending on the previous (the
// loop as written in a document-at-a-time inverter).
std::uint64_t st_probe_big(state& s) {
	std::uint64_t acc = 0;
	std::size_t k = 0;
	for (const auto& d : s.c.docs) {
		for_tokens(text_of(s.c, d), [&](std::string_view t) {
			const std::uint64_t h = ::wordwise::hash(t.data(), t.size());
			acc += s.table->find(t, h);
			++k;
		});
	}
	return acc;
}

// The same probes, hashes computed 16 tokens ahead and their slots prefetched.
std::uint64_t st_probe_big_prefetch(state& s) {
	std::uint64_t acc = 0;
	std::array<std::string_view, 16> toks;
	std::array<std::uint64_t, 16> hs;
	unsigned fill = 0;
	auto drain = [&] {
		for (unsigned i = 0; i < fill; ++i) { acc += s.table->find(toks[i], hs[i]); }
		fill = 0;
	};
	for (const auto& d : s.c.docs) {
		for_tokens(text_of(s.c, d), [&](std::string_view t) {
			const std::uint64_t h = ::wordwise::hash(t.data(), t.size());
			__builtin_prefetch(s.table->slot_of(h));
			toks[fill] = t;
			hs[fill] = h;
			if (++fill == 16) { drain(); }
		});
	}
	drain();
	return acc;
}

// Tuples from the hashes (the tokenizer's output in the partitioned design).
std::uint64_t st_tuples(state& s) {
	s.tuples.resize(s.hashes.size());
	std::size_t k = 0;
	std::uint32_t ord = 0;
	for (const auto& d : s.c.docs) {
		std::uint32_t pos = 0;
		for_tokens(text_of(s.c, d), [&](std::string_view t) {
			tuple& out = s.tuples[k];
			out.hash = s.hashes[k];
			out.ord = ord;
			out.off = std::uint32_t(t.data() - s.c.file.data());
			out.pos_len = (++pos) << 8 | std::uint32_t(std::min<std::size_t>(t.size(), 255));
			++k;
		});
		++ord;
	}
	return s.tuples.size();
}

std::uint64_t st_partition(state& s) {
	partition(s.tuples, s.parts);
	return s.parts.start[256];
}

// Each partition: a small table assigns ids, a counting sort groups the tuples by term
// (stable, so each term's ordinals stay ascending), and the groups are the postings.
// Returns the posting count; keeps the grouped tuples in s.parts.out.
std::uint64_t st_group(state& s) {
	std::uint64_t postings = 0, vocab = 0;
	std::vector<std::uint32_t> ids, counts;
	std::vector<tuple> sorted;
	for (unsigned b = 0; b < 256; ++b) {
		tuple* begin = &s.parts.out[s.parts.start[b]];
		const std::size_t n = s.parts.count[b];
		small_table table(n / 8 + 16);
		ids.resize(n);
		for (std::size_t i = 0; i < n; ++i) { ids[i] = table.intern(begin[i].hash); }
		counts.assign(table.count + 1, 0);
		for (std::size_t i = 0; i < n; ++i) { ++counts[ids[i] + 1]; }
		for (std::size_t t = 0; t < table.count; ++t) { counts[t + 1] += counts[t]; }
		sorted.resize(n);
		for (std::size_t i = 0; i < n; ++i) { sorted[counts[ids[i]]++] = begin[i]; }
		std::memcpy(begin, sorted.data(), n * sizeof(tuple));
		vocab += table.count;
		for (std::size_t i = 0; i < n; ++i) {
			if (i == 0 || begin[i].hash != begin[i - 1].hash || begin[i].ord != begin[i - 1].ord) { ++postings; }
		}
	}
	s.postings = postings;
	s.vocab = vocab;
	return postings;
}

// After grouping, each term's tokens are contiguous: every token's bytes are compared with
// the group's first, so two terms with one hash can never merge. Reads of the text are
// random (the tuples are in term order), prefetched 16 tuples ahead.
std::uint64_t st_verify(state& s) {
	std::uint64_t mismatches = 0, checked = 0;
	const char* text = s.c.file.data();
	auto length_of = [&](const tuple& t) -> std::size_t {
		if (t.len() < 255) { return t.len(); }
		const char* p = text + t.off;
		const char* e = p;
		while (*e != ' ' && *e != '"') { ++e; }
		return std::size_t(e - p);
	};
	for (unsigned b = 0; b < 256; ++b) {
		const tuple* p = &s.parts.out[s.parts.start[b]];
		const std::size_t n = s.parts.count[b];
		const char* first = nullptr;
		std::size_t first_len = 0;
		for (std::size_t i = 0; i < n; ++i) {
			if (i + 16 < n) { __builtin_prefetch(text + p[i + 16].off); }
			if (i == 0 || p[i].hash != p[i - 1].hash) {
				first = text + p[i].off;
				first_len = length_of(p[i]);
				continue;
			}
			const std::size_t len = length_of(p[i]);
			mismatches += len != first_len || std::memcmp(text + p[i].off, first, len) != 0;
			++checked;
		}
	}
	return checked * 7 + mismatches;
}

// Appends every posting into per-term LEB128 streams (strata's shape), in token order.
std::uint64_t st_append_leb(state& s) {
	leb_streams streams(s.table->count);
	std::size_t k = 0;
	std::uint32_t ord = 0;
	for (const auto& d : s.c.docs) {
		std::uint32_t pos = 0;
		for_tokens(text_of(s.c, d), [&](std::string_view) { streams.add(s.ids[k++], ord, ++pos); });
		++ord;
	}
	return streams.bytes();
}

std::uint64_t st_append_slices(state& s) {
	slice_streams streams(s.table->count);
	std::size_t k = 0;
	std::uint32_t ord = 0;
	for (const auto& d : s.c.docs) {
		std::uint32_t pos = 0;
		for_tokens(text_of(s.c, d), [&](std::string_view) { streams.add(s.ids[k++], ord, ++pos); });
		++ord;
	}
	return streams.bytes();
}

// Encodes the grouped postings: per term, ordinal deltas in FOR blocks of 128 (PFOR for
// wdf, PFOR for position deltas), LEB128 tails, as the segment format lays them out.
std::uint64_t st_encode(state& s) {
	std::string& out = s.encoded;
	out.clear();
	std::uint32_t ords[128], wdfs[128], deltas[128], posd[128];
	std::uint8_t buf[bitpack::max_encoded_bytes];
	std::vector<std::uint32_t> t_ords, t_wdfs, t_pos;
	auto flush_term = [&] {
		const std::size_t df = t_ords.size();
		const std::size_t full = df / 128 * 128;
		std::uint32_t prev = ~0u;
		for (std::size_t b = 0; b < full; b += 128) {
			std::memcpy(ords, &t_ords[b], 512);
			std::memcpy(wdfs, &t_wdfs[b], 512);
			bitpack::delta_encode(prev, ords, deltas);
			out.append(reinterpret_cast<char*>(buf), bitpack::encode_for(deltas, buf));
			out.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(wdfs, buf));
			prev = ords[127];
		}
		char lb[varint::leb128::max_bytes];
		for (std::size_t i = full; i < df; ++i) {
			out.append(lb, std::size_t(varint::leb128::encode(lb, t_ords[i] - prev) - lb));
			prev = t_ords[i];
		}
		for (std::size_t i = full; i < df; ++i) { out.append(lb, std::size_t(varint::leb128::encode(lb, t_wdfs[i]) - lb)); }
		const std::size_t pfull = t_pos.size() / 128 * 128;
		for (std::size_t b = 0; b < pfull; b += 128) {
			std::memcpy(posd, &t_pos[b], 512);
			out.append(reinterpret_cast<char*>(buf), bitpack::encode_pfor(posd, buf));
		}
		for (std::size_t i = pfull; i < t_pos.size(); ++i) { out.append(lb, std::size_t(varint::leb128::encode(lb, t_pos[i]) - lb)); }
		t_ords.clear(), t_wdfs.clear(), t_pos.clear();
	};
	std::uint64_t terms = 0;
	for (unsigned b = 0; b < 256; ++b) {
		const tuple* p = &s.parts.out[s.parts.start[b]];
		const std::size_t n = s.parts.count[b];
		for (std::size_t i = 0; i < n; ++i) {
			const bool new_term = i == 0 || p[i].hash != p[i - 1].hash;
			if (new_term && i) { flush_term(), ++terms; }
			const bool new_doc = new_term || p[i].ord != p[i - 1].ord;
			if (new_doc) {
				t_ords.push_back(p[i].ord);
				t_wdfs.push_back(1);
				t_pos.push_back(p[i].pos());
			} else {
				++t_wdfs.back();
				t_pos.push_back(p[i].pos() - p[i - 1].pos());
			}
		}
		if (n) { flush_term(), ++terms; }
	}
	s.encoded_bytes = out.size();
	return out.size() + terms;
}

std::uint64_t st_write(state& s) {
	const char* path = ".scratch/research_bound.out";
	std::FILE* f = std::fopen(path, "wb");
	if (!f) { std::fprintf(stderr, "cannot write %s\n", path); return 0; }
	std::fwrite(s.encoded.data(), 1, s.encoded.size(), f);
	std::fflush(f);
	std::fclose(f);
	return s.encoded.size();
}

// Memory bandwidth: sum over 1 GiB on one thread and on eight.
double bandwidth_gbs(unsigned threads) {
	const std::size_t total = std::size_t(1) << 30;
	std::vector<std::uint64_t> buf(total / 8, 1);
	std::atomic<std::uint64_t> sink{0};
	auto t0 = clock_type::now();
	std::vector<std::thread> ts;
	for (unsigned t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			const std::size_t n = buf.size() / threads;
			const std::uint64_t* p = buf.data() + n * t;
			std::uint64_t a = 0, b = 0, c = 0, d = 0;
			for (std::size_t i = 0; i + 4 <= n; i += 4) { a += p[i], b += p[i + 1], c += p[i + 2], d += p[i + 3]; }
			sink += a + b + c + d;
		});
	}
	for (auto& t : ts) { t.join(); }
	const double secs = std::chrono::duration<double>(clock_type::now() - t0).count();
	return double(total) / secs / 1e9 + double(sink.load() == 1);
}

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: research_bound CORPUS.json [trials]\n"); return 2; }
	const int trials = argc > 2 ? std::atoi(argv[2]) : 5;
	corpus c = load(argv[1]);
	for (const auto& d : c.docs) { for_tokens(text_of(c, d), [&](std::string_view) { ++c.tokens; }); }
	std::printf("corpus: %zu docs, %zu file bytes, %llu text bytes, %llu tokens (%.1f per doc, %.2f bytes each)\n", c.docs.size(), c.file.size(),
		static_cast<unsigned long long>(c.text_bytes), static_cast<unsigned long long>(c.tokens), double(c.tokens) / double(c.docs.size()),
		double(c.text_bytes) / double(c.tokens));
	std::printf("bandwidth: 1 thread %.1f GB/s, 8 threads %.1f GB/s (sum over 1 GiB)\n", bandwidth_gbs(1), bandwidth_gbs(8));

	state s(c);
	const std::uint64_t T = c.tokens, B = c.text_bytes;
	std::vector<stage> stages = {
		{"copy text (memcpy)", "byte", st_copy, [=](const state&) { return B; }},
		{"parse: line ends, id and text fields (memchr, find)", "byte", st_parse, [=](const state& st) { return st.c.file.size(); }},
		{"split tokens, memchr", "token", st_scan_memchr, [=](const state&) { return T; }},
#if defined(__SSE2__)
		{"split tokens, SSE2 mask", "token", st_scan_sse2, [=](const state&) { return T; }},
#endif
		{"split + hash (wordwise)", "token", st_hash_wordwise, [=](const state&) { return T; }},
		{"split + hash (xxh64)", "token", st_hash_xxh64, [=](const state&) { return T; }},
#if defined(__SSE2__)
		{"split SSE2 + hash8 (one masked load, multiply)", "token", st_hash8_sse2, [=](const state&) { return T; }},
#endif
		{"big table: build (intern every token)", "token", st_table_build, [=](const state&) { return T; }},
		{"big table: split + hash + probe, dependent", "token", st_probe_big, [=](const state&) { return T; }},
		{"big table: same, 16 ahead + prefetch", "token", st_probe_big_prefetch, [=](const state&) { return T; }},
		{"append LEB128 streams per term (ords + positions)", "token", st_append_leb, [=](const state&) { return T; }},
		{"append byte slices per term (arena)", "token", st_append_slices, [=](const state&) { return T; }},
		{"tuples (hash, ord, pos) from hashes", "token", st_tuples, [=](const state&) { return T; }},
		{"radix partition tuples by hash, 256 ways", "token", st_partition, [=](const state&) { return T; }},
		{"per partition: small table + counting sort", "token", st_group, [=](const state&) { return T; }},
		{"verify every token's bytes against its term's", "token", st_verify, [=](const state&) { return T; }},
		{"encode FOR/PFOR blocks + LEB tails", "token", st_encode, [=](const state&) { return T; }},
		{"write encoded bytes to a file", "byte", st_write, [](const state& st) { return st.encoded_bytes; }},
	};
	std::vector<result> results(stages.size());
	std::vector<std::uint64_t> checks(stages.size(), 0);
	for (int t = 0; t < trials; ++t) {
		for (std::size_t i = 0; i < stages.size(); ++i) {
			const auto t0 = clock_type::now();
			const std::uint64_t check = stages[i].run(s);
			const double ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
			if (t == 0) { checks[i] = check; } else if (checks[i] != check) { std::fprintf(stderr, "stage %s: checksum changed between trials\n", stages[i].name); }
			results[i].name = stages[i].name;
			results[i].unit = stages[i].unit;
			results[i].ns.push_back(ns);
			results[i].units = stages[i].units(s);
		}
		std::fprintf(stderr, "trial %d done\n", t + 1);
	}
	std::printf("\n%-52s %10s %9s %9s %9s\n", "stage (one thread, median of trials)", "ms", "ns/unit", "unit", "GB/s");
	for (const auto& r : results) {
		const double ms = median(r.ns) / 1e6;
		const double per = median(r.ns) / double(r.units);
		const double gbs = (r.unit == std::string("byte") ? double(r.units) : double(c.text_bytes)) / median(r.ns);
		std::printf("%-52s %10.1f %9.2f %9s %9.2f\n", r.name.c_str(), ms, per, r.unit.c_str(), gbs);
	}
	std::printf("\npostings %llu, vocabulary %llu, encoded postings+positions %llu bytes (%.2f per posting), tuples %zu bytes\n",
		static_cast<unsigned long long>(s.postings), static_cast<unsigned long long>(s.vocab), static_cast<unsigned long long>(s.encoded_bytes),
		double(s.encoded_bytes) / double(std::max<std::uint64_t>(1, s.postings)), s.tuples.size() * sizeof(tuple));
	std::printf("(sizes: big table %zu slots x %zu bytes = %.1f MB; tuple %zu bytes)\n", s.table->slots.size(), sizeof(big_table::slot),
		double(s.table->slots.size() * sizeof(big_table::slot)) / 1e6, sizeof(tuple));
	return 0;
}
