// The reclaim queue keeps pace with merges whether or not the host commits (limits.md,
// "Reclaim quota"): a published merge reclaims as many bytes as it obsoleted.
//   1. Many merges, no commit: the segment files on disk are the live segments
//      after every publish, none queued, and every read still hits (the mapped run aborts on a truncation below a
//      live mapping).
//   2. With commits in between: the newest manifest on disk is intact after every step, since
//      a file a kept manifest names is never reclaimed by a merge's publish.
//   3. A pending merge's source that left the segment list is spared when another merge
//      publishes, and a pending merge still runs over it.
#include "kv_test.hh"

#include "strata/core/executor.hh"

#include <algorithm>
#include <memory>
#include <set>

static const auto uuid = uuid_of("core-reclaim-bl");

struct on_disk {
	std::size_t segments = 0;
	std::set<std::string> names;
};

static on_disk survey(const faultfs::filesystem& fs) {
	on_disk r;
	for (const auto& n : fs.list()) {
		auto p = index_detail::parse_name(n);
		if (p && p->type == index_detail::parsed_name::segment) {
			++r.segments;
			r.names.insert(n);
		}
	}
	return r;
}

// What recover --to g checks, read-only: the newest manifest names only files that exist with
// the entry's size.
static std::string newest_intact(faultfs::filesystem& fs) {
	std::string head;
	std::uint64_t top = 0;
	for (const auto& n : fs.list()) {
		auto p = index_detail::parse_name(n);
		if (p && p->type == index_detail::parsed_name::manifest && p->a >= top) {
			top = p->a;
			head = n;
		}
	}
	if (head.empty()) { return "no manifest"; }
	try {
		const auto m = files::decode_manifest(fs.view(head));
		for (const auto& r : m.segments) {
			const auto name = index_detail::segment_name(r.id);
			if (fs.view(name).size() != r.size) { return name + " size"; }
		}
	} catch (const std::exception& e) {
		return std::string("unreadable: ") + e.what();
	}
	return {};
}

static std::vector<operation> round(int r, int keys) {
	std::vector<operation> ops;
	for (int i = 0; i < keys; ++i) { ops.push_back(replace("Q" + std::to_string(i), "round" + std::to_string(r) + std::string(300, char('a' + r % 26)))); }
	return ops;
}

static kv_index::options small_steps() {
	kv_index::options o;
	o.key_prefix = "Q";
	o.auto_merge = true;
	o.reclaim_step = 256;  // a segment is truncated over several steps before it is unlinked
	o.reclaim_slice = 0;  // a commit reclaims nothing: only the merges' quota does
	o.policy.segments_per_tier = 2;
	o.policy.floor_bytes = 1;
	return o;
}

// Runs posted work one item at a time, so a test publishes one merge while another waits.
class manual_executor final : public executor {
public:
	struct item {
		work fn;
		task::cancel_token stop;
		bool ran = false;
	};
	std::unique_ptr<group> open() override { return std::make_unique<manual_group>(*this); }
	std::uint64_t now() override { return 0; }
	bool sleep_until(std::uint64_t, const task::cancel_token& stop) override { return !stop.requested(); }
	std::size_t queued() const {
		return std::size_t(std::count_if(items_.begin(), items_.end(), [](const auto& i) { return !i->ran && i->fn; }));
	}
	// Runs the n-th item not yet run.
	void run_nth(std::size_t n) {
		for (const auto& i : items_) {
			if (i->ran || !i->fn) { continue; }
			if (n-- == 0) {
				i->ran = true;
				i->fn(i->stop);
				return;
			}
		}
	}
	void run_all() {
		for (const auto& i : items_) {
			if (!i->ran && i->fn) {
				i->ran = true;
				i->fn(i->stop);
			}
		}
	}

private:
	class manual_group final : public group {
	public:
		explicit manual_group(manual_executor& e) : e_(&e) {}
		~manual_group() override { cancel(); }
		void post(work w) override {
			auto i = std::make_shared<item>(item{std::move(w), source_.token()});
			mine_.push_back(i);
			e_->items_.push_back(i);
		}
		void cancel() noexcept override {
			source_.request();
			for (const auto& i : mine_) { i->fn = nullptr; }
			mine_.clear();
			source_ = task::cancel_source();
		}
		void wait() override { e_->run_all(); }

