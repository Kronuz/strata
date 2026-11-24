// Field ids at each map (docs/mechanisms/field-check.md): a manifest resealed with a field dropped,
// or with a segment's field made the key field, is refused with corrupt_segment wherever a segment
// maps: an eager open, a lazy segment's first read, a merge's input. The layout's stored plugin
// keeps each document's field id as its record, and its reader checks them as an inverted
// dictionary checks its terms' fields.
#include "faultlab/faultfs.h"
#include "strata/core/index.hh"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fl = kronuz::faultlab;
using namespace strata;

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

// A stored plugin whose record is one field id (4 bytes), and whose reader has check_fields.
struct field_ids_plugin : stored_plugin {
	static inline int checks = 0;          // check_fields calls
	static inline std::string refusal;     // the last refusal's message
	class reader : public stored_plugin::reader {
	public:
		explicit reader(const open_context& c) : stored_plugin::reader(c), docs_(c.doc_count) {}
		void check_fields(std::uint64_t size, std::optional<std::uint32_t> key) const {
			++checks;
			stored_frame cache;
			for (ordinal_t o = 0; o < docs_; ++o) {
				const std::string_view r = stored(o, cache);
				if (r.size() != 4) { corrupt("field record"); }
				std::uint32_t id;
				std::memcpy(&id, r.data(), 4);
				if (id < size && id != key) { continue; }
				refusal = "field id " + std::to_string(id) + (id < size ? " is the key field" : " outside the field table");
				throw corrupt_segment(refusal.c_str());
			}
		}

	private:
		std::uint64_t docs_;
	};
	static built merge(const merge_context& m, std::span<const reader* const> in, section_sink& sink) {
		const std::vector<const stored_plugin::reader*> base(in.begin(), in.end());
		return stored_plugin::merge(m, base, sink);
	}
};
using index_t = basic_index<layout<keys_plugin, field_ids_plugin>>;

static const std::array<char, 16> uuid = {'f', 'i', 'e', 'l', 'd', '-', 'c', 'h', 'e', 'c', 'k', '-', 'm', 'a', 'p', '!'};

static index_t::options opts(bool eager) {
	index_t::options o;
	o.keyed = true;
	o.auto_merge = false;
	o.identity_seed = 7;
	o.eager_segments = eager;
	return o;
}

static std::string id_bytes(std::uint32_t id) {
	std::string s(4, '\0');
	std::memcpy(s.data(), &id, 4);
	return s;
}

// One committed segment per entry of `segments`, each document holding the field id given.
// Fields a, b, c (ids 0, 1, 2), each a valid key field's declaration.
static std::uint64_t build(fl::io& io, const std::vector<std::vector<std::uint32_t>>& segments) {
	auto ix = index_t::create(io, opts(true), uuid);
	int n = 0;
	for (const auto& ids : segments) {
		index_t::transaction t;
		if (n == 0) {
			for (const char* f : {"a", "b", "c"}) { t.fields.push_back({f, field_role::indexed, field_postings::docs, false}); }
		}
		for (std::uint32_t id : ids) {
			index_t::operation o;
			o.type = index_t::operation::add;
			o.doc.key = "k" + std::to_string(n++);
			o.doc.stored = id_bytes(id);
			t.ops.push_back(std::move(o));
		}
		CHECK(ix.apply(t, {}).result == verdict::accepted);
		ix.refresh();
		ix.commit();
	}
	CHECK(ix.segments().size() == segments.size() && ix.fields().size() == 3);
	return ix.digest();
}

// The head state written back as a fresh log at its generation.
static void reseal(fl::io& io, files::log_state m) {
	files::recount(m);
	const std::string name = index_detail::manifest_name(m.generation);
	io.remove(name);
	auto f = io.create_exclusive(name);
	index_detail::write_all(*f, files::frame_snapshot(m));
	f->sync();
	io.sync_directory();
}
static void drop_last_field(fl::io& io) {
	auto m = index_detail::read_state(io);
	m.fields.truncate(2);
	reseal(io, m);
}
static void make_c_the_key(fl::io& io) {
	auto m = index_detail::read_state(io);
	field_table t;
	t.append({"a", field_role::indexed, field_postings::docs, false});
	t.append({"b", field_role::indexed, field_postings::docs, false});
	t.append({"c", field_role::key, field_postings::docs, false});
	m.fields = t;
	reseal(io, m);
}

static std::string error_of(const std::function<void()>& f, bool* segment = nullptr) {
	if (segment) { *segment = false; }
	try {
		f();
	} catch (const corrupt_segment& e) {
		if (segment) { *segment = true; }
		return e.what();
	} catch (const std::exception& e) {
		return e.what();
	}
	return {};
}
static bool names(const std::string& what, const char* field) { return what.find(field) != std::string::npos; }

