// The cold tier's step 2, item 6 (docs/design/cold-tier.md D4, D6; CT-R10, CT-R31, CT-R41; criteria S8.6 to S8.8):
// the closing policy and the frequency bits. The ghost against a list; the catalog in lock-step with the
// S3-FIFO model, request by request; each named choice of D6 on a scene of its own; every request-pin row of D4's
// table setting the frequency for a use. inline_executor: single-threaded, deterministic. Mutants of catalog.hh
// are run by .scratch/mutants6.py (docs/status/cold-tier-step2.md, item 6). The policy alone
// (catalog_detail::s3fifo) has cases of its own, each runnable by name: `catalog_policy_test fifo_order`.
#include "catalog_world.hh"
#include "s3fifo_model.hh"
#include "strata/core/catalog_s3fifo.hh"
#include "strata/core/ghost_set.hh"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

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

// --- the policy alone: catalog_detail::s3fifo over a table of links (D6) ----------------------------------

// The slots the policy sees: links it writes, a generation and a closing flag the test sets.
struct fake_slots {
	struct row {
		std::uint32_t prev = nope, next = nope;
		std::uint8_t q = 0;
		std::uint64_t gen = 0;
		bool closing = false;
	};
	std::vector<row>* rows;
	std::uint32_t& prev(std::uint32_t i) const { return (*rows)[i].prev; }
	std::uint32_t& next(std::uint32_t i) const { return (*rows)[i].next; }
	std::uint8_t& queue(std::uint32_t i) const { return (*rows)[i].q; }
	std::uint64_t gen(std::uint32_t i) const { return (*rows)[i].gen; }
	bool queueable(std::uint32_t i, std::uint64_t g) const { return (*rows)[i].gen == g && !(*rows)[i].closing; }
};
using fifo = strata::catalog_detail::s3fifo<fake_slots>;
using strata::catalog_detail::visit;
using limits_t = strata::catalog_detail::policy_limits;
using result_t = strata::catalog_detail::policy_result;

struct fifo_scene {
	std::vector<fake_slots::row> rows;
	fifo f;
	fifo_scene(std::size_t n, std::uint64_t soft_open) : rows(n), f(fake_slots{&rows}) { f.cap(soft_open); }
	// Queue q's slots, head to tail, by the links; each one's back link checked.
	std::vector<std::uint32_t> order(unsigned q) const {
		std::vector<std::uint32_t> out;
		std::uint32_t head = nope;
		for (std::uint32_t i = 0; i < rows.size(); ++i) {
			if (rows[i].q == q && rows[i].prev == nope) { head = i; }
		}
		for (std::uint32_t i = head, prev = nope; i != nope; prev = i, i = rows[i].next) {
			CHECK(rows[i].q == q && rows[i].prev == prev);
			out.push_back(i);
			if (out.size() > rows.size()) { break; }
		}
		return out;
	}
	static std::uint64_t hash(std::uint32_t i) { return 0x9e3779b97f4a7c15ull * (i + 1); }
};
using ids = std::vector<std::uint32_t>;

// A slot enters the small FIFO's tail; picks take the head first (FIFO order, by value).
void fifo_order() {
	fifo_scene s(8, 100);  // small cap 10: never over here, so the small FIFO is picked only while main is empty
	for (std::uint32_t i : {3, 1, 4, 0, 5}) { CHECK(!s.f.enqueue(i, fifo_scene::hash(i))); }
	CHECK((s.order(1) == ids{3, 1, 4, 0, 5}));
	CHECK(s.f.sizes().small == 5 && s.f.sizes().main == 0);
	CHECK(!s.f.enqueue(4, fifo_scene::hash(4)) && s.f.sizes().small == 5);  // queued already: stays where it is
	ids picked;
	std::uint32_t idx;
	std::uint64_t gen, round;
	unsigned q;
	for (std::uint64_t n = 5; s.f.pick(idx, gen, q, round); --n) {
		CHECK(q == 1 && round == n && s.rows[idx].q == 0);
		picked.push_back(idx);
	}
	CHECK((picked == ids{3, 1, 4, 0, 5}));
	CHECK(s.f.sizes().small == 0);
}

