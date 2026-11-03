// The cold tier's step 2, item 5 (docs/design/cold-tier.md D5, D6, D10; S7; CT-R4, CT-R6, CT-R36; C15):
// the sweep and shutdown. Everything runs on inline_executor with the host's timer injected
// (world::timers): single-threaded and deterministic, `before_cas` placing another actor's compare-
// exchange between a load and its own. Each case names its criterion; the mutants each must kill are
// run by .scratch/mutants5.py (docs/status/cold-tier-step2.md).
#include "catalog_world.hh"

#include <csignal>
#include <execinfo.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using W = world<inline_executor>;
using strata::pin_kind;
using strata::slot_state;
using S = slot_state;
using strata::resource;
using strata::shutdown_result;
constexpr std::uint64_t sec = 1'000'000'000, ms = 1'000'000;

std::string kn(int i) { return "k" + std::to_string(i); }

template <class F>
void run_task(W& w, F&& body) {
	task::run(w.exec.scheduler(), body());
}
std::uint64_t pins(W& w, std::uint32_t i) { return cat_t::pins_of(w.cat->word_of(i)); }
S st(W& w, std::uint32_t i) { return cat_t::state_of(w.cat->word_of(i)); }
std::uint64_t gen(W& w, std::uint32_t i) { return cat_t::gen_of(w.cat->word_of(i)); }
std::uint64_t edge(W& w, S a, S b) { return w.cat->stats().edges[unsigned(a)][unsigned(b)]; }
std::uint64_t maps(W& w, std::uint32_t i) { return w.cat->count_of(i, resource::maps); }

std::optional<kv_index> reopen(W& w, int dir) {
	auto o = w.iopts;
	o.readers = nullptr;
	return std::optional<kv_index>(kv_index::open(w.dirs[dir].io, o));
}

// A request pin and its release: the use stamps the slot's last use.
void touch(W& w, int k) {
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(k), false); co_return; });
}
void age(W& w, std::uint64_t ns) { w.exec.scheduler().advance(ns); }
// One pass, posted as a crossing or the timer would, and everything it leads to.
void pass(W& w) {
	w.cat->post_pass();
	w.exec.run();
}
void options(W& w, std::function<void(cat_t::host&)> f, bool defer = false) {
	(void)defer;
	w.tweak = std::move(f);
	w.make_catalog({});
}
// An applied, flushed write the index has not committed.
void write_uncommitted(W& w, int k, const char* key = "zz") {
	run_task(w, [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(k), false);
		CHECK(c.index().apply(tx({add(key, "w")}), {}).result == verdict::accepted);
		c.index().refresh();
	});
}

// --- S7.1: demotion only from `{0, open}` --------------------------------------------------------------

// A request pin that lands between the sweep's load and its compare-exchange keeps the slot open.
void demote_window() {
	bool armed = false;
	W* wp = nullptr;
	cat_t::try_result held;
	W w(1, 2, false, true, [&](const char* site, bool) {
		if (armed && std::strcmp(site, "sweep.demote") == 0) {
			armed = false;
			held = wp->cat->try_pin(kn(0), pin_kind::request, false);
		}
	});
	wp = &w;
	touch(w, 0);
	armed = true;
	CHECK(!w.cat->begin_demote(0));
	CHECK(!armed && held.status == cat_t::try_status::pinned);
	CHECK(st(w, 0) == S::open && pins(w, 0) == 1 && edge(w, S::open, S::demoting) == 0);
	held.p.release();
	CHECK(w.cat->begin_demote(0));  // unpinned, it demotes
	w.exec.run();
	CHECK(st(w, 0) == S::open && maps(w, 0) == 0);
	wp = nullptr;
	std::printf("demote window: ok\n");
}

// The pass leaves a pinned or non-open slot alone, demotes an idle hot one at 30 s and not before.
void demote_idle() {
	W w(2, 2, false);
	touch(w, 0);
	touch(w, 1);
	CHECK(maps(w, 0) > 0);
	auto held = w.cat->try_pin(kn(0), pin_kind::request, false);
	CHECK(held.status == cat_t::try_status::pinned);
	age(w, 29 * sec + 900 * ms);
	pass(w);
	CHECK(w.cat->stats().pass_demotes == 0 && edge(w, S::open, S::demoting) == 0);  // not yet idle
	age(w, 200 * ms);
	pass(w);
	CHECK(st(w, 0) == S::open && pins(w, 0) == 1 && maps(w, 0) > 0);  // pinned: left alone
	CHECK(w.cat->stats().pass_demotes == 1 && edge(w, S::open, S::demoting) == 1);  // k1 only
	CHECK(maps(w, 1) == 0);
	held.p.release();
	age(w, 31 * sec);
	pass(w);
	CHECK(maps(w, 0) == 0 && w.cat->stats().pass_demotes == 2);
	// An owed slot (defer_io) is not demoted again.
	W d(1, 2, true);
	touch(d, 0);
	CHECK(d.cat->begin_demote(0));
	d.exec.run();
	CHECK(st(d, 0) == S::owed && !d.cat->begin_demote(0));
	std::printf("demote idle: ok\n");
}

