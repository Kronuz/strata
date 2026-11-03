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

// The relief a begun transition booked is released when it ends (a replace begun from a demotion included).
template <class WW>
void no_relief_pending(WW& w) {
	CHECK(w.cat->pending_maps() == 0 && w.cat->pending_descriptors() == 0 && w.cat->pending_vnodes() == 0 && w.cat->pending_hot() == 0 && w.cat->pending_open() == 0);
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
	no_relief_pending(w);
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

// A fenced slot's replacement races pins: one arrives while the replace chore runs and one just before its close() call;
// both wait in `closing` and get the one fresh index.
void replace_races_pins() {
	got early, late;
	actor a_early, a_late;
	W* wp = nullptr;
	int step = 0;
	W w(1, 1, false, true, [&](const char* site, bool) {
		if (!wp) { return; }
		if (std::strcmp(site, "chore.replace") == 0 && step == 0) {
			++step;
			a_early = ask(*wp, early, kn(0), open_intent::open);
			start(*wp, a_early);
		} else if (std::strcmp(site, "close.call") == 0 && step == 1) {
			++step;
			a_late = ask(*wp, late, kn(0), open_intent::open);
			start(*wp, a_late);
		}
	});
	wp = &w;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		CHECK(fence(w, 0, p));
		w.cat->reopen(std::move(p));  // no other holder: the close is posted at once
	});
	w.settle();
	CHECK(step == 2 && early.pinned && late.pinned && early.serial == 2 && late.serial == 2 && w.made == 2);
	CHECK(early.gen == late.gen && st(w, 0) == S::open && w.cat->stats().replaces == 1);
	wp = nullptr;
}

// A holder's unpin lands right after reopen's exchange, before reopen returns: `drain` is already stored, so it is
// counted as a drain unpin and the last holder still posts the close.
void unpin_after_reopen_exchange() {
	std::vector<std::unique_ptr<cat_t::pin>> held;
	bool fired = false;
	W w(1, 1, false, true, [&](const char* site, bool) {
		if (!fired && std::strcmp(site, "reopen.exchanged") == 0) {
			fired = true;
			held[1].reset();
		}
	});
	run_task(w, [&]() -> task::task<void> {
		for (int i = 0; i < 3; ++i) { held.push_back(std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0)))); }
	});
	CHECK(fence(w, 0, *held[0]));
	w.cat->reopen(std::move(*held[0]));
	held[0].reset();
	CHECK(fired && st(w, 0) == S::closing && pins(w, 0) == 1 && w.cat->drain_of(0) == 1);
	held[2].reset();
	w.settle();
	CHECK(st(w, 0) == S::free && w.cat->stats().replaces == 1 && w.cat->stats().drain_unpins == 2);
}