// A key whose hash is in the ghost enters the main FIFO, and the ghost gives the hash up.
void ghost_to_main() {
	fifo_scene s(8, 100);  // ghost of 50
	CHECK(s.f.ghost_capacity() == 50 && s.f.small_cap() == 10);
	s.f.ghost(fifo_scene::hash(7));
	CHECK(s.f.sizes().ghost == 1);
	CHECK(s.f.enqueue(7, fifo_scene::hash(7)));
	CHECK(s.f.queue_of(7) == 2 && s.f.sizes().ghost == 0);
	CHECK(!s.f.enqueue(6, fifo_scene::hash(6)) && s.f.queue_of(6) == 1);
	s.f.unlink(7);
	CHECK(!s.f.enqueue(7, fifo_scene::hash(7)) && s.f.queue_of(7) == 1);  // taken from the ghost once
}

// The small FIFO's head is taken while the small FIFO is over its cap or the main one is empty, else the main's.
void pick_queue() {
	fifo_scene s(8, 20);  // small cap 2
	for (std::uint32_t i : {0, 1, 2}) { s.f.enqueue(i, fifo_scene::hash(i)); }
	s.f.ghost(fifo_scene::hash(5));
	s.f.enqueue(5, fifo_scene::hash(5));
	ids picked;
	std::uint32_t idx;
	std::uint64_t gen, round;
	unsigned q;
	while (s.f.pick(idx, gen, q, round)) { picked.push_back(idx); }
	CHECK((picked == ids{0, 5, 1, 2}));  // 3 > 2: small; 2 = 2 and main has one: main; main empty: small
}

// evict: a used candidate goes to the main FIFO's tail, whichever FIFO it came from; an unused one closes.
void evict_order() {
	fifo_scene s(8, 20);  // small cap 2
	for (std::uint32_t i = 0; i < 6; ++i) { s.f.enqueue(i, fifo_scene::hash(i)); }
	std::vector<bool> used{false, true, false, true, false, false};
	ids closed, came;
	result_t r;
	std::uint64_t posts = 0;
	auto decide = [&](std::uint32_t idx, unsigned q, result_t&, std::uint64_t& p) {
		came.push_back(q * 10 + idx);
		if (used[idx]) {
			used[idx] = false;
			return visit::used;
		}
		closed.push_back(idx), ++p;
		return visit::closed;
	};
	s.f.evict(limits_t::lifted(), r, posts, [&] { return closed.size() < 3; }, decide);
	// Small {0..5}, over its cap of 2: 0 closes, 1 goes to main, 2 closes, 3 goes to main; small {4, 5} is at
	// its cap, so main's head 1, its use spent, closes.
	CHECK((came == ids{10, 11, 12, 13, 21}));
	CHECK((closed == ids{0, 2, 1}));
	CHECK((s.order(1) == ids{4, 5}) && (s.order(2) == ids{3}));
	CHECK(r.visited == 5 && posts == 3);
}

// evict: a kept candidate goes back to its own FIFO's tail; a full round of kept ones ends the pass.
void kept_round() {
	fifo_scene s(8, 100);
	for (std::uint32_t i = 0; i < 4; ++i) { s.f.enqueue(i, fifo_scene::hash(i)); }
	result_t r;
	std::uint64_t posts = 0;
	ids came;
	s.f.evict({50, ~std::uint64_t(0)}, r, posts, [] { return true; }, [&](std::uint32_t idx, unsigned, result_t&, std::uint64_t&) {
		came.push_back(idx);
		return visit::kept;
	});
	CHECK((came == ids{0, 1, 2, 3, 0}));  // the fifth exceeds the round of four
	CHECK(r.visited == 5 && posts == 0);
	CHECK((s.order(1) == ids{1, 2, 3, 0}));
}