// --- S7.2: cold only from warm, with a maps count of zero and close_ready() -----------------------

// Over the open cap, hot slots are demoted first and the warm ones close; no hot slot is picked to close.
void close_from_warm_only() {
	W w(3, 2, false);
	options(w, [](cat_t::host& h) { h.soft_open = 1; });
	for (int i = 0; i < 3; ++i) { touch(w, i); age(w, 1 * ms); }
	age(w, 1 * sec);
	pass(w);
	CHECK(edge(w, S::open, S::demoting) == 3);   // every hot slot goes through a demotion first
	CHECK(edge(w, S::open, S::closing) == 2);    // then two close: only warm ones were chosen
	CHECK(edge(w, S::closing, S::open) == 0);    // no close was refused
	CHECK(w.cat->open_slots() == 1 && w.cat->table_size() == 1);
	CHECK(st(w, 2) == S::open && w.cat->stats().pass_closes == 2);  // the most recently used stays
	std::printf("close from warm only: ok\n");
}

// The chore decides, after the compare-exchange: a hot slot, a retired view and an uncommitted write each
// end the close at `open`.
void close_chore_refuses() {
	{
		W w(1, 2, false);
		touch(w, 0);
		CHECK(maps(w, 0) > 0);
		CHECK(w.cat->begin_close(0));  // the sweep never picks it; a direct call must still be refused
		w.exec.run();
		CHECK(st(w, 0) == S::open && w.cat->index_of(0) && !w.cat->index_of(0)->closing() && maps(w, 0) > 0);
		CHECK(edge(w, S::closing, S::open) == 1 && w.cat->stats().bound_closes == 0 && !w.cat->stuck_of(0));
	}
	{  // a retired view: demoted with a reader's guard kept past its pin
		W w(1, 2, false);
		std::optional<decltype(std::declval<cat_t::pin&>().reader().acquire())> g;
		run_task(w, [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(0), false);
			g.emplace(p.reader().acquire());
		});
		CHECK(w.cat->begin_demote(0));
		w.exec.run();
		CHECK(maps(w, 0) > 0);  // the retired view keeps its records counted
		CHECK(w.cat->begin_close(0));
		w.exec.run();
		CHECK(st(w, 0) == S::open && !w.cat->stuck_of(0) && !w.cat->index_of(0)->closing() && edge(w, S::closing, S::open) == 1);
		g.reset();
		age(w, 31 * sec);
		pass(w);  // idle and still counted: demoted again, which frees the released view
		CHECK(maps(w, 0) == 0);
		age(w, 601 * sec);
		pass(w);
		CHECK(st(w, 0) == S::free && w.cat->table_size() == 0);
	}
	{  // an uncommitted write (warm, lazy)
		W w(1, 2, false, false);
		touch(w, 0);
		write_uncommitted(w, 0);
		CHECK(w.cat->index_of(0)->uncommitted());
		CHECK(w.cat->begin_close(0));
		w.exec.run();
		CHECK(st(w, 0) == S::open && w.cat->index_of(0)->uncommitted() && edge(w, S::closing, S::open) == 1);
	}
	std::printf("close chore refuses: ok\n");
}

// A warm slot idle 600 s closes; at 599 s it does not.
void warm_idle_closes() {
	W w(1, 2, false, false);
	touch(w, 0);
	CHECK(maps(w, 0) == 0);
	age(w, 599 * sec + 900 * ms);
	pass(w);
	CHECK(st(w, 0) == S::open);
	age(w, 200 * ms);
	pass(w);
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && w.cat->open_slots() == 0 && w.cat->occupied() == 0);
	CHECK(w.budget.used(resource::descriptors) == 0 && w.budget.used(resource::maps) == 0);
	std::printf("warm idle closes: ok\n");
}

// The close takes the slot only unpinned: a pin between the load and the compare-exchange keeps it open.
void close_window() {
	bool armed = false;
	W* wp = nullptr;
	cat_t::try_result held;
	W w(1, 2, false, false, [&](const char* site, bool) {
		if (armed && std::strcmp(site, "sweep.close") == 0) {
			armed = false;
			held = wp->cat->try_pin(kn(0), pin_kind::request, false);
		}
	});
	wp = &w;
	touch(w, 0);
	armed = true;
	CHECK(!w.cat->begin_close(0));
	CHECK(!armed && st(w, 0) == S::open && pins(w, 0) == 1 && edge(w, S::open, S::closing) == 0);
	held.p.release();
	wp = nullptr;
	std::printf("close window: ok\n");
}

