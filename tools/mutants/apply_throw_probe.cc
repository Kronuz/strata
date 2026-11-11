// CT-R5's probe (docs/reviews/cold-tier-design.md), on main's API: what apply() leaves when a
// segment read inside stage 1 throws. Two committed segments: S1 holds Q1, S2 holds Q2; one section
// of S1 is damaged at its size, so the open passes and the first read of that region throws. One
// transaction replaces Q2 (in S2), then Q1 (in S1, whose lookup throws); a clean one follows.
// Exits 1 when the failed transaction is half visible or no case reaches stage 1. Built and run by
// tools/mutants/rollback.py, which also runs it against the rollback's mutants.
#include "kv_test.hh"

#include "strata/core/format.hh"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

static const auto uuid = uuid_of("ct-review-throw");

static std::uint64_t head_generation(const faultfs::filesystem& fs) {
	std::uint64_t g = 0;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { g = std::max(g, p->a); }
	}
	return g;
}

// 1: the throw reached stage 1 and left nothing; 0: the open refused the damage; -1: half visible.
static int attempt(format::section kind, const char* label, bool lazy) {
	faultfs::filesystem fs;
	faultfs::io io(fs);
	kv_index::options o;
	o.keyed = true;
	o.auto_merge = false;
	o.eager_segments = !lazy;
	{
		auto ix = kv_index::create(io, o, uuid);
		ix.apply(tx({add("Q1", "a")}), {});
		ix.commit();
		ix.apply(tx({add("Q2", "b")}), {});
		ix.commit();
		ix.close();
	}
	const auto g = head_generation(fs);
	const auto m = files::read_log(fs.view(index_detail::manifest_name(g)), g).state;
	const std::string s1 = index_detail::segment_name(m.segments.begin()->first);
	std::string bytes(fs.view(s1));
	bool damaged = false;
	for (const auto& e : format::decode_toc(bytes, format::decode_footer(bytes))) {
		if (e.kind == kind && e.length) {
			bytes[e.offset + e.length / 2] ^= 1;
			damaged = true;
		}
	}
	if (!damaged) {
		std::printf("[%s%s] S1 has no such section\n", label, lazy ? ", lazy" : "");
		return 0;
	}
	io.remove(s1);
	{
		auto f = io.create_exclusive(s1);
		f->write_at(0, bytes);
		f->sync();
	}
	std::optional<kv_index> opened;
	try {
		opened.emplace(kv_index::open(io, o));
	} catch (const std::exception& e) {
		std::printf("[%s%s] the open refuses the damage (%s): not a stage-1 case\n", label, lazy ? ", lazy" : "", e.what());
		return 0;
	}
	kv_index& ix = *opened;
	std::string threw = "nothing";
	try {
		const auto v = ix.apply(tx({replace("Q2", "b2"), replace("Q1", "a2")}), {});
		threw = "no exception, verdict " + std::to_string(int(v.result));
	} catch (const corrupt_segment& e) {
		threw = std::string("corrupt_segment: ") + e.what();
	} catch (const std::exception& e) {
		threw = std::string("other exception: ") + e.what();
	}
	const auto v2 = ix.apply(tx({add("Q3", "c")}), {});
	std::optional<std::string> q2, q3;
	std::string after = "ok";
	try {
		ix.refresh();
		q2 = ix.get("Q2");
		q3 = ix.get("Q3");
	} catch (const std::exception& e) {
		after = e.what();
	}
	const bool half = q2 && *q2 == "b2";
	std::printf("[%s%s] apply of {replace Q2, replace Q1}: %s; then {add Q3}: verdict %d; refresh and reads: %s; Q2 = %s, Q3 = %s; the failed transaction is %s\n",
		label, lazy ? ", lazy" : "", threw.c_str(), int(v2.result), after.c_str(), q2 ? q2->c_str() : "(none)", q3 ? q3->c_str() : "(none)",
		half ? "HALF VISIBLE" : "not visible");
	try {
		ix.close();
	} catch (const std::exception& e) {
		std::printf("[%s] close: %s\n", label, e.what());
	}
	if (half || q3 != std::optional<std::string>("c")) { return -1; }
	return threw.rfind("corrupt_segment", 0) == 0 ? 1 : 0;
}

int main() {
	int reached = 0;
	bool bad = false;
	for (const bool lazy : {false, true}) {
		const std::pair<format::section, const char*> sections[] = {
			{format::section::keys, "keys"}, {format::section::key_index, "key_index"}, {format::section::versions, "versions"}, {format::section::stored, "stored"}};
		for (const auto& [kind, label] : sections) {
			const int r = attempt(kind, label, lazy);
			bad = bad || r < 0;
			reached += r > 0;
		}
	}
	std::printf("apply_throw_probe: %d stage-1 cases, %s\n", reached, bad ? "HALF VISIBLE" : "none visible");
	return bad || reached == 0 ? 1 : 0;
}