// The same for a fenced wake: its one holder unpins right after the wake's exchange; the waiter gets the new index.
void wake_holder_unpins_after_exchange() {
	std::unique_ptr<cat_t::pin> holder;
	bool fired = false;
	W w(1, 2, true, true, [&](const char* site, bool) {
		if (!fired && std::strcmp(site, "replace.exchanged") == 0) {
			fired = true;
			holder.reset();
		}
	});
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	CHECK(w.cat->begin_demote(0));
	w.settle();
	run_task(w, [&]() -> task::task<void> {
		holder = std::make_unique<cat_t::pin>(co_await w.cat->pin_chore(kn(0)));
		CHECK(fence(w, 0, *holder));
	});
	got g;
	actor a = ask(w, g, kn(0), open_intent::open);
	start(w, a);
	w.settle();
	CHECK(fired && g.pinned && g.serial == 2 && st(w, 0) == S::open && w.cat->stats().drain_unpins == 1);
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
	no_relief_pending(w);
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
	no_relief_pending(w);
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
	no_relief_pending(w);
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
	std::uint64_t last_fence_at = 0;
	// `rounds` times: pin, fence, hand the pin to reopen(); then a last pin, kept, waits for the final reopen.
	auto loop = [&](int rounds) {
		a.fn = [&, rounds]() -> task::task<void> {
			for (int i = 0; i < rounds; ++i) {
				auto p = co_await w.cat->pin_request(kn(0));
				CHECK(fence(w, 0, p, 100 + i));
				++fenced_n;
				last_fence_at = w.exec.now();
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
	CHECK(opened.size() == before + 1 && opened.back() == last_fence_at);  // at once: the count started over
	keep.reset();
}

// --- the fix round for the item 5b review (L1 to L3, N3) -------------------------------------------------------------------

// L1. A host whose `open_index` does not create (a replica, a read-only host): an attempt that ran with create and
// failed `index_not_created` leaves a negative entry that gates a create pin like any other, so a create storm costs
// one resolve and one open, then nothing inside the backoff.
void create_storm_gated() {
	W w(1, 1, false);
	absent_dir(w, "new");
	int opens = 0;
	w.tweak = [&](cat_t::host& h) {
		h.open_index = [&](kronuz::journal::IO& io, const kv_index::options& o, const cat_t::opening&) {
			++opens;
			return kv_index::open(io, o);
		};
	};
	w.make_catalog({});
	got r;
	go(w, ask(w, r, "new", open_intent::open));
	CHECK(r.done && is_absent(r) && w.resolves == 1 && opens == 1);
	int absent = 0, resolved = 0, opened = 0;
	for (int i = 0; i < 10; ++i) {
		got c;
		go(w, ask(w, c, "new", open_intent::create));
		absent += is_absent(c);
		if (i == 0) { resolved = w.resolves; opened = opens; }
	}
	CHECK(absent == 10);
	CHECK(resolved == 2 && opened == 2);  // the first create pin passes the read's entry, once
	CHECK(w.resolves == 2 && opens == 2 && w.cat->stats().failed_fast == 9 && st(w, 0) == S::failed);
	// past the retry time one more create pin gets one more attempt
	w.exec.scheduler().advance(20'000 * ms);
	got late;
	go(w, ask(w, late, "new", open_intent::create));
	CHECK(is_absent(late) && w.resolves == 3 && opens == 3);
}

// N3. `wake_bound` above 255 (the slot counts failures in a byte) is refused, not clamped; 255 is accepted.
void wake_bound_refused() {
	{ W w(1, 1, false, true, {}, 255); }
	bool refused = false;
	try {
		W w(1, 1, false, true, {}, 256);
	} catch (const std::invalid_argument&) {
		refused = true;
	}
	CHECK(refused);
}

// L2. `snapshot()` loads a slot's word, then rechecks its `key_hash`: a free slot whose stale `key_hash` names the
// snapshot's shard, claimed by a key of another shard inside that window, is skipped, and its key is not read under the
// wrong shard's mutex. The hook runs between the word load and the recheck.
void snapshot_skips_reused_slot() {
	bool armed = false, claimed = false;
	int b = -1;
	W w(8, 1, false, true, [&](const char* site, bool) {
		if (armed && !claimed && std::string_view(site) == "snapshot.slot") {
			claimed = true;
			got g;
			go(w, ask(w, g, kn(b), open_intent::open));  // b's shard is not the one the snapshot holds
			CHECK(g.pinned);
		}
	});
	const auto shard = [](int k) { return strata::index_detail::key_hash(kn(k)) & 63u; };
	for (int k = 1; k < 8 && b < 0; ++k) {
		if (shard(k) != shard(0)) { b = k; }
	}
	CHECK(b > 0);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	w.drain();
	CHECK(st(w, 0) == S::free && w.cat->slots_made() == 1);  // freed, its key_hash still naming k0's shard
	armed = true;
	const auto snap = w.cat->snapshot();
	CHECK(claimed && st(w, 0) == S::open && w.cat->key_at(0) == kn(b));
	CHECK(snap.empty());  // the slot changed owner after its word was read as free: skipped
	CHECK(w.cat->snapshot().size() == 1);
}

// The review's probes (P2 to P8), kept as tests. P5 and P5c are the invariant that lets `run_close` skip the check-in: an
// idle close never meets a finished merge, because a demotion (with its check-in) always comes first.

// P2. A create pin joins while the non-create attempt is inside `open_index` (past `resolve`), and a read pin joins the
// reposted attempt: every waiter gets the one created index, with one repost.
void create_joins_in_open_index() {
	W w(1, 1, false);
	absent_dir(w, "new");
	got read, create, late;
	actor a_read = ask(w, read, "new", open_intent::open), a_create = ask(w, create, "new", open_intent::create), a_late = ask(w, late, "new", open_intent::open);
	int calls = 0;
	w.tweak = [&](cat_t::host& h) {
		auto inner = h.open_index;
		h.open_index = [&, inner](kronuz::journal::IO& io, const kv_index::options& o, const cat_t::opening& op) {
			++calls;
			if (calls == 1) {
				CHECK(!op.create);
				start(w, a_create);
			} else if (calls == 2) {
				CHECK(op.create);
				start(w, a_late);
			}
			return inner(io, o, op);
		};
	};
	w.make_catalog({});
	start(w, a_read);
	w.settle();
	const auto s = w.cat->stats();
	CHECK(read.pinned && create.pinned && late.pinned && read.serial == create.serial && create.serial == late.serial);
	CHECK(s.create_reposts == 1 && s.open_failures == 0 && calls == 2);
	w.drain();
}


// P3. H is built in place: the address H's constructor saw is the address the pin hands out (no move, no copy).
struct addr_h {
	int serial = 0;
	static inline const void* born = nullptr;
	explicit addr_h(int s) : serial(s) { born = this; }
	addr_h(const addr_h&) = delete;
	addr_h(addr_h&&) = delete;
};
void payload_address() {
	using AW = world<inline_executor, addr_h>;
	AW w(1, 1, false);
	const void* seen = nullptr;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		seen = &p.payload();
	});
	CHECK(seen != nullptr && seen == addr_h::born);
	w.drain();
}


// P4. A fenced `owed` slot met by the sweep's idle close (not the bound close) is replaced and the slot frees.
void fenced_owed_sweep_close() {
	W w(1, 2, true);
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	CHECK(w.cat->begin_demote(0));
	w.settle();
	CHECK(st(w, 0) == S::owed);
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		CHECK(fence(w, 0, p));
	});
	w.exec.scheduler().advance(700'000 * ms);
	w.cat->post_pass();
	w.settle();
	const auto s = w.cat->stats();
	CHECK(st(w, 0) == S::free && s.replaces == 1 && w.kern.fds.load() == 0);
}


// P5. An `open` slot with no maps (eager_segments off) is a close candidate: a chore pin's writer commits, a background
// merge finishes after the unpin; the sweep demotes (check-in and the commit of the merge's output), then closes.
void close_meets_no_finished_merge() {
	W w(1, 8, false, false);
	w.iopts.auto_merge = true;
	w.iopts.background = &w.exec;
	w.iopts.policy.segments_per_tier = 2;
	w.iopts.policy.floor_bytes = 1;
	strata::rate_limiter rate(0);
	w.iopts.rate = &rate;
	w.make_catalog({});
	std::size_t pending = 0;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		const strata::lease_scope hold{p.index().lease()};
		CHECK(p.index().apply(tx({add(key_of(300), "m")}), {}).result == verdict::accepted);
		p.index().refresh();
		p.index().commit();
		pending = p.index().merges_pending();
	});
	w.exec.run();  // the merge job runs: finished, not published
	int passes = 0;
	for (; passes < 6 && st(w, 0) != S::free; ++passes) {
		w.exec.scheduler().advance(700'000 * ms);
		w.cat->post_pass();
		w.exec.run();
		w.exec.run();
	}
	const auto s = w.cat->stats();
	CHECK(pending > 0 && st(w, 0) == S::free && s.pass_closes == 1 && s.pass_demotes >= 1);  // demoted first, then closed
}


