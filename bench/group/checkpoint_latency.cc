// Quorum write latency under periodic checkpoints with an install in flight (journal-binding.md
// check C5). Three voters on in-memory faultfs and inline schedulers (test/group/world.hh), a
// learner whose fetches fail (its source unreachable on the fetch lane), so every flight fails and
// the next starts after rpc_timeout: a peer always awaits an install, and a flight is Sending
// between its image and its failed pin fetch. The leader checkpoints every `every` writes. Each
// write's wall time runs from propose to its quorum answer and includes the chore work the step
// pumped meanwhile (apply, the checkpoint's capture, syncs, commit and, under D5, its pin's read
// of every segment). The device is memory: this measures the chore's CPU, not fsync.
// `group_checkpoint_latency [writes] [every] [seed]`: prints p50 and p99 in microseconds.
#include "../../test/group/world.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace group_test;

namespace {

std::uint64_t refused_fetches = 0;  // the learner's, each a flight's pin or chunk

// Messages, IO, loops and chores at `dt` ms; the learner's fetches fail.
void pump(world& w, cs::NodeId held, std::uint64_t dt) {
	w.advance(dt);
	for (int quiet = 0; quiet < 1000; ++quiet) {
		bool any = !w.net.empty();
		while (!w.net.empty()) {
			auto e = std::move(w.net.front());
			w.net.pop_front();
			w.deliver(std::move(e));
		}
		while (!w.fetches.empty()) {
			auto f = std::move(w.fetches.front());
			w.fetches.pop_front();
			if (f.from == held) {
				++refused_fetches;
				f.done(std::make_error_code(std::errc::connection_aborted), std::nullopt);
				w.nodes.at(held)->run();
			} else {
				w.answer(std::move(f));
			}
			any = true;
		}
		for (auto& [id, x] : w.nodes) {
			if (!x->up()) { continue; }
			while (x->p->held.held()) {
				any = true;
				x->p->held.complete(x->p->held.tickets().front());
				x->run();
			}
			any = x->p->chore_s.run() > 0 || any;
			x->run();
		}
		if (!any) { break; }
	}
}

}  // namespace

int main(int argc, char** argv) {
	const int writes = argc > 1 ? std::stoi(argv[1]) : 2000;
	const int every = argc > 2 ? std::stoi(argv[2]) : 20;
	const std::uint64_t seed = argc > 3 ? std::stoull(argv[3]) : 77;
	context = "checkpoint latency";
	world w(3, seed);
	std::mt19937_64 gen(seed);
	if (!check(w.settle(), "the cluster settles")) { return 2; }
	write_and_compact(w, gen, 6);
	auto* l = w.leader();
	if (!check(l, "a leader")) { return 2; }
	auto& j = w.add_joiner(4);
	auto next = membership_at(*l, 3);
	next.members.push_back({j.id, j.id, cs::MemberRole::Learner});
	next.next_incarnation = j.id + 1;
	auto placed = start(l->p->loop_ref(), l->p->lp->driver().change_membership(gid, next));
	(void)placed;
	for (int r = 0; r < 200; ++r) { pump(w, j.id, 1); }
	std::vector<double> us;
	us.reserve(std::size_t(writes));
	// Every voter checkpoints, each one at a time: a follower's log is bounded too.
	std::map<cs::NodeId, std::shared_ptr<started<cs::CheckpointPublished>>> k;
	int checkpoints = 0, published = 0;
	const auto fetched_before = refused_fetches;
	for (int n = 0; n < writes; ++n) {
		for (cs::NodeId v = 1; v <= 3; ++v) {
			auto& x = *w.nodes.at(v);
			if (!k[v] && n % every == every - 1) {
				k[v] = start(x.p->loop_ref(), x.p->g->checkpoint());
				checkpoints += x.id == l->id;
			}
		}
		const auto key = group_test::keys[gen() % group_test::keys.size()];
		const auto t0 = std::chrono::steady_clock::now();
		auto s = start(l->p->loop_ref(), l->p->g->propose("P|" + key + "|" + text(gen, 16)));
		for (int r = 0; r < 100000 && !s->done; ++r) { pump(w, j.id, 1); }
		us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		if (!check(s->done && !s->error, "quorum write " + std::to_string(n) + " (" + std::to_string(published) + " checkpoints published): " + (s->done ? what(s->error) : "not answered"))) { return 2; }
		for (auto& [v, c] : k) {
			if (c && c->done) {
				published += v == l->id && !c->error;
				c.reset();
			}
		}
	}
	std::sort(us.begin(), us.end());
	auto at = [&](double q) { return us[std::min(us.size() - 1, std::size_t(q * double(us.size())))]; };
	std::printf("writes %d, leader checkpoints started %d, published %d, learner fetches refused %llu, installs %llu: p50 %.1f us, p99 %.1f us, max %.1f us\n", writes,
		checkpoints, published, (unsigned long long)(refused_fetches - fetched_before), (unsigned long long)j.p->g->installs(), at(0.50), at(0.99), us.back());
	return failures ? 1 : 0;
}
