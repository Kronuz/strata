// A begun close's booked relief never covers descriptors or vnodes its index already let go of
// (docs/perf/flakes.md, "Descriptor and vnode relief"). Deterministic: inline_executor, and the
// catalog's `before_cas` hook and the host's commit sample the booking and the closing slot's
// counts at every site: a sweep close, and a replace whose draining holder drops counts.
#include "catalog_world.hh"

namespace {

using W = world<inline_executor>;
using strata::resource;
using strata::slot_state;

// While `on`, every hook site and host commit checks the booking against slot `idx`'s counts.
struct sampler {
	W& v;
	std::uint32_t idx = 0;
	bool on = false;
	int sites = 0, over = 0;
	void sample(const char* site) {
		if (!on) { return; }
		++sites;
		const std::uint64_t d = v.cat->count_of(idx, resource::descriptors), n = v.cat->count_of(idx, resource::vnodes);
		if (v.cat->pending_descriptors() > d || v.cat->pending_vnodes() > n) {
			++over;
			std::fprintf(stderr, "at %s: booked %llu descriptors %llu vnodes, slot holds %llu and %llu\n", site, (unsigned long long)v.cat->pending_descriptors(),
				(unsigned long long)v.cat->pending_vnodes(), (unsigned long long)d, (unsigned long long)n);
		}
	}
	// A catalog made again with the sampler at every site.
	void attach(std::uint64_t soft_open = 0) {
		v.tweak = [this, soft_open](cat_t::host& h) {
			h.soft_open = soft_open;
			h.before_cas = [this](const char* site, bool) { sample(site); };
		};
		v.commit_gate = [this] { sample("host.commit"); };
		v.make_catalog({});
	}
};

template <class F>
void run_task(W& w, F&& body) {
	task::run(w.exec.scheduler(), body());
}
slot_state st(W& w, std::uint32_t i) { return cat_t::state_of(w.cat->word_of(i)); }

void touch(W& w, int k) {
	task::run(w.exec.scheduler(), [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request("k" + std::to_string(k), false);
		co_return;
	}());
}

// One close for the open cap; at every hook site after the pass begins it, the descriptors and
// vnodes still booked are at most what slot 0 still holds.
void relief_not_past_drop() {
	W v(2, 2, false, false);
	sampler sm{v};
	sm.attach(1);
	touch(v, 0);
	v.exec.scheduler().advance(1'000'000);
	touch(v, 1);
	const std::uint64_t d = v.cat->count_of(0, resource::descriptors);
	CHECK(d > 0);
	(void)v.cat->pass();
	CHECK(v.cat->pending_descriptors() == d && v.cat->stats().pass_closes == 1);
	sm.on = true;
	v.exec.run();
	sm.on = false;
	CHECK(sm.sites > 0 && sm.over == 0);
	CHECK(v.cat->pending_descriptors() == 0 && v.cat->pending_vnodes() == 0 && v.cat->open_slots() == 1);
	std::printf("relief not past the drop: ok\n");
}

// A healthy index reopened (F4) with two holders: the replace books its relief at the reopen, and a
// holder still draining merges, commits and reclaims, dropping a segment's vnodes before the replace
// chore runs; the booking falls with them.
void replace_with_draining_holders() {
	W v(1, 2, false);
	sampler sm{v};
	sm.attach();
	std::vector<std::unique_ptr<cat_t::pin>> held;
	run_task(v, [&]() -> task::task<void> {
		for (int i = 0; i < 3; ++i) { held.push_back(std::make_unique<cat_t::pin>(co_await v.cat->pin_request("k0"))); }
	});
	sm.on = true;
	v.cat->reopen(std::move(*held[0]));
	held[0].reset();
	const std::uint64_t d = v.cat->count_of(0, resource::descriptors), n = v.cat->count_of(0, resource::vnodes);
	CHECK(st(v, 0) == slot_state::closing && v.cat->drain_of(0) == 2 && v.cat->pending_descriptors() == d && v.cat->pending_vnodes() == n);
	auto& ix = held[1]->index();
	ix.force_merge(1);
	ix.commit();
	(void)ix.reclaim();
	CHECK(v.cat->count_of(0, resource::vnodes) < n);  // the holder let go of vnodes the replace booked
	sm.sample("holder.reclaim");
	held[1].reset();
	v.settle();
	held[2].reset();
	v.settle();
	sm.on = false;
	CHECK(sm.sites > 0 && sm.over == 0 && st(v, 0) == slot_state::free && v.cat->stats().replaces == 1);
	CHECK(v.cat->pending_descriptors() == 0 && v.cat->pending_vnodes() == 0);
	std::printf("replace with draining holders: ok (%d sites)\n", sm.sites);
}

}  // namespace

int main() {
	relief_not_past_drop();
	replace_with_draining_holders();
	if (failures) {
		std::fprintf(stderr, "catalog_relief: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_relief: ok");
	return 0;
}