// evict: at most `transitions` posts and `visits` candidates, and none while nothing is over.
void limits() {
	for (int which = 0; which < 3; ++which) {
		fifo_scene s(16, 1000);
		for (std::uint32_t i = 0; i < 10; ++i) { s.f.enqueue(i, fifo_scene::hash(i)); }
		limits_t lim = limits_t::lifted();
		(which == 0 ? lim.transitions : lim.visits) = 3;
		result_t r;
		std::uint64_t posts = 0;
		ids closed;
		s.f.evict(lim, r, posts, [&] { return which != 2; }, [&](std::uint32_t idx, unsigned, result_t&, std::uint64_t& p) {
			closed.push_back(idx), ++p;
			return visit::closed;
		});
		const ids want = which == 2 ? ids{} : ids{0, 1, 2};
		CHECK(closed == want);
		CHECK(r.visited == want.size() && posts == want.size());
		CHECK(s.f.sizes().small == 10 - want.size());
	}
}

// requeue refuses a slot already queued, one whose generation moved on, and one closing (or free).
void requeue_refused() {
	fifo_scene s(8, 100);
	s.f.enqueue(0, fifo_scene::hash(0));
	s.f.enqueue(1, fifo_scene::hash(1));
	std::uint32_t idx;
	std::uint64_t gen, round;
	unsigned q;
	s.rows[0].gen = 5;
	CHECK(s.f.pick(idx, gen, q, round) && idx == 0 && gen == 5);
	s.f.requeue(1, 0, 2);  // queued already
	CHECK((s.order(1) == ids{1}) && s.f.sizes().main == 0);
	s.rows[0].gen = 6;  // closed and reused since the pick
	s.f.requeue(0, gen, 1);
	CHECK(s.f.queue_of(0) == 0 && s.f.sizes().small == 1);
	s.rows[0].gen = 5, s.rows[0].closing = true;
	s.f.requeue(0, gen, 1);
	CHECK(s.f.queue_of(0) == 0 && s.f.sizes().small == 1);
	s.rows[0].closing = false;
	s.f.requeue(0, gen, 2);
	CHECK(s.f.queue_of(0) == 2 && (s.order(2) == ids{0}));
}

