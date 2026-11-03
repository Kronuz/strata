// The cold tier's step 2, item 6 (docs/design/cold-tier.md D4, D6; CT-R10, CT-R31, CT-R41; criteria S8.6 to S8.8):
// the closing policy and the frequency bits. The ghost against a list; the catalog in lock-step with the
// S3-FIFO model, request by request; each named choice of D6 on a scene of its own; every request-pin row of D4's
// table setting the frequency for a use. inline_executor: single-threaded, deterministic. Mutants of catalog.hh
// are run by .scratch/mutants6.py (docs/status/cold-tier-step2.md, item 6).
#include "catalog_world.hh"
#include "s3fifo_model.hh"
#include "strata/core/ghost_set.hh"

#include <algorithm>
#include <deque>
#include <list>
#include <random>

namespace {

using W = world<inline_executor>;
using strata::ghost_set;
using strata::pin_kind;
using strata::slot_state;
using S = slot_state;

std::string kn(int i) { return "k" + std::to_string(i); }
template <class F>
void run_task(W& w, F&& body) {
	task::run(w.exec.scheduler(), body());
}
std::uint64_t pins(W& w, std::uint32_t i) { return cat_t::pins_of(w.cat->word_of(i)); }
S st(W& w, std::uint32_t i) { return cat_t::state_of(w.cat->word_of(i)); }
unsigned freq(W& w, std::uint32_t i) { return cat_t::freq_of(w.cat->word_of(i)); }
constexpr std::uint32_t nope = ~std::uint32_t(0);

// The slot holding key i, or nope.
std::uint32_t slot_of(W& w, int i) {
	for (std::uint32_t s = 0; s < w.cat->slots_made(); ++s) {
		if (st(w, s) != S::free && w.cat->key_at(s) == kn(i)) { return s; }
	}
	return nope;
}
// One request: a use by key, then a re-pin by handle that is not one; both released.
void touch(W& w, int i) {
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(i), true);
		auto q = co_await w.cat->repin(p.where(), kn(i), pin_kind::request, false);
	});
}

// The invariants at rest: every open or warm slot is in one queue and no other slot is; the queues' sizes add up.
void queues_agree(W& w) {
	std::uint64_t live = 0;
	for (std::uint32_t s = 0; s < w.cat->slots_made(); ++s) {
		const S state = st(w, s);
		const bool resident = state == S::open || state == S::owed;
		live += resident;
		CHECK((w.cat->queue_of(s) != 0) == resident);
	}
	const auto q = w.cat->queue_sizes();
	CHECK(q.small + q.main == live);
}

// --- the ghost (D6) ----------------------------------------------------------------------------------

// Against a list: add drops the oldest past the capacity, a hit removes its entry, a re-add makes the hash the
// newest. Hashes share their high half (the table's key) in groups, so the full hash decides.
void ghost_oracle() {
	std::mt19937_64 rng(7);
	for (std::size_t cap : {0, 1, 2, 5, 64}) {
		ghost_set g(cap);
		std::list<std::uint64_t> m;  // newest first
		auto hash = [](std::uint64_t k) { return ((k % 7) << 32) | k; };
		for (int step = 0; step < 20000; ++step) {
			const std::uint64_t h = hash(rng() % (2 * cap + 3));
			switch (rng() % 3) {
			case 0:
				g.add(h);
				if (cap) {
					m.remove(h);
					m.push_front(h);
					if (m.size() > cap) { m.pop_back(); }
				}
				break;
			case 1: {
				const bool in = std::find(m.begin(), m.end(), h) != m.end();
				CHECK(g.take(h) == in);
				m.remove(h);
				break;
			}
			default: CHECK(g.contains(h) == (std::find(m.begin(), m.end(), h) != m.end()));
			}
			CHECK(g.size() == m.size());
		}
	}
}

// --- lock-step with the model (S8.1's behavior at a small scale; S8.7) ------------------------------------

