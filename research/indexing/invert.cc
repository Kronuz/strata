// Three inverters over the same token stream, each writing the same segment through
// strata's segment_writer flush path (docs/research/indexing.md, "Ideas and prototypes"):
//
//   A  today's shape: tokens grouped into a document (an unordered_map per document, a
//      string and a positions vector per term, the document copied into a buffer as
//      index::evaluate does), then segment_writer::add per document at flush, which
//      interns terms in one big table and appends LEB128 streams; then flush().
//   B  Lucene's and Tantivy's shape: one big table keyed by the term bytes, postings
//      appended to byte slices in an arena as tokens arrive; flush sorts the terms and
//      re-reads the slices through flush_from.
//   C  the partitioned shape: tokens become (hash, ordinal, offset, position) tuples in
//      arrival order with the text kept; flush radix-partitions them by hash, groups each
//      partition with a cache-resident table and a counting sort, verifies every token's
//      bytes against its term's, sorts the terms, and encodes through flush_from.
//
// The three outputs must be byte-identical. Add and flush phases are timed apart; five
// interleaved trials by default.
//
//   research_invert CORPUS.json [trials=5] [docs=0 (all)]

#include "stream.hh"

#include "strata/index.hh"
#include "hashes.hh"
#include "varint.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
using strata::ordinal_t;

using research::tok;
using research::doc_toks;
using research::stream;
using research::bytes_of;

std::string alphabet_of(const std::vector<std::string_view>& terms) {
	std::array<bool, 256> used{};
	for (const auto& t : terms) {
		for (char c : t) { used[std::uint8_t(c)] = true; }
	}
	std::string a;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { a.push_back(char(c)); }
	}
	return a;
}