// unlink takes a slot out of the head, the middle or the tail and keeps both directions linked.
void unlink_any() {
	fifo_scene s(8, 100);
	for (std::uint32_t i = 0; i < 5; ++i) { s.f.enqueue(i, fifo_scene::hash(i)); }
	s.f.unlink(2);
	CHECK((s.order(1) == ids{0, 1, 3, 4}));
	s.f.unlink(4);
	s.f.unlink(0);
	CHECK((s.order(1) == ids{1, 3}));
	CHECK(s.rows[4].q == 0 && s.rows[4].prev == nope && s.rows[4].next == nope);
	s.f.enqueue(6, fifo_scene::hash(6));
	s.f.unlink(2);  // in no queue: nothing
	CHECK((s.order(1) == ids{1, 3, 6}) && s.f.sizes().small == 3);
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

// The ghost holds half of `soft_open` hashes (D6, limits.md): the capacity, and the oldest hash gone past it.
void ghost_half() {
	for (std::uint64_t so : {0, 1, 2, 3, 10, 11, 110000}) {
		W w(4, 1, false, false);
		w.remake(so, 0);
		CHECK(w.cat->ghost_capacity() == so / 2);
	}
	W w(20, 1, false, false);
	w.remake(10, 0);  // a small FIFO of 1, a ghost of 5
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	for (int j = 10; j <= 16; ++j) {  // each new key closes the small FIFO's head: k0 to k6 enter the ghost, k0 and k1 leave it
		touch(w, j);
		w.cat->policy_pass();
		w.exec.run();
	}
	CHECK(w.cat->queue_sizes().ghost == 5 && w.cat->stats().ghost_adds == 7);
	touch(w, 0), touch(w, 1);
	CHECK(w.cat->queue_of(slot_of(w, 0)) == 1 && w.cat->queue_of(slot_of(w, 1)) == 1 && w.cat->stats().ghost_hits == 0);
	touch(w, 2), touch(w, 6);
	CHECK(w.cat->queue_of(slot_of(w, 2)) == 2 && w.cat->queue_of(slot_of(w, 6)) == 2 && w.cat->stats().ghost_hits == 2);
	queues_agree(w);
}

// The ghost's memory: a 24 B node and an 8 B table slot, both arrays a power of two. At half of a 110,000-slot
// `soft_open` (55,000 hashes) the real bytes are printed and checked.
void ghost_bytes() {
	CHECK(ghost_set::node_bytes() == 24);
	std::mt19937_64 rng(11);
	for (std::size_t n : {std::size_t(1), std::size_t(100), std::size_t(1000), std::size_t(55000)}) {
		ghost_set g(n);
		CHECK(g.bytes() == 0);  // allocates only as it fills
		for (std::size_t i = 0; i < n; ++i) { g.add(rng()); }
		CHECK(g.size() == n);
		std::size_t p2 = 1;
		while (p2 < n) { p2 *= 2; }
		std::size_t slots = 16;
		while (8 * n > 7 * slots) { slots *= 2; }
		const std::uint64_t want = p2 * 24 + slots * 8;
		std::printf("ghost %zu hashes: %llu B (%.1f B a hash)%s\n", n, (unsigned long long)g.bytes(), double(g.bytes()) / double(n), n == 55000 ? " = 55,000 of 110,000 open, a catalog's ghost" : "");
		CHECK(g.bytes() == want);
		CHECK(g.bytes() >= n * 33 && g.bytes() <= n * 67 + 1024);  // the header's 33 to 66 B per live entry
	}
	ghost_set full(55000);
	for (int i = 0; i < 55000; ++i) { full.add(rng()); }
	CHECK(full.bytes() == std::uint64_t(65536) * 32);  // 2 MiB: 19 B per open index of 110,000
}

// A new slot enters the small FIFO; an evicted key's hash enters the ghost and its next open goes to the main FIFO.
void ghost_and_entry() {
	W w(12, 1, false, false);
	w.remake(10, 0);  // a small FIFO of 1, a ghost of 5
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
	W w(20, 1, false, false);
	w.remake(18, 0);  // a small FIFO of 1, a ghost of 9
	for (int i = 0; i < 18; ++i) { touch(w, i); }
	touch(w, 0), touch(w, 0);  // k0: frequency 2 (a use each)
	touch(w, 18);
	auto r = w.cat->policy_pass();
	w.exec.run();
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(r.promoted == 1 && r.closes == 1 && s0 != nope && r.decayed == 0);
	CHECK(w.cat->queue_of(s0) == 2 && freq(w, s0) == 0);  // moved, the frequency cleared
	CHECK(slot_of(w, 1) == nope);                         // then the next head, k1, closed
	CHECK(w.cat->queue_sizes().small == 17 && w.cat->queue_sizes().main == 1);
	// Each evicted key that comes back is a ghost hit: it enters the main FIFO, and the small FIFO, which a miss no
	// longer refills, shrinks to its share.
	for (int j = 1; j <= 16; ++j) {
		touch(w, j);
		CHECK(w.cat->queue_of(slot_of(w, j)) == 2);
		w.cat->policy_pass();
		w.exec.run();
	}
	CHECK(w.cat->queue_sizes().small == 1 && w.cat->queue_sizes().main == 17 && w.cat->stats().ghost_adds == 17);
	touch(w, 0), touch(w, 0);  // k0, in the main FIFO: frequency 2
	touch(w, 17);              // k17 was closed from the small FIFO: a ghost hit
	CHECK(freq(w, s0) == 2 && w.cat->queue_sizes().main == 18);
	r = w.cat->policy_pass();  // within its share the small FIFO is left alone: k0 is decremented and put back, k1 closes
	w.exec.run();
	CHECK(r.decayed == 1 && r.promoted == 0 && r.closes == 1);
	CHECK(st(w, s0) == S::open && freq(w, s0) == 1 && w.cat->queue_of(s0) == 2 && slot_of(w, 1) == nope && slot_of(w, 2) != nope);
	CHECK(w.cat->stats().ghost_adds == 17);  // closed from the main FIFO: no ghost
	touch(w, 1);
	CHECK(w.cat->queue_of(slot_of(w, 1)) == 1);  // so its next open enters the small FIFO
	queues_agree(w);
}

// A hot candidate (it holds a map) is demoted first, and only as many as the cap asks to close: the demotion books
// the close it leads to (B2), which begins when the demotion ends (B5), and carries the ghost mark of the small FIFO.
void hot_first() {
	W w(12, 1, true, true);  // defer_io, eager: hot until demoted
	w.remake(10, 0);
	for (int i = 0; i <= 10; ++i) { touch(w, i); }
	auto r = w.cat->policy_pass();
	CHECK(r.closes == 0 && r.demotes == 1 && w.cat->pending_open() == 1);
	w.exec.run();
	CHECK(slot_of(w, 0) == nope && w.cat->stats().ghost_adds == 1);  // the head, closed from the small FIFO
	for (int i = 1; i <= 10; ++i) { CHECK(st(w, slot_of(w, i)) == S::open && w.cat->count_of(slot_of(w, i), strata::resource::maps) != 0); }
	CHECK(w.cat->stats().evict_demotes == 1 && w.cat->pending_open() == 0 && w.cat->open_slots() == 10);
	r = w.cat->policy_pass();  // within the cap: nothing more
	CHECK(r.closes == 0 && r.demotes == 0);
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

// A candidate closed, freed and reopened for another key behind the pass's back is not put back (its generation moved on):
// the open's own enqueue decides its queue.
void reopened_behind_the_pass() {
	bool armed = false;
	W* wp = nullptr;
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		if (armed && std::string_view(site) == "sweep.close") {
			armed = false;
			const std::uint32_t s = slot_of(*wp, 0);
			CHECK(wp->cat->begin_close(s));  // another closer takes the candidate
			wp->exec.run();                  // and finishes: the slot is free
			auto p = wp->cat->try_pin(kn(11), pin_kind::request, true);  // a miss reuses it: opening, not yet run
		}
	});
	wp = &w;
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	armed = true;
	w.cat->policy_pass();
	CHECK(!armed && st(w, s0) == S::opening && w.cat->queue_of(s0) == 0);
	w.exec.run();
	CHECK(st(w, s0) == S::open && w.cat->queue_of(s0) != 0);
	queues_agree(w);
}

