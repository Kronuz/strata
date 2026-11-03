// The cold tier's step 2, item 5b (docs/reviews/cold-tier-xpool-host.md F1 to F6; S10 to S16, C17): the
// catalog's host interface for X-POOL. Pin intents (create, resident), the payload built in place with its
// open step, `reopen()` with the holder drain, a fenced index replaced by every chore, the check-in before a
// demotion or a close, `snapshot()`, and the host's own files counted on the slot. Everything runs on
// inline_executor: single-threaded and deterministic, with the hooks of `before_cas` placing another actor's
// step between a load and the exchange it feeds. The mutants each case must kill are in
// docs/status/cold-tier-step2.md (item 5b).
#include "catalog_world.hh"

#include <cstdio>

namespace {

constexpr std::uint64_t ms = 1'000'000;

using strata::open_intent;
using strata::pin_kind;
using strata::slot_state;
using S = slot_state;

// H that cannot move: a deleted move, an atomic member, and a destructor that tells the test what is still alive.
struct pinned {
	int serial = 0;
	std::atomic<int> turns{0};
	explicit pinned(int s) : serial(s) {}
	pinned(const pinned&) = delete;
	pinned& operator=(const pinned&) = delete;
	~pinned() {
		if (on_destroy) { on_destroy(); }
	}
	static inline std::function<void()> on_destroy;
};

using W = world<inline_executor>;
using PW = world<inline_executor, pinned>;
using pcat_t = PW::cat_t;

std::string kn(int i) { return "k" + std::to_string(i); }

template <class WW, class F>
void run_task(WW& w, F&& body) {
	task::run(w.exec.scheduler(), body());
}
template <class WW>
std::uint64_t pins(WW& w, std::uint32_t i) { return WW::cat_t::pins_of(w.cat->word_of(i)); }
template <class WW>
S st(WW& w, std::uint32_t i) { return WW::cat_t::state_of(w.cat->word_of(i)); }
// The slot a key is in (slots are numbered by allocation, and reused).
template <class WW>
std::uint32_t sl(WW& w, const std::string& key) {
	for (std::uint32_t i = 0; i < w.cat->slots_made(); ++i) {
		if (w.cat->key_at(i) == key) { return i; }
	}
	return ~std::uint32_t(0);
}
template <class WW>
std::uint64_t gen(WW& w, std::uint32_t i) { return WW::cat_t::gen_of(w.cat->word_of(i)); }

// A coroutine started on the test thread at once, so that it registers before the chores queued ahead of
// it run. Held by the test until it finishes.
struct actor {
	std::function<task::task<void>()> fn;
	task::task<void> t;
};
template <class WW>
void start(WW& w, actor& a) {
	const task::running_on on(task::scheduler_ref::of(w.exec.scheduler()));
	a.t.handle().resume();
}

// What a pin by key ends in.
struct got {
	bool done = false, pinned = false, empty = false;
	std::string error;
	std::uint64_t serial = 0, gen = 0;
	bool has_doc = false;
};

template <class WW>
actor ask(WW& w, got& g, std::string key, open_intent intent, pin_kind kind = pin_kind::request, int doc = -1) {
	actor a;
	a.fn = [&w, &g, key, intent, kind, doc]() -> task::task<void> {
		try {
			auto p = kind == pin_kind::request ? co_await w.cat->pin_request(key, true, intent) : co_await w.cat->pin_chore(key, false, intent);
			g.done = true;
			if (!p) {
				g.empty = true;
				co_return;
			}
			g.pinned = true;
			g.serial = std::uint64_t(p.payload().serial);
			g.gen = p.where().gen;
			if (doc >= 0) { g.has_doc = p.index().get(key_of(doc)).has_value(); }
		} catch (const std::exception& e) {
			g.done = true;
			g.error = typeid(e).name();
		}
	};
	a.t = a.fn();
	return a;
}
// Runs an actor to its end on the inline executor.
template <class WW>
void go(WW& w, actor& a) {
	start(w, a);
	w.settle();
}
template <class WW>
void go(WW& w, actor&& a) { go(w, a); }

bool is_absent(const got& g) { return g.error.find("index_not_created") != std::string::npos; }

void absent_dir(W& w, const char* alias) {
	w.dirs.emplace_back();  // an empty directory: no index
	w.alias[alias] = int(w.dirs.size()) - 1;
}

// Fences the index behind `p`: a document applied, a sync that fails under commit.
template <class WW, class P>
bool fence(WW& w, int dir, P& p, int round = 0) {
	w.dirs[dir].fs.add_fault({faultfs::op::sync, 1, EIO});
	bool fenced = false;
	try {
		(void)p.index().apply(tx({set_meta("fence" + std::to_string(round), "x")}), {});  // an owed index answers map_owed
		p.index().commit();
	} catch (const index_fenced&) {
		fenced = true;
	}
	w.dirs[dir].fs.clear_faults();
	return fenced;
}

// --- S10 create and resident (F1) ------------------------------------------------------------------------------

// (a) Ten read pins on an absent key fail index_not_created, nine of them without an open; a create pin
// inside the backoff creates; the reads after it see the index.
void create_passes_absent() {
	W w(1, 1, false);
	absent_dir(w, "new");
	for (int i = 0; i < 10; ++i) {
		got g;
		go(w, ask(w, g, "new", open_intent::open));
		CHECK(g.done && is_absent(g));
	}
	CHECK(w.resolves == 1 && w.cat->stats().failed_fast == 9 && st(w, 0) == S::failed);
	got c;
	go(w, ask(w, c, "new", open_intent::create));
	CHECK(c.pinned && c.error.empty() && w.resolves == 2 && st(w, 0) == S::open);
	got r;
	go(w, ask(w, r, "new", open_intent::open));
	CHECK(r.pinned && w.resolves == 2 && r.serial == c.serial);
	const auto s = w.cat->stats();
	CHECK(s.open_failures == 1 && s.retries == 1);
	// the created index is a real one: it survives a close and a reopen through the catalog
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request("new");
		CHECK(p.index().apply(tx({add(key_of(7), "seven")}), {}).result == verdict::accepted);
		p.index().commit();
	});
	w.drain();
	CHECK(st(w, 0) == S::free);
	got again;
	go(w, ask(w, again, "new", open_intent::open, pin_kind::request, 7));
	CHECK(again.pinned && again.has_doc);
}