// C19: an idle `owed` slot whose maps are not zero (a commit under a chore pin mapped its records) goes cold
// through the plain close too, not only at the bound; a hot `open` slot is still refused.
void owed_with_maps_closes() {
	W w(1, 2, true);
	touch(w, 0);
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	CHECK(st(w, 0) == S::owed && maps(w, 0) == 0);
	run_task(w, [&]() -> task::task<void> {  // a chore pin on `owed` reads: the lazy map under its lease is not counted as a reader's
		auto c = co_await w.cat->pin_chore(kn(0), false);
		const strata::lease_scope hold{c.index().lease()};
		CHECK(c.index().get(key_of(0)) == std::optional<std::string>("v0"));
	});
	CHECK(st(w, 0) == S::owed && maps(w, 0) > 0);
	age(w, 601 * sec);
	pass(w);
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && w.budget.used(resource::maps) == 0);
	std::printf("owed with maps closes: ok\n");
}

// --- S7.3: a queued reclaim posts and closes on a later pass ---------------------------------------------

void queued_reclaim() {
	for (const bool posted : {false, true}) {
		W w(1, 6, false);
		w.iopts.reclaim_slice = 0;  // a commit leaves the merged sources queued
		if (posted) { w.iopts.background = &w.exec; }
		w.make_catalog({});
		touch(w, 0);
		auto* ix = w.cat->index_of(0);  // unpinned, nothing else runs: the test is the only user
		{
			const strata::lease_scope hold{ix->lease()};
			ix->force_merge(1);
		}
		w.exec.run();
		{
			const strata::lease_scope hold{ix->lease()};
			ix->commit();
		}
		w.exec.run();
		CHECK(ix->reclaim_pending() > 0);
		age(w, 31 * sec);
		pass(w);  // demoted: a queue does not block warm
		CHECK(maps(w, 0) == 0 && st(w, 0) == S::open);
		age(w, 601 * sec);
		pass(w);
		// the first pass posted the reclaim and ended at open; the queue is empty, the slot not closed
		CHECK(w.cat->stats().reclaims == 1 && st(w, 0) == S::open && w.cat->index_of(0)->reclaim_pending() == 0);
		CHECK(edge(w, S::closing, S::open) >= 1);
		pass(w);  // a later pass closes it
		CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && w.cat->stats().reclaims == 1);
	}
	std::printf("queued reclaim: ok\n");
}

// --- S7.4: the demotion chore, reclaim(0) included, runs under `demoting` -------------------------------

void reclaim_zero_under_demoting() {
	S seen = S::free;
	W* wp = nullptr;
	W w(1, 2, false, true, [&](const char* site, bool) {
		if (wp && std::strcmp(site, "chore.demote") == 0) { seen = st(*wp, 0); }
	});
	wp = &w;
	std::optional<decltype(std::declval<cat_t::pin&>().reader().acquire())> g;
	run_task(w, [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_request(kn(0), false);
		g.emplace(c.reader().acquire());  // the view the merge retires, kept past the pin
	});
	auto* ix = w.cat->index_of(0);
	{
		const strata::lease_scope hold{ix->lease()};
		ix->force_merge(1);
		ix->commit();
		CHECK(ix->apply(tx({add("zz", "w")}), {}).result == verdict::accepted);  // buffered: demote() refuses
	}
	const std::uint64_t before = maps(w, 0);
	CHECK(before > 0);
	g.reset();  // released: the retired view is freed only by a call that frees views
	CHECK(maps(w, 0) == before);
	age(w, 31 * sec);
	pass(w);
	CHECK(seen == S::demoting);  // the chore ran under `demoting`
	CHECK(w.cat->index_of(0)->uncommitted());
	CHECK(maps(w, 0) < before);  // reclaim(0) freed the released retired view though demote() refused
	std::printf("reclaim(0) under demoting: ok\n");
}

// --- S7.5, C15.4: a view kept past its pin keeps the slot `closing`, no thread blocked --------------------

void stuck_close() {
	strata::detail::abort_on_owner_break().store(false);
	const std::uint64_t breaks = strata::detail::off_owner_changes().load();
	W w(1, 2, false, false);
	std::optional<decltype(std::declval<cat_t::pin&>().reader().acquire())> g;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0), false);
		g.emplace(p.reader().acquire());  // kept past the pin: a host bug
	});
	CHECK(w.cat->begin_close(0));
	w.exec.run();
	CHECK(st(w, 0) == S::closing && w.cat->stuck_of(0) && w.cat->index_of(0)->closing());
	CHECK(w.cat->stats().stuck_closes == 1 && strata::detail::off_owner_changes().load() == breaks + 1);  // reported once
	// A pin waits (suspended, nothing blocked) while a pass retries without success.
	int resumed = 0;
	std::uint64_t serial = 0;
	std::function<task::task<void>()> waiter = [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0), false);
		++resumed;
		serial = std::uint64_t(p.payload().serial);
	};
	task::spawn(task::scheduler_ref::of(w.exec.scheduler()), waiter());
	w.exec.run();
	CHECK(resumed == 0 && pins(w, 0) == 1 && w.cat->waiters_of(0) == 1);
	pass(w);
	pass(w);
	CHECK(w.cat->stats().stuck_retries == 2 && st(w, 0) == S::closing && resumed == 0);
	CHECK(strata::detail::off_owner_changes().load() == breaks + 1);
	const std::uint64_t g0 = gen(w, 0);
	g.reset();  // the view goes: the next pass finishes the close, and the waiter reopens in place
	pass(w);
	w.exec.run();
	CHECK(resumed == 1 && serial == 2 && gen(w, 0) > g0 && st(w, 0) == S::open && pins(w, 0) == 0);
	CHECK(!w.cat->stuck_of(0) && w.cat->stats().stuck_retries == 3);
	// Without a waiter the retry frees the slot.
	std::optional<decltype(std::declval<cat_t::pin&>().reader().acquire())> g2;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0), false);
		g2.emplace(p.reader().acquire());
	});
	CHECK(w.cat->begin_close(0));
	w.exec.run();
	CHECK(st(w, 0) == S::closing);
	g2.reset();
	pass(w);
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0);
	strata::detail::abort_on_owner_break().store(true);
	std::printf("stuck close: ok\n");
}

