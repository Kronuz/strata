// basic_index's file ledger (quest index-restructure A3): each transition keeps the invariant
// (on_disk is the files' sum, in_flight the posted files' sum, no name in both) and counts each
// file's bytes once until a directory sync frees them. Modes: track, forget, post, unpost,
// rename, model; none runs all. `model` takes an optional seed and iteration count.
#include "strata/core/index_ledger.hh"

#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>

namespace {

using strata::index_detail::file_ledger;

int failures = 0;
void check(bool ok, const std::string& what) {
	if (!ok && ++failures <= 20) { std::cerr << "FAIL: " << what << '\n'; }
}

// Re-tracking a file counts its new size, not both.
void track_case() {
	file_ledger l;
	l.track("a", 100);
	l.track("b", 50);
	check(l.on_disk() == 150 && l.consistent(), "track: two files");
	l.track("a", 300);
	check(l.on_disk() == 350 && l.consistent(), "track: a grown file counts its new size");
	l.track("a", 10);
	check(l.on_disk() == 60 && l.consistent(), "track: a truncated file counts its new size");
	check(l.size_of("a") == 10 && l.size_of("missing") == 0, "track: size_of");
	check(l.counted() == 60 && !l.unlinked() && !l.in_flight(), "track: nothing unlinked or in flight");
}

// A removed file's bytes stay counted, as unlinked, until the sync.
void forget_case() {
	file_ledger l;
	l.track("a", 100);
	l.track("b", 7);
	l.forget("a");
	check(l.on_disk() == 7 && l.unlinked() == 100 && l.counted() == 107 && l.consistent(), "forget: on disk to unlinked");
	check(!l.files().count("a"), "forget: the name leaves the record");
	l.forget("missing");
	check(l.counted() == 107 && l.consistent(), "forget: an unknown name changes nothing");
	l.synced();
	check(l.unlinked() == 0 && l.counted() == 7 && l.consistent(), "forget: the sync frees the unlinked bytes");
}

// A posted delete moves the bytes in flight; its removal makes them unlinked.
void post_case() {
	file_ledger l;
	l.track("a", 100);
	l.track("b", 5);
	check(l.post("a") == 100, "post: returns the size");
	check(!l.files().count("a") && l.posted().count("a"), "post: the name moves from the record to posted");
	check(l.on_disk() == 5 && l.in_flight() == 100 && l.counted() == 105 && l.consistent(), "post: on disk to in flight");
	check(l.post("missing") == 0 && l.counted() == 105 && l.consistent(), "post: an unknown name posts nothing");
	l.removed("a");
	check(l.in_flight() == 0 && l.unlinked() == 100 && l.counted() == 105 && l.consistent(), "post: removed, in flight to unlinked");
	l.removed("a");
	check(l.unlinked() == 100 && l.consistent(), "post: a second removal changes nothing");
	l.synced();
	check(l.counted() == 5 && l.consistent(), "post: the sync frees it");
}

// An undone delete leaves flight, and the file is tracked again at the size left.
void unpost_case() {
	file_ledger l;
	l.track("a", 100);
	l.post("a");
	check(l.unpost("a"), "unpost: the name was posted");
	check(l.in_flight() == 0 && l.posted().empty() && l.consistent(), "unpost: the bytes leave flight");
	l.track("a", 40);
	check(l.on_disk() == 40 && l.counted() == 40 && l.consistent(), "unpost: tracked again at the size left");
	check(!l.unpost("a") && !l.unpost("missing"), "unpost: not posted");
}

// A rename moves the bytes to the new name, replacing what it named.
void rename_case() {
	file_ledger l;
	l.track("tmp", 64);
	l.rename("tmp", "manifest");
	check(!l.files().count("tmp") && l.size_of("manifest") == 64 && l.on_disk() == 64 && l.consistent(), "rename: same bytes, new name");
	l.track("tmp", 80);
	l.rename("tmp", "manifest");
	check(l.files().size() == 1 && l.size_of("manifest") == 80 && l.on_disk() == 80 && l.consistent(), "rename: replaces the old file's bytes");
}

// Random transitions against a model of each file's state: the invariant and the total hold at
// every step.
void model_case(std::uint64_t seed, std::size_t iterations) {
	enum class st { disk, flight, unlinked };
	struct entry {
		st state;
		std::uint64_t size;
	};
	std::mt19937_64 rng(seed);
	file_ledger l;
	std::multimap<std::string, entry> m;  // unlinked entries keep their name until the sync
	auto find = [&](const std::string& n, st s) {
		auto [lo, hi] = m.equal_range(n);
		for (auto it = lo; it != hi; ++it) {
			if (it->second.state == s) { return it; }
		}
		return m.end();
	};
	for (std::size_t i = 0; i < iterations && failures == 0; ++i) {
		const std::string n = "f" + std::to_string(rng() % 8);
		const std::uint64_t size = rng() % 1000;
		const auto disk = find(n, st::disk), flight = find(n, st::flight);
		switch (rng() % 7) {
		case 0:
		case 1:
			if (flight != m.end()) { break; }  // a posted file is never tracked until it is unposted
			l.track(n, size);
			if (disk != m.end()) {
				disk->second.size = size;
			} else {
				m.insert({n, {st::disk, size}});
			}
			break;
		case 2:
			l.forget(n);
			if (disk != m.end()) { disk->second.state = st::unlinked; }
			break;
		case 3:
			if (flight != m.end()) { break; }  // the reclaim queue never posts a name twice
			l.post(n);
			if (disk != m.end()) {
				disk->second.state = st::flight;
			} else {
				m.insert({n, {st::flight, 0}});
			}
			break;
		case 4:
			l.removed(n);
			if (flight != m.end()) { flight->second.state = st::unlinked; }
			break;
		case 5:
			if (l.unpost(n) != (flight != m.end())) { check(false, "model: unpost's answer"); }
			if (flight != m.end()) {
				m.erase(flight);
				if (disk == m.end()) {
					l.track(n, size);
					m.insert({n, {st::disk, size}});
				}
			}
			break;
		default:
			l.synced();
			for (auto it = m.begin(); it != m.end();) { it = it->second.state == st::unlinked ? m.erase(it) : std::next(it); }
		}
		std::uint64_t d = 0, f = 0, u = 0;
		for (const auto& [name, e] : m) { (e.state == st::disk ? d : e.state == st::flight ? f : u) += e.size; }
		check(l.consistent(), "model: consistent at step " + std::to_string(i));
		check(l.on_disk() == d && l.in_flight() == f && l.unlinked() == u, "model: each state's bytes at step " + std::to_string(i));
		check(l.counted() == d + f + u, "model: every file's bytes counted once at step " + std::to_string(i));
	}
}

}  // namespace

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "all";
	const std::uint64_t seed = argc > 2 ? std::stoull(argv[2]) : 1;
	const std::size_t iterations = argc > 3 ? std::stoull(argv[3]) : 200000;
	const bool all = mode == "all";
	if (all || mode == "track") { track_case(); }
	if (all || mode == "forget") { forget_case(); }
	if (all || mode == "post") { post_case(); }
	if (all || mode == "unpost") { unpost_case(); }
	if (all || mode == "rename") { rename_case(); }
	if (all || mode == "model") { model_case(seed, iterations); }
	if (failures) {
		std::cerr << failures << " failures\n";
		return 1;
	}
	std::cout << "ok\n";
	return 0;
}