// CR1: a pass holding a slot it picked requeues it while another closer's close ends (after its unlink, before the word's
// compare-exchange): the slot is freed and must not be left in a queue. Two threads, two hooks: the pass P stops in
// `clear_freq` ("policy.clear", after its pick), the close chore C stops at "close.end" (after `policy_.unlink`).
struct latch {
	std::mutex m;
	std::condition_variable cv;
	bool v = false;
	void set() {
		{
			std::lock_guard l(m);
			v = true;
		}
		cv.notify_all();
	}
	void wait() {
		std::unique_lock l(m);
		cv.wait(l, [&] { return v; });
	}
};
void requeue_races_close() {
	latch p_at, p_go, c_at, c_go;
	std::atomic<int> armed{0};
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		const std::string_view site_name(site);
		int e = 1;
		if (site_name == "policy.clear" && armed.compare_exchange_strong(e, 2)) {
			p_at.set();
			p_go.wait();
		}
		e = 2;
		if (site_name == "close.end" && armed.compare_exchange_strong(e, 3)) {
			c_at.set();
			c_go.wait();
		}
	});
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 0);   // a use: k0's frequency is 1, so the pass moves it and requeues
	touch(w, 10), touch(w, 11);  // 12 queued; with k0's close begun, one over a soft_open of 10 still
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(w.cat->begin_close(s0));  // a direct closer; k0 stays queued
	armed = 1;
	std::thread pass([&] { w.cat->policy_pass(); });
	p_at.wait();  // the pass has picked k0 (out of its queue)
	std::thread chore([&] { w.exec.run(); });
	c_at.wait();  // the chore has unlinked k0 (a no-op) and waits before the word's compare-exchange
	p_go.set();
	pass.join();  // the pass's requeue lands in the gap
	c_go.set();
	chore.join();
	w.exec.run();
	CHECK(armed == 3);
	CHECK(st(w, s0) == S::free && w.cat->queue_of(s0) == 0);
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