// P5c. A warm `open` slot written by a request pin maps a segment again: it demotes before it closes, eager or not.
void warm_close_after_write(bool eager) {
	W w(1, 8, false, eager);
	w.iopts.auto_merge = true;
	w.iopts.background = &w.exec;
	w.iopts.policy.segments_per_tier = 2;
	w.iopts.policy.floor_bytes = 1;
	strata::rate_limiter rate(0);
	w.iopts.rate = &rate;
	w.make_catalog({});
	run_task(w, [&]() -> task::task<void> { auto p = co_await w.cat->pin_request(kn(0)); });
	for (int i = 0; i < 8 && w.cat->count_of(0, strata::resource::maps) != 0; ++i) {  // demote, letting any merge cascade finish
		(void)w.cat->begin_demote(0);
		w.exec.run();
		w.exec.run();
	}
	const auto maps_warm = w.cat->count_of(0, strata::resource::maps);
	std::size_t pending = 0;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_request(kn(0));
		const strata::lease_scope hold{p.index().lease()};
		CHECK(p.index().apply(tx({add(key_of(302), "m")}), {}).result == verdict::accepted);
		p.index().refresh();
		p.index().commit();
		pending = p.index().merges_pending();
	});
	const auto maps_written = w.cat->count_of(0, strata::resource::maps);
	w.exec.run();  // the merge job runs: finished, not published
	const auto maps_merged = w.cat->count_of(0, strata::resource::maps);
	int passes = 0;
	for (; passes < 8 && st(w, 0) != S::free; ++passes) {
		w.exec.scheduler().advance(700'000 * ms);
		w.cat->post_pass();
		w.exec.run();
		w.exec.run();
	}
	const auto s = w.cat->stats();
	// the write maps a segment, so the slot is hot again and demotes (checking in) before it can close
	CHECK(maps_written > maps_warm && st(w, 0) == S::free && s.pass_closes == 1 && s.pass_demotes == 1 && pending == 0 && maps_merged == maps_written);
}


