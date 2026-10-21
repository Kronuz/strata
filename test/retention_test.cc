// Manifest retention (design D4, I14): the default keeps no predecessor, and an open that falls
// back to one reports it.
//   - With the default, a merge leaves one manifest and only the files it names, once the
//     reclaim queue drains; with 1 or 2 the merged-away segments stay pinned.
//   - With 1, a current manifest or newest segment damaged for good is recovered by
//     open(..., to) at the predecessor, and fallback() says which generation and how many
//     newer ones are gone; an ordinary open reports none.
//   - With 0, the same damage is refused: no predecessor to open.
#include "kv_test.hh"

#include <algorithm>
#include <set>

static const auto uuid = uuid_of("core-retention");

struct files_on_disk {
	std::size_t manifests = 0, segments = 0;
	std::set<std::string> data;  // every strata file but the manifests
};

static files_on_disk survey(const faultfs::filesystem& fs) {
	files_on_disk r;
	for (const auto& n : fs.list()) {
		auto p = index_detail::parse_name(n);
		if (!p) { continue; }
		if (p->type == index_detail::parsed_name::manifest) {
			++r.manifests;
		} else {
			r.data.insert(n);
			r.segments += p->type == index_detail::parsed_name::segment;
		}
	}
	return r;
}

static std::vector<operation> batch(int from, int count) {
	std::vector<operation> ops;
	for (int i = from; i < from + count; ++i) { ops.push_back(add("Q" + std::to_string(i), "doc" + std::to_string(i) + "alphabetagamma" + std::to_string(i * 7919))); }
	return ops;
}

// Three commits of 30 documents, the last one a flush of its own: returns the generation and
// digest after the second commit (the predecessor) and after the third (the head).
struct history {
	std::uint64_t gen_before = 0, gen_head = 0;
	std::uint64_t digest_before = 0, digest_head = 0;
	std::uint64_t head_segment = 0;
};

static history build(kv_index::options o, faultfs::io& io) {
	history h;
	auto ix = kv_index::create(io, o, uuid);
	ix.apply(tx(batch(0, 30)), {});
	ix.commit();
	ix.apply(tx(batch(30, 30)), {});
	ix.commit();
	h.gen_before = ix.manifest().generation;
	h.digest_before = ix.digest();
	ix.apply(tx(batch(60, 30)), {});
	ix.commit();
	h.gen_head = ix.manifest().generation;
	h.digest_head = ix.digest();
	h.head_segment = ix.manifest().segments.back().id;
	return h;
}

static void flip(faultfs::filesystem& fs, const std::string& name) {
	const std::uint64_t size = fs.view(name).size();
	fs.add_media_fault(name, faultfs::media::bit_flip, size / 2, size / 2 + 1, 0);
}

// True when the open at the head, with every check, fails or reports damage.
static bool head_refused(faultfs::io& io, const kv_index::options& o) {
	try {
		auto ix = kv_index::open(io, o);
		(void)ix.check();
		if (!ix.heal().empty()) { return true; }
		(void)ix.digest();
		return false;
	} catch (const corrupt_index&) {
		return true;
	} catch (const corrupt_segment&) {
		return true;
	}
}

int main() {
	CHECK(kv_index::options{}.retained_predecessors == 0);

	// The default leaves no predecessor once a merge settles; 1 and 2 keep the merged-away segments.
	for (std::size_t keep : {std::size_t(0), std::size_t(1), std::size_t(2)}) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.key_prefix = "Q";
		if (keep) { o.retained_predecessors = keep; }
		auto ix = kv_index::create(io, o, uuid);
		for (int s = 0; s < 3; ++s) {
			ix.apply(tx(batch(s * 30, 30)), {});
			ix.commit();
		}
		ix.force_merge(1);
		ix.commit();
		ix.reclaim(UINT64_MAX);
		CHECK(ix.reclaim_pending() == 0 && ix.manifest().segments.size() == 1);
		const auto d = survey(fs);
		if (keep == 0) {
			CHECK(d.manifests == 1 && d.segments == 1);
		} else {
			CHECK(d.manifests == 1 + keep && d.segments > 1);
		}
		std::set<std::string> named;
		for (const auto& s : ix.manifest().segments) { named.insert(index_detail::segment_name(s.id)); }
		if (keep == 0) {
			for (const auto& n : d.data) {
				auto p = index_detail::parse_name(n);
				CHECK(p->type != index_detail::parsed_name::segment || named.count(n));
			}
		}
	}

	// A damaged head, a retained predecessor: the fall back is reported. 0: it is refused.
	for (const bool segment_damage : {false, true}) {
		for (std::size_t keep : {std::size_t(0), std::size_t(1)}) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			kv_index::options o;
			o.key_prefix = "Q";
			o.retained_predecessors = keep;
			const auto h = build(o, io);
			{
				auto ix = kv_index::open(io, o);  // an ordinary open reports no fallback
				CHECK(!ix.fallback() && ix.digest() == h.digest_head);
			}
			flip(fs, segment_damage ? index_detail::segment_name(h.head_segment) : index_detail::manifest_name(h.gen_head));
			CHECK(head_refused(io, o));
			if (keep == 0) {
				CHECK(survey(fs).manifests == 1);
				CHECK(throws([&] { (void)kv_index::open(io, o, h.gen_before); }));
				continue;
			}
			auto ix = kv_index::open(io, o, h.gen_before);
			CHECK(ix.digest() == h.digest_before && ix.digest() != h.digest_head);
			CHECK(ix.fallback().has_value());
			if (ix.fallback()) {
				CHECK(ix.fallback()->to == h.gen_before && ix.fallback()->newest == h.gen_head && ix.fallback()->discarded == 1);
			}
		}
	}

	// Opening the head by name is not a fallback.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.key_prefix = "Q";
		o.retained_predecessors = 1;
		const auto h = build(o, io);
		auto ix = kv_index::open(io, o, h.gen_head);
		CHECK(!ix.fallback() && ix.digest() == h.digest_head);
	}
	std::printf("retention: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