// A hot candidate taken from the main FIFO is demoted for a booked close that is not ghosted (only the small FIFO's are).
void booked_close_from_main_not_ghosted() {
	W w(12, 1, false, true);  // eager: hot
	w.remake(10, 0);
	for (int i = 0; i <= 10; ++i) { touch(w, i); }
	for (int i = 0; i < 10; ++i) {  // each pass closes the oldest small entry, ghosted; its key comes back into the main FIFO
		CHECK(w.cat->policy_pass(cat_t::policy_limits::lifted()).demotes == 1);
		w.exec.run();
		touch(w, i);
	}
	CHECK(w.cat->queue_sizes().small == 1 && w.cat->queue_sizes().main == 10 && w.cat->open_slots() == 11);
	const auto adds = w.cat->stats().ghost_adds;
	CHECK(w.cat->policy_pass(cat_t::policy_limits::lifted()).demotes == 1);  // taken from the main FIFO
	w.exec.run();
	CHECK(w.cat->open_slots() == 10 && w.cat->stats().ghost_adds == adds);
	queues_agree(w);
}

// A close() that throws ends the close at the prior state like any refusal: the slot is back in the policy and the
// ghost mark is gone, so a later close that no pass chose does not enter the ghost.
void refused_close_throws() {
	bool armed = true;
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		if (armed && std::string_view(site) == "close.call") {
			armed = false;
			throw std::logic_error("close refused");
		}
	});
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	const auto r = w.cat->policy_pass();  // k0 is the head: its close is begun, then throws
	CHECK(r.closes == 1);
	w.exec.run();
	CHECK(!armed && st(w, s0) == S::open && w.cat->queue_of(s0) == 2 && w.cat->stats().ghost_adds == 0);
	queues_agree(w);
	CHECK(w.cat->begin_close(s0));  // a direct close, no pass
	w.exec.run();
	CHECK(st(w, s0) == S::free && w.cat->stats().ghost_adds == 0);
	queues_agree(w);
}

// A refused close whose slot another closer closes, frees and reopens for another key before the refused close requeues it
// does not requeue the reopened slot (it would be freed with its queue links if that open failed): the open's enqueue decides.
void refused_close_reopened_behind() {
	bool throw_close = true, behind = true;
	W* wp = nullptr;
	W w(12, 1, false, false);
	w.remake(10, 0, [&](const char* site, bool) {
		const std::string_view name(site);
		if (throw_close && name == "close.call") {
			throw_close = false;
			throw std::logic_error("close refused");
		}
		if (behind && name == "refuse.end") {
			behind = false;
			const std::uint32_t s = slot_of(*wp, 0);
			CHECK(wp->cat->begin_close(s));  // another closer takes the slot at its prior state
			wp->exec.run();                  // and finishes: the slot is free
			auto p = wp->cat->try_pin(kn(11), pin_kind::request, true);  // a miss reuses it: opening, not yet run
		}
	});
	wp = &w;
	for (int i = 0; i < 10; ++i) { touch(w, i); }
	touch(w, 10);
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(w.cat->policy_pass().closes == 1);  // k0 is the head: its close is begun, then throws
	w.exec.run();
	CHECK(!throw_close && !behind && st(w, s0) == S::open);
	CHECK(w.cat->queue_of(s0) == 1);  // k11's own enqueue (a new key: the small FIFO), not the refused close's requeue (the main FIFO)
	queues_agree(w);
}