constexpr std::array<std::uint8_t, 16> segment_id{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

struct timing {
	double add_ns = 0, flush_ns = 0;  // the add phase (per document) and the flush
	std::vector<std::pair<const char*, double>> parts;  // the flush's parts
	std::uint64_t memory = 0;  // bytes the inverter held before the flush
	std::string out;
};

// --- A: today's shape ---------------------------------------------------------------------

struct a_doc {
	std::vector<strata::posting_input> in;  // views into `terms` and `positions`
	std::vector<std::string> terms;
	std::vector<std::vector<std::uint32_t>> positions;
};

timing run_a(const stream& s) {
	timing t;
	// Add phase: tokens_document (an unordered_map per document, a string per term) and the
	// buffer copy, as the engine and index::evaluate do today.
	auto t0 = clock_type::now();
	std::vector<a_doc> buffer;
	buffer.reserve(s.docs.size());
	std::uint64_t memory = 0;
	for (const auto& d : s.docs) {
		std::unordered_map<std::string_view, std::vector<std::uint32_t>> at;
		for (std::uint32_t i = d.first; i < d.first + d.count; ++i) { at[bytes_of(s.toks[i])].push_back(s.toks[i].pos); }
		a_doc built;
		for (auto& [term, positions] : at) {
			built.terms.emplace_back(term);
			built.positions.push_back(std::move(positions));
		}
		a_doc copy = built;  // index::evaluate copies the transaction's document into the buffer
		for (std::size_t i = 0; i < copy.terms.size(); ++i) {
			copy.in.push_back({copy.terms[i], std::uint32_t(copy.positions[i].size()), copy.positions[i]});
			memory += sizeof(strata::term_entry) + copy.terms[i].size() + 4 * copy.positions[i].size();
		}
		memory += 96 + d.id.size();
		buffer.push_back(std::move(copy));
	}
	t.add_ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	t.memory = memory;
	// Flush: the writer interns every term and appends, then encodes.
	t0 = clock_type::now();
	strata::segment_writer w;
	strata::flush_charge charge(nullptr);
	w.memory = &charge;
	for (std::size_t n = 0; n < s.docs.size(); ++n) {
		const auto& d = s.docs[n];
		w.add(n + 1, buffer[n].in, d.count, d.id, 1, d.key);
	}
	const auto t1 = clock_type::now();
	t.out = w.flush(segment_id);
	const auto t2 = clock_type::now();
	t.flush_ns = std::chrono::duration<double, std::nano>(t2 - t0).count();
	t.parts = {{"writer.add (intern + LEB128 appends)", std::chrono::duration<double, std::nano>(t1 - t0).count()},
		{"writer.flush (sort, decode, encode, dictionary)", std::chrono::duration<double, std::nano>(t2 - t1).count()}};
	t.memory += charge.held();
	return t;
}

// --- B: one big table, byte slices --------------------------------------------------------

struct b_inverter {
	struct slot {
		std::uint64_t hash = 0;
		const char* key = nullptr;
		std::uint32_t length = 0, id = 0;
	};
	struct term {
		std::uint32_t docs_head = ~0u, docs_tail = ~0u, docs_fill = 0, docs_size = 0;
		std::uint32_t pos_head = ~0u, pos_tail = ~0u, pos_fill = 0, pos_size = 0;
		ordinal_t last = 0;      // the open posting's ordinal
		std::uint32_t wdf = 0;   // the open posting's wdf so far
		std::uint32_t prev_pos = 0;
		std::uint32_t last_written = 0;  // the ordinal the last written delta reached
		bool open = false, any = false;
		std::uint32_t df = 0;
		std::uint64_t cf = 0;
	};
	std::vector<slot> table;
	std::deque<std::string> arena;
	std::vector<term> terms;
	std::vector<std::uint8_t> slices;
	std::vector<std::uint32_t> touched;

	b_inverter() : table(1 << 16) { slices.reserve(256 << 20); }

	std::uint32_t intern(std::string_view key) {
		const std::uint64_t h = ::wordwise::hash(key.data(), key.size());
		for (;;) {
			const std::size_t mask = table.size() - 1;
			for (std::size_t i = h & mask;; i = (i + 1) & mask) {
				slot& sl = table[i];
				if (!sl.key) {
					if ((terms.size() + 1) * 2 > table.size()) { break; }  // grow first
					if (arena.empty() || arena.back().size() + key.size() > arena.back().capacity()) {
						arena.emplace_back();
						arena.back().reserve(std::max<std::size_t>(1 << 20, key.size()));
					}
					std::string& chunk = arena.back();
					const char* at = chunk.data() + chunk.size();
					chunk.append(key);
					sl = {h, at, std::uint32_t(key.size()), std::uint32_t(terms.size())};
					terms.emplace_back();
					return sl.id;
				}
				if (sl.hash == h && sl.length == key.size() && std::memcmp(sl.key, key.data(), key.size()) == 0) { return sl.id; }
			}
			std::vector<slot> old(table.size() * 2);
			old.swap(table);
			const std::size_t mask2 = table.size() - 1;
			for (const auto& sl : old) {
				if (!sl.key) { continue; }
				std::size_t i = sl.hash & mask2;
				while (table[i].key) { i = (i + 1) & mask2; }
				table[i] = sl;
			}
		}
	}
	// Appends one LEB128 value to a term's docs or positions slice chain.
	void put(std::uint32_t& head, std::uint32_t& tail, std::uint32_t& fill, std::uint32_t& size, std::uint64_t v) {
		std::uint8_t b[varint::leb128::max_bytes];
		const auto n = std::size_t(varint::leb128::encode(reinterpret_cast<char*>(b), v) - reinterpret_cast<char*>(b));
		for (std::size_t i = 0; i < n; ++i) {
			if (tail == ~0u || fill + 4 >= size) {
				const std::uint32_t next_size = tail == ~0u ? 16 : std::min<std::uint32_t>(size * 2, 1024);
				const std::uint32_t next = std::uint32_t(slices.size());
				slices.resize(slices.size() + next_size);
				if (tail == ~0u) {
					head = next;
				} else {
					std::memcpy(&slices[tail + fill], &next, 4);
				}
				tail = next;
				size = next_size;
				fill = 0;
			}
			slices[tail + fill++] = b[i];
		}
	}
	void close(term& t) {
		// The open posting's (ordinal delta, wdf) go to the docs slice when it closes.
		put(t.docs_head, t.docs_tail, t.docs_fill, t.docs_size, t.any ? t.last - t.last_written : t.last + 1);
		put(t.docs_head, t.docs_tail, t.docs_fill, t.docs_size, t.wdf);
		t.last_written = t.last;
		t.any = true;
		++t.df;
		t.cf += t.wdf;
		t.open = false;
	}
	void add(ordinal_t ord, const tok* toks, std::uint32_t count) {
		touched.clear();
		for (std::uint32_t i = 0; i < count; ++i) {
			const std::uint32_t id = intern(bytes_of(toks[i]));
			term& t = terms[id];
			if (!t.open || t.last != ord) {
				if (t.open) { close(t); }
				t.open = true;
				t.last = ord;
				t.wdf = 0;
				t.prev_pos = 0;
				touched.push_back(id);
			}
			++t.wdf;
			put(t.pos_head, t.pos_tail, t.pos_fill, t.pos_size, toks[i].pos - t.prev_pos);
			t.prev_pos = toks[i].pos;
		}
		for (auto id : touched) { close(terms[id]); }
	}
	// Reads a slice chain back into values.
	void read(std::uint32_t head, std::uint32_t tail, std::uint32_t fill, std::vector<std::uint32_t>& out) const {
		std::uint32_t at = head, size = 16;
		std::string bytes;
		while (at != ~0u) {
			const bool last = at == tail;
			const std::uint32_t used = last ? fill : size - 4;
			bytes.append(reinterpret_cast<const char*>(&slices[at]), used);
			if (last) { break; }
			std::memcpy(&at, &slices[at + used], 4);
			size = std::min<std::uint32_t>(size * 2, 1024);
		}
		const char* p = bytes.data();
		const char* end = p + bytes.size();
		while (p < end) {
			std::uint64_t v;
			varint::leb128::decode(&p, end, v);
			out.push_back(std::uint32_t(v));
		}
	}
	std::uint64_t memory() const {
		std::uint64_t n = table.size() * sizeof(slot) + terms.size() * sizeof(term) + slices.size();
		for (const auto& a : arena) { n += a.capacity(); }
		return n;
	}
};

timing run_b(const stream& s) {
	timing t;
	auto t0 = clock_type::now();
	b_inverter inv;
	strata::segment_writer w;
	strata::flush_charge charge(nullptr);
	w.memory = &charge;
	for (std::size_t n = 0; n < s.docs.size(); ++n) {
		const auto& d = s.docs[n];
		w.add_document(n + 1, d.count, d.id, 1, d.key);
		inv.add(ordinal_t(n), &s.toks[d.first], d.count);
	}
	t.add_ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	t.memory = inv.memory() + charge.held();
	t0 = clock_type::now();
	std::vector<std::pair<std::string_view, std::uint32_t>> keys;
	keys.reserve(inv.terms.size());
	for (const auto& sl : inv.table) {
		if (sl.key) { keys.push_back({std::string_view(sl.key, sl.length), sl.id}); }
	}
	std::sort(keys.begin(), keys.end());
	std::vector<std::string_view> names;
	for (const auto& k : keys) { names.push_back(k.first); }
	const std::string alphabet = alphabet_of(names);
	const auto t1 = clock_type::now();
	std::string file;
	strata::string_output out(file);
	strata::string_spill spill;
	std::size_t next = 0;
	std::vector<std::uint32_t> docs, pos;
	w.flush_from(segment_id, out, spill, alphabet, [&](strata::term_postings& tp) -> std::optional<std::string_view> {
		if (next == keys.size()) { return std::nullopt; }
		const auto [key, id] = keys[next++];
		const auto& term = inv.terms[id];
		docs.clear(), pos.clear();
		inv.read(term.docs_head, term.docs_tail, term.docs_fill, docs);
		inv.read(term.pos_head, term.pos_tail, term.pos_fill, pos);
		tp.clear();
		ordinal_t ord = 0;
		for (std::size_t i = 0; i + 1 < docs.size(); i += 2) {
			ord = i ? ord + docs[i] : docs[i] - 1;
			tp.ords.push_back(ord);
			tp.wdfs.push_back(docs[i + 1]);
		}
		tp.counts = tp.wdfs;
		tp.positions = pos;
		tp.cf = term.cf;
		tp.all_zero_wdf = false;
		tp.any_positions = true;
		tp.counts_are_wdf = true;
		return key;
	});
	const auto t2 = clock_type::now();
	t.out = std::move(file);
	t.flush_ns = std::chrono::duration<double, std::nano>(t2 - t0).count();
	t.parts = {{"sort terms", std::chrono::duration<double, std::nano>(t1 - t0).count()},
		{"read slices + flush_from (encode, dictionary)", std::chrono::duration<double, std::nano>(t2 - t1).count()}};
	return t;
}

// --- C: tuples, partition, group ---------------------------------------------------------

struct __attribute__((packed)) tuple {
	std::uint64_t hash;
	std::uint32_t ord, off, pos_len;
	std::uint32_t pos() const noexcept { return pos_len >> 8; }
	std::uint32_t len() const noexcept { return pos_len & 255; }
};
static_assert(sizeof(tuple) == 20);
inline constexpr std::size_t wc_tuples = 16;
inline unsigned bucket_of(std::uint64_t h) { return unsigned(h >> 56); }

struct c_inverter {
	std::string text;  // every document's tokens' bytes, copied as they arrive
	std::vector<tuple> tuples, parts;  // parts: bucket b at [start[b], start[b] + count[b])
	std::array<std::uint64_t, 257> start{};
	std::array<std::uint64_t, 256> count{};

	c_inverter() { text.reserve(160 << 20); tuples.reserve(20 << 20); }

	void add(ordinal_t ord, const tok* toks, std::uint32_t count) {
		for (std::uint32_t i = 0; i < count; ++i) {
			const tok& tk = toks[i];
			const std::uint32_t off = std::uint32_t(text.size());
			text.append(tk.p, tk.len);
			text.push_back(' ');  // a separator past every token, so a length escape can scan
			tuples.push_back({::wordwise::hash(tk.p, tk.len), ord, off, tk.pos << 8 | std::min<std::uint32_t>(tk.len, 255)});
		}
	}
	std::uint64_t memory() const { return text.size() + tuples.size() * sizeof(tuple); }

	void partition() {
		count.fill(0);
		for (const auto& t : tuples) { ++count[bucket_of(t.hash)]; }
		std::uint64_t at = 0;
		for (unsigned b = 0; b < 256; ++b) {
			at = (at + wc_tuples - 1) / wc_tuples * wc_tuples;
			start[b] = at;
			at += count[b];
		}
		start[256] = at;
		parts.resize(at + wc_tuples);
		alignas(64) static std::array<std::array<tuple, wc_tuples>, 256> wc;
		std::array<std::uint8_t, 256> fill{};
		std::array<std::uint64_t, 256> cursor;
		for (unsigned b = 0; b < 256; ++b) { cursor[b] = start[b]; }
		for (const auto& t : tuples) {
			const unsigned b = bucket_of(t.hash);
			wc[b][fill[b]++] = t;
			if (fill[b] == wc_tuples) {
				std::memcpy(&parts[cursor[b]], wc[b].data(), sizeof(tuple) * wc_tuples);
				cursor[b] += wc_tuples;
				fill[b] = 0;
			}
		}
		for (unsigned b = 0; b < 256; ++b) {
			for (unsigned i = 0; i < fill[b]; ++i) { parts[cursor[b]++] = wc[b][i]; }
		}
	}

	struct group {
		std::string_view term;
		std::uint64_t begin, end;  // in parts
	};
	std::vector<group> groups;

	// Per partition: ids from a small hash-keyed table, a stable counting sort by id, then
	// every token's bytes checked against its group's first (a hash collision between two
	// terms is caught here, never merged).
	std::uint64_t group_all() {
		groups.clear();
		std::vector<std::uint64_t> hashes;
		std::vector<std::uint32_t> ids, tids, counts;
		std::vector<tuple> sorted;
		std::uint64_t mismatches = 0;
		for (unsigned b = 0; b < 256; ++b) {
			tuple* p = &parts[start[b]];
			const std::size_t n = count[b];
			if (!n) { continue; }
			const std::size_t cap = std::bit_ceil(std::max<std::size_t>(16, n / 4));
			hashes.assign(cap, 0);
			ids.assign(cap, 0);
			std::uint32_t distinct = 0;
			tids.resize(n);
			for (std::size_t i = 0; i < n; ++i) {
				const std::uint64_t h = p[i].hash;
				for (std::size_t k = (h >> 8) & (cap - 1);; k = (k + 1) & (cap - 1)) {
					if (hashes[k] == h) { tids[i] = ids[k]; break; }
					if (hashes[k] == 0) { hashes[k] = h; ids[k] = distinct; tids[i] = distinct++; break; }
				}
			}
			counts.assign(distinct + 1, 0);
			for (std::size_t i = 0; i < n; ++i) { ++counts[tids[i] + 1]; }
			for (std::uint32_t t = 0; t < distinct; ++t) { counts[t + 1] += counts[t]; }
			sorted.resize(n);
			for (std::size_t i = 0; i < n; ++i) { sorted[counts[tids[i]]++] = p[i]; }
			std::memcpy(p, sorted.data(), n * sizeof(tuple));
			const char* txt = text.data();
			auto length_of = [&](const tuple& t) -> std::size_t {
				if (t.len() < 255) { return t.len(); }
				const char* q = txt + t.off;
				const char* e = q;
				while (*e != ' ') { ++e; }
				return std::size_t(e - q);
			};
			std::size_t gb = 0;
			std::size_t first_len = 0;
			for (std::size_t i = 0; i < n; ++i) {
				if (i + 16 < n) { __builtin_prefetch(txt + p[i + 16].off); }
				if (i == 0 || p[i].hash != p[i - 1].hash) {
					if (i) { groups.push_back({std::string_view(txt + p[gb].off, first_len), start[b] + gb, start[b] + i}); }
					gb = i;
					first_len = length_of(p[i]);
					continue;
				}
				const std::size_t len = length_of(p[i]);
				mismatches += len != first_len || std::memcmp(txt + p[i].off, txt + p[gb].off, len) != 0;
			}
			groups.push_back({std::string_view(txt + p[gb].off, first_len), start[b] + gb, start[b] + n});
		}
		return mismatches;
	}
};

timing run_c(const stream& s) {
	timing t;
	auto t0 = clock_type::now();
	c_inverter inv;
	strata::segment_writer w;
	strata::flush_charge charge(nullptr);
	w.memory = &charge;
	for (std::size_t n = 0; n < s.docs.size(); ++n) {
		const auto& d = s.docs[n];
		w.add_document(n + 1, d.count, d.id, 1, d.key);
		inv.add(ordinal_t(n), &s.toks[d.first], d.count);
	}
	t.add_ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	t.memory = inv.memory() + charge.held();
	t0 = clock_type::now();
	inv.partition();
	const auto t1 = clock_type::now();
	const std::uint64_t mismatches = inv.group_all();
	if (mismatches) { std::fprintf(stderr, "C: %llu tokens whose bytes differ from their term's (hash collisions)\n", static_cast<unsigned long long>(mismatches)); }
	const auto t2 = clock_type::now();
	std::sort(inv.groups.begin(), inv.groups.end(), [](const c_inverter::group& a, const c_inverter::group& b) { return a.term < b.term; });
	std::vector<std::string_view> names;
	names.reserve(inv.groups.size());
	for (const auto& g : inv.groups) { names.push_back(g.term); }
	const std::string alphabet = alphabet_of(names);
	const auto t3 = clock_type::now();
	std::string file;
	strata::string_output out(file);
	strata::string_spill spill;
	std::size_t next = 0;
	w.flush_from(segment_id, out, spill, alphabet, [&](strata::term_postings& tp) -> std::optional<std::string_view> {
		if (next == inv.groups.size()) { return std::nullopt; }
		const auto& g = inv.groups[next++];
		tp.clear();
		for (std::uint64_t i = g.begin; i < g.end; ++i) {
			const tuple& tu = inv.parts[i];
			if (i == g.begin || tu.ord != inv.parts[i - 1].ord) {
				tp.ords.push_back(tu.ord);
				tp.wdfs.push_back(1);
				tp.positions.push_back(tu.pos());
			} else {
				++tp.wdfs.back();
				tp.positions.push_back(tu.pos() - inv.parts[i - 1].pos());
			}
		}
		tp.counts = tp.wdfs;
		tp.cf = g.end - g.begin;
		tp.all_zero_wdf = false;
		tp.any_positions = true;
		tp.counts_are_wdf = true;
		return g.term;
	});
	const auto t4 = clock_type::now();
	t.out = std::move(file);
	t.flush_ns = std::chrono::duration<double, std::nano>(t4 - t0).count();
	t.parts = {{"partition (256 ways)", std::chrono::duration<double, std::nano>(t1 - t0).count()},
		{"group (small tables, counting sort) + verify", std::chrono::duration<double, std::nano>(t2 - t1).count()},
		{"sort terms", std::chrono::duration<double, std::nano>(t3 - t2).count()},
		{"flush_from (encode, dictionary)", std::chrono::duration<double, std::nano>(t4 - t3).count()}};
	t.memory += inv.parts.size() * sizeof(tuple);  // the partition buffer, transient
	return t;
}

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: research_invert CORPUS.json [trials] [docs]\n"); return 2; }
	const int trials = argc > 2 ? std::atoi(argv[2]) : 5;
	const std::size_t max_docs = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0;
	const stream s = research::load(argv[1], max_docs);
	const double T = double(s.toks.size());
	std::printf("stream: %zu docs, %zu tokens (keys included), %llu text bytes\n", s.docs.size(), s.toks.size(), static_cast<unsigned long long>(s.text_bytes));
	struct variant {
		const char* name;
		timing (*run)(const stream&);
		std::vector<double> add, flush;
		std::vector<std::vector<double>> parts;
		std::vector<const char*> part_names;
		std::uint64_t memory = 0;
		std::string out;
	};
	std::vector<variant> variants = {{"A: today (document, big table, LEB128 streams)", run_a},
		{"B: big table, byte slices (Lucene/Tantivy shape)", run_b}, {"C: tuples, radix partition, counting sort", run_c}};
	for (int t = 0; t < trials; ++t) {
		for (std::size_t k = 0; k < variants.size(); ++k) {
			variant& v = variants[(k + std::size_t(t)) % variants.size()];  // the order rotates per trial
			timing tm = v.run(s);
			v.add.push_back(tm.add_ns);
			v.flush.push_back(tm.flush_ns);
			if (v.parts.empty()) {
				v.parts.resize(tm.parts.size());
				for (const auto& p : tm.parts) { v.part_names.push_back(p.first); }
			}
			for (std::size_t i = 0; i < tm.parts.size(); ++i) { v.parts[i].push_back(tm.parts[i].second); }
			v.memory = tm.memory;
			if (v.out.empty()) {
				v.out = std::move(tm.out);
			} else if (v.out != tm.out) {
				std::fprintf(stderr, "%s: output changed between trials\n", v.name);
			}
		}
		std::fprintf(stderr, "trial %d done\n", t + 1);
	}
	std::printf("\n%-54s %9s %9s %9s %9s %9s\n", "variant (one thread, median of trials)", "add ms", "flush ms", "total ms", "ns/token", "MB held");
	for (const auto& v : variants) {
		const double add = median(v.add), flush = median(v.flush);
		std::printf("%-54s %9.1f %9.1f %9.1f %9.1f %9.1f\n", v.name, add / 1e6, flush / 1e6, (add + flush) / 1e6, (add + flush) / T, double(v.memory) / 1e6);
		for (std::size_t i = 0; i < v.parts.size(); ++i) {
			std::printf("    %-50s %9s %9.1f %9s %9.1f\n", v.part_names[i], "", median(v.parts[i]) / 1e6, "", median(v.parts[i]) / T);
		}
	}
	std::printf("\nsegment bytes: %zu\n", variants[0].out.size());
	for (std::size_t k = 1; k < variants.size(); ++k) {
		if (variants[k].out == variants[0].out) {
			std::printf("%s: output identical to A\n", variants[k].name);
		} else {
			std::size_t at = 0;
			while (at < variants[k].out.size() && at < variants[0].out.size() && variants[k].out[at] == variants[0].out[at]) { ++at; }
			std::printf("%s: output DIFFERS from A at byte %zu (sizes %zu and %zu)\n", variants[k].name, at, variants[k].out.size(), variants[0].out.size());
		}
	}
	return 0;
}
