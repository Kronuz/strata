// An install lands the learner's index on its bundle (docs/design/journal-binding.md D5, check
// C2). Three voters hold a bundle K, `extra` writes land past it, and a learner joins: it installs
// the leader's pin, which holds strata at the bundle itself, so the learner's strata head after
// activation is the boundary its log installed.
//
// restart: the learner, right after its activation, restarted by a kill, a power cut or a clean
// close, with 0 and 5 writes past the bundle: it opens each time, serves every acknowledged write,
// and its digest equals a replay.
// busy: the order the design review's probe found (job 261125-021025-991b02). The leader's chore
// runs only while the learner holds the image and fetches its pin (the flight Sending), so the
// pinning checkpoint reaches consensus inside that window and is refused Busy; its publication
// waits for the flight's release and retries, and the next flight installs: within two flights.
// `group_install_bundle_test [restart|busy]`.
#include "world.hh"

#include <cstdio>
#include <string>

namespace {

using namespace group_test;
using ull = unsigned long long;

bool write_n(world& w, std::mt19937_64& gen, int n) {
	for (int k = 0; k < n; ++k) {
		auto* l = w.leader();
		if (!l) {
			w.settle(false);
			continue;
		}
		const auto key = group_test::keys[gen() % group_test::keys.size()];
		auto s = start(l->p->loop_ref(), l->p->g->propose("P|" + key + "|" + text(gen, 8)));
		for (int i = 0; i < 40000 && !s->done; ++i) { w.step(); }
		if (!s->done || s->error) { return false; }
	}
	return true;
}

// Three voters, a bundle on each, `extra` writes past it, then a learner (node 4) added.
struct joined {
	world w;
	node* l = nullptr;
	node* j = nullptr;
	cs::Index bundle_at = 0;
	joined(int extra, std::uint64_t seed) : w(3, seed) {
		std::mt19937_64 gen(seed);
		if (!check(w.settle(), "the cluster settles")) { return; }
		write_and_compact(w, gen, 6);
		if (!check(write_n(w, gen, extra), "the writes past the bundle")) { return; }
		if (!check(w.settle(), "the cluster settles after the writes")) { return; }
		l = w.leader();
		if (!check(l, "a leader")) { return; }
		bundle_at = l->p->g->recovered().position;
		j = &w.add_joiner(4);
		auto next = membership_at(*l, 3);
		next.members.push_back({j->id, j->id, cs::MemberRole::Learner});
		next.next_incarnation = j->id + 1;
		auto placed = start(l->p->loop_ref(), l->p->lp->driver().change_membership(gid, next));
		(void)placed;
		l->run();
	}
	bool installed() const { return j->up() && j->p->g && j->p->g->installs() >= 1; }
};

void restart(const std::string& how, int extra, std::uint64_t seed) {
	context = "restart " + how + ", " + std::to_string(extra) + " writes past the bundle, seed " + std::to_string(seed);
	joined s(extra, seed);
	if (!s.j) { return; }
	auto& w = s.w;
	auto& j = *s.j;
	for (int i = 0; i < 400000 && !s.installed(); ++i) { w.step(); }
	if (!check(s.installed(), "the learner activates an install")) { return; }
	const auto at = j.p->g->installed_at();
	const auto head = j.p->g->durable_head().index;
	std::printf("%s: bundle %llu when the learner joined; installed at %llu, strata head %llu\n", context.c_str(), ull(s.bundle_at), ull(at), ull(head));
	check(at >= s.bundle_at, "the install is at or past the bundle the learner joined at");
	check(head == at, "the learner's strata head after activation is its install's boundary: P_H " + std::to_string(head) + ", B " + std::to_string(at));
	if (how == "close") {
		w.isolated.insert(j.id);  // nothing more reaches it: its close covers what it holds now
		auto k = start(j.p->loop_ref(), j.p->g->close());
		for (int i = 0; i < 40000 && !k->done; ++i) { w.step(); }
		check(k->done && !k->error, "the learner's clean close: " + what(k->error));
		w.shut(j, false);
		w.isolated.clear();
	} else {
		w.shut(j, how == "power");
	}
	w.boot(j);
	for (int i = 0; i < 40000 && !j.p->opening->done; ++i) { w.step(); }
	const bool opened = j.p->opening->done && !j.p->opening->error;
	check(opened, "the restarted learner opens: " + what(j.p->opening->error));
	if (opened && check(w.settle(), "the cluster settles with the learner")) { same_digests(w); }
}

// The learner's fetches of a pin ("K/pin").
std::vector<std::string> pin_fetches(world& w, cs::NodeId from) {
	std::vector<std::string> out;
	for (const auto& f : w.fetches) {
		if (f.from == from && f.name.ends_with("/pin")) { out.push_back(f.name); }
	}
	return out;
}

// Answers the queued fetches, a pin's only when `pins`; true if any.
bool answer_fetches(world& w, bool pins) {
	bool any = false;
	for (std::size_t i = 0; i < w.fetches.size();) {
		if (!pins && w.fetches[i].name.ends_with("/pin")) {
			++i;
			continue;
		}
		auto f = std::move(w.fetches[i]);
		w.fetches.erase(w.fetches.begin() + std::ptrdiff_t(i));
		w.answer(std::move(f));
		any = true;
	}
	return any;
}

// One round at `dt` ms: messages, IO, loops and every chore but `held`'s; fetches answered, a
// pin's only when `pins`.
bool pump(world& w, node* held, bool pins, std::uint64_t dt) {
	w.advance(dt);
	bool moved = false;
	for (int quiet = 0; quiet < 1000; ++quiet) {
		bool any = !w.net.empty();
		while (!w.net.empty()) {
			auto e = std::move(w.net.front());
			w.net.pop_front();
			w.deliver(std::move(e));
		}
		any = answer_fetches(w, pins) || any;
		for (auto& [id, x] : w.nodes) {
			if (!x->up()) { continue; }
			while (x->p->held.held()) {
				any = true;
				x->p->held.complete(x->p->held.tickets().front());
				x->run();
			}
			if (x.get() != held) { any = x->p->chore_s.run() > 0 || any; }
			x->run();
		}
		moved = moved || any;
		if (!any) { break; }
	}
	return moved;
}

// The leader's loop and chore until both idle, fetches held.
void run_leader(world& w, node& l) {
	for (int round = 0; round < 1000; ++round) {
		bool any = l.p->chore_s.run() > 0;
		while (l.p->held.held()) {
			any = true;
			l.p->held.complete(l.p->held.tickets().front());
		}
		any = l.p->loop_s.run() > 0 || any;
		while (!w.net.empty()) {
			any = true;
			auto e = std::move(w.net.front());
			w.net.pop_front();
			w.deliver(std::move(e));
		}
		if (!any) { return; }
	}
}

void busy(std::uint64_t seed) {
	context = "busy, seed " + std::to_string(seed);
	joined s(5, seed);
	if (!s.j) { return; }
	auto& w = s.w;
	auto& l = *s.l;
	auto& j = *s.j;
	const auto k = s.bundle_at;
	check(l.p->g->strata().applied().index > k, "the leader's strata is past its bundle");
	int flights = 0;
	while (flights < 6 && !s.installed()) {
		// The flight Sends: the learner holds the image and fetches the pin, the leader's chore held.
		for (int r = 0; r < 4000 && pin_fetches(w, j.id).empty() && !s.installed(); ++r) { pump(w, &l, false, 1); }
		if (s.installed()) { break; }
		if (!check(!pin_fetches(w, j.id).empty(), "flight " + std::to_string(flights + 1) + ": the learner fetches a pin")) { return; }
		++flights;
		const auto generation = l.p->g->strata().manifest().generation;
		const auto latest = l.p->g->recovered().position;
		// Inside the Sending window: SnapshotNeeded's pinning checkpoint, if any, reaches consensus.
		run_leader(w, l);
		if (flights == 1) {
			check(l.p->g->strata().manifest().generation > generation, "the pinning checkpoint committed strata");
			check(l.p->g->recovered().position == latest && latest == k, "its publication waits: the flight at K is Sending (Busy)");
		}
		// The flight ends: its pin fetch answered, the leader's chore still held; the next
		// flight's pin fetch waits for the top of the loop.
		answer_fetches(w, true);
		for (int r = 0; r < 4000 && !s.installed() && pin_fetches(w, j.id).empty(); ++r) { pump(w, &l, false, 1); }
	}
		std::printf("%s: bundle K %llu; installed at %llu after %d flights held in the Sending order\n", context.c_str(), ull(k), ull(s.installed() ? j.p->g->installed_at() : 0), flights);
	check(s.installed(), "the learner installs");
	check(flights <= 2, "within two flights: " + std::to_string(flights));
	if (s.installed()) {
		check(j.p->g->installed_at() > k, "it installs the pinning checkpoint's bundle, past K");
		check(j.p->g->durable_head().index == j.p->g->installed_at(), "its strata head is that bundle's boundary");
	}
	if (check(w.settle(), "the cluster settles with the learner")) { same_digests(w); }
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	if (only.empty() || only == "restart") {
		for (const auto* how : {"kill", "power", "close"}) {
			for (int extra : {0, 5}) { restart(how, extra, 77); }
		}
	}
	if (only.empty() || only == "busy") {
		for (std::uint64_t seed : {77, 3, 11}) { busy(seed); }
	}
	std::printf("install-bundle%s%s: %s\n", only.empty() ? "" : " ", only.c_str(), failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