// Request by request, the catalog's miss or hit equals the design's rule on lists (model_total) at the same capacity, with the pass after
// every request and the chores run: the small FIFO's tenth, the ghost, the frequency from uses only (the re-pin by
// handle is none), the move to main with the frequency cleared, the main FIFO's decrement.
void lockstep(double theta, std::size_t keys, std::size_t cap, std::size_t requests) {
	W w(keys, 1, false, false);
	w.remake(cap, 0);
	s8::model_total m(cap);
	const s8::zipf z(keys, theta);
	std::mt19937_64 rng(42);
	std::uint64_t misses = 0;
	for (std::size_t i = 0; i < requests; ++i) {
		const int k = int(z.draw(rng));
		const std::uint64_t before = w.cat->misses();
		touch(w, k);
		const bool miss = w.cat->misses() != before;
		w.cat->policy_pass(cat_t::policy_limits::lifted());
		w.exec.run();
		misses += miss;
		if (miss != !m.access(k)) {
			std::fprintf(stderr, "lockstep theta %.2f: request %zu (key %d) differs: catalog %s, model %s\n", theta, i, k, miss ? "miss" : "hit", miss ? "hit" : "miss");
			CHECK(false);
			return;
		}
		if (i % 5000 == 4999) { queues_agree(w); }
	}
	queues_agree(w);
	const auto q = w.cat->queue_sizes();
	CHECK(q.small + q.main == cap && q.small <= w.cat->small_cap());
	CHECK(misses > requests / 20 && misses < requests * 19 / 20);  // not a degenerate trace
	const auto s = w.cat->stats();
	CHECK(s.promoted > 0 && s.decayed > 0 && s.ghost_hits > 0 && s.evictions > 0);
	std::printf("lockstep theta %.2f: %zu requests, %llu misses; promoted %llu decayed %llu ghost hits %llu closes %llu\n", theta, requests, (unsigned long long)misses,
		(unsigned long long)s.promoted, (unsigned long long)s.decayed, (unsigned long long)s.ghost_hits, (unsigned long long)s.evictions);
}

// --- the policy's choices, one scene each (S8.7) -----------------------------------------------------------

// A new slot enters the small FIFO; an evicted key's hash enters the ghost and its next open goes to the main FIFO.
void ghost_and_entry() {
	W w(12, 1, false, false);
	w.remake(10, 0);  // a small FIFO of 1, a ghost of 9
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	for (std::uint32_t s = 0; s < 10; ++s) { CHECK(w.cat->queue_of(s) == 1); }
	CHECK(w.cat->queue_sizes().small == 10 && w.cat->queue_sizes().ghost == 0);
	touch(w, 10);
	const auto r = w.cat->policy_pass();
	w.exec.run();
	CHECK(r.closes == 1 && slot_of(w, 0) == nope);  // the small FIFO's head, its frequency clear
	CHECK(w.cat->queue_sizes().small == 10 && w.cat->queue_sizes().ghost == 1 && w.cat->stats().ghost_adds == 1);
	touch(w, 0);  // its hash is in the ghost: the main FIFO
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(s0 != nope && w.cat->queue_of(s0) == 2 && w.cat->stats().ghost_hits == 1 && w.cat->queue_sizes().ghost == 0);
	touch(w, 11);  // a key never seen: the small FIFO
	CHECK(w.cat->queue_of(slot_of(w, 11)) == 1);
	queues_agree(w);
}