// --- C15.5: a throw from close() ends the close at the prior state --------------------------------------

void close_throws() {
	bool armed = false;
	W w(1, 2, false, false, [&](const char* site, bool) {
		if (armed && std::strcmp(site, "close.call") == 0) {
			armed = false;
			throw std::logic_error("close refused");
		}
	});
	touch(w, 0);
	armed = true;
	CHECK(w.cat->begin_close(0));
	w.exec.run();
	CHECK(!armed && st(w, 0) == S::open && pins(w, 0) == 0 && w.cat->stats().close_failures == 1);
	CHECK(edge(w, S::closing, S::open) == 1);
	CHECK(w.cat->begin_close(0));  // the slot is usable and closes next time
	w.exec.run();
	CHECK(st(w, 0) == S::free);
	std::printf("close throws: ok\n");
}

// --- S7.6: a crossing posts one coalesced pass that reposts while over the soft cap ----------------------

void coalesced_pass() {
	W w(3, 2, false, false);
	for (int i = 0; i < 3; ++i) { touch(w, i); }
	for (int i = 0; i < 100; ++i) { w.cat->post_pass(); }
	w.exec.run();
	CHECK(w.cat->stats().passes == 1);  // 100 crossings, one pass
	w.cat->post_pass();
	w.exec.run();
	CHECK(w.cat->stats().passes == 2);
	std::printf("coalesced pass: ok\n");
}

void repost_while_over() {
	W w(7, 2, false, false);
	options(w, [](cat_t::host& h) { h.soft_open = 2; h.pass_posts = 2; });
	for (int i = 0; i < 7; ++i) { touch(w, i); age(w, 1 * ms); }
	w.cat->post_pass();
	w.exec.run();
	const auto s = w.cat->stats();
	CHECK(w.cat->open_slots() == 2);          // closed down to the cap, no further
	CHECK(s.passes == 3 && s.pass_reposts == 2 && s.pass_closes == 5);  // two posts, two posts, one
	CHECK(w.cat->table_size() == 2 && st(w, 6) == S::open && st(w, 5) == S::open);  // the newest two stay
	// A pass that can post nothing does not repost: every slot pinned.
	W v(3, 2, false, false);
	options(v, [](cat_t::host& h) { h.soft_open = 1; });
	std::vector<cat_t::try_result> held;
	for (int i = 0; i < 3; ++i) { touch(v, i); }
	for (int i = 0; i < 3; ++i) { held.push_back(v.cat->try_pin(kn(i), pin_kind::request, false)); }
	for (auto& h : held) { CHECK(h.status == cat_t::try_status::pinned); }
	v.cat->post_pass();
	v.exec.run();
	CHECK(v.cat->stats().passes == 1 && v.cat->stats().pass_reposts == 0);
	std::printf("repost while over: ok\n");
}

// The relief a begun demotion or close will bring is booked until it ends: a second pass before the chores run does
// not repeat it (the serving of the item 6 seam's corrections 1 and 3).
void relief_in_flight() {
	W w(3, 2, false);
	for (int i = 0; i < 3; ++i) { touch(w, i); }
	const std::uint64_t m0 = maps(w, 0), total = m0 + maps(w, 1) + maps(w, 2);
	CHECK(m0 > 0);
	options(w, [&](cat_t::host& h) { h.soft_hot = total - m0; });  // over by exactly one slot's maps
	for (int i = 0; i < 3; ++i) { touch(w, i); }
	CHECK(w.cat->pass());  // demotes one; the caps still ask nothing more once it is counted
	CHECK(w.cat->pending_maps() == m0);
	(void)w.cat->pass();  // the chores have not run: the booked relief covers the excess
	w.exec.run();
	CHECK(w.cat->stats().pass_demotes == 1 && w.cat->pending_maps() == 0);
	// A close begun books its descriptors and vnodes until it ends.
	W v(2, 2, false, false);
	options(v, [](cat_t::host& h) { h.soft_open = 1; });
	touch(v, 0);
	age(v, 1 * ms);
	touch(v, 1);
	const std::uint64_t d = v.cat->count_of(0, resource::descriptors);
	CHECK(d > 0);
	(void)v.cat->pass();
	CHECK(v.cat->pending_descriptors() == d && v.cat->stats().pass_closes == 1);
	(void)v.cat->pass();  // the close is counted as open going away: not repeated
	CHECK(v.cat->stats().pass_closes == 1);
	v.exec.run();
	CHECK(v.cat->pending_descriptors() == 0 && v.cat->pending_vnodes() == 0 && v.cat->open_slots() == 1);
	std::printf("relief in flight: ok\n");
}

