// encode_once_check A B: A is a segment of today's flush (format 12), B one of the
// encode-once flush (format 13's bit 14 run layout: a run table, short blocks, position
// tails mid-stream) of the same documents. Decodes every list of both with one decoder,
// checks A's against the real reader, B's skip data against its own lists, and that every
// posting, wdf, count, and position is equal; the dictionaries' contents and every other
// section must be byte-identical. Prints both segments' section sizes.
#include "strata/reader.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace strata;

namespace {

std::string slurp(const char* path) {
	std::ifstream f(path, std::ios::binary);
	std::ostringstream s;
	s << f.rdbuf();
	return s.str();
}

struct decoded {
	std::vector<std::uint32_t> ords, wdfs, counts, positions;  // positions as deltas within each document
};

struct stats {
	std::uint64_t terms = 0, postings = 0, blocks = 0, bitsets = 0, shorts = 0, short_postings = 0, tail_postings = 0, pos_blocks = 0, pos_tail_values = 0;
};

[[noreturn]] void fail(std::string_view term, const std::string& what) {
	std::cerr << "MISMATCH at term '" << term << "' (" << term.size() << " bytes): " << what << "\n";
	std::exit(1);
}

std::uint64_t leb(const char*& p, const char* end) { return format::get_leb(&p, end); }

class decoder {
public:
	decoder(const segment& seg, std::string_view file, bool bit14) : seg_(seg), bit14_(bit14) {
		P_ = seg.postings_bytes();
		Q_ = seg.positions_bytes();
		runs_ = {0};
		for (const auto& e : seg.sections()) {
			if (std::uint16_t(e.kind) != segment_writer::runs_section) { continue; }
			const char* p = file.data() + e.offset;
			const std::uint32_t n = format::get<std::uint32_t>(p);
			runs_.clear();
			for (std::uint32_t i = 0; i < n; ++i) { runs_.push_back(format::get<std::uint32_t>(p + 4 + 4 * i)); }
		}
		if (bit14_ && runs_.empty()) { throw std::runtime_error("no run table"); }
	}
	stats st;

	std::size_t run_of(std::uint32_t ord) const { return std::size_t(std::upper_bound(runs_.begin(), runs_.end(), ord) - runs_.begin()) - 1; }