// The small FIFO's head with its frequency set moves to the main FIFO with the frequency cleared; the main FIFO's
// head is decremented and put back while it is set, else closed, and a key closed from the main FIFO is not ghosted;
// the small FIFO is taken only while it is over its share.
void promote_and_decay() {
	W w(14, 1, false, false);
	w.remake(10, 0);  // a small FIFO of 1, a ghost of 9
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 0), touch(w, 0);  // k0: frequency 2 (a use each)
	touch(w, 10);
	auto r = w.cat->policy_pass();
	w.exec.run();
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(r.promoted == 1 && r.closes == 1 && s0 != nope && r.decayed == 0);
	CHECK(w.cat->queue_of(s0) == 2 && freq(w, s0) == 0);  // moved, the frequency cleared
	CHECK(slot_of(w, 1) == nope);                         // then the next head, k1, closed
	CHECK(w.cat->queue_sizes().small == 9 && w.cat->queue_sizes().main == 1);
	// Each evicted key that comes back is a ghost hit: it enters the main FIFO, and the small FIFO, which a miss no
	// longer refills, shrinks to its share.
	for (int j = 1; j <= 8; ++j) {
		touch(w, j);
		CHECK(w.cat->queue_of(slot_of(w, j)) == 2);
		w.cat->policy_pass();
		w.exec.run();
	}
	CHECK(w.cat->queue_sizes().small == 1 && w.cat->queue_sizes().main == 9 && w.cat->stats().ghost_adds == 9);
	touch(w, 0), touch(w, 0);  // k0, in the main FIFO: frequency 2
	touch(w, 9);               // k9 was closed from the small FIFO: a ghost hit
	CHECK(freq(w, s0) == 2 && w.cat->queue_sizes().main == 10);
	r = w.cat->policy_pass();  // within its share the small FIFO is left alone: k0 is decremented and put back, k1 closes
	w.exec.run();
	CHECK(r.decayed == 1 && r.promoted == 0 && r.closes == 1);
	CHECK(st(w, s0) == S::open && freq(w, s0) == 1 && w.cat->queue_of(s0) == 2 && slot_of(w, 1) == nope && slot_of(w, 2) != nope);
	CHECK(w.cat->stats().ghost_adds == 9);  // closed from the main FIFO: no ghost
	touch(w, 1);
	CHECK(w.cat->queue_of(slot_of(w, 1)) == 1);  // so its next open enters the small FIFO
	queues_agree(w);
}

// A hot candidate (it holds a map) is demoted first and closes on a later pass.
void hot_first() {
	W w(12, 1, true, true);  // defer_io, eager: hot until demoted
	w.remake(10, 0);
	for (int i = 0; i <= 10; ++i) { touch(w, i); }
	auto r = w.cat->policy_pass();
	CHECK(r.closes == 0 && r.demotes == 11);
	w.exec.run();
	for (int i = 0; i <= 10; ++i) { CHECK(st(w, slot_of(w, i)) == S::owed); }
	r = w.cat->policy_pass();
	w.exec.run();
	CHECK(r.closes == 1 && r.demotes == 0);
	int gone = 0;
	for (int i = 0; i <= 10; ++i) { gone += slot_of(w, i) == nope; }
	CHECK(gone == 1 && w.cat->stats().evict_demotes == 11);
	queues_agree(w);
}

// A candidate that is pinned is put back, not closed; the next one closes.
void pinned_head() {
	W w(12, 1, false, false);
	w.remake(10, 0);
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	auto held = w.cat->try_pin(kn(0), pin_kind::chore, false);  // k0 pinned, no use
	CHECK(held.status == cat_t::try_status::pinned);
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	const auto r = w.cat->policy_pass();
	w.exec.run();
	CHECK(r.busy >= 1 && r.closes == 1);
	CHECK(st(w, s0) == S::open && pins(w, s0) == 1 && w.cat->queue_of(s0) == 1);
	CHECK(slot_of(w, 1) == nope);
	held.p.release();
	queues_agree(w);
}

// The decision to close is one compare-exchange on the word: a use that lands between the choice and the
// compare-exchange (a pin and its release, so the slot is unpinned again) keeps the slot.
void used_since() {
	bool armed = false;
	W* wp = nullptr;
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		if (armed && std::string_view(site) == "sweep.close") {
			armed = false;
			auto p = wp->cat->try_pin(kn(0));  // a use of the candidate
		}
	});
	wp = &w;
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	armed = true;
	const auto r = w.cat->policy_pass();
	w.exec.run();
	CHECK(!armed);
	CHECK(st(w, s0) == S::open && pins(w, s0) == 0 && freq(w, s0) == 1);  // kept, its use counted
	CHECK(slot_of(w, 1) == nope && r.closes == 1 && r.busy >= 1);         // the next head went
	queues_agree(w);
}