// P6 (C17). The guard follows the index through a replacement, and an `owed` slot with no pin breaks too.
void c17_after_replace() {
#ifndef NDEBUG
	W w(1, 2, true);
	auto& abort_flag = strata::detail::abort_on_owner_break();
	const bool was = abort_flag.exchange(false);
	const auto breaks = [] { return strata::detail::off_owner_changes().load(); };
	std::unique_ptr<cat_t::pin> holder;
	run_task(w, [&]() -> task::task<void> { holder = std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0))); });
	CHECK(fence(w, 0, *holder));
	got waiter;
	actor a = ask(w, waiter, kn(0), open_intent::open);
	w.cat->reopen(std::move(*holder));
	holder.reset();
	start(w, a);
	w.settle();
	CHECK(waiter.pinned && waiter.serial == 2 && st(w, 0) == S::open);
	std::uint64_t base = breaks();
	w.cat->index_of(0)->refresh();
	const bool replaced_breaks = breaks() == base + 1;
	CHECK(w.cat->begin_demote(0));
	w.settle();
	CHECK(st(w, 0) == S::owed);
	base = breaks();
	w.cat->index_of(0)->refresh();
	const bool owed_breaks = breaks() == base + 1;
	abort_flag.store(was);
	CHECK(replaced_breaks && owed_breaks);
#else
#endif
}