// (b) A create pin joins an `opening` slot whose attempt runs without create: every waiter, read and create,
// gets the created index and none fails.
void create_joins_open() {
	W w(1, 1, false);
	absent_dir(w, "new");
	got read, create, late;
	actor a_read = ask(w, read, "new", open_intent::open), a_create = ask(w, create, "new", open_intent::create);
	bool joined = false;
	w.tweak = [&](cat_t::host& h) {
		auto inner = h.resolve;
		h.resolve = [&, inner](const cat_t::opening& op) {
			if (!joined) {  // the first attempt is under way without create: a create pin joins it now
				joined = true;
				CHECK(!op.create);
				start(w, a_create);
			}
			return inner(op);
		};
	};
	w.make_catalog({});
	start(w, a_read);
	w.settle();
	CHECK(read.done && read.pinned && read.error.empty());
	CHECK(create.done && create.pinned && create.error.empty());
	const auto s = w.cat->stats();
	CHECK(w.resolves == 2 && s.create_reposts == 1 && s.open_failures == 0 && s.opens == 1 && s.misses == 1);
	CHECK(st(w, 0) == S::open && pins(w, 0) == 0 && w.made == 1);
	CHECK(read.serial == create.serial && read.gen == create.gen);
}

// (c) A create pin on an EIO negative entry fails fast, like every pin.
void create_obeys_other_errors() {
	W w(1, 1, false);
	w.fail_resolve = EIO;
	w.fail_left = 1;
	got a, b;
	go(w, ask(w, a, kn(0), open_intent::open));
	CHECK(a.done && !a.error.empty() && st(w, 0) == S::failed);
	go(w, ask(w, b, kn(0), open_intent::create));
	CHECK(b.done && !b.error.empty() && b.error == a.error);
	CHECK(w.resolves == 1 && w.cat->stats().failed_fast == 1 && st(w, 0) == S::failed);
	// past the retry time the create pin retries as every pin does
	w.exec.scheduler().advance(20'000 * ms);
	got c;
	go(w, ask(w, c, kn(0), open_intent::create));
	CHECK(c.pinned && w.resolves == 2);
}

// (d) A resident pin never opens.
void resident_never_opens() {
	W w(3, 1, true);
	// no slot: an empty pin, nothing made
	{
		got g;
		go(w, ask(w, g, kn(0), open_intent::resident, pin_kind::chore));
		CHECK(g.done && g.empty && g.error.empty());
		CHECK(w.cat->table_size() == 0 && w.resolves == 0 && w.cat->slots_made() == 0);
		const auto r = w.cat->try_pin(kn(0), pin_kind::chore, false, open_intent::resident);
		CHECK(r.status == cat_t::try_status::absent && !r.p && w.cat->table_size() == 0);
	}
	// a request pin cannot be resident
	bool threw = false;
	try {
		(void)w.cat->pin_request(kn(0), true, open_intent::resident);
	} catch (const std::logic_error&) {
		threw = true;
	}
	CHECK(threw);
	// open: granted
	got o;
	go(w, ask(w, o, kn(0), open_intent::open));
	CHECK(o.pinned && st(w, 0) == S::open);
	got g;
	go(w, ask(w, g, kn(0), open_intent::resident, pin_kind::chore));
	CHECK(g.pinned && w.resolves == 1 && pins(w, 0) == 0);
	// owed: granted as a chore pin is, no wake
	CHECK(w.cat->begin_demote(0));
	w.settle();
	CHECK(st(w, 0) == S::owed);
	got ow;
	go(w, ask(w, ow, kn(0), open_intent::resident, pin_kind::chore));
	CHECK(ow.pinned && w.cat->stats().wakes_posted == 0 && st(w, 0) == S::owed);
	// closing: an empty pin, the slot unchanged
	CHECK(w.cat->begin_close(0) && st(w, 0) == S::closing);
	got cl;
	go(w, ask(w, cl, kn(0), open_intent::resident, pin_kind::chore));
	CHECK(cl.empty && !cl.pinned);
	{
		const auto r = w.cat->try_pin(kn(0), pin_kind::chore, false, open_intent::resident);
		CHECK(r.status == cat_t::try_status::absent);
	}
	w.settle();
	// failed (a negative entry): an empty pin, no retry
	w.fail_resolve = EIO;
	w.fail_left = 1;
	got f;
	go(w, ask(w, f, kn(1), open_intent::open));
	const std::uint32_t k1 = sl(w, kn(1));
	CHECK(!f.error.empty() && st(w, k1) == S::failed);
	const int resolves = w.resolves;
	w.exec.scheduler().advance(20'000 * ms);  // past the retry time: a resident pin still does not retry
	got rf;
	go(w, ask(w, rf, kn(1), open_intent::resident, pin_kind::chore));
	CHECK(rf.empty && w.resolves == resolves && st(w, k1) == S::failed);
	// opening: waits
	got op;
	actor a = ask(w, op, kn(2), open_intent::open);
	start(w, a);
	CHECK(st(w, sl(w, kn(2))) == S::opening);  // the open is posted, not run
	const auto t = w.cat->try_pin(kn(2), pin_kind::chore, false, open_intent::resident);
	CHECK(t.status == cat_t::try_status::would_wait);
	w.settle();
	CHECK(op.done);
}

// --- S11 the payload in place and the open step (F3) ---------------------------------------------------------------

void payload_in_place() {
	PW w(2, 2, false);
	S step_state = S::free;
	std::uint64_t step_pins = 99;
	pinned::on_destroy = nullptr;
	w.tweak = [&](pcat_t::host& h) {
		h.open_payload = [&](const pcat_t::opening& op, kv_index& ix, std::optional<pinned>& p) {
			p.emplace(++w.made);
			step_state = pcat_t::state_of(w.cat->word_of(0));
			step_pins = pcat_t::pins_of(w.cat->word_of(0));
			if (op.key == "k1") { throw std::runtime_error("the step failed"); }
			CHECK(ix.apply(tx({add(key_of(500), "step")}), {}).result == verdict::accepted);
		};
	};
	w.make_catalog({});
	// the step applies a document; the first waiter's read sees it, and no pin was handed out before the step ended
	got first, second;
	actor a = ask(w, first, kn(0), open_intent::open, pin_kind::request, 500), b = ask(w, second, kn(0), open_intent::open, pin_kind::request, 500);
	start(w, a);
	start(w, b);
	w.settle();
	CHECK(first.pinned && second.pinned && first.has_doc && second.has_doc && first.serial == second.serial);
	CHECK(step_state == S::opening && step_pins == 2);  // the waiters were counted, none handed a pin
	CHECK(w.made == 1);
	// a step that throws: the open and its waiters fail, H goes first, then the reader, the index, the IO
	std::int64_t maps_at_h = -1, desc_at_h = -1;
	pinned::on_destroy = [&] {
		maps_at_h = std::int64_t(w.cat->count_of(1, strata::resource::maps));
		desc_at_h = std::int64_t(w.cat->count_of(1, strata::resource::descriptors));
	};
	got x, y;
	actor ax = ask(w, x, kn(1), open_intent::open), ay = ask(w, y, kn(1), open_intent::open);
	start(w, ax);
	start(w, ay);
	w.settle();
	CHECK(x.done && y.done && !x.error.empty() && x.error == y.error);
	CHECK(st(w, 1) == S::failed && pins(w, 1) == 0);
	if (!(maps_at_h > 0 && desc_at_h >= 1)) { std::fprintf(stderr, "maps_at_h %lld desc_at_h %lld\n", (long long)maps_at_h, (long long)desc_at_h); }
	CHECK(maps_at_h > 0 && desc_at_h >= 1);  // the index and the IO were still alive when H went
	CHECK(w.cat->count_of(1, strata::resource::maps) == 0 && w.cat->count_of(1, strata::resource::descriptors) == 0);
	pinned::on_destroy = nullptr;
	CHECK(w.cat->index_of(1) == nullptr);
	w.drain();
}

// --- S12 the drain and reopen (F4) -------------------------------------------------------------------------------------

// Three holders; one reopens after a fenced commit; pins by key and by handle that arrive wait in `closing`;
// the last holder's unpin posts the close; the waiters get a fresh, unfenced index.
void reopen_drains_holders() {
	W w(1, 2, false);
	std::vector<std::unique_ptr<cat_t::pin>> held;
	std::vector<cat_t::handle> handles;
	run_task(w, [&]() -> task::task<void> {
		for (int i = 0; i < 3; ++i) {
			held.push_back(std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0))));
			handles.push_back(held.back()->where());
		}
	});
	const std::uint64_t g0 = gen(w, 0);
	CHECK(pins(w, 0) == 3 && w.made == 1);
	CHECK(fence(w, 0, *held[0]) && held[0]->index().fenced());
	w.cat->reopen(std::move(*held[0]));
	held[0].reset();
	CHECK(st(w, 0) == S::closing && pins(w, 0) == 2 && w.cat->drain_of(0) == 2);
	// a pin by key and one by handle arrive: both wait
	got by_key, by_handle;
	actor a = ask(w, by_key, kn(0), open_intent::open), b;
	b.fn = [&]() -> task::task<void> {
		try {
			auto p = co_await w.cat->repin(handles[1], kn(0));
			by_handle.done = true, by_handle.pinned = true, by_handle.gen = p.where().gen, by_handle.serial = std::uint64_t(p.payload().serial);
		} catch (...) {
			by_handle.done = true;
		}
	};
	b.t = b.fn();
	start(w, a);
	start(w, b);
	CHECK(w.cat->waiters_of(0) == 2 && pins(w, 0) == 4 && !by_key.done && !by_handle.done);
	w.settle();
	CHECK(st(w, 0) == S::closing && w.made == 1 && w.cat->index_of(0) != nullptr);  // holders still out: nothing closed under them
	CHECK(held[1]->index().get(key_of(0)).has_value());                           // a holder's index is alive
	held[1].reset();
	w.settle();
	CHECK(st(w, 0) == S::closing && pins(w, 0) == 3 && w.cat->drain_of(0) == 1 && w.made == 1);
	CHECK(held[2]->index().get(key_of(0)).has_value());
	held[2].reset();
	w.settle();
	CHECK(by_key.pinned && by_handle.pinned && by_key.serial == 2 && by_handle.serial == 2);
	CHECK(st(w, 0) == S::open && gen(w, 0) == g0 + 1 && by_key.gen == g0 + 1 && w.made == 2);
	CHECK(pins(w, 0) == 0 && w.cat->waiters_of(0) == 0);  // the waiters' pins ended with their tasks
	CHECK(w.cat->stats().replaces == 1 && w.cat->stats().drain_unpins == 2);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(!p.index().fenced() && p.payload().serial == 2);
		CHECK(p.index().get(key_of(0)).has_value());
	});
}