// A booked close that cannot begin (a pin took the slot between the booking and its end) leaves no ghost mark behind.
void booked_close_refused_no_mark() {
	bool armed = false;
	W w(12, 1, false, true);
	w.remake(10, 0, [&](const char* site, bool) {
		if (armed && std::string_view(site) == "sweep.close") { w.cat->set_pins_for_test(slot_of(w, 0), 1); }
	});
	for (int i = 0; i <= 10; ++i) { touch(w, i); }
	const std::uint32_t s0 = slot_of(w, 0);
	CHECK(w.cat->policy_pass().demotes == 1);  // k0, from the small FIFO: its booked close would be ghosted
	armed = true;
	w.exec.run();  // the demotion ends; the close it booked meets the pin
	armed = false;
	CHECK(st(w, s0) == S::open && w.cat->due_of(s0));
	w.cat->set_pins_for_test(s0, 0);
	{
		auto p = w.cat->try_pin(kn(0), pin_kind::chore, false);  // a pin by key drops the booking
		CHECK(p.status == cat_t::try_status::pinned && !w.cat->due_of(s0));
	}
	CHECK(w.cat->begin_close(s0));  // an unbooked close that no pass chose
	w.exec.run();
	CHECK(st(w, s0) == S::free && w.cat->stats().ghost_adds == 0);
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

// Hot slots over soft_hot demote, the oldest last-use first among the slots the pass's window visits; the demotions in
// flight count, so a second pass before the chores run demotes none; a window of 3 slots demotes within those.
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
	CHECK(r.demotes == 4);
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
	lim.visits = 3;
	r = v.cat->policy_pass(lim);
	CHECK(r.demotes == 3);
	v.exec.run();
	for (int i = 0; i < 12; ++i) { CHECK(st(v, slot_of(v, i)) == (i < 3 ? S::owed : S::open)); }
	r = v.cat->policy_pass(lim);  // the window resumes at slot 3: k3 to k5, the oldest k3
	CHECK(r.demotes == 1);
	v.exec.run();
	CHECK(st(v, slot_of(v, 3)) == S::owed && st(v, slot_of(v, 4)) == S::open);
}

// The pass's limits bound its work.
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

// A close's end lowers the closing count and then the open count; a pass that runs between the two sees one close
// more in flight than there is, never one fewer. Three open slots over a cap of one, one close begun: when it ends,
// two slots are over the cap and the pass between the two lowerings must still close one (with the two lowered in
// the other order the pass sees the open count after the close and the closing count before it, and closes none).
void pass_between_a_closes_counts() {
	W w(4, 1, false, false);
	std::uint64_t closes = ~std::uint64_t(0);
	bool ran = false;
	w.remake(1, 0, [&](const char* site, bool) {
		if (ran || std::strcmp(site, "note.mid") != 0) { return; }
		ran = true;
		closes = w.cat->policy_pass(cat_t::policy_limits::lifted()).closes;
	});
	for (int i = 0; i < 3; ++i) { touch(w, i); }
	CHECK(w.cat->open_slots() == 3);
	CHECK(w.cat->begin_close(slot_of(w, 0)));
	w.exec.run();
	CHECK(ran && closes >= 1);
	w.exec.run();
	queues_agree(w);
}

}  // namespace

int main(int argc, char** argv) {
	// A case name runs that case of the policy alone (each one a mutant's named test); a number, the requests.
	const std::pair<const char*, void (*)()> unit[] = {{"fifo_order", fifo_order}, {"ghost_to_main", ghost_to_main}, {"pick_queue", pick_queue},
		{"evict_order", evict_order}, {"kept_round", kept_round}, {"limits", limits}, {"requeue_refused", requeue_refused}, {"unlink_any", unlink_any}};
	if (argc > 1 && !std::isdigit(static_cast<unsigned char>(argv[1][0]))) {
		bool known = false;
		for (const auto& [name, fn] : unit) {
			if (std::strcmp(argv[1], name) == 0) { known = true, fn(); }
		}
		if (!known) {
			std::fprintf(stderr, "catalog_policy: no case %s\n", argv[1]);
			return 2;
		}
		std::printf("catalog_policy %s: %s\n", argv[1], failures ? "FAILED" : "ok");
		return failures ? 1 : 0;
	}
	const std::size_t requests = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 60000;
	for (const auto& [name, fn] : unit) { fn(); }
	ghost_oracle();
	ghost_half();
	ghost_bytes();
	ghost_and_entry();
	promote_and_decay();
	hot_first();
	pinned_head();
	used_since();
	closed_behind_the_pass();
	reopened_behind_the_pass();
	requeue_races_close();
	refused_close_reopened_behind();
	refused_close();
	booked_close_from_main_not_ghosted();
	refused_close_throws();
	booked_close_refused_no_mark();
	direct_close();
	demote_hot();
	policy_limits_hold();
	use_rows();
	pass_between_a_closes_counts();
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