	void decode(std::string_view term, const format::term_info& info, decoded& L) {
		L = {};
		const bool no_wdf = info.flags & format::no_wdf, no_pos = info.flags & format::no_positions, cw = info.flags & format::counts_are_wdf;
		const bool with_counts = !cw && !no_pos;
		const std::uint32_t df = info.df;
		struct blk {
			std::uint32_t n;
			std::uint8_t code;
			std::uint64_t fp, po, co;
			std::uint32_t last;
			std::string_view impacts;
		};
		std::vector<blk> bl;
		std::uint64_t tail_pos = 0, tail_off = 0;
		const bool skip = !(info.flags & format::single_posting) && df >= format::block;
		++st.terms;
		st.postings += df;
		if (info.flags & format::single_posting) {
			L.ords = {std::uint32_t(info.postings)};
			L.wdfs = {info.single_wdf};
		} else if (!skip) {
			const char* p = P_.data() + info.postings;
			const char* end = p + info.postings_length;
			std::uint32_t prev = ~0u;
			for (std::uint32_t i = 0; i < df; ++i) { L.ords.push_back(prev += std::uint32_t(leb(p, end))); }
			for (std::uint32_t i = 0; i < df; ++i) { L.wdfs.push_back(no_wdf ? 0 : std::uint32_t(leb(p, end))); }
			if (p != end) { fail(term, "tail-only list length"); }
			st.tail_postings += df;
		} else {
			const char* p = P_.data() + info.postings;
			const char* end = p + info.postings_length;
			const std::uint64_t groups = leb(p, end);
			const bool wide = *p++;
			if (!no_pos) {
				tail_pos = leb(p, end);
				tail_off = leb(p, end);
			}
			const std::uint64_t pairs_size = leb(p, end), l0_size = leb(p, end), blocks_size = leb(p, end);
			const std::size_t ow = wide ? 8 : 4;
			const std::size_t l1_size = 8 + 2 * ow + (no_pos ? 0 : 3 * ow);
			const char* l1 = p;
			const char* l0 = l1 + groups * l1_size + pairs_size;
			const char* const l0_begin = l0;
			const char* const l0_end = l0 + l0_size;
			const char* const blocks = l0_end;
			const char* b = blocks;
			const char* const tail = blocks + blocks_size;
			auto get_off = [&](const char*& q) {
				const std::uint64_t v = wide ? format::get<std::uint64_t>(q) : format::get<std::uint32_t>(q);
				q += ow;
				return v;
			};
			const unsigned shift = bit14_ ? 2 : 1;
			std::int64_t prev_last = -1;
			std::uint32_t delta_prev = 0;
			std::size_t index = 0;
			for (std::uint64_t g = 0; g < groups; ++g) {
				const char* q = l1 + g * l1_size;
				const std::uint32_t g_last = format::get<std::uint32_t>(q);
				q += 8;  // last, pairs offset
				const std::uint64_t g_l0 = get_off(q), g_blocks = get_off(q);
				std::uint64_t fp = 0, po = 0, co = 0;
				if (!no_pos) { fp = get_off(q), po = get_off(q), co = get_off(q); }
				if (g_l0 != std::uint64_t(l0 - l0_begin) || g_blocks != std::uint64_t(b - blocks)) { fail(term, "level-1 offsets"); }
				for (int i = 0; i < 32 && l0 < l0_end; ++i, ++index) {
					blk k{};
					k.last = (index ? delta_prev : 0) + std::uint32_t(leb(l0, l0_end));
					delta_prev = k.last;
					const std::uint64_t bb = leb(l0, l0_end);
					const std::size_t bytes = std::size_t(bb >> shift);
					k.code = std::uint8_t(bb & ((1u << shift) - 1));
					if (!no_pos) {
						fp += leb(l0, l0_end), po += leb(l0, l0_end);
						if (with_counts) { co += leb(l0, l0_end); }
					}
					k.fp = fp, k.po = po, k.co = co;
					k.impacts = format::skip_sized_impacts(&l0, l0_end);
					std::int64_t base = prev_last;
					if (bit14_) { base = std::max<std::int64_t>(base, std::int64_t(runs_[run_of(k.last)]) - 1); }
					const std::uint32_t ubase = std::uint32_t(base);
					std::uint32_t out[format::block];
					std::size_t used = 0, n = format::block;
					if (k.code == 0) {
						used = bitpack::decode_delta(reinterpret_cast<const std::uint8_t*>(b), bytes, ubase, out);
						if (!used) { fail(term, "FOR block"); }
					} else if (k.code == 1) {
						const std::uint64_t span = std::uint64_t(k.last) - std::uint32_t(ubase + 1) + 1;
						std::size_t c = 0;
						for (std::uint64_t at = 0; at < span; ++at) {
							if ((std::uint8_t(b[at >> 3]) >> (at & 7)) & 1) {
								if (c == format::block) { fail(term, "bitset count"); }
								out[c++] = std::uint32_t(ubase + 1 + at);
							}
						}
						if (c != format::block) { fail(term, "bitset count"); }
						used = std::size_t((span + 7) / 8);
						++st.bitsets;
					} else if (k.code == 2 && bit14_) {
						const char* s = b;
						n = std::size_t(leb(s, b + bytes));
						if (n == 0 || n >= format::block) { fail(term, "short block count"); }
						std::uint32_t prev = ubase;
						for (std::size_t i2 = 0; i2 < n; ++i2) { out[i2] = prev += std::uint32_t(leb(s, b + bytes)); }
						used = std::size_t(s - b);
						++st.shorts;
						st.short_postings += n;
					} else {
						fail(term, "block code");
					}
					if (out[n - 1] != k.last) { fail(term, "block last"); }
					L.ords.insert(L.ords.end(), out, out + n);
					if (no_wdf) {
						L.wdfs.insert(L.wdfs.end(), n, 0u);
						if (used != bytes) { fail(term, "block bytes"); }
					} else if (k.code == 2) {
						const char* s = b + used;
						for (std::size_t i2 = 0; i2 < n; ++i2) { L.wdfs.push_back(std::uint32_t(leb(s, b + bytes))); }
						if (s != b + bytes) { fail(term, "short block bytes"); }
					} else {
						std::uint32_t w[format::block];
						const std::size_t wu = bitpack::decode(reinterpret_cast<const std::uint8_t*>(b + used), bytes - used, w);
						if (!wu || used + wu != bytes) { fail(term, "wdf block"); }
						L.wdfs.insert(L.wdfs.end(), w, w + format::block);
					}
					k.n = std::uint32_t(n);
					b += bytes;
					prev_last = k.last;
					bl.push_back(k);
					++st.blocks;
				}
				if (bl.back().last != g_last) { fail(term, "level-1 last"); }
			}
			if (l0 != l0_end || b != tail) { fail(term, "skip data lengths"); }
			p = tail;
			const std::size_t rest = df - L.ords.size();
			std::uint32_t prev = std::uint32_t(prev_last);
			for (std::size_t i = 0; i < rest; ++i) { L.ords.push_back(prev += std::uint32_t(leb(p, end))); }
			for (std::size_t i = 0; i < rest; ++i) { L.wdfs.push_back(no_wdf ? 0 : std::uint32_t(leb(p, end))); }
			if (p != end) { fail(term, "list length"); }
			st.tail_postings += rest;
			// Each block's pairs against its postings.
			std::size_t at = 0;
			for (const auto& k : bl) {
				std::vector<format::impact> pairs;
				for (std::size_t i = at; i < at + k.n; ++i) { pairs.push_back({L.wdfs[i], seg_.norm(L.ords[i])}); }
				std::string want;
				format::encode_sized_impacts(want, format::competitive(pairs));
				if (want != k.impacts) { fail(term, "block impacts"); }
				at += k.n;
			}
		}
		if (L.ords.size() != df || L.wdfs.size() != df) { fail(term, "posting count"); }
		for (std::size_t i = 1; i < df; ++i) {
			if (L.ords[i] <= L.ords[i - 1]) { fail(term, "ordinals not increasing"); }
		}
		// Pieces: one per run the list touches (format 12: one).
		std::vector<std::size_t> piece_end;  // in postings
		for (std::size_t i = 0; i < df; ++i) {
			if (i + 1 == df || (bit14_ && run_of(L.ords[i + 1]) != run_of(L.ords[i]))) { piece_end.push_back(i + 1); }
		}
		if (no_pos) {
			L.counts.assign(df, 0);
			return;
		}
		const char* q = Q_.data() + info.positions;
		const char* const qend = q + info.positions_length;
		std::vector<std::uint64_t> count_off(bl.size());
		if (!cw) {
			const std::uint64_t before_tail = leb(q, qend);
			const char* const cdata = q;
			std::size_t at = 0;
			for (std::size_t i = 0; i < bl.size(); ++i) {
				count_off[i] = std::uint64_t(q - cdata);
				if (bl[i].code == 2) {
					for (std::uint32_t c = 0; c < bl[i].n; ++c) { L.counts.push_back(std::uint32_t(leb(q, qend))); }
				} else {
					std::uint32_t c[format::block];
					const std::size_t u = bitpack::decode(reinterpret_cast<const std::uint8_t*>(q), std::size_t(qend - q), c);
					if (!u) { fail(term, "count block"); }
					q += u;
					L.counts.insert(L.counts.end(), c, c + format::block);
				}
				at += bl[i].n;
				if (bl[i].co != count_off[i]) { fail(term, "count offset"); }
			}
			// The rest: the list's tail, or every piece's counts in a list without blocks; the
			// header counts the bytes before the last piece's.
			std::uint64_t last_tail = std::uint64_t(q - cdata);
			for (std::size_t k = 0; k < piece_end.size(); ++k) {
				const std::size_t from = std::max(at, k ? piece_end[k - 1] : 0);
				if (from >= piece_end[k]) { continue; }
				if (k + 1 == piece_end.size()) { last_tail = std::uint64_t(q - cdata); }
				for (std::size_t i = from; i < piece_end[k]; ++i) { L.counts.push_back(std::uint32_t(leb(q, qend))); }
			}
			if (L.counts.size() != df) { fail(term, "count total"); }
			if (before_tail != last_tail) { fail(term, "count bytes before the tail"); }
		} else {
			L.counts = L.wdfs;
		}
		const std::uint64_t full_hdr = leb(q, qend), before_tail = leb(q, qend);
		const char* const pdata = q;
		std::uint64_t full = 0, last_tail = 0, first = 0;
		struct piece_layout {
			std::uint64_t first;
			std::vector<std::uint64_t> offsets;
			std::uint64_t tail;
		};
		std::vector<piece_layout> layout;
		std::size_t from = 0;
		for (std::size_t end_i : piece_end) {
			std::uint64_t m = 0;
			for (std::size_t i = from; i < end_i; ++i) { m += L.counts[i]; }
			piece_layout pl{first, {}, 0};
			for (std::uint64_t j = 0; j < m / format::block; ++j) {
				pl.offsets.push_back(std::uint64_t(q - pdata));
				std::uint32_t v[format::block];
				const std::size_t u = bitpack::decode(reinterpret_cast<const std::uint8_t*>(q), std::size_t(qend - q), v);
				if (!u) { fail(term, "position block"); }
				q += u;
				L.positions.insert(L.positions.end(), v, v + format::block);
				++full, ++st.pos_blocks;
			}
			pl.tail = last_tail = std::uint64_t(q - pdata);
			for (std::uint64_t j = 0; j < m % format::block; ++j) { L.positions.push_back(std::uint32_t(leb(q, qend))); }
			st.pos_tail_values += m % format::block;
			first += m;
			layout.push_back(std::move(pl));
			from = end_i;
		}
		if (q != qend) { fail(term, "positions length"); }
		if (full != full_hdr || before_tail != last_tail) { fail(term, "position headers"); }
		if (skip) {
			auto offset_of = [&](std::size_t piece, std::uint64_t position) {
				const piece_layout& pl = layout[piece];
				const std::uint64_t j = (position - pl.first) / format::block;
				return j < pl.offsets.size() ? pl.offsets[j] : pl.tail;
			};
			std::uint64_t position = 0;
			std::size_t at = 0;
			std::size_t piece = 0;
			for (const auto& k : bl) {
				while (piece_end[piece] <= at) { ++piece; }
				if (k.fp != position || k.po != offset_of(piece, position)) { fail(term, "block position fields"); }
				for (std::size_t i = at; i < at + k.n; ++i) { position += L.counts[i]; }
				at += k.n;
			}
			while (at < df && piece_end[piece] <= at) { ++piece; }
			if (at == df) { piece = piece_end.size() - 1; }
			if (tail_pos != position || tail_off != offset_of(piece, position)) { fail(term, "tail position fields"); }
		}
	}

private:
	const segment& seg_;
	bool bit14_;
	std::string_view P_, Q_;
	std::vector<std::uint32_t> runs_;
};

// A's lists through the real reader.
void check_reader(const segment& seg, std::string_view term, const format::term_info& info, const decoded& L) {
	postings_iterator it(seg, info);
	positions_reader pr(seg, info);
	std::size_t i = 0, at = 0;
	for (ordinal_t d = it.next(); d != no_more_docs; d = it.next(), ++i) {
		if (i >= L.ords.size() || d != L.ords[i] || it.wdf() != L.wdfs[i]) { fail(term, "decoder against the reader: postings"); }
		if (!(info.flags & format::no_positions)) {
			const auto ps = pr.read(it);
			if (ps.size() != L.counts[i]) { fail(term, "decoder against the reader: count"); }
			std::uint32_t pos = 0;
			for (std::size_t k = 0; k < ps.size(); ++k) {
				if (ps[k] != (pos += L.positions[at + k])) { fail(term, "decoder against the reader: position"); }
			}
			at += ps.size();
		}
	}
	if (i != L.ords.size()) { fail(term, "decoder against the reader: df"); }
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 3) {
		std::cerr << "usage: encode_once_check TODAY.seg ENCODE_ONCE.seg\n";
		return 2;
	}
	const std::string fa = slurp(argv[1]), fb = slurp(argv[2]);
	const segment a(fa), b(fb);
	std::map<int, std::pair<std::uint64_t, std::uint64_t>> sizes;
	std::map<int, std::string_view> bytes_a, bytes_b;
	for (const auto& e : a.sections()) {
		sizes[int(e.kind)].first = e.length;
		bytes_a[int(e.kind)] = std::string_view(fa).substr(e.offset, e.length);
	}
	for (const auto& e : b.sections()) {
		sizes[int(e.kind)].second = e.length;
		bytes_b[int(e.kind)] = std::string_view(fb).substr(e.offset, e.length);
	}
	const char* names[] = {"header", "terms", "term_index", "postings", "positions", "lengths", "stored", "docids", "stats", "norms", "versions", "pk_filter", "runs"};
	std::printf("%-12s %14s %14s %8s\n", "section", "today", "encode-once", "ratio");
	for (const auto& [k, s] : sizes) {
		std::printf("%-12s %14llu %14llu %8.4f\n", k < 13 ? names[k] : "?", (unsigned long long)s.first, (unsigned long long)s.second, s.first ? double(s.second) / double(s.first) : 0.0);
	}
	std::printf("%-12s %14zu %14zu %8.4f\n", "file", fa.size(), fb.size(), double(fb.size()) / double(fa.size()));
	for (int k : {0, 5, 6, 7, 8, 9, 10, 11}) {
		if (bytes_a[k] != bytes_b[k]) {
			std::cerr << "MISMATCH: section " << names[k] << " differs\n";
			return 1;
		}
	}
	decoder da(a, fa, false), db(b, fb, true);
	segment::term_cursor ca(a), cb(b);
	decoded la, lb;
	for (;;) {
		const bool na = ca.next(), nb = cb.next();
		if (na != nb) { fail(na ? ca.term() : cb.term(), "dictionary length"); }
		if (!na) { break; }
		const auto& ia = ca.info();
		const auto& ib = cb.info();
		if (ca.term() != cb.term()) { fail(ca.term(), "dictionary term"); }
		const bool single = ia.flags & format::single_posting;
		if (ia.df != ib.df || ia.cf != ib.cf || ia.flags != ib.flags || (single && (ia.postings != ib.postings || ia.single_wdf != ib.single_wdf))) {
			fail(ca.term(), "term info");
		}
		if (ia.impacts.size() != ib.impacts.size()) { fail(ca.term(), "term impacts"); }
		for (std::size_t i = 0; i < ia.impacts.size(); ++i) {
			if (ia.impacts[i].wdf != ib.impacts[i].wdf || ia.impacts[i].length != ib.impacts[i].length) { fail(ca.term(), "term impacts"); }
		}
		da.decode(ca.term(), ia, la);
		check_reader(a, ca.term(), ia, la);
		db.decode(cb.term(), ib, lb);
		if (la.ords != lb.ords) { fail(ca.term(), "ordinals"); }
		if (la.wdfs != lb.wdfs) { fail(ca.term(), "wdfs"); }
		if (la.counts != lb.counts) { fail(ca.term(), "counts"); }
		if (la.positions != lb.positions) { fail(ca.term(), "positions"); }
	}
	auto show = [](const char* name, const stats& s) {
		std::printf("%s: %llu terms, %llu postings; %llu blocks (%llu bitsets, %llu short holding %llu postings), %llu postings in tails; %llu position blocks, %llu positions in tails\n",
			name, (unsigned long long)s.terms, (unsigned long long)s.postings, (unsigned long long)s.blocks, (unsigned long long)s.bitsets, (unsigned long long)s.shorts,
			(unsigned long long)s.short_postings, (unsigned long long)s.tail_postings, (unsigned long long)s.pos_blocks, (unsigned long long)s.pos_tail_values);
	};
	show("today", da.st);
	show("encode-once", db.st);
	std::printf("OK: every list equal (today's decoded by the reader too); other sections byte-identical\n");
	return 0;
}