// A holder's unpin lands between reopen's load and its exchange: the exchange fails, the count is read again.
void unpin_races_reopen_exchange() {
	std::vector<std::unique_ptr<cat_t::pin>> held;
	bool fired = false;
	W w(1, 1, false, true, [&](const char* site, bool) {
		if (!fired && std::strcmp(site, "reopen") == 0) {
			fired = true;
			held[1].reset();  // lock-free: no mutex taken
		}
	});
	run_task(w, [&]() -> task::task<void> {
		for (int i = 0; i < 3; ++i) { held.push_back(std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0)))); }
	});
	CHECK(fence(w, 0, *held[0]));
	w.cat->reopen(std::move(*held[0]));
	held[0].reset();
	CHECK(fired && st(w, 0) == S::closing);
	CHECK(pins(w, 0) == 1 && w.cat->drain_of(0) == 1);  // the unpin that raced is not a holder to wait for
	w.settle();
	CHECK(st(w, 0) == S::closing && w.made == 1);
	held[2].reset();
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->stats().replaces == 1);  // no waiter: the slot frees
}

// Two holders unpin at once in a drain: exactly one posts the close.
void drain_unpins_race() {
	std::vector<std::unique_ptr<cat_t::pin>> held;
	bool inner = false;
	W w(1, 1, false, true, [&](const char* site, bool) {
		if (std::strcmp(site, "drain.unpin") == 0 && !inner && held.size() > 2 && held[2]) {
			inner = true;
			held[2].reset();  // the other holder's unpin, between this one's and its decrement
		}
	});
	run_task(w, [&]() -> task::task<void> {
		for (int i = 0; i < 3; ++i) { held.push_back(std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0)))); }
	});
	CHECK(fence(w, 0, *held[0]));
	w.cat->reopen(std::move(*held[0]));
	held[0].reset();
	CHECK(w.cat->drain_of(0) == 2);
	held[1].reset();
	CHECK(inner && w.cat->drain_of(0) == 0 && pins(w, 0) == 0);
	w.settle();
	CHECK(w.cat->stats().replaces == 1 && st(w, 0) == S::free);
}