// The budget's listener posts the pass: a reservation that crosses a descriptor soft cap.
void budget_crossing() {
	W w(8, 2, false, false, {}, 8, 700);  // descriptors: soft 393, hard 498
	for (int i = 0; i < 8; ++i) { touch(w, i); }
	w.cat->start();
	CHECK(w.timers_pending() == 1 && w.timer_delays.back() == 1 * sec);
	const std::uint64_t soft = w.budget.soft(resource::descriptors);
	std::vector<strata::resource_reservation> held;
	held.push_back(w.budget.reserve(resource::descriptors, soft - w.budget.used(resource::descriptors)));
	w.exec.run();
	CHECK(w.cat->stats().passes == 0 && w.cat->open_slots() == 8);
	held.push_back(w.budget.reserve(resource::descriptors, 3));  // crosses
	w.exec.run();
	CHECK(w.cat->stats().passes >= 1 && w.cat->stats().pass_closes >= 1 && w.cat->open_slots() < 8);
	w.cat->stop_sweep();
	held.clear();
	std::printf("budget crossing: ok\n");
}

// The sweep re-arms its timer once per period, whatever posts passes.
void periodic_timer() {
	W w(1, 2, false, false);
	w.cat->start();
	CHECK(w.timers_pending() == 1);
	for (int i = 0; i < 3; ++i) {
		CHECK(w.fire_next());
		w.exec.run();
		CHECK(w.timers_pending() == 1);
	}
	CHECK(w.cat->stats().passes == 3);
	w.cat->post_pass();
	w.cat->post_pass();
	w.exec.run();
	CHECK(w.timers_pending() == 1 && w.cat->stats().passes == 4);
	w.cat->stop_sweep();
	CHECK(w.fire_next());
	w.exec.run();
	CHECK(w.timers_pending() == 0);
	std::printf("periodic timer: ok\n");
}

// --- S6's close at the bound is the sweep's: the pass calls it ------------------------------------------------

std::uint64_t backoff_ns(unsigned failures) { return (10 * ms) << (failures - 1); }
void fail_wakes(W& w, unsigned n) {
	w.kern.fail_map = EMFILE;
	for (unsigned k = 1; k <= n; ++k) {
		run_task(w, [&]() -> task::task<void> {
			try {
				auto p = co_await w.cat->pin_request(kn(0));
			} catch (const std::system_error&) {
			}
		});
		age(w, backoff_ns(k));
	}
	CHECK(w.cat->wake_failures_of(0) == n);
}

// A warm slot whose wake failed 7 times is left alone; at the 8th a pass commits what is uncommitted and closes
// it; a failed commit makes the pass wait for the 10 s probe time; the write survives the close.
void pass_closes_at_the_bound() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(0), false);
		CHECK(c.index().apply(tx({erase(key_of(0))}), {}).result == verdict::accepted);
		c.index().refresh();
	});
	CHECK(w.cat->begin_demote(0));
	w.exec.run();
	fail_wakes(w, 7);
	w.kern.fail_map = 0;
	pass(w);
	CHECK(st(w, 0) == S::owed && w.cat->stats().bound_closes == 0 && w.commits == 0);  // below the bound
	w.kern.fail_map = EMFILE;
	run_task(w, [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->pin_request(kn(0));
		} catch (const std::system_error&) {
		}
	});
	CHECK(w.cat->wake_failures_of(0) == 8);
	w.kern.fail_map = 0;
	w.fail_commit = EIO;
	const std::uint64_t t0 = w.exec.now();
	pass(w);
	CHECK(st(w, 0) == S::owed && w.commits == 1 && w.cat->failing() == 1 && w.cat->probe_at_of(0) == t0 + 10 * sec);
	pass(w);
	CHECK(w.commits == 1);  // not before the probe time
	age(w, 10 * sec);
	w.fail_commit = 0;
	pass(w);
	CHECK(st(w, 0) == S::free && w.commits == 2 && w.cat->stats().bound_closes == 1 && w.cat->failing() == 0);
	age(w, 20 * sec);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(!p.index().get(key_of(0)));
		CHECK(p.index().get(key_of(1)) == std::optional<std::string>("v1"));
	});
	std::printf("pass closes at the bound: ok\n");
}