// Segments 0 and 2 hold ids 0 and 1; segment 1 holds id 2 (field c).
static const std::vector<std::vector<std::uint32_t>> shape = {{0, 1, 0}, {2, 0}, {1}};

// A good index: every map checks its fields, and the content is what was written.
static void good() {
	fl::filesystem fs;
	fl::io io{fs};
	const std::uint64_t digest = build(io, shape);
	for (bool eager : {true, false}) {
		field_ids_plugin::checks = 0;
		auto ix = index_t::open(io, opts(eager));
		CHECK(field_ids_plugin::checks == (eager ? 3 : 0));
		for (const auto& v : ix.segments()) { (void)v.seg(); }
		CHECK(field_ids_plugin::checks == 3);  // each segment once, at its map
		CHECK(ix.digest() == digest && !ix.needs_heal());
		ix.force_merge(1);
		CHECK(ix.segments().size() == 1 && ix.digest() == digest);
	}
}

// A segment this process wrote keeps the default vouch: a remap after demote() checks no field.
static void own_segments() {
	fl::filesystem fs;
	fl::io io{fs};
	auto ix = index_t::create(io, opts(false), uuid);
	index_t::transaction t;
	t.fields = {{"a", field_role::indexed, field_postings::docs, false}};
	index_t::operation o;
	o.type = index_t::operation::add;
	o.doc.key = "k";
	o.doc.stored = id_bytes(0);
	t.ops = {o};
	CHECK(ix.apply(t, {}).result == verdict::accepted);
	ix.refresh();
	ix.commit();
	CHECK(ix.demote());
	field_ids_plugin::checks = 0;
	for (const auto& v : ix.segments()) { (void)v.seg(); }
	CHECK(field_ids_plugin::checks == 0);
}

// An eager open refuses the index; the files stay as they were.
static void eager_open(const std::function<void(fl::io&)>& damage, const char* field) {
	fl::filesystem fs;
	fl::io io{fs};
	build(io, shape);
	damage(io);
	bool segment = false;
	const std::string what = error_of([&] { index_t::open(io, opts(true)); }, &segment);
	CHECK(segment && names(what, field));
	if (!segment || !names(what, field)) { std::fprintf(stderr, "  eager open: \"%s\"\n", what.c_str()); }
}

// A lazy open succeeds; the segment's first read refuses it, the others read, and the record
// stays failed until heal().
static void lazy_first_read(const std::function<void(fl::io&)>& damage, const char* field) {
	fl::filesystem fs;
	fl::io io{fs};
	build(io, shape);
	damage(io);
	auto ix = index_t::open(io, opts(false));
	const auto views = ix.segments();
	CHECK(views.size() == 3);
	CHECK(error_of([&] { (void)views[0].seg(); }).empty());
	bool segment = false;
	std::string what = error_of([&] { (void)views[1].seg(); }, &segment);
	CHECK(segment && names(what, field));
	if (!segment || !names(what, field)) { std::fprintf(stderr, "  first read: \"%s\"\n", what.c_str()); }
	CHECK(error_of([&] { (void)views[2].seg(); }).empty());
	what = error_of([&] { (void)views[1].seg(); }, &segment);
	CHECK(segment && names(what, "failed its checks"));
	CHECK(ix.needs_heal());
}

// A merge maps its inputs: the damaged one is refused and left out, the others merge.
static void merge_input(const std::function<void(fl::io&)>& damage, const char* field) {
	fl::filesystem fs;
	fl::io io{fs};
	build(io, shape);
	damage(io);
	auto ix = index_t::open(io, opts(false));
	std::vector<std::uint64_t> ids;
	for (const auto& v : ix.segments()) { ids.push_back(v.id); }
	const std::uint64_t bad = ids[1];
	field_ids_plugin::refusal.clear();
	ix.merge(ids);
	CHECK(names(field_ids_plugin::refusal, field));
	if (!names(field_ids_plugin::refusal, field)) { std::fprintf(stderr, "  merge input: \"%s\"\n", field_ids_plugin::refusal.c_str()); }
	const auto after = ix.segments();
	CHECK(after.size() == 2);  // the merged output and the refused input
	bool kept = false;
	for (const auto& v : after) { kept = kept || v.id == bad; }
	CHECK(kept && ix.needs_heal());
	bool segment = false;
	(void)error_of([&] { ix.commit(); }, &segment);
	CHECK(segment);  // a commit refuses while a first map failed (cold-tier D11)
}

int main() {
	good();
	own_segments();
	const std::pair<std::function<void(fl::io&)>, const char*> damages[] = {
		{drop_last_field, "field id 2 outside the field table"},
		{make_c_the_key, "field id 2 is the key field"},
	};
	for (const auto& [damage, field] : damages) {
		eager_open(damage, field);
		lazy_first_read(damage, field);
		merge_input(damage, field);
	}
	std::printf("field_check_map: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