// A host may reopen a healthy index (a corrupt segment it healed another way): what it wrote is committed first.
void reopen_commits_unfenced() {
	W w(1, 1, false);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(p.index().apply(tx({add(key_of(77), "seven")}), {}).result == verdict::accepted);
		CHECK(p.index().uncommitted() && !p.index().fenced());
		w.cat->reopen(std::move(p));
	});
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->stats().replaces == 1 && w.cat->stats().replace_commit_failures == 0 && w.commits == 1);
	got g;
	go(w, ask(w, g, kn(0), open_intent::open, pin_kind::request, 77));
	CHECK(g.pinned && g.has_doc);
}

// A reopen on a pin in `waking` (a chore pin) or in a drain already begun is a plain unpin.
void reopen_plain_unpin() {
	W w(1, 1, false);
	std::vector<std::unique_ptr<cat_t::pin>> held;
	run_task(w, [&]() -> task::task<void> {
		for (int i = 0; i < 3; ++i) { held.push_back(std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0)))); }
	});
	CHECK(fence(w, 0, *held[0]));
	w.cat->reopen(std::move(*held[0]));
	CHECK(st(w, 0) == S::closing && w.cat->drain_of(0) == 2);
	w.cat->reopen(std::move(*held[1]));  // a second holder meets the same fence: one more unpin of the drain
	CHECK(st(w, 0) == S::closing && w.cat->drain_of(0) == 1 && pins(w, 0) == 1);
	held[2].reset();
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->stats().replaces == 1);
	cat_t::pin empty;
	w.cat->reopen(std::move(empty));  // an empty pin: nothing
}