// A candidate closed and freed behind the pass's back is not put back (its generation moved on).
void closed_behind_the_pass() {
	bool armed = false;
	W* wp = nullptr;
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		if (armed && std::string_view(site) == "sweep.close") {
			armed = false;
			const std::uint32_t s = slot_of(*wp, 0);
			CHECK(wp->cat->begin_close(s));  // another closer takes the candidate
			wp->exec.run();                  // and finishes: the slot is free
		}
	});
	wp = &w;
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	armed = true;
	w.cat->policy_pass();
	w.exec.run();
	CHECK(!armed && st(w, s0) == S::free && w.cat->queue_of(s0) == 0);
	queues_agree(w);
}

// A close that the index refuses (a map made between the choice and the chore) puts the slot back and leaves it open.
void refused_close() {
	bool armed = false;
	W* wp = nullptr;
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		if (armed && std::string_view(site) == "sweep.close") {
			armed = false;
			auto p = wp->cat->try_pin(kn(0), pin_kind::chore, false);  // a read maps the segment, no use
			CHECK(p.p.index().get(key_of(0)).has_value());
		}
	});
	wp = &w;
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	armed = true;
	const auto r = w.cat->policy_pass();  // k0 is the head: its close is begun, then refused
	CHECK(!armed && r.closes == 1 && st(w, s0) == S::closing);
	w.exec.run();
	CHECK(st(w, s0) == S::open && pins(w, s0) == 0 && w.cat->queue_of(s0) == 2 && w.cat->stats().ghost_adds == 0);  // back, in the main FIFO, no ghost
	queues_agree(w);
}

// A close that no pass begins (a direct one) takes its slot out of the queue, and is not ghosted.
void direct_close() {
	W w(8, 1, false, false);
	w.remake(10, 0);
	for (int i = 0; i < 5; ++i) { touch(w, i); }
	const std::uint32_t s2 = slot_of(w, 2);
	CHECK(w.cat->begin_close(s2));
	w.exec.run();
	CHECK(st(w, s2) == S::free && w.cat->queue_of(s2) == 0);
	CHECK(w.cat->queue_sizes().small == 4 && w.cat->queue_sizes().ghost == 0);
	queues_agree(w);
	touch(w, 2);  // reuses the freed slot: it is in a queue again
	CHECK(slot_of(w, 2) == s2 && w.cat->queue_of(s2) == 1);
	queues_agree(w);
}

// Hot slots over soft_hot demote, the oldest last-use first among the slots a scan visits; the demotions in flight
// count, so a second pass before the chores run demotes none; a scan of 3 slots demotes within those.
void demote_hot() {
	W w(12, 1, true, true);
	w.remake(0, 8);
	for (int i = 0; i < 12; ++i) {
		touch(w, i);
		w.exec.scheduler().advance(1000);
	}
	const std::uint64_t per = w.cat->counts().get(resource::maps) / 12;
	CHECK(per >= 1 && w.cat->counts().get(resource::maps) == 12 * per);
	auto r = w.cat->policy_pass();
	CHECK(r.demotes == 4 && r.scanned == 12);
	CHECK(w.cat->policy_pass().demotes == 0);  // the four in flight count
	w.exec.run();
	for (int i = 0; i < 12; ++i) { CHECK(st(w, slot_of(w, i)) == (i < 4 ? S::owed : S::open)); }
	CHECK(w.cat->counts().get(resource::maps) == 8 * per);
	CHECK(w.cat->policy_pass().demotes == 0);
	W v(12, 1, true, true);
	v.remake(0, 8);
	for (int i = 0; i < 12; ++i) {
		touch(v, i);
		v.exec.scheduler().advance(1000);
	}
	cat_t::policy_limits lim;
	lim.scan = 3;
	r = v.cat->policy_pass(lim);
	CHECK(r.scanned == 3 && r.demotes == 3);
	v.exec.run();
	for (int i = 0; i < 12; ++i) { CHECK(st(v, slot_of(v, i)) == (i < 3 ? S::owed : S::open)); }
	r = v.cat->policy_pass(lim);  // the scan resumes at slot 3: k3 to k5, the oldest k3
	CHECK(r.scanned == 3 && r.demotes == 1);
	v.exec.run();
	CHECK(st(v, slot_of(v, 3)) == S::owed && st(v, slot_of(v, 4)) == S::open);
}