// --- FREE: the sweep erases a negative entry 10 s past its retry time --------------------------------------

void negative_entry_erased() {
	W w(2, 2, false);
	w.fail_resolve = EIO;
	w.fail_left = 100;
	auto r = w.cat->try_pin(kn(0), pin_kind::request, false);
	w.exec.run();
	CHECK(st(w, 0) == S::failed && w.cat->table_size() == 1);
	age(w, 10 * ms + 10 * sec - 1);
	pass(w);
	CHECK(st(w, 0) == S::failed && w.cat->stats().erased == 0);
	age(w, 1);
	pass(w);
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && w.cat->stats().erased == 1);
	CHECK(edge(w, S::failed, S::free) == 1 && w.cat->occupied() == 0 && gen(w, 0) > 1);
	// A pin that arrives past the retry time takes the slot back before the pass: it reopens, nothing is erased.
	W v(1, 2, false);
	v.fail_resolve = EIO;
	v.fail_left = 1;
	(void)v.cat->try_pin(kn(0), pin_kind::request, false);
	v.exec.run();
	CHECK(st(v, 0) == S::failed);
	age(v, 11 * ms);
	(void)v.cat->try_pin(kn(0), pin_kind::request, false);
	age(v, 11 * sec);
	pass(v);
	CHECK(st(v, 0) == S::open && v.cat->stats().erased == 0 && v.cat->table_size() == 1);
	std::printf("negative entry erased: ok\n");
}

// A pin that retries the open between the pass's look at a negative entry and its erase wins: nothing is erased.
void negative_entry_race() {
	bool armed = false;
	W* wp = nullptr;
	W w(1, 2, false, true, [&](const char* site, bool) {
		if (armed && std::strcmp(site, "erase.pre") == 0) {
			armed = false;
			(void)wp->cat->try_pin(kn(0), pin_kind::request, false);  // past the retry time: failed to opening, no pin
		}
	});
	wp = &w;
	w.fail_resolve = EIO;
	w.fail_left = 1;
	(void)w.cat->try_pin(kn(0), pin_kind::request, false);
	w.exec.run();
	CHECK(st(w, 0) == S::failed);
	age(w, 11 * sec);  // past the retry time and the erase time
	armed = true;
	w.cat->post_pass();
	w.exec.run();
	CHECK(!armed && st(w, 0) == S::open && w.cat->stats().erased == 0 && w.cat->table_size() == 1);
	wp = nullptr;
	std::printf("negative entry race: ok\n");
}

// A reused slot starts clean: the new key's first failure backs off 10 ms, not its predecessor's doubled delay (I3-N3).
void reuse_is_clean() {
	W w(2, 2, false);
	w.fail_resolve = EIO;
	w.fail_left = 3;
	for (int i = 0; i < 3; ++i) {
		(void)w.cat->try_pin(kn(0), pin_kind::request, false);
		w.exec.run();
		age(w, 10 * sec);
	}
	CHECK(st(w, 0) == S::failed && w.cat->wake_failures_of(0) == 3);  // three failed opens: the next backoff is 40 ms
	age(w, 20 * sec);
	pass(w);
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0);
	age(w, 5 * sec);
	w.fail_resolve = EIO;
	w.fail_left = 1;
	(void)w.cat->try_pin(kn(1), pin_kind::request, false);  // the same slot, another key
	w.exec.run();
	CHECK(w.cat->key_at(0) == kn(1) && st(w, 0) == S::failed);
	const int resolves = w.resolves;
	age(w, 9 * ms);
	auto fast = w.cat->try_pin(kn(1), pin_kind::request, false);
	CHECK(fast.status == cat_t::try_status::failed && w.resolves == resolves);  // inside its own 10 ms
	age(w, 2 * ms);
	auto again = w.cat->try_pin(kn(1), pin_kind::request, false);  // 11 ms: its first failure's backoff has passed
	CHECK(again.status == cat_t::try_status::would_wait);
	w.exec.run();
	CHECK(w.resolves == resolves + 1);
	CHECK(st(w, 0) == S::open);
	// The new key's last use is its own open, not the old key's.
	CHECK(w.cat->last_use_of(0) >= w.exec.now() - 11 * ms);
	std::printf("reuse is clean: ok\n");
}

// --- S7.7, S7.8: shutdown --------------------------------------------------------------------------------


