// Open cost of a small index by its file work and decode alone (no strata index code): main's
// files (a manifest, metadata and field table, decoded by strata's own files.hh and fields.hh at
// main 2769928, after recover's scan opens every file for its size, index.hh:4895-4913) against one
// manifest log read whole and replayed. The log is journal's framing at 512-byte blocks
// (turn_log.h at journal:bfad97f) holding a snapshot, whose parts are main's own encodings (D1),
// then N edits of D3's revision-1 shape (a presence mask of changed scalars, then sections),
// decoded and applied by a model decoder here with running totals (ML-R12).
// Not part of the CMake build; docs/design/manifest-log.md quotes its output.
// Build: c++ -std=c++20 -O2 -I<strata>/include -I<deps>/crc32c-src/include -I<deps>/varint-src
//   -I<deps>/varint-src/include -I<deps>/hashes-src -I<deps>/termdict-src -o open_probe open_probe.cc
// Usage: open_probe DIR [rounds [reps]], under a strict hold.
#include "strata/core/fields.hh"
#include "strata/core/files.hh"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <random>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace strata;

static double cpu() {
	rusage r;
	getrusage(RUSAGE_SELF, &r);
	return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}
[[noreturn]] static void die(const char* what) {
	std::perror(what);
	std::exit(1);
}

// --- LEB128 -------------------------------------------------------------------------------
static void put(std::string& o, std::uint64_t v) {
	while (v >= 0x80) {
		o.push_back(char(v | 0x80));
		v >>= 7;
	}
	o.push_back(char(v));
}
struct reader {
	const unsigned char* p;
	const unsigned char* end;
	std::uint64_t get() {
		std::uint64_t v = 0;
		for (int s = 0; s < 64; s += 7) {
			if (p == end) { throw std::runtime_error("short"); }
			const unsigned b = *p++;
			v |= std::uint64_t(b & 0x7f) << s;
			if (!(b & 0x80)) { return v; }
		}
		throw std::runtime_error("long");
	}
	std::string_view bytes(std::size_t n) {
		if (std::size_t(end - p) < n) { throw std::runtime_error("short"); }
		std::string_view v(reinterpret_cast<const char*>(p), n);
		p += n;
		return v;
	}
};

// --- journal's framing at 512-byte blocks, no block manifest ------------------------------
constexpr std::size_t block = 512, hb = 62, frag = 24;
static void put16(std::string& o, std::uint16_t v) { o.append({char(v), char(v >> 8)}); }
static void put32(std::string& o, std::uint32_t v) { for (int i = 0; i < 4; ++i) { o.push_back(char(v >> 8 * i)); } }
static void put64(std::string& o, std::uint64_t v) { for (int i = 0; i < 8; ++i) { o.push_back(char(v >> 8 * i)); } }
static std::uint64_t g64(const char* p) {
	std::uint64_t v;
	std::memcpy(&v, p, 8);
	return v;
}
static std::uint32_t g32(const char* p) {
	std::uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}