// A second pass while one runs does nothing (one at most); the pass's limits bound its work.
void policy_limits_hold() {
	W w(40, 1, false, false);
	w.remake(10, 0);
	for (int i = 0; i < 30; ++i) { touch(w, i); }  // 20 over, with no pass in between
	cat_t::policy_limits lim;
	lim.transitions = 4;
	auto r = w.cat->policy_pass(lim);
	CHECK(r.closes == 4);
	w.exec.run();
	CHECK(w.cat->queue_sizes().small + w.cat->queue_sizes().main == 26);
	lim = {};
	lim.visits = 3;
	r = w.cat->policy_pass(lim);
	CHECK(r.visited == 3 && r.closes == 3);
	w.exec.run();
	r = w.cat->policy_pass(cat_t::policy_limits::lifted());
	w.exec.run();
	CHECK(w.cat->queue_sizes().small + w.cat->queue_sizes().main == 10);
	queues_agree(w);
}

// --- every request-pin row sets the frequency for a use (CT-R41, C10; S8.6) -------------------------------

struct actor {
	std::function<task::task<void>()> fn;  // the coroutine's closure lives as long as it runs
	task::task<void> t;
};
// A request pin (or a chore pin) started at once on the test thread, so that it registers before the chores
// queued ahead of it run. The deque keeps the actors where they are.
actor& spawn_pin(W& w, std::deque<actor>& as, int key, bool use, pin_kind kind = pin_kind::request) {
	actor& a = as.emplace_back();
	a.fn = [&w, key, use, kind]() -> task::task<void> {
		try {
			auto p = kind == pin_kind::request ? co_await w.cat->pin_request(kn(key), use) : co_await w.cat->pin_chore(kn(key), use);
		} catch (...) {
		}
	};
	a.t = a.fn();
	const task::running_on on(task::scheduler_ref::of(w.exec.scheduler()));
	a.t.handle().resume();
	return a;
}