void shutdown_commits_and_closes() {
	payload::destroyed = 0;
	payload::destroyed_in_close = 0;
	W w(6, 2, false, false);
	for (int i = 0; i < 5; ++i) { touch(w, i); }
	write_uncommitted(w, 0, "a0");  // committed by shutdown
	write_uncommitted(w, 1, "a1");  // its commit fails: closed uncommitted, named
	write_uncommitted(w, 2, "a2");
	w.fail_commit = EIO;
	w.fail_commit_serial = 2;  // the second open's H
	// k3 is a negative entry, k4 is pinned.
	w.fail_resolve = EIO;
	w.fail_left = 1;
	(void)w.cat->try_pin(kn(5), pin_kind::request, false);
	w.exec.run();
	CHECK(st(w, 5) == S::failed);
	auto held = w.cat->try_pin(kn(4), pin_kind::request, false);
	CHECK(held.status == cat_t::try_status::pinned);
	w.start_shutdown();
	w.exec.run();
	CHECK(w.cat->shutting_down() && !w.sd_done && !w.cat->shut_down());  // the pinned slot holds it
	CHECK(w.cat->occupied() == 1 && st(w, 4) == S::open && pins(w, 4) == 1);
	// A new pin fails with catalog_closed, by key and by handle.
	auto late = w.cat->try_pin(kn(3), pin_kind::request, false);
	CHECK(late.status == cat_t::try_status::failed);
	bool closed = false;
	try {
		std::rethrow_exception(late.error);
	} catch (const strata::catalog_closed&) {
		closed = true;
	} catch (...) {
	}
	CHECK(closed);
	held.p.release();
	w.exec.run();
	const shutdown_result res = w.sd_res;
	CHECK(w.sd_done && w.cat->shut_down() && w.cat->occupied() == 0 && w.cat->table_size() == 0);
	CHECK(res.uncommitted.size() == 1 && res.uncommitted[0] == kn(1));
	CHECK(w.cat->stats().shutdown_commits == 3 && w.cat->stats().shutdown_commit_failures == 1);
	CHECK(w.budget.used(resource::descriptors) == 0 && w.budget.used(resource::maps) == 0);
	CHECK(payload::destroyed == 5 && payload::destroyed_in_close == 0);  // H goes after its close chore returned
	CHECK(reopen(w, 0)->get("a0") == std::optional<std::string>("w"));
	CHECK(!reopen(w, 1)->get("a1"));  // closed uncommitted
	CHECK(reopen(w, 2)->get("a2") == std::optional<std::string>("w"));
	// Awaiting it again completes at once.
	w.start_shutdown();
	w.exec.run();
	CHECK(w.sd_done && w.sd_res.uncommitted.size() == 1);
	std::printf("shutdown commits and closes: ok\n");
}

// Shutdown reaches slots in every state: opening, demoting, owed, waking, closing.
void shutdown_every_state() {
	W w(6, 2, true);
	for (int i = 0; i < 5; ++i) { touch(w, i); }
	CHECK(w.cat->begin_demote(0));   // demoting
	CHECK(w.cat->begin_demote(1));
	w.exec.run();                    // owed
	CHECK(st(w, 1) == S::owed);
	CHECK(w.cat->begin_demote(2));   // demoting
	CHECK(w.cat->begin_close(3));    // closing
	auto opening = w.cat->try_pin(kn(5), pin_kind::request, false);  // opening
	CHECK(opening.status == cat_t::try_status::would_wait);
	w.start_shutdown();
	w.exec.run();
	CHECK(w.sd_done && w.cat->occupied() == 0 && w.cat->table_size() == 0);
	std::printf("shutdown every state: ok\n");
}

// A shutdown with nothing open completes at once; one that races a pin's wake completes after it.
void shutdown_empty_and_waiting() {
	{
		W w(1, 2, false);
		w.start_shutdown();
		w.exec.run();
		CHECK(w.sd_done && w.sd_res.uncommitted.empty() && w.cat->shut_down());
	}
	{  // a request waiting on `opening` gets its pin, then shutdown waits for its release
		W w(1, 2, false);
		std::optional<cat_t::pin> p;
		std::function<task::task<void>()> pinner = [&]() -> task::task<void> { p.emplace(co_await w.cat->pin_request(kn(0), false)); };
		task::spawn(task::scheduler_ref::of(w.exec.scheduler()), pinner());
		w.start_shutdown();
		w.exec.run();
		CHECK(p && !w.sd_done);
		p.reset();
		w.exec.run();
		CHECK(w.sd_done && w.cat->occupied() == 0);
	}
	std::printf("shutdown empty and waiting: ok\n");
}

// A transition that ends after shutdown() began closes the slot at its own end: an open, a demotion.
void transitions_race_shutdown() {
	for (const char* site : {"open.end", "transition.end", "wake.end"}) {
		bool armed = false;
		W* wp = nullptr;
		std::unique_ptr<cat_t::shutdown_awaiter> aw;
		W w(1, 2, std::strcmp(site, "wake.end") == 0, true, [&](const char* at, bool) {
			if (armed && std::strcmp(at, site) == 0) {
				armed = false;
				aw.reset(new cat_t::shutdown_awaiter(wp->cat->shutdown()));
				(void)aw->await_ready();  // joins: shutting_ is set while the transition is mid-flight
			}
		});
		wp = &w;
		armed = true;
		if (std::strcmp(site, "open.end") == 0) {
			(void)w.cat->try_pin(kn(0), pin_kind::request, false);  // a miss: opening, no pin
		} else if (std::strcmp(site, "wake.end") == 0) {
			touch(w, 0);
			armed = false;
			CHECK(w.cat->begin_demote(0));
			w.exec.run();
			CHECK(st(w, 0) == S::owed);
			armed = true;
			CHECK(w.cat->try_pin(kn(0), pin_kind::request, false).status == cat_t::try_status::would_wait);  // starts the wake, holds nothing
		} else {
			touch(w, 0);
			armed = true;
			CHECK(w.cat->begin_demote(0));
		}
		w.exec.run();
		CHECK(!armed && w.cat->shut_down() && w.cat->occupied() == 0 && w.cat->table_size() == 0);
		wp = nullptr;
	}
	std::printf("transitions race shutdown: ok\n");
}

