// A begun close's booked relief never covers descriptors or vnodes its index already let go of
// (docs/perf/flakes.md, "Descriptor and vnode relief"). Deterministic: inline_executor, and the
// catalog's `before_cas` hook samples the booking and the closing slot's counts at every site.
#include "catalog_world.hh"

namespace {

using W = world<inline_executor>;
using strata::resource;

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
	bool closing = false;
	int sites = 0, over = 0;
	v.tweak = [&](cat_t::host& h) {
		h.soft_open = 1;
		h.before_cas = [&](const char* site, bool) {
			if (!closing) { return; }
			++sites;
			const std::uint64_t d = v.cat->count_of(0, resource::descriptors), n = v.cat->count_of(0, resource::vnodes);
			if (v.cat->pending_descriptors() > d || v.cat->pending_vnodes() > n) {
				++over;
				std::fprintf(stderr, "at %s: booked %llu descriptors %llu vnodes, slot holds %llu and %llu\n", site, (unsigned long long)v.cat->pending_descriptors(),
					(unsigned long long)v.cat->pending_vnodes(), (unsigned long long)d, (unsigned long long)n);
			}
		};
	};
	v.make_catalog({});
	touch(v, 0);
	v.exec.scheduler().advance(1'000'000);
	touch(v, 1);
	const std::uint64_t d = v.cat->count_of(0, resource::descriptors);
	CHECK(d > 0);
	(void)v.cat->pass();
	CHECK(v.cat->pending_descriptors() == d && v.cat->stats().pass_closes == 1);
	closing = true;
	v.exec.run();
	closing = false;
	CHECK(sites > 0 && over == 0);
	CHECK(v.cat->pending_descriptors() == 0 && v.cat->pending_vnodes() == 0 && v.cat->open_slots() == 1);
	std::printf("relief not past the drop: ok\n");
}

}  // namespace

int main() {
	relief_not_past_drop();
	if (failures) {
		std::fprintf(stderr, "catalog_relief: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_relief: ok");
	return 0;
}