static const char ident[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr std::uint64_t seg_no = 5, epoch = 1;

struct emitter {
	std::string out;
	void enter() {
		if (out.size() % block) { return; }
		std::string h("STRLOG01", 8);
		h.append(ident, 16);
		put64(h, seg_no);
		put32(h, std::uint32_t(out.size() / block));
		put64(h, epoch);
		put64(h, 0);
		put32(h, 0);
		put16(h, 0xffff);
		put32(h, kronuz::crc32c::Crc32c().update(h).value());
		out += h;
	}
	void record(std::string_view rest) {
		bool first = true;
		while (!rest.empty()) {
			enter();
			const std::size_t room = block - out.size() % block;
			if (room < frag + 1) {
				out.append(room, '\0');
				continue;
			}
			const std::size_t n = std::min(rest.size(), room - frag);
			const bool last = n == rest.size();
			const char t = first ? (last ? 1 : 2) : (last ? 4 : 3);
			std::string h;
			put16(h, std::uint16_t(n));
			h.push_back(t);
			h.push_back(0);
			put64(h, seg_no);
			put64(h, epoch);
			put32(out, kronuz::crc32c::Crc32c().update(h).update(rest.substr(0, n)).value());
			out += h;
			out.append(rest.substr(0, n));
			rest.remove_prefix(n);
			first = false;
		}
	}
};

// --- the state, and the records ----------------------------------------------------------
struct state {
	files::manifest m;  // segments in id order; totals as running sums
	std::vector<std::vector<std::uint64_t>> bits;
	std::map<std::string, std::string> meta;
	field_table fields;
};
// Scalars below `docs` are assigned (total length too: segments do not record it); the totals from
// `docs` on are checked against running sums.
enum scalar : unsigned { revision, last_docid, next_segment, applied_index, applied_applied, schema, configured, mask, total, docs, live, live_len, n_scalars };
enum section : unsigned { seg_removed, seg_added, new_deletes, meta_set, meta_removed, decls, n_sections };

static std::uint64_t& scalar_ref(files::manifest& m, unsigned s) {
	switch (s) {
	case revision: return m.revision;
	case last_docid: return m.last_docid;
	case next_segment: return m.next_segment;
	case applied_index: return m.applied.index;
	case applied_applied: return m.applied.applied;
	case schema: return m.schema_version;
	case configured: return m.configured;
	case mask: return m.mask;
	case total: return m.total_length;
	case docs: return m.doc_count;
	case live: return m.live_count;
	default: return m.live_length;
	}
}
static void put_entry(std::string& o, const files::segment_ref& s) {
	put(o, s.id);
	o.append(reinterpret_cast<const char*>(s.identity.data()), 16);
	put(o, s.size);
	put32(o, s.footer_crc);
	put(o, s.doc_count);
	put(o, s.live_count);
	put(o, s.live_length);
	put(o, 0);  // deleted set: none (a flush's segment)
	put(o, s.values_generation);
	put(o, 0);  // attributes
}

static void snapshot_record(std::string& o, const state& st) {
	o.push_back(1);
	put(o, st.m.generation);
	const std::string a = files::encode(st.m), b = files::encode(files::metadata{1, st.meta}), c = files::encode_fields(st.fields);
	for (const std::string* p : {&a, &b, &c}) {
		put(o, p->size());
		o += *p;
	}
}

// Replays one logical record onto `st`; throws on any check.
static void apply_record(state& st, std::string_view rec) {
	reader r{reinterpret_cast<const unsigned char*>(rec.data()), reinterpret_cast<const unsigned char*>(rec.data() + rec.size())};
	const auto kind = *r.p++;
	const std::uint64_t gen = r.get();
	if (kind == 1) {
		st.m = files::decode_manifest(r.bytes(r.get()));
		st.meta = files::decode_metadata(r.bytes(r.get())).entries;
		st.fields = field_table{};
		files::decode_fields(r.bytes(r.get()), st.fields);
		st.bits.assign(st.m.segments.size(), {});
		for (std::size_t i = 0; i < st.m.segments.size(); ++i) { st.bits[i].assign((st.m.segments[i].doc_count + 63) / 64, 0); }
		return;
	}
	if (gen != st.m.generation + 1) { throw std::runtime_error("generation"); }
	st.m.generation = gen;
	auto& m = st.m;
	const std::uint64_t present = r.get();
	std::uint64_t carried[n_scalars];
	for (unsigned s = 0; s < n_scalars; ++s) {
		if (present >> s & 1) { carried[s] = r.get(); }
	}
	for (unsigned s = 0; s < docs; ++s) {
		if (present >> s & 1) { scalar_ref(m, s) = carried[s]; }
	}
	const std::uint64_t sections = r.get();
	auto find = [&](std::uint64_t id) {
		auto it = std::lower_bound(m.segments.begin(), m.segments.end(), id, [](const auto& s, std::uint64_t x) { return s.id < x; });
		if (it == m.segments.end() || it->id != id) { throw std::runtime_error("no such segment"); }
		return std::size_t(it - m.segments.begin());
	};
	if (sections >> seg_removed & 1) {
		std::uint64_t id = 0;
		for (std::uint64_t n = r.get(); n--;) {
			id += r.get();
			const std::size_t i = find(id);
			m.doc_count -= m.segments[i].doc_count;
			m.live_count -= m.segments[i].live_count;
			m.live_length -= m.segments[i].live_length;
			m.segments.erase(m.segments.begin() + std::ptrdiff_t(i));
			st.bits.erase(st.bits.begin() + std::ptrdiff_t(i));
		}
	}
	if (sections >> seg_added & 1) {
		for (std::uint64_t n = r.get(); n--;) {
			files::segment_ref s;
			s.id = r.get();
			std::memcpy(s.identity.data(), r.bytes(16).data(), 16);
			s.size = r.get();
			s.footer_crc = g32(r.bytes(4).data());
			s.doc_count = r.get();
			s.live_count = r.get();
			s.live_length = r.get();
			if (r.get() != 0) { throw std::runtime_error("deleted set"); }
			s.values_generation = r.get();
			if (r.get() != 0) { throw std::runtime_error("attributes"); }
			if (s.id >= m.next_segment) { throw std::runtime_error("id"); }
			auto it = std::lower_bound(m.segments.begin(), m.segments.end(), s.id, [](const auto& x, std::uint64_t v) { return x.id < v; });
			if (it != m.segments.end() && it->id == s.id) { throw std::runtime_error("duplicate"); }
			const auto at = it - m.segments.begin();
			m.doc_count += s.doc_count;
			m.live_count += s.live_count;
			m.live_length += s.live_length;
			st.bits.insert(st.bits.begin() + at, std::vector<std::uint64_t>((s.doc_count + 63) / 64, 0));
			m.segments.insert(it, std::move(s));
		}
	}
	if (sections >> new_deletes & 1) {
		for (std::uint64_t n = r.get(); n--;) {
			const std::size_t i = find(r.get());
			auto& s = m.segments[i];
			std::uint64_t o = 0;
			for (std::uint64_t k = r.get(), first = 1; k--; first = 0) {
				o += r.get() + (first ? 0 : 1);
				if (o >= s.doc_count || (st.bits[i][o >> 6] >> (o & 63) & 1)) { throw std::runtime_error("ordinal"); }
				st.bits[i][o >> 6] |= std::uint64_t(1) << (o & 63);
			}
			const std::uint64_t lc = r.get(), ll = r.get();
			m.live_count = m.live_count - s.live_count + lc;
			m.live_length = m.live_length - s.live_length + ll;
			s.live_count = lc;
			s.live_length = ll;
			s.live = {1, 0, 0};  // main's decoder wants a live-docs reference beside deletes
		}
	}
	if (sections >> meta_set & 1) {
		for (std::uint64_t n = r.get(); n--;) {
			std::string k(r.bytes(r.get()));
			st.meta[std::move(k)] = std::string(r.bytes(r.get()));
		}
	}
	if (sections >> meta_removed & 1) {
		for (std::uint64_t n = r.get(); n--;) {
			if (!st.meta.erase(std::string(r.bytes(r.get())))) { throw std::runtime_error("key"); }
		}
	}
	if (sections >> decls & 1) {
		if (r.get() != st.fields.size()) { throw std::runtime_error("table size"); }
		for (std::uint64_t n = r.get(); n--;) {
			field_declaration d;
			d.name = std::string(r.bytes(r.get()));
			d.role = field_role(r.get());
			d.postings = field_postings(r.get());
			d.norms = r.get();
			if (field_problem(d) || st.fields.find(d.name)) { throw std::runtime_error("declaration"); }
			st.fields.append(std::move(d));
		}
	}
	// Carried totals check against the running sums (ML-R12), never a recount.
	for (unsigned s = docs; s < n_scalars; ++s) {
		if ((present >> s & 1) && carried[s] != scalar_ref(m, s)) { throw std::runtime_error("totals"); }
	}
	if (r.p != r.end) { throw std::runtime_error("trailing"); }
}

// Reads every record of a log image and replays it; returns the generation.
static std::uint64_t replay(const std::string& img, state& st) {
	std::string rec;
	bool assembling = false;
	for (std::size_t b = 0; b < img.size(); b += block) {
		const char* h = img.data() + b;
		if (img.size() - b < hb || std::memcmp(h, "STRLOG01", 8) || std::memcmp(h + 8, ident, 16) || g64(h + 24) != seg_no || g32(h + 32) != b / block ||
			g64(h + 36) != epoch || g32(h + hb - 4) != kronuz::crc32c::Crc32c().update(h, hb - 4).value()) {
			throw std::runtime_error("block header");
		}
		for (std::size_t p = b + hb; block - (p - b) >= frag + 1 && p < img.size();) {
			const char* f = img.data() + p;
			std::uint16_t len;
			std::memcpy(&len, f + 4, 2);
			const char t = f[6];
			if (!len || frag + len > block - (p - b) || p + frag + len > img.size() || t < 1 || t > 4 || f[7] || g64(f + 8) != seg_no || g64(f + 16) != epoch ||
				kronuz::crc32c::Crc32c().update(f + 4, frag - 4).update(f + frag, len).value() != g32(f)) {
				throw std::runtime_error("fragment");
			}
			if (t == 1 || t == 2) {
				rec.assign(f + frag, len);
				assembling = t == 2;
			} else {
				rec.append(f + frag, len);
				assembling = t == 3;
			}
			if (!assembling) { apply_record(st, rec); }
			p += frag + len;
		}
	}
	return st.m.generation;
}

// --- the model index ----------------------------------------------------------------------
// A small index under synchronous commits: each commit flushes a segment; every tenth merges the
// ten newest into one; one in six deletes three documents in an older segment; one in twenty sets
// a metadata key. Every edit carries the scalars that changed.
struct model {
	state st;
	std::mt19937_64 rnd{42};
	std::uint64_t docid = 0;

	std::uint64_t flush_docs = 1;  // documents per flush, times 1 to 3

	model() {
		st.m.uuid.fill('u');
		st.m.generation = 1;
		st.m.next_segment = 1;
		st.m.configured = st.m.mask = 0x3f;
		st.m.min_version = st.m.max_version = 14;
		st.m.next_live = 2;
		st.meta = {{"owner", "the-host-application"}, {"schema", "v12"}};
		for (int i = 0; i < 4; ++i) { st.fields.append(field_declaration{"field-" + std::to_string(i), field_role::indexed, field_postings::freqs, false}); }
		add_segment(1);
	}
	files::segment_ref new_segment(std::uint64_t docs) {
		files::segment_ref s;
		s.id = st.m.next_segment++;
		for (auto& b : s.identity) { b = std::uint8_t(rnd()); }
		s.size = 1088 + 300 * docs;
		s.footer_crc = std::uint32_t(rnd());
		s.doc_count = s.live_count = docs;
		s.live_length = 12 * docs;
		return s;
	}
	void add_segment(std::uint64_t docs) {
		auto s = new_segment(docs);
		st.m.doc_count += docs;
		st.m.live_count += docs;
		st.m.live_length += s.live_length;
		st.m.total_length += s.live_length;
		st.m.last_docid = docid += docs;
		st.bits.emplace_back((docs + 63) / 64, 0);
		st.m.segments.push_back(std::move(s));
	}
	// The next edit's bytes: the state advances with it.
	std::string next_edit(std::uint64_t n) {
		const files::manifest before = st.m;
		const auto before_meta = st.meta;
		std::string sec[n_sections];
		std::uint64_t sections = 0;
		++st.m.revision;
		++st.m.applied.index;
		if (n % 10 == 9 && st.m.segments.size() >= 10) {  // a merge of the ten newest
			std::uint64_t docs = 0, id = 0;
			put(sec[seg_removed], 10);
			for (std::size_t i = st.m.segments.size() - 10; i < st.m.segments.size(); ++i) {
				docs += st.m.segments[i].live_count;
				put(sec[seg_removed], st.m.segments[i].id - id);
				id = st.m.segments[i].id;
			}
			for (int i = 0; i < 10; ++i) {
				const auto& s = st.m.segments.back();
				st.m.doc_count -= s.doc_count;
				st.m.live_count -= s.live_count;
				st.m.live_length -= s.live_length;
				st.m.segments.pop_back();
				st.bits.pop_back();
			}
			auto s = new_segment(docs);
			st.m.doc_count += docs;
			st.m.live_count += docs;
			st.m.live_length += s.live_length;
			put(sec[seg_added], 1);
			put_entry(sec[seg_added], s);
			st.bits.emplace_back((docs + 63) / 64, 0);
			st.m.segments.push_back(std::move(s));
			sections |= 1u << seg_removed | 1u << seg_added;
		} else {
			add_segment(flush_docs * (1 + rnd() % 3));
			put(sec[seg_added], 1);
			put_entry(sec[seg_added], st.m.segments.back());
			sections |= 1u << seg_added;
		}
		if (n % 6 == 5) {  // three deletes in the oldest segment with live documents
			for (std::size_t i = 0; i < st.m.segments.size(); ++i) {
				auto& s = st.m.segments[i];
				if (s.live_count < 4) { continue; }
				put(sec[new_deletes], 1);
				put(sec[new_deletes], s.id);
				put(sec[new_deletes], 3);
				std::uint64_t prev = 0, k = 0;
				for (std::uint64_t o = 0; o < s.doc_count && k < 3; ++o) {
					if (st.bits[i][o >> 6] >> (o & 63) & 1) { continue; }
					st.bits[i][o >> 6] |= std::uint64_t(1) << (o & 63);
					put(sec[new_deletes], k ? o - prev - 1 : o);
					prev = o;
					++k;
				}
				s.live_count -= 3;
				s.live_length -= 36;
				s.live = {1, 0, 0};
				st.m.live_count -= 3;
				st.m.live_length -= 36;
				put(sec[new_deletes], s.live_count);
				put(sec[new_deletes], s.live_length);
				sections |= 1u << new_deletes;
				break;
			}
		}
		if (n % 20 == 19) {
			const std::string k = "checkpoint", v = std::to_string(n);
			st.meta[k] = v;
			put(sec[meta_set], 1);
			put(sec[meta_set], k.size());
			sec[meta_set] += k;
			put(sec[meta_set], v.size());
			sec[meta_set] += v;
			sections |= 1u << meta_set;
		}
		return encode_edit(before, sections, sec);
	}
	// A delete-only commit: one document deleted in the segment with the most live documents.
	std::string next_delete_edit() {
		const files::manifest before = st.m;
		std::string sec[n_sections];
		++st.m.revision;
		++st.m.applied.index;
		std::size_t i = 0;
		for (std::size_t j = 1; j < st.m.segments.size(); ++j) {
			if (st.m.segments[j].live_count > st.m.segments[i].live_count) { i = j; }
		}
		auto& s = st.m.segments[i];
		if (!s.live_count) { die("no live document"); }
		std::uint64_t o = 0;
		while (st.bits[i][o >> 6] >> (o & 63) & 1) { ++o; }
		st.bits[i][o >> 6] |= std::uint64_t(1) << (o & 63);
		--s.live_count;
		s.live_length -= 12;
		s.live = {1, 0, 0};
		--st.m.live_count;
		st.m.live_length -= 12;
		for (std::uint64_t v : {std::uint64_t(1), s.id, std::uint64_t(1), o, s.live_count, s.live_length}) { put(sec[new_deletes], v); }
		return encode_edit(before, 1u << new_deletes, sec);
	}
	std::string encode_edit(const files::manifest& before, std::uint64_t sections, const std::string (&sec)[n_sections]) {
		std::string o;
		o.push_back(2);
		put(o, ++st.m.generation);
		std::uint64_t present = 0;
		for (unsigned s = 0; s < n_scalars; ++s) {
			if (scalar_ref(st.m, s) != scalar_ref(const_cast<files::manifest&>(before), s)) { present |= 1u << s; }
		}
		put(o, present);
		for (unsigned s = 0; s < n_scalars; ++s) {
			if (present >> s & 1) { put(o, scalar_ref(st.m, s)); }
		}
		put(o, sections);
		for (unsigned s = 0; s < n_sections; ++s) {
			if (sections >> s & 1) { o += sec[s]; }
		}
		return o;
	}
};

static void write_file(const std::string& dir, const std::string& name, const std::string& bytes) {
	const std::string p = dir + "/" + name;
	const int fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (fd < 0 || ::write(fd, bytes.data(), bytes.size()) != ssize_t(bytes.size())) { die("write"); }
	::close(fd);
}
static std::uint64_t size_of(const std::string& p) {  // recover's scan: open, size, close
	const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) { die("open"); }
	struct stat s;
	if (::fstat(fd, &s)) { die("fstat"); }
	::close(fd);
	return std::uint64_t(s.st_size);
}
static std::string read_all(const std::string& p) {
	const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) { die("open"); }
	struct stat s;
	if (::fstat(fd, &s)) { die("fstat"); }
	std::string b(std::size_t(s.st_size), '\0');
	if (::pread(fd, b.data(), b.size(), 0) != ssize_t(b.size())) { die("read"); }
	::close(fd);
	return b;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s DIR [rounds]\n", argv[0]);
		return 2;
	}
	const int rounds = argc > 2 ? std::atoi(argv[2]) : 1;
	const int reps = argc > 3 ? std::atoi(argv[3]) : 3000;
	volatile std::uint64_t sink = 0;
	for (int round = 0; round < rounds; ++round) {
		const std::string dir = std::string(argv[1]) + "/r" + std::to_string(round);
		if (::mkdir(dir.c_str(), 0700) && errno != EEXIST) { die("mkdir"); }
		std::printf("== round %d in %s\n", round, dir.c_str());

		// Main's files for the model index after 200 commits (its state; the manifest names them).
		model mm;
		for (std::uint64_t n = 0; n < 200; ++n) { (void)mm.next_edit(n); }
		files::manifest man = mm.st.m;
		man.next_meta = man.next_fields = 3;
		const std::string mb = files::encode(files::metadata{2, mm.st.meta}), fb = files::encode_fields(mm.st.fields);
		man.meta = {2, mb.size(), files::trailing_crc(mb)};
		man.fields = {2, fb.size(), files::trailing_crc(fb)};
		// Main's live-docs files for segments with deletes are left out: that understates main.
		write_file(dir, "manifest-201", files::encode(man));
		write_file(dir, "meta-2.kv", mb);
		write_file(dir, "fields-2.kv", fb);
		write_file(dir, "seg-1.sx", std::string(4096, 's'));
		const std::vector<std::string> main_files = {"manifest-201", "seg-1.sx", "meta-2.kv", "fields-2.kv"};

		auto time = [&](const char* what, int n, auto&& body) {
			const double c0 = cpu();
			for (int i = 0; i < n; ++i) { body(); }
			const double us = (cpu() - c0) / n * 1e6;
			std::printf("%-58s %8.2f us cpu/op  (n=%d)\n", what, us, n);
			return us;
		};
		time("main: scan 4 files, read and decode manifest, meta, fields", reps, [&] {
			for (const auto& f : main_files) { sink = sink + size_of(dir + "/" + f); }
			const auto m = files::decode_manifest(read_all(dir + "/manifest-201"));
			const std::string meta = read_all(dir + "/meta-2.kv");
			if (files::trailing_crc(meta) != files::trailing_crc(mb)) { die("meta crc"); }
			const auto md = files::decode_metadata(meta);
			field_table t;
			files::decode_fields(read_all(dir + "/fields-2.kv"), t);
			sink = sink + m.segments.size() + md.entries.size() + t.size();
		});

		// Logs: a fresh one (the snapshot); ones whose edits of the commit mix fill 4, 8, 16 and 32
		// KiB or number 128; and delete-only commits (smaller edits) to 128 edits and to 16 KiB.
		struct config {
			bool deletes, by_edits;
			std::size_t limit;
		};
		for (const config c : {config{false, false, 0}, config{false, false, 4096}, config{false, false, 8192}, config{false, true, 128},
				 config{false, false, 16384}, config{false, false, 32768}, config{true, true, 128}, config{true, false, 16384}}) {
			const std::size_t limit = c.limit;
			model lm;
			if (c.deletes) { lm.flush_docs = 20; }
			for (std::uint64_t n = 0; n < 200; ++n) { (void)lm.next_edit(n); }
			emitter em;
			std::string snap;
			snapshot_record(snap, lm.st);
			em.record(snap);
			const std::size_t snap_bytes = em.out.size();
			std::uint64_t edits = 0, payload = 0;
			for (std::uint64_t n = 200; limit && (c.by_edits ? edits < limit : em.out.size() < snap_bytes + limit); ++n, ++edits) {
				const std::string e = c.deletes ? lm.next_delete_edit() : lm.next_edit(n);
				payload += e.size();
				em.record(e);
			}
			const std::string name = std::string("log-") + (c.deletes ? "d" : "m") + (c.by_edits ? "e" : "b") + std::to_string(limit);
			write_file(dir, name, em.out);
			state check;
			if (replay(em.out, check) != lm.st.m.generation || check.m != lm.st.m || check.meta != lm.st.meta || !(check.fields == lm.st.fields)) { die("replay differs"); }
			char what[160];
			std::snprintf(what, sizeof what, "log, %s: %zu B (snapshot %zu B, %llu edits, %.0f B payload each)", c.deletes ? "delete-only commits" : "commit mix",
				em.out.size(), snap_bytes,
				(unsigned long long)edits, edits ? double(payload) / double(edits) : 0.0);
			std::printf("%s\n", what);
			time("  log: scan 2 files, read the log, replay it", reps, [&] {
				sink = sink + size_of(dir + "/" + name) + size_of(dir + "/seg-1.sx");
				state s;
				sink = sink + replay(read_all(dir + "/" + name), s);
			});
			time("  log: replay alone, from memory", reps, [&] {
				state s;
				sink = sink + replay(em.out, s);
			});
		}
	}
	return int(sink & 0);
}