// P7. A drain in progress when shutdown begins: the waiter that joined first gets the reopened index, shutdown waits for
// the last holder, and a pin after shutdown began is refused.
void drain_at_shutdown() {
	W w(1, 1, false);
	std::vector<std::unique_ptr<cat_t::pin>> held;
	run_task(w, [&]() -> task::task<void> {
		for (int i = 0; i < 2; ++i) { held.push_back(std::make_unique<cat_t::pin>(co_await w.cat->pin_request(kn(0)))); }
	});
	CHECK(fence(w, 0, *held[0]));
	w.cat->reopen(std::move(*held[0]));
	held[0].reset();
	got waiter, after;
	actor a = ask(w, waiter, kn(0), open_intent::open);
	start(w, a);
	w.cat->stop_sweep();
	w.start_shutdown();
	w.settle();
	const bool held_back = !w.sd_done && st(w, 0) == S::closing;
	go(w, ask(w, after, kn(0), open_intent::open));  // after shutdown began: catalog_closed
	held[1].reset();
	w.settle();
	CHECK(held_back && waiter.pinned && waiter.serial == 2 && !after.pinned && w.sd_done && st(w, 0) == S::free);
}


// P8 (F5). A resident pin that waits on the `opening` of an absent key gets the open's error, not an empty pin.
void resident_meets_failing_open() {
	W w(1, 1, false);
	absent_dir(w, "new");
	got r, res;
	actor a = ask(w, r, "new", open_intent::open), b = ask(w, res, "new", open_intent::resident, pin_kind::chore);
	start(w, a);
	start(w, b);
	w.settle();
	CHECK(is_absent(r) && res.done && !res.pinned && !res.empty && is_absent(res));  // the open's error, not an empty pin
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
	std::size_t pending = 0;
	run_task(w, [&]() -> task::task<void> {
		auto p = co_await w.cat->pin_chore(kn(0));
		const strata::lease_scope hold{p.index().lease()};
		CHECK(p.index().apply(tx({add(key_of(300), "m")}), {}).result == verdict::accepted);
		p.index().refresh();
		p.index().commit();
		pending = p.index().merges_pending();
	});
	CHECK(pending > 0);
	const int commits = w.commits;
	w.exec.run();  // the merge job runs: finished, not published, its output not synced
	CHECK(w.cat->count_of(0, strata::resource::maps) > 0 && w.commits == commits);
	// Idle from here, and the test commits nothing: each pass checks the index in (publishing the finished merge) and,
	// as only the merge's output is unsynced, has the host's commit make it durable (which may start the next merge of
	// a cascade); the pass after the last merge demotes.
	int passes = 0;
	for (; passes < 12 && w.cat->count_of(0, strata::resource::maps) != 0; ++passes) {
		w.exec.scheduler().advance(31'000 * ms);
		w.cat->post_pass();
		w.exec.run();
		w.exec.run();  // a merge the host's commit started
	}
	CHECK(passes == 2 && w.cat->count_of(0, strata::resource::maps) == 0);  // the cascade: the first pass after the last merge demotes
	CHECK(w.cat->stats().merge_commits >= 1 && w.cat->stats().merge_commit_failures == 0 && w.commits == commits + int(w.cat->stats().merge_commits));
	std::printf("check_in_before_demote: demoted after %d passes, %llu host commits for merge output\n", passes, (unsigned long long)w.cat->stats().merge_commits);
	// and a later pass closes it
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
	create_storm_gated();
	wake_bound_refused();
	snapshot_skips_reused_slot();
	create_joins_in_open_index();
	payload_address();
	fenced_owed_sweep_close();
	close_meets_no_finished_merge();
	warm_close_after_write(true);
	warm_close_after_write(false);
	c17_after_replace();
	drain_at_shutdown();
	resident_meets_failing_open();
	create_joins_open();
	create_obeys_other_errors();
	resident_never_opens();
	payload_in_place();
	reopen_drains_holders();
	unpin_races_reopen_exchange();
	drain_unpins_race();
	reopen_plain_unpin();
	reopen_commits_unfenced();
	replace_races_pins();
	unpin_after_reopen_exchange();
	wake_holder_unpins_after_exchange();
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