// --- S13 a fence is met by every chore (F4) -------------------------------------------------------------------------

// An idle fenced slot is replaced on the next pass that picks it.
void fenced_idle_is_replaced() {
	W w(1, 2, false);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		CHECK(fence(w, 0, p));
	});
	CHECK(st(w, 0) == S::open && pins(w, 0) == 0);
	CHECK(w.kern.fds.load() > 0);
	w.exec.scheduler().advance(31'000 * ms);
	w.cat->post_pass();
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0);
	CHECK(w.kern.fds.load() == 0 && w.kern.maps.load() == 0);
	CHECK(w.cat->stats().replaces == 1 && w.cat->stats().replace_commit_failures == 0);  // a fenced index is not committed
	// a pin now opens a fresh, unfenced index
	got g;
	go(w, ask(w, g, kn(0), open_intent::open));
	CHECK(g.pinned && g.serial == 2);
}

// A fenced `owed` slot: its wake replaces it; the request waiters get the new index; the wake's failures count nothing.
void fenced_owed_wake_replaces() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	CHECK(w.cat->begin_demote(0));
	w.settle();
	CHECK(st(w, 0) == S::owed);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		CHECK(fence(w, 0, p));  // an add needs no body: accepted under the owed flag
	});
	CHECK(st(w, 0) == S::owed && pins(w, 0) == 0);
	got g;
	go(w, ask(w, g, kn(0), open_intent::open));
	CHECK(g.pinned && g.serial == 2 && st(w, 0) == S::open && w.made == 2);
	const auto s = w.cat->stats();
	CHECK(s.replaces == 1 && s.wake_failures == 0 && w.cat->wake_failures_of(0) == 0 && w.cat->streak_of(0) == 0);
	CHECK(s.edges[unsigned(S::waking)][unsigned(S::closing)] == 1);
}

// The same, with a chore pin out on the slot while the wake runs: the waiter waits for the holder to drain.
void fenced_wake_counts_holders() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	CHECK(w.cat->begin_demote(0));
	w.settle();
	std::unique_ptr<cat_t::pin> holder;
	run_task(w, [&]() -> task::task<void> {
		holder = std::make_unique<cat_t::pin>(co_await w.cat->pin_chore(kn(0)));
		CHECK(fence(w, 0, *holder));
	});
	got g;
	actor a = ask(w, g, kn(0), open_intent::open);
	start(w, a);
	CHECK(st(w, 0) == S::waking && pins(w, 0) == 2);
	w.exec.run();  // the wake meets the fence
	CHECK(st(w, 0) == S::closing && w.cat->drain_of(0) == 1 && pins(w, 0) == 2 && !g.done);
	w.settle();
	CHECK(!g.done && w.made == 1);  // the holder still has the old index
	holder.reset();
	w.settle();
	CHECK(g.pinned && g.serial == 2 && w.made == 2 && st(w, 0) == S::open);
	CHECK(w.cat->stats().wake_failures == 0 && w.cat->streak_of(0) == 0);
}

