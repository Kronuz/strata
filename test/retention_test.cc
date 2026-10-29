// Manifest retention (design D4, I14): the default keeps no predecessor, and an open that falls
// back to one reports it.
//   - With the default, a merge leaves one manifest and only the files it names, once the
//     reclaim queue drains; with 1 or 2 the merged-away segments stay pinned.
//   - With 1, a current manifest or newest segment damaged for good is recovered by
//     open(..., to) at the predecessor, and fallback() says which generation and how many
//     newer ones are gone; an ordinary open reports none.
//   - With 0, the same damage is refused: no predecessor to open.
//   - `discarded` counts the newer manifests found, not generations: a pinned checkpoint leaves a gap.
//   - A background merge's source, all of whose documents are deleted and committed while the
//     merge is queued, is not truncated under the merge's mapping, at 0, 1 and 2 predecessors
//     (the mapped run, STRATA_FAULTFS_MAP=1, aborts on a truncation below a live mapping).
#include "kv_test.hh"

#include "strata/core/executor.hh"

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
		o.keyed = true;
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
	const bool mapped = std::getenv("STRATA_FAULTFS_MAP") != nullptr;  // mapped reads bypass the media faults on segments
	for (const bool segment_damage : {false, true}) {
		if (segment_damage && mapped) { continue; }
		for (std::size_t keep : {std::size_t(0), std::size_t(1)}) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			kv_index::options o;
			o.keyed = true;
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
		o.keyed = true;
		o.retained_predecessors = 1;
		const auto h = build(o, io);
		auto ix = kv_index::open(io, o, h.gen_head);
		CHECK(!ix.fallback() && ix.digest() == h.digest_head);
	}
	// A pinned checkpoint under later commits: the gap shows in newest - to, and discarded still
	// counts only the manifests found.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options o;
		o.keyed = true;
		o.retained_predecessors = 1;
		std::uint64_t pinned = 0, head = 0;
		{
			auto ix = kv_index::create(io, o, uuid);
			ix.apply(tx(batch(0, 30)), {});
			pinned = ix.checkpoint();
			for (int k = 0; k < 5; ++k) {
				ix.apply(tx(batch(30 + k * 30, 30)), {});
				ix.commit();
			}
			head = ix.manifest().generation;
		}
		auto ix = kv_index::open(io, o, pinned);
		CHECK(ix.fallback().has_value());
		if (ix.fallback()) {
			CHECK(ix.fallback()->to == pinned && ix.fallback()->newest == head);
			CHECK(ix.fallback()->newest - ix.fallback()->to > 2 && ix.fallback()->discarded == 2);
		}
	}

	// RET-1: a background merge holds its sources; every document of one is deleted and committed
	// before the merge runs, with more commits and a full reclaim in between. The file must stay
	// until the merge publishes, whichever the retention (one extra commit per predecessor kept).
	for (std::size_t keep : {std::size_t(0), std::size_t(1), std::size_t(2)}) {
		for (int victim = 0; victim < 6; ++victim) {
			faultfs::filesystem fs;
			faultfs::io io(fs);
			inline_executor exec;  // queued work runs only on exec.run()
			kv_index::options o;
			o.keyed = true;
			o.retained_predecessors = keep;
			o.reclaim_step = 256;  // a segment larger than a step is truncated before it is unlinked
			o.background = &exec;
			o.policy.segments_per_tier = 2;
			o.policy.floor_bytes = 1;
			auto ix = kv_index::create(io, o, uuid);
			for (int s = 0; s < 6; ++s) {
				std::vector<operation> ops;
				for (int i = 0; i < 40; ++i) { ops.push_back(add("Q" + std::to_string(s * 100 + i), std::string(200, char('a' + s)) + std::to_string(i))); }
				ix.apply(tx(ops), {});
				ix.commit();
			}
			ix.maybe_merge();
			CHECK(ix.merges_pending() > 0);
			std::vector<operation> dels;
			for (int i = 0; i < 40; ++i) { dels.push_back(erase("Q" + std::to_string(victim * 100 + i))); }
			ix.apply(tx(dels), {});
			ix.commit();
			for (std::size_t k = 0; k < keep; ++k) {
				ix.apply(tx({add("Qx" + std::to_string(k), "x")}), {});
				ix.commit();
			}
			ix.reclaim(UINT64_MAX);
			exec.run();
			ix.wait_merges();
			ix.commit();
			ix.reclaim(UINT64_MAX);
			CHECK(!ix.needs_heal());
			(void)ix.check();
		}
	}
	std::printf("retention: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