// A use by key sets the frequency in the compare-exchange it makes, in every state it meets, the synchronous
// try_pin included where it makes one; a chore pin, a re-pin by handle and a miss's own pin set none.
void use_rows() {
	W w(8, 1, true);  // defer_io, eager: demotion ends at owed
	std::deque<actor> as;
	// open: a use by key; a chore pin and a handle re-pin set none
	touch(w, 0);
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(freq(w, s0) == 0);  // the miss's own pin
	{
		auto p = w.cat->try_pin(kn(0));
		CHECK(p.status == cat_t::try_status::pinned && freq(w, s0) == 1);
	}
	run_task(w, [&]() -> task::task<void> {
		auto c = co_await w.cat->pin_chore(kn(0));
		auto h = co_await w.cat->repin(c.where(), kn(0));
		CHECK(freq(w, s0) == 1);
	});
	// demoting: a waiter's use is set when it registers; the demotion ends at waking for it
	touch(w, 1);
	const std::uint32_t s1 = slot_of(w, 1);
	CHECK(w.cat->begin_demote(s1) && st(w, s1) == S::demoting);
	spawn_pin(w, as, 1, true);
	CHECK(freq(w, s1) == 1);
	w.exec.run();
	CHECK(st(w, s1) == S::open && pins(w, s1) == 0 && freq(w, s1) == 1);
	// owed: the wake's winner, by key; waking: a second waiter
	touch(w, 2);
	const std::uint32_t s2 = slot_of(w, 2);
	CHECK(w.cat->begin_demote(s2));
	w.exec.run();
	CHECK(st(w, s2) == S::owed && freq(w, s2) == 0);
	spawn_pin(w, as, 2, true);
	CHECK(st(w, s2) == S::waking && freq(w, s2) == 1);
	spawn_pin(w, as, 2, true);
	CHECK(freq(w, s2) == 2);
	w.exec.run();
	CHECK(st(w, s2) == S::open && pins(w, s2) == 0);
	// owed: a try_pin makes the compare-exchange that starts the wake, and counts the use there (C10)
	CHECK(w.cat->begin_demote(s2));
	w.exec.run();
	CHECK(st(w, s2) == S::owed && freq(w, s2) == 2);
	w.cat->decay(s2), w.cat->decay(s2);
	CHECK(freq(w, s2) == 0);
	const auto t = w.cat->try_pin(kn(2));
	CHECK(t.status == cat_t::try_status::would_wait && st(w, s2) == S::waking && freq(w, s2) == 1);
	w.exec.run();
	// opening: the miss's own pin sets none, a second request registers one use
	spawn_pin(w, as, 3, true);
	const std::uint32_t s3 = slot_of(w, 3);
	CHECK(st(w, s3) == S::opening && freq(w, s3) == 0);
	spawn_pin(w, as, 3, true);
	CHECK(freq(w, s3) == 1);
	w.exec.run();
	// closing: a waiter on a close registers a use; the close reopens in place
	touch(w, 4);
	const std::uint32_t s4 = slot_of(w, 4);
	CHECK(w.cat->begin_demote(s4));
	w.exec.run();
	CHECK(w.cat->begin_close(s4) && st(w, s4) == S::closing);
	spawn_pin(w, as, 4, true);
	CHECK(freq(w, s4) == 1);
	w.exec.run();
	CHECK(slot_of(w, 4) == s4 && st(w, s4) == S::open && pins(w, s4) == 0);
	// failed: the retry after the backoff, by key and by try_pin (C10)
	for (bool sync : {false, true}) {
		const int k = sync ? 6 : 5;
		w.fail_resolve = EIO;
		w.fail_left = 1;
		spawn_pin(w, as, k, true);
		w.exec.run();
		const std::uint32_t sf = slot_of(w, k);
		CHECK(st(w, sf) == S::failed && freq(w, sf) == 0);
		w.exec.scheduler().advance(11'000'000);
		if (sync) {
			const auto r = w.cat->try_pin(kn(k));
			CHECK(r.status == cat_t::try_status::would_wait && st(w, sf) == S::opening && freq(w, sf) == 1);
		} else {
			spawn_pin(w, as, k, true);
			CHECK(st(w, sf) == S::opening && freq(w, sf) == 1);
		}
		w.exec.run();
		CHECK(st(w, sf) == S::open);
	}
	// saturation at 3
	for (int i = 0; i < 6; ++i) { touch(w, 7); }
	CHECK(freq(w, slot_of(w, 7)) == 3);
	as.clear();
}

}  // namespace

int main(int argc, char** argv) {
	const std::size_t requests = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 60000;
	ghost_oracle();
	ghost_and_entry();
	promote_and_decay();
	hot_first();
	pinned_head();
	used_since();
	closed_behind_the_pass();
	refused_close();
	direct_close();
	demote_hot();
	policy_limits_hold();
	use_rows();
	lockstep(0.99, 2000, 200, requests);
	lockstep(1.2, 2000, 200, requests);
	lockstep(0.6, 3000, 100, requests);
	if (failures) {
		std::fprintf(stderr, "catalog_policy: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_policy: ok");
	return 0;
}