// A fenced slot at the wake's bound: the sweep's bound close replaces it.
void fenced_bound_close_closes() {
	W w(1, 2, true, true, {}, 2);  // the bound: 2 failed wakes
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	CHECK(w.cat->begin_demote(0));
	w.settle();
	CHECK(st(w, 0) == S::owed);
	w.kern.fail_map = ENOMEM;
	for (int i = 0; i < 2; ++i) {
		got g;
		go(w, ask(w, g, kn(0), open_intent::open));
		CHECK(g.done && !g.error.empty());
		w.exec.scheduler().advance(11'000 * ms);
	}
	w.kern.fail_map = 0;
	CHECK(w.cat->wake_failures_of(0) == 2 && st(w, 0) == S::owed);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		CHECK(fence(w, 0, p));
	});
	CHECK(w.cat->begin_bound_close(0));
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->table_size() == 0 && w.kern.fds.load() == 0);
	CHECK(w.cat->stats().replaces == 1);
}

// A persistent sync fault: the reopens are bounded by the open backoff, one at once, then 10 ms doubling; a quiet
// 10 s starts the count over.
void reopen_backoff() {
	W w(1, 1, false);
	std::vector<std::uint64_t> opened;
	w.tweak = [&](cat_t::host& h) {
		h.open_payload = [&](const cat_t::opening&, kv_index&, std::optional<payload>& p) {
			p.emplace(++w.made);
			opened.push_back(w.exec.now());
		};
	};
	w.make_catalog({});
	int fenced_n = 0;
	actor a;
	std::optional<cat_t::pin> keep;
	// `rounds` times: pin, fence, hand the pin to reopen(); then a last pin, kept, waits for the final reopen.
	auto loop = [&](int rounds) {
		a.fn = [&, rounds]() -> task::task<void> {
			for (int i = 0; i < rounds; ++i) {
				auto p = co_await w.cat->pin_request(kn(0));
				CHECK(fence(w, 0, p, 100 + i));
				++fenced_n;
				w.cat->reopen(std::move(p));
			}
			keep.emplace(co_await w.cat->pin_request(kn(0)));
		};
		a.t = a.fn();
		go(w, a);
	};
	loop(12);
	CHECK(fenced_n == 12 && opened.size() == 13 && keep);
	// opens: the first, then each reopen: 0, 10 ms, 20 ms, 40 ms ... after the previous
	const std::uint64_t t0 = opened[0];
	int within = 0;
	for (std::size_t i = 1; i < opened.size(); ++i) {
		const std::uint64_t gap = opened[i] - opened[i - 1];
		const std::uint64_t want = i == 1 ? 0 : std::min<std::uint64_t>(10'000 * ms, (10 * ms) << (i - 2));
		if (gap != want) { std::fprintf(stderr, "reopen %zu: gap %llu ns, want %llu\n", i, (unsigned long long)gap, (unsigned long long)want); }
		CHECK(gap == want);
		within += opened[i] - t0 < 10'000 * ms;
	}
	CHECK(within <= 10);
	std::printf("reopen_backoff: %d reopens in the first 10 s, the last gap %llu ms\n", within, (unsigned long long)((opened.back() - opened[opened.size() - 2]) / ms));
	// a quiet 10 s: the next fence reopens at once
	keep.reset();
	w.exec.scheduler().advance(11'000 * ms);
	const std::size_t before = opened.size();
	loop(1);
	CHECK(opened.size() == before + 1 && opened.back() == w.exec.now());
	keep.reset();
}

// --- S14 the check-in before a demotion and a close (F6) ------------------------------------------------------------------

// An idle index whose background merge finished demotes on the first pass after the merge, and closes later.
void check_in_before_demote() {
	W w(1, 8, false);
	w.iopts.auto_merge = true;
	w.iopts.background = &w.exec;  // merge jobs are posted to the executor and run at its next run()
	w.iopts.policy.segments_per_tier = 2;
	w.iopts.policy.floor_bytes = 1;
	strata::rate_limiter rate(0);
	w.iopts.rate = &rate;
	w.make_catalog({});
	std::size_t after_refresh = 0, after_commit = 0;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		const strata::lease_scope hold{p.index().lease()};
		CHECK(p.index().apply(tx({add(key_of(300), "m")}), {}).result == verdict::accepted);
		p.index().refresh();
		after_refresh = p.index().merges_pending();
		p.index().commit();
		after_commit = p.index().merges_pending();
	});
	CHECK(after_refresh + after_commit > 0);
	w.exec.run();  // the merge job runs: finished, not published
	const std::uint64_t maps_before = w.cat->count_of(0, strata::resource::maps);
	CHECK(maps_before > 0);
	w.exec.scheduler().advance(31'000 * ms);
	w.cat->post_pass();
	w.exec.run();
	// The check-in published the finished merge on this pass; its output is unsynced, so the demotion waits for the
	// host's next commit (the autocommit it re-arms while merges are pending), as warm_quiescent() says.
	std::size_t pending_after = 99;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		pending_after = p.index().merges_pending();
	});
	CHECK(pending_after == 0 && w.cat->count_of(0, strata::resource::maps) != 0);  // published, and still hot
	// the host's commits until the merges settle, then the pass demotes and, much later, closes
	for (int round = 0; round < 12 && w.cat->count_of(0, strata::resource::maps) != 0; ++round) {
		run_task(w, [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_chore(kn(0));
			const strata::lease_scope hold{p.index().lease()};
			p.index().commit();
		});
		w.exec.run();
		w.exec.scheduler().advance(31'000 * ms);
		w.cat->post_pass();
		w.exec.run();
	}
	CHECK(w.cat->count_of(0, strata::resource::maps) == 0);
	w.exec.scheduler().advance(700'000 * ms);
	w.cat->post_pass();
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->stats().pass_closes == 1);
}

