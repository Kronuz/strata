// apply() is atomic when a segment read in stage 1 throws (CT-R5): the failed transaction leaves
// no record, key slot, pending delete or metadata change, so the next accepted transaction does
// not publish its first operations. One section of the older segment is damaged at its size, so
// the open passes and the first read of that region throws corrupt_segment.
#include "kv_test.hh"

#include "strata/core/format.hh"

#include <algorithm>
#include <string>

static const auto uuid = uuid_of("apply-atomic");

static std::uint64_t head_generation(const faultfs::filesystem& fs) {
	std::uint64_t g = 0;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { g = std::max(g, p->a); }
	}
	return g;
}

static void rewrite(faultfs::io& io, const std::string& name, const std::string& bytes) {
	io.remove(name);
	auto f = io.create_exclusive(name);
	f->write_at(0, bytes);
	f->sync();
}

// S1 holds Q1 and S2 holds Q2; S1's `kind` section is damaged mid-way. Returns false when S1 has none.
static bool setup(faultfs::filesystem& fs, faultfs::io& io, const kv_index::options& o, format::section kind) {
	auto ix = kv_index::create(io, o, uuid);
	ix.apply(tx({add("Q1", "a")}), {});
	ix.commit();
	ix.apply(tx({add("Q2", "b")}), {});
	ix.commit();
	ix.close();
	const auto m = files::decode_manifest(fs.view(index_detail::manifest_name(head_generation(fs))));
	CHECK(m.segments.size() == 2);
	const std::string s1 = index_detail::segment_name(m.segments[0].id);
	std::string bytes(fs.view(s1));
	bool damaged = false;
	for (const auto& e : format::decode_toc(bytes, format::decode_footer(bytes))) {
		if (e.kind == kind && e.length) {
			bytes[e.offset + e.length / 2] ^= 1;
			damaged = true;
		}
	}
	if (damaged) { rewrite(io, s1, bytes); }
	return damaged;
}

// Runs `failing` against the damaged index and checks that nothing of it is visible afterwards.
static void run(const char* label, std::vector<operation> failing, bool meta_first) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.key_prefix = "Q";
	o.auto_merge = false;
	if (!setup(fs, io, o, format::section::keys)) {
		std::fprintf(stderr, "%s: S1 has no keys section\n", label);
		CHECK(false);
		return;
	}
	auto ix = kv_index::open(io, o);
	const auto meta_before = ix.metadata();
	bool threw = false;
	try {
		ix.apply(tx(std::move(failing)), {});
	} catch (const corrupt_segment&) {
		threw = true;
	}
	CHECK(threw);
	CHECK(ix.metadata() == meta_before);
	CHECK(!meta_first || ix.metadata().count("m") == 0);

	const auto v = ix.apply(tx({add("Q3", "c")}), {});
	CHECK(v.result == verdict::accepted);
	ix.refresh();
	CHECK(ix.get("Q2") == std::optional<std::string>("b"));  // the failed replace is not published
	CHECK(ix.get("Q3") == std::optional<std::string>("c"));
	CHECK(!ix.get("Q9"));
	try {
		ix.close();
	} catch (const std::exception&) {
	}
}

int main() {
	run("replace-replace", {replace("Q2", "b2"), replace("Q1", "a2")}, false);
	run("add-replace-replace", {add("Q9", "z"), replace("Q2", "b2"), replace("Q1", "a2")}, false);
	run("meta-replace-replace", {set_meta("m", "1"), replace("Q2", "b2"), replace("Q1", "a2")}, true);
	run("delete-replace", {erase("Q2"), replace("Q1", "a2")}, false);
	std::printf("apply_atomic: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