// The shutdown close meets a lease held by a call that is no pin's: it tries again shortly, through the timer.
void shutdown_lease_busy() {
	W w(1, 2, false);
	touch(w, 0);
	std::optional<strata::lease_scope> hold;
	hold.emplace(w.cat->index_of(0)->lease());
	w.start_shutdown();
	w.exec.run();
	CHECK(!w.sd_done && st(w, 0) == S::closing && w.timers_pending() == 1);
	hold.reset();
	w.settle();
	CHECK(w.sd_done && w.cat->occupied() == 0);
	std::printf("shutdown lease busy: ok\n");
}

// --- C15.2: a catalog destroyed with slots open leaks their IO ---------------------------------------------

void destroyed_open_leaks() {
	strata::detail::abort_on_owner_break().store(false);
	const std::uint64_t breaks = strata::detail::off_owner_changes().load();
	W w(2, 2, false);
	touch(w, 0);
	touch(w, 1);
	CHECK(w.budget.used(resource::descriptors) > 0);
	w.cat.reset();
	CHECK(strata::detail::off_owner_changes().load() == breaks + 1);
	CHECK(w.budget.used(resource::descriptors) > 0);  // the IO is still open: its reservations stay
	bool locked = false;
	try {
		(void)reopen(w, 0);
	} catch (...) {
		locked = true;
	}
	CHECK(locked);  // its lock is kept
	strata::detail::abort_on_owner_break().store(true);
	std::printf("destroyed open leaks: ok\n");
}

// --- C15.1: a catalog destroyed with a pin held asserts (debug builds) --------------------------------------

void destroyed_pinned_asserts() {
#ifndef NDEBUG
	{  // the assert: the destructor aborts
		const pid_t pid = ::fork();
		if (pid == 0) {
			W w(1, 2, false);
			touch(w, 0);
			auto held = w.cat->try_pin(kn(0), pin_kind::request, false);
			CHECK(held.status == cat_t::try_status::pinned);
			w.cat.reset();
			::_exit(0);  // no assert: the pin would be released into freed memory
		}
		int status = 0;
		CHECK(::waitpid(pid, &status, 0) == pid);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	}
	{  // with the assert off, the pin is reported on top of the open slot: two breaks
		const pid_t pid = ::fork();
		if (pid == 0) {
			strata::detail::abort_on_owner_break().store(false);
			const std::uint64_t before = strata::detail::off_owner_changes().load();
			{
				W w(1, 2, false);
				touch(w, 0);
				auto held = w.cat->try_pin(kn(0), pin_kind::request, false);
				w.cat.reset();
				::_exit(strata::detail::off_owner_changes().load() == before + 2 ? 0 : 3);  // the pin is never released
			}
		}
		int status = 0;
		CHECK(::waitpid(pid, &status, 0) == pid);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	std::printf("destroyed pinned asserts: ok\n");
#endif
}

}  // namespace

int main(int argc, char** argv) {
	const char* only = argc > 1 ? argv[1] : nullptr;
#define RUN(f) \
	if (!only || !std::strcmp(only, #f)) { f(); }
	RUN(demote_window)
	RUN(demote_idle)
	RUN(close_from_warm_only)
	RUN(close_chore_refuses)
	RUN(warm_idle_closes)
	RUN(close_window)
	RUN(owed_with_maps_closes)
	RUN(queued_reclaim)
	RUN(reclaim_zero_under_demoting)
	RUN(stuck_close)
	RUN(close_throws)
	RUN(coalesced_pass)
	RUN(repost_while_over)
	RUN(relief_in_flight)
	RUN(budget_crossing)
	RUN(periodic_timer)
	RUN(pass_closes_at_the_bound)
	RUN(negative_entry_erased)
	RUN(negative_entry_race)
	RUN(reuse_is_clean)
	RUN(shutdown_commits_and_closes)
	RUN(shutdown_every_state)
	RUN(shutdown_empty_and_waiting)
	RUN(transitions_race_shutdown)
	RUN(shutdown_lease_busy)
	RUN(destroyed_open_leaks)
	RUN(destroyed_pinned_asserts)
	if (failures) {
		std::fprintf(stderr, "catalog_sweep: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_sweep: ok");
	return 0;
}