// --- S15 snapshot (F6) -------------------------------------------------------------------------------------------------------

void snapshot_and_flush() {
	W w(5, 1, true);
	const auto write = [&](int key, int doc) {
		run_task(w, [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(key));
			CHECK(p.index().apply(tx({add(key_of(doc), "d")}), {}).result == verdict::accepted);
		});
	};
	write(0, 200);                     // open, uncommitted
	write(1, 201);                     // to be owed
	{
		run_task(w, [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_request(kn(1));
			p.index().commit();
		});
		CHECK(w.cat->begin_demote(1));
		w.settle();
		CHECK(st(w, 1) == S::owed);
	}
	write(3, 203);                     // open, uncommitted, closed below
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(4)); });  // open, committed
	w.fail_resolve = EIO;              // a negative entry
	w.fail_left = 1;
	{
		got g;
		go(w, ask(w, g, kn(2), open_intent::open));
		CHECK(!g.error.empty());
	}
	auto snap = w.cat->snapshot();
	std::map<std::string, S> by_key;
	for (const auto& v : snap) { by_key[v.key] = v.state; }
	CHECK(snap.size() == 5 && by_key[kn(0)] == S::open && by_key[kn(1)] == S::owed && by_key[kn(2)] == S::failed && by_key[kn(3)] == S::open && by_key[kn(4)] == S::open);
	// k3 closes after the snapshot (commit it, demote it, close it)
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(3));
		p.index().commit();
	});
	CHECK(w.cat->begin_demote(3));
	w.settle();
	CHECK(st(w, 3) == S::owed || st(w, 3) == S::open);
	CHECK(w.cat->begin_close(3));
	w.settle();
	CHECK(st(w, 3) == S::free);
	// FLUSH: each key of the snapshot pinned resident; a pin that comes back empty is skipped
	const int resolves = w.resolves;
	int committed = 0, skipped = 0;
	for (const auto& v : snap) {
		run_task(w, [&]() -> task::task<void> {
			auto p = co_await w.cat->pin_chore(v.key, false, open_intent::resident);
			if (!p) {
				++skipped;
				co_return;
			}
			if (p.index().uncommitted()) {
				p.index().commit();
				++committed;
			}
		});
	}
	CHECK(committed == 1 && skipped == 2 && w.resolves == resolves);  // k0 committed; k2 (failed) and k3 (closed) empty
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0), false, open_intent::resident);
		CHECK(p && !p.index().uncommitted());
	});
	CHECK(w.cat->snapshot().size() == 4);
}

// --- S16 the host's files counted on the slot (F5) ----------------------------------------------------------------------------

// H holding two files of its own: a descriptor and a vnode reserved before each, the call guarded.
struct host_files {
	int serial = 0;
	std::vector<strata::resource_reservation> held;
	std::vector<kernel::fd_token> fds;
	explicit host_files(int s) : serial(s) {}
	~host_files() {
		fds.clear();  // the kernel object first, then its reservations
		held.clear();
	}
};