	private:
		manual_executor* e_;
		task::cancel_source source_;
		std::vector<std::shared_ptr<item>> mine_;
	};
	std::vector<std::shared_ptr<item>> items_;
};

int main() {
	// 1. Many merges, no commit, with the index's own executor: merged-away files leave as the
	// merges publish.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = kv_index::create(io, small_steps(), uuid);
		std::size_t worst = 0, merged = 0;
		for (int r = 0; r < 80; ++r) {
			ix.apply(tx(round(r, 50)), {});
			ix.refresh();
			const auto d = survey(fs);
			const std::size_t live = ix.segments().size();
			worst = std::max(worst, d.segments - std::min(d.segments, live));
			merged += ix.reclaim_pending();
			for (int k = 0; k < 50; k += 7) { CHECK(ix.get("Q" + std::to_string(k)).has_value()); }
		}
		std::printf("reclaim_backlog: no commit: worst dead files on disk %zu, pending sum %zu, live %zu\n", worst, merged, ix.segments().size());
		// Every publish reclaims at least the bytes it obsoleted, so nothing waits.
		CHECK(worst == 0);
		CHECK(merged == 0);
		ix.commit();
		ix.reclaim();
		CHECK(survey(fs).segments == ix.segments().size());
	}

	// 2. Commits in between: a file the newest manifest names is never reclaimed by a publish.
	{
		faultfs::filesystem fs;
		faultfs::io io(fs);
		auto ix = kv_index::create(io, small_steps(), uuid);
		for (int r = 0; r < 60; ++r) {
			ix.apply(tx(round(r, 50)), {});
			ix.refresh();
			CHECK(newest_intact(fs).empty());
			if (r % 7 == 6) {
				ix.commit();
				CHECK(newest_intact(fs).empty());
			}
		}
		ix.commit();
		ix.reclaim();
		CHECK(newest_intact(fs).empty());
		CHECK(survey(fs).segments == ix.segments().size());
	}

	// 3. Merge A publishes while merge B is pending over a source that has left the segment
	// list (all its documents deleted and committed): B's source stays whole until B publishes.
	for (int victim = 0; victim < 3; ++victim) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		manual_executor exec;
		auto o = small_steps();
		o.auto_merge = false;  // the merges are scheduled together below
		o.background = &exec;
		auto ix = kv_index::create(io, o, uuid);
		for (int s = 0; s < 12; ++s) {
			std::vector<operation> ops;
			for (int i = 0; i < 40; ++i) { ops.push_back(add("Q" + std::to_string(s * 100 + i), std::string(200, char('a' + s)) + std::to_string(i))); }
			ix.apply(tx(ops), {});
			ix.refresh();
		}
		ix.maybe_merge();
		CHECK(ix.merges_pending() >= 2);
		CHECK(exec.queued() >= 2);
		// A segment late in the order, so a merge other than the first holds it.
		std::vector<operation> dels;
		for (int i = 0; i < 40; ++i) { dels.push_back(erase("Q" + std::to_string((8 + victim) * 100 + i))); }
		ix.apply(tx(dels), {});
		ix.commit();
		exec.run_nth(0);  // merge A finishes; B is queued
		ix.refresh();     // A publishes, and its quota reclaims
		exec.run_all();
		ix.wait_merges();
		ix.commit();
		ix.reclaim();
		CHECK(!ix.needs_heal());
		(void)ix.check();
		CHECK(newest_intact(fs).empty());
	}
	std::printf("reclaim_backlog: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