void host_files_counted() {
	using HW = world<inline_executor, host_files>;
	using hcat = HW::cat_t;
	HW w(2, 1, false, true, {}, 8, 1 << 20);
	std::atomic<int> opened_files{0};
	w.tweak = [&](hcat::host& h) {
		h.open_payload = [&](const hcat::opening& op, kv_index&, std::optional<host_files>& p) {
			p.emplace(++w.made);
			for (int i = 0; i < 2; ++i) {
				auto desc = op.counts.reserve(strata::resource::descriptors);
				auto vnode = op.counts.reserve(strata::resource::vnodes);
				p->fds.push_back(op.counts.guarded(desc, [&] { return w.kern.take_fd(); }));
				p->held.push_back(std::move(desc));
				p->held.push_back(std::move(vnode));
				++opened_files;
			}
		};
	};
	w.make_catalog({});
	// without the host's files a slot holds descriptor and vnode counts of the index's own
	std::uint64_t index_only = 0, peak_only = 0;
	{
		HW w0(1, 1, false);
		run_task(w0, [&]() -> task::task<void> { auto p = co_await w0.cat->pin_request(kn(0)); });
		index_only = w0.cat->count_of(0, strata::resource::descriptors);
		peak_only = std::uint64_t(w0.kern.fd_peak.load());
		w0.drain();
	}
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	CHECK(w.cat->count_of(0, strata::resource::descriptors) == index_only + 2 && w.cat->count_of(0, strata::resource::vnodes) >= 2);
	CHECK(w.kern.fds.load() == std::int64_t(w.cat->count_of(0, strata::resource::descriptors)));  // T7: the budget and the kernel agree
	CHECK(w.cat->counts().get(strata::resource::descriptors) == w.cat->count_of(0, strata::resource::descriptors));
	CHECK(opened_files == 2);
	w.drain();
	CHECK(st(w, 0) == S::free);
	CHECK(w.cat->counts().get(strata::resource::descriptors) == 0 && w.cat->counts().get(strata::resource::vnodes) == 0 && w.kern.fds.load() == 0);
	// the hard cap: the reservation fails with too_many_open before the kernel is asked
	const std::uint64_t used = w.budget.used(strata::resource::descriptors), hard = w.budget.hard(strata::resource::descriptors);
	strata::resource_reservation fill = w.budget.reserve(strata::resource::descriptors, hard - used - (index_only + 1));  // room for the index and one host file
	CHECK(peak_only <= index_only + 1);  // the index's own open needs no more than the room left
	const int files_before = opened_files;
	got g;
	go(w, ask(w, g, kn(1), open_intent::open));
	CHECK(g.done && g.error.find("too_many_open") != std::string::npos);
	const std::uint32_t k1 = sl(w, kn(1));
	CHECK(opened_files == files_before + 1 && w.kern.refused == 0 && st(w, k1) == S::failed);  // the index's files and one of the host's went through; the second never reached the kernel
	CHECK(w.cat->count_of(k1, strata::resource::descriptors) == 0 && w.cat->counts().get(strata::resource::descriptors) == 0 && w.kern.fds.load() == 0);
	CHECK(w.cat->stats().open_failures == 1);
	fill = {};
	w.exec.scheduler().advance(20'000 * ms);
	got ok;
	go(w, ask(w, ok, kn(1), open_intent::open));
	CHECK(ok.pinned);
	w.drain();
	CHECK(w.cat->counts().get(strata::resource::descriptors) == 0 && w.kern.fds.load() == 0);
}

// --- C17 CT-R15: a call on a catalog index holds a pin or is inside a transition -------------------------------------------

void call_without_pin_breaks() {
#ifdef NDEBUG
	return;
#else
	W w(1, 1, false);
	auto& abort_flag = strata::detail::abort_on_owner_break();
	const bool was = abort_flag.exchange(false);
	const auto breaks = [] { return strata::detail::off_owner_changes().load(); };
	std::uint64_t base = breaks();
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		p.index().refresh();  // pinned: fine
	});
	CHECK(breaks() == base);
	CHECK(st(w, 0) == S::open && pins(w, 0) == 0);
	w.cat->index_of(0)->refresh();  // no pin, no transition
	CHECK(breaks() == base + 1);
	CHECK(w.cat->begin_demote(0));
	w.cat->index_of(0)->refresh();  // inside a transition: a chore may call
	CHECK(breaks() == base + 1);
	w.settle();
	base = breaks();
	w.cat->index_of(0)->refresh();
	CHECK(breaks() == base + 1);
	abort_flag.store(was);
#endif
}

}  // namespace

int main() {
	create_passes_absent();
	create_joins_open();
	create_obeys_other_errors();
	resident_never_opens();
	payload_in_place();
	reopen_drains_holders();
	unpin_races_reopen_exchange();
	drain_unpins_race();
	reopen_plain_unpin();
	reopen_commits_unfenced();
	fenced_idle_is_replaced();
	fenced_owed_wake_replaces();
	fenced_wake_counts_holders();
	fenced_bound_close_closes();
	reopen_backoff();
	check_in_before_demote();
	snapshot_and_flush();
	host_files_counted();
	call_without_pin_breaks();
	if (failures) {
		std::fprintf(stderr, "catalog_host: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_host: ok");
	return 0;
}
