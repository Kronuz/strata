// forgetful_check: forgetful_model.py's model in C++, for five and six voters and greater depth.
//
// The same state, actions, labels and checks as forgetful_model.py (revisions 1 and 2; --compat
// reproduces the architect's DM-R1 probe; revision 2's same-boot restarts, torn tails, retried
// rounds, membership changes, and the recovery count are off unless asked for, so every
// revision-1 configuration keeps its state count). Search: a depth-bounded DFS over a table that
// keeps, per state, the most remaining depth it was expanded with, so it visits exactly the states
// reachable within the depth, as a BFS does; the DFS stack is each counterexample's trace (not
// necessarily the shortest). --schedule FILE replays a forced schedule (one label per line) and,
// with --from-step K, searches from the state its first K steps reach. States are keyed by 128-bit
// hashes of a canonical encoding: with --symmetry, the least encoding over the node relabelings
// that sort nodes by an invariant signature (voters the configuration script does not name are
// interchangeable); hash compaction can omit a state with probability about n^2 / 2^129 (n^2 /
// 2^65 with --key64), which a pass states as bounded evidence anyway.
//
// Build: c++ -std=c++20 -O2 -o forgetful_check forgetful_check.cc

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

constexpr int MAXN = 6, MAXLOG = 14, MAXSCRIPT = 2;
enum Role : uint8_t { FOLLOWER, CANDIDATE, LEADER, RCAND };
enum Mode { NONE, HEARD, TIMEOUT, FPLUS, FPLUS_HEARD, ROUND, ROUND_HEARD };
const char* mode_names[] = {"none", "heard", "timeout", "fplus", "fplus_heard", "round", "round_heard"};
constexpr uint8_t NOOP = 0, REC = 1, CFG = 100;  // a client write k is 2 + k; configuration change k is CFG + k

struct Entry { uint8_t term = 0, val = 0; bool operator==(const Entry&) const = default; };
struct Pos {
	uint8_t term = 0, index = 0;
	auto operator<=>(const Pos&) const = default;
};
struct Answer { uint8_t present = 0, forg = 0, granted = 0; bool operator==(const Answer&) const = default; };
struct Node {
	uint8_t term = 0; int8_t voted = -1; uint8_t len = 0; Entry log[MAXLOG];
	uint8_t synced = 0, forg = 0, role = FOLLOWER, commit = 0, seen = 0; Pos smax; uint8_t tfired = 0;
	uint8_t has_match = 0, match[MAXN] = {}, smatch[MAXN] = {};
	uint8_t seenf = 0, cdur = 0, pcdur = 0, has_rnd = 0; Answer rnd[MAXN];
	Pos last() const { return len ? Pos{log[len - 1].term, len} : Pos{}; }
};
struct Acked { uint8_t index, term, val; auto operator<=>(const Acked&) const = default; };
struct State {
	Node nodes[MAXN];
	uint8_t namem = 0, nadur = 0; Acked amem[16], adur[16];
	uint8_t nprop = 0, nos = 0, nr = 0;
};

enum Restart { NO_RESTART, TEXT, PERSIST };
struct Config {
	Mode mode = NONE; int N = 3, F = 1, MAJ = 2, FMAX = 1, MAXOS = 2, MAXP = 1, MAXT = 2;
	bool compat = false, flags = false, timeout = false, rounds = false;
	uint8_t all = 7;
	Restart restart = NO_RESTART; int MAXR = 0; bool torn = false, retry = false, major = false;
	int nscript = 0; char op[MAXSCRIPT] = {}; int node[MAXSCRIPT] = {};
	uint8_t M[MAXSCRIPT + 1] = {};  // the membership after each change, as voter masks
	uint8_t cls[MAXN] = {};        // symmetry class: 0 unnamed, 1 + k named by change k
};
Config cfg;

int popcount(unsigned x) { return __builtin_popcount(x); }
int fof(uint8_t m) { return (popcount(m) - 1) / 2; }
int maj(uint8_t m) { return popcount(m) / 2 + 1; }

// The membership in force: the newest configuration entry in the synced prefix (Track C D1).
uint8_t members(const Node& n) {
	if (!cfg.nscript) { return cfg.M[0]; }
	for (int i = n.synced - 1; i >= 0; --i) { if (n.log[i].val >= CFG) { return cfg.M[n.log[i].val - CFG]; } }
	return cfg.M[0];
}

// Actions, kept compact on the DFS stack and formatted only for traces.
enum Kind : uint8_t { ELECT, FAIL_ELECT, PROBE, TIMEOUT_FIRE, WRITE, REPLICATE, COMMIT, SYNC, CRASH, ROUND_START, ASK, ASK_REFUSED, DECIDE,
	RESTART, TORN, ROUND_AGAIN, CHANGE, NKINDS };
struct Action { uint8_t kind, c, r, term, mask, a, b, extra; };
enum Check : uint8_t { CK_NONE = 0, CK_ELECT = 1, CK_FLOOR = 2, CK_FENCE = 3 };
struct Succ { Action act; State s; uint8_t check = CK_NONE, ok_mem = 1, ok_dur = 1; };

bool grant_ok(const Node& r, Pos cand) {
	if (!r.forg) { return true; }
	if (cfg.mode == NONE || cfg.mode == ROUND || cfg.mode == ROUND_HEARD) { return false; }
	bool heard = r.seen == cfg.all, allowed;
	if (cfg.mode == HEARD) { allowed = heard; }
	else if (cfg.mode == TIMEOUT) { allowed = heard || r.tfired; }
	else { allowed = popcount(r.seenf) >= cfg.F + 1 && (heard || (cfg.mode == FPLUS && r.tfired)); }
	return allowed && cand >= r.smax;
}
bool campaign_ok(const Node& n) { return !n.forg || grant_ok(n, n.last()); }

void add_writes(Acked* set, uint8_t& count, const Node& n, int upto) {
	for (int i = 0; i < upto; ++i) {
		if (n.log[i].val < 2 || n.log[i].val >= CFG) { continue; }
		Acked a{uint8_t(i + 1), n.log[i].term, n.log[i].val};
		bool found = false;
		for (int k = 0; k < count; ++k) { if (set[k] == a) { found = true; break; } }
		if (!found) {
			if (count >= 16) { std::fprintf(stderr, "acked set overflow\n"); std::exit(2); }
			set[count++] = a;
		}
	}
	std::sort(set, set + count);
}
bool holds(const Node& n, const Acked* set, int count) {
	for (int k = 0; k < count; ++k) {
		const auto& a = set[k];
		if (a.index > n.len || n.log[a.index - 1].term != a.term || n.log[a.index - 1].val != a.val) { return false; }
	}
	return true;
}
void clear_match(Node& n) { n.has_match = 0; std::memset(n.match, 0, sizeof n.match); std::memset(n.smatch, 0, sizeof n.smatch); }
void clear_rnd(Node& n) { n.has_rnd = 0; for (auto& a : n.rnd) { a = Answer{}; } }

void push_entry(Node& n, Entry e) {
	if (n.len >= MAXLOG) { std::fprintf(stderr, "log overflow\n"); std::exit(2); }
	n.log[n.len++] = e;
}
void truncate(Node& n, int len) { n.len = uint8_t(len); for (int k = len; k < MAXLOG; ++k) { n.log[k] = Entry{}; } }
void become_leader(Node& n, int c) {
	n.role = LEADER; clear_match(n); n.has_match = 1; n.match[c] = n.len;
}

void successors(const State& st, std::vector<Succ>& out) {
	out.clear();
	const int N = cfg.N;
	int forgetful_now = 0; for (int k = 0; k < N; ++k) { forgetful_now += st.nodes[k].forg; }
	for (int c = 0; c < N; ++c) {
		const Node& n = st.nodes[c];
		const uint8_t mc = members(n);
		const int F = fof(mc), MAJ = maj(mc);
		const bool voter = mc >> c & 1;
		// A normal election, won or failed.
		if (voter && n.role != LEADER && n.term < cfg.MAXT && campaign_ok(n) && !(cfg.rounds && n.forg)) {
			uint8_t T = n.term + 1;
			int others[MAXN], no = 0;
			for (int x = 0; x < N; ++x) { if (x != c && (mc >> x & 1)) { others[no++] = x; } }
			for (int mask = 1; mask < (1 << no); ++mask) {
				Succ s; s.s = st;
				Node cand = n; cand.term = T; cand.voted = int8_t(c); cand.role = CANDIDATE; clear_match(cand); clear_rnd(cand);
				Pos cpos = cand.last();
				int votes = 1; uint8_t reached = 0;
				for (int b = 0; b < no; ++b) {
					if (!(mask >> b & 1)) { continue; }
					int r = others[b]; reached |= uint8_t(1 << r);
					Node& rn = s.s.nodes[r];
					if (T > rn.term) { rn.term = T; rn.voted = -1; rn.role = FOLLOWER; clear_match(rn); clear_rnd(rn); }
					if (rn.forg) { rn.seen |= uint8_t(1 << c); rn.smax = std::max(rn.smax, cpos); }
					if (rn.term == T && (rn.voted == -1 || rn.voted == c) && cpos >= rn.last() && grant_ok(rn, cpos)) { rn.voted = int8_t(c); ++votes; }
				}
				if (votes >= MAJ) {
					s.ok_mem = holds(cand, st.amem, st.namem); s.ok_dur = holds(cand, st.adur, st.nadur);
					push_entry(cand, {T, NOOP});
					become_leader(cand, c);
					cand.smatch[c] = 0;
					if (!cfg.compat) { cand.forg = 0; cand.seen = 0; cand.smax = Pos{}; cand.tfired = 0; cand.seenf = 0; }
					s.s.nodes[c] = cand; s.check = CK_ELECT;
					s.act = {ELECT, uint8_t(c), 0, T, reached, 0, 0, 0};
					out.push_back(s);
				} else if (!cfg.compat) {
					s.s.nodes[c] = cand;
					s.act = {FAIL_ELECT, uint8_t(c), 0, T, reached, 0, 0, 0};
					out.push_back(s);
				}
			}
		}
		// Pre-vote probes, to the voters of the sender's membership.
		if (cfg.mode != NONE && n.role != LEADER && voter) {
			Pos pos = n.last();
			for (int r = 0; r < N; ++r) {
				if (!(mc >> r & 1)) { continue; }
				const Node& rn = st.nodes[r];
				if (r == c || !(rn.forg || (cfg.rounds && n.forg && rn.role != LEADER))) { continue; }
				uint8_t seen = rn.forg ? uint8_t(rn.seen | (1 << c)) : rn.seen; Pos smax = rn.forg ? std::max(rn.smax, pos) : rn.smax;
				uint8_t seenf = rn.seenf | uint8_t(cfg.flags && n.forg ? 1 << c : 0);
				if (seen != rn.seen || smax != rn.smax || seenf != rn.seenf) {
					Succ s; s.s = st; Node& x = s.s.nodes[r]; x.seen = seen; x.smax = smax; x.seenf = seenf;
					s.act = {PROBE, uint8_t(c), uint8_t(r), 0, 0, 0, 0, 0};
					out.push_back(s);
				}
			}
		}
		if (cfg.timeout && (n.forg || (cfg.rounds && popcount(n.seenf & mc) >= F + 1)) && !n.tfired) {
			Succ s; s.s = st; s.s.nodes[c].tfired = 1; s.act = {TIMEOUT_FIRE, uint8_t(c), 0, 0, 0, 0, 0, 0}; out.push_back(s);
		}
		if (n.role == LEADER && st.nprop < cfg.MAXP) {
			Succ s; s.s = st; Node& x = s.s.nodes[c];
			push_entry(x, {x.term, uint8_t(2 + st.nprop)}); x.match[c] = x.len; s.s.nprop = st.nprop + 1;
			s.act = {WRITE, uint8_t(c), 0, n.term, 0, st.nprop, 0, 0};
			out.push_back(s);
		}
		// The next configuration change (Track C D3), admitted on the durable cursor (D7).
		if (cfg.nscript && n.role == LEADER) {
			int newest = 0; bool above = false, own = false;
			for (int i = 0; i < n.len; ++i) {
				if (n.log[i].val >= CFG) { newest = n.log[i].val - CFG; if (i + 1 > n.cdur) { above = true; } }
				if (i < n.cdur && n.log[i].term == n.term) { own = true; }
			}
			int k = newest + 1;
			if (k <= cfg.nscript && !above && own) {
				int x = cfg.node[k - 1];
				if (cfg.op[k - 1] == 'r' || (n.match[x] >= n.commit && n.smatch[x] >= n.cdur)) {
					Succ s; s.s = st; Node& y = s.s.nodes[c];
					push_entry(y, {n.term, uint8_t(CFG + k)}); y.match[c] = y.len;
					s.act = {CHANGE, uint8_t(c), uint8_t(x), n.term, 0, uint8_t(k), 0, 0};
					out.push_back(s);
				}
			}
		}
		if (n.role == LEADER) {
			uint8_t rterm = 0;
			for (int i = 0; i < n.len; ++i) { if (n.log[i].val == REC) { rterm = std::max(rterm, n.log[i].term); } }
			for (int f = 0; f < N; ++f) {
				if (f == c || st.nodes[f].term > n.term) { continue; }
				const Node& fn = st.nodes[f];
				int i = 0;
				while (i < n.len && i < fn.len && fn.log[i] == n.log[i]) { ++i; }
				Succ s; s.s = st;
				Node fn2 = fn; int replaced_from = 0;
				if (i != n.len) {
					std::memcpy(fn2.log, n.log, sizeof n.log); fn2.len = n.len;
					if (i < fn.len) { replaced_from = i + 1; }
				}
				int base = fn.commit;
				if (cfg.compat) {
					if (replaced_from && replaced_from <= fn.commit) { s.check = CK_FENCE; }
				} else if (replaced_from) {
					if (replaced_from <= fn.cdur) { s.check = CK_FLOOR; }
					else if (replaced_from <= fn.commit) {
						int top = std::min<int>(fn.commit, fn.len);
						for (int k = replaced_from - 1; k < top; ++k) { if (fn.log[k].term >= rterm) { s.check = CK_FENCE; break; } }
						base = replaced_from - 1;
					}
				}
				int synced = std::min<int>(fn.synced, i);
				fn2.term = n.term; if (fn.term != n.term) { fn2.voted = -1; }
				fn2.role = FOLLOWER; fn2.synced = uint8_t(synced);
				fn2.commit = uint8_t(std::max(base, std::min<int>(n.commit, n.len)));
				fn2.forg = 0; fn2.seen = 0; fn2.smax = Pos{}; fn2.tfired = 0; clear_match(fn2); fn2.seenf = 0; clear_rnd(fn2);
				if (!cfg.compat) { fn2.cdur = uint8_t(std::max<int>(fn.cdur, std::min<int>(n.cdur, n.len))); }
				Node l2 = n; l2.match[f] = n.len; l2.smatch[f] = uint8_t(std::min<int>(synced, n.len));
				if (std::memcmp(&fn2, &fn, sizeof fn) == 0 && std::memcmp(&l2, &n, sizeof n) == 0) { continue; }
				s.s.nodes[f] = fn2; s.s.nodes[c] = l2;
				s.act = {REPLICATE, uint8_t(c), uint8_t(f), 0, 0, s.check, 0, 0};
				out.push_back(s);
			}
			// Commit, over the voters of the leader's membership in force.
			uint8_t m[MAXN], sm[MAXN];
			std::memcpy(m, n.match, sizeof m); std::memcpy(sm, n.smatch, sizeof sm);
			m[c] = n.len; sm[c] = n.synced;
			int newc = n.commit;
			for (int k = n.len; k > n.commit; --k) {
				int count = 0; for (int x = 0; x < N; ++x) { count += (mc >> x & 1) && m[x] >= k; }
				if (n.log[k - 1].term == n.term && count >= MAJ) { newc = k; break; }
			}
			int dur = 0;
			for (int k = n.len; k > 0; --k) {
				int count = 0; for (int x = 0; x < N; ++x) { count += (mc >> x & 1) && sm[x] >= k; }
				if (n.log[k - 1].term == n.term && count >= MAJ) { dur = k; break; }
			}
			Succ s; s.s = st;
			add_writes(s.s.amem, s.s.namem, n, newc); add_writes(s.s.adur, s.s.nadur, n, dur);
			int cdur = cfg.compat ? n.cdur : std::max<int>(n.cdur, dur);
			bool changed = newc != n.commit || s.s.namem != st.namem || s.s.nadur != st.nadur || cdur != n.cdur;
			if (!changed) {
				for (int k = 0; k < st.namem; ++k) { if (!(s.s.amem[k] == st.amem[k])) { changed = true; } }
				for (int k = 0; k < st.nadur; ++k) { if (!(s.s.adur[k] == st.adur[k])) { changed = true; } }
			}
			if (changed) {
				s.s.nodes[c].commit = uint8_t(newc); s.s.nodes[c].cdur = uint8_t(cdur);
				s.act = {COMMIT, uint8_t(c), 0, 0, 0, uint8_t(newc), uint8_t(dur), 0};
				out.push_back(s);
			}
		}
		{
			uint8_t pcdur = cfg.compat ? n.pcdur : n.cdur;
			if (n.synced < n.len || pcdur != n.pcdur) {
				Succ s; s.s = st; s.s.nodes[c].synced = n.len; s.s.nodes[c].pcdur = pcdur;
				s.act = {SYNC, uint8_t(c), 0, 0, 0, 0, 0, 0}; out.push_back(s);
			}
		}
		const int forgetful_after = forgetful_now - n.forg + 1;
		if (st.nos < cfg.MAXOS) {
			if (forgetful_after <= cfg.FMAX) {
				Succ s; s.s = st; Node& x = s.s.nodes[c];
				truncate(x, n.synced);
				uint8_t cdur = cfg.compat ? 0 : std::min(n.pcdur, x.len);
				x.forg = 1; x.role = FOLLOWER; x.commit = cdur; x.seen = uint8_t(1 << c); x.smax = x.last(); x.tfired = 0;
				clear_match(x); x.seenf = cfg.flags ? uint8_t(1 << c) : 0; x.cdur = cdur; clear_rnd(x);
				s.s.nos = st.nos + 1;
				s.act = {CRASH, uint8_t(c), 0, 0, 0, x.len, 0, 0};
				out.push_back(s);
			}
		}
		// A same-boot process restart, or a clean close and reboot (revision 2).
		if (cfg.restart && st.nr < cfg.MAXR) {
			{
				Succ s; s.s = st; Node& x = s.s.nodes[c];
				bool forg = cfg.restart == PERSIST ? n.forg : false;
				x.role = FOLLOWER; x.synced = n.len; x.pcdur = n.cdur; x.commit = std::min<uint8_t>(n.cdur, n.len); x.forg = forg;
				x.seen = forg ? uint8_t(1 << c) : 0; x.smax = forg ? n.last() : Pos{}; x.tfired = 0; clear_match(x);
				x.seenf = (forg && cfg.flags) ? uint8_t(1 << c) : 0; clear_rnd(x);
				s.s.nr = st.nr + 1;
				s.act = {RESTART, uint8_t(c), 0, 0, 0, uint8_t(forg), 0, 0};
				out.push_back(s);
			}
			if (cfg.torn && (n.forg || forgetful_after <= cfg.FMAX)) {
				for (int keep = n.synced; keep < n.len; ++keep) {
					Succ s; s.s = st; Node& x = s.s.nodes[c];
					truncate(x, keep);
					uint8_t cdur = std::min<uint8_t>(n.cdur, uint8_t(keep));
					x.role = FOLLOWER; x.synced = uint8_t(keep); x.pcdur = cdur; x.commit = cdur; x.forg = 1;
					x.seen = uint8_t(1 << c); x.smax = x.last(); x.tfired = 0; clear_match(x);
					x.seenf = cfg.flags ? uint8_t(1 << c) : 0; x.cdur = cdur; clear_rnd(x);
					s.s.nr = st.nr + 1;
					s.act = {TORN, uint8_t(c), 0, 0, 0, uint8_t(keep), 0, 0};
					out.push_back(s);
				}
			}
		}
		if (cfg.rounds) {
			if (voter && (n.role == FOLLOWER || n.role == CANDIDATE) && n.term < cfg.MAXT && popcount(n.seenf & mc) >= F + 1) {
				Succ s; s.s = st; Node& x = s.s.nodes[c];
				x.term = n.term + 1; x.voted = int8_t(c); x.role = RCAND; clear_match(x); clear_rnd(x);
				x.has_rnd = 1; x.rnd[c] = Answer{1, n.forg, 1};
				s.act = {ROUND_START, uint8_t(c), 0, x.term, 0, 0, 0, 0};
				out.push_back(s);
			}
			if (n.role == RCAND) {
				uint8_t U = n.term; Pos cpos = n.last();
				if (cfg.retry && voter && U < cfg.MAXT && popcount(n.seenf & mc) >= F + 1) {
					Succ s; s.s = st; Node& x = s.s.nodes[c];
					x.term = U + 1; x.voted = int8_t(c); clear_rnd(x); x.has_rnd = 1; x.rnd[c] = Answer{1, n.forg, 1};
					s.act = {ROUND_AGAIN, uint8_t(c), 0, x.term, 0, 0, 0, 0};
					out.push_back(s);
				}
				for (int r = 0; r < N; ++r) {
					if (r == c || !(mc >> r & 1) || n.rnd[r].present) { continue; }
					Succ s; s.s = st;
					Node rn = st.nodes[r];
					if (rn.term > U) {
						Node& x = s.s.nodes[c]; x.term = rn.term; x.voted = -1; x.role = FOLLOWER; clear_rnd(x);
						s.act = {ASK_REFUSED, uint8_t(c), uint8_t(r), rn.term, 0, 0, 0, 0};
						out.push_back(s); continue;
					}
					if (U > rn.term) { rn.term = U; rn.voted = -1; rn.role = FOLLOWER; clear_match(rn); clear_rnd(rn); }
					if (rn.forg) { rn.seen |= uint8_t(1 << c); rn.smax = std::max(rn.smax, cpos); }
					bool granted = (rn.voted == -1 || rn.voted == c) && cpos >= rn.last();
					if (granted) { rn.voted = int8_t(c); }
					s.s.nodes[r] = rn;
					s.s.nodes[c].rnd[r] = Answer{1, rn.forg, uint8_t(granted)};
					s.act = {ASK, uint8_t(c), uint8_t(r), 0, 0, uint8_t(granted), rn.forg, 0};
					out.push_back(s);
				}
				int answers = 0, grants = 0, forgetful = 0;
				for (int r = 0; r < N; ++r) {
					const auto& a = n.rnd[r];
					if (!a.present || !(mc >> r & 1)) { continue; }
					++answers; grants += a.granted; forgetful += a.forg;
				}
				bool heard = answers == popcount(mc);
				bool allowed = cfg.mode == ROUND_HEARD ? heard : heard || n.tfired;
				int need = cfg.major ? MAJ : F + 1;
				// Every answer a grant: a refusal ends the round.
				if (voter && grants == answers && grants >= MAJ && forgetful >= need && allowed) {
					Succ s; s.s = st; Node& x = s.s.nodes[c];
					s.ok_mem = holds(n, st.amem, st.namem); s.ok_dur = holds(n, st.adur, st.nadur);
					push_entry(x, {U, REC}); push_entry(x, {U, NOOP});
					clear_rnd(x); become_leader(x, c);
					x.forg = 0; x.seen = 0; x.smax = Pos{}; x.tfired = 0; x.seenf = 0;
					s.check = CK_ELECT;
					s.act = {DECIDE, uint8_t(c), 0, U, 0, uint8_t(grants), uint8_t(forgetful), 0};
					out.push_back(s);
				}
			}
		}
	}
}

std::string mask_list(uint8_t mask) {
	std::string s = "[";
	for (int i = 0; i < MAXN; ++i) { if (mask >> i & 1) { if (s.size() > 1) { s += ", "; } s += std::to_string(i); } }
	return s + "]";
}
std::string label(const Action& a) {
	char buf[160];
	switch (a.kind) {
	case ELECT: std::snprintf(buf, sizeof buf, "elect %d term %d votes from %s", a.c, a.term, mask_list(a.mask).c_str()); break;
	case FAIL_ELECT: std::snprintf(buf, sizeof buf, "fail-elect %d term %d reaching %s", a.c, a.term, mask_list(a.mask).c_str()); break;
	case PROBE: std::snprintf(buf, sizeof buf, "probe %d->%d", a.c, a.r); break;
	case TIMEOUT_FIRE: std::snprintf(buf, sizeof buf, "recovery_timeout %d", a.c); break;
	case WRITE: std::snprintf(buf, sizeof buf, "write v%d at %d term %d", a.a, a.c, a.term); break;
	case REPLICATE: std::snprintf(buf, sizeof buf, "replicate %d->%d%s", a.c, a.r, a.a == CK_FLOOR ? " FLOOR" : a.a == CK_FENCE ? " FENCE" : ""); break;
	case COMMIT: std::snprintf(buf, sizeof buf, "commit at %d: memory %d, durable %d", a.c, a.a, a.b); break;
	case SYNC: std::snprintf(buf, sizeof buf, "sync %d", a.c); break;
	case CRASH: std::snprintf(buf, sizeof buf, "os-crash %d (keeps %d synced entries)", a.c, a.a); break;
	case ROUND_START: std::snprintf(buf, sizeof buf, "round %d term %d", a.c, a.term); break;
	case ASK: std::snprintf(buf, sizeof buf, "ask %d->%d%s%s", a.c, a.r, a.a ? " granted" : "", a.b ? " forgetful" : ""); break;
	case ASK_REFUSED: std::snprintf(buf, sizeof buf, "ask %d->%d refused at term %d", a.c, a.r, a.term); break;
	case DECIDE: std::snprintf(buf, sizeof buf, "decide %d term %d (%d grants, %d forgetful)", a.c, a.term, a.a, a.b); break;
	case RESTART: std::snprintf(buf, sizeof buf, "process-restart %d%s", a.c, a.a ? " forgetful" : ""); break;
	case TORN: std::snprintf(buf, sizeof buf, "torn-restart %d (keeps %d entries)", a.c, a.a); break;
	case ROUND_AGAIN: std::snprintf(buf, sizeof buf, "round-again %d term %d", a.c, a.term); break;
	case CHANGE: std::snprintf(buf, sizeof buf, "%s %d at %d term %d", cfg.op[a.a - 1] == 'a' ? "add" : "remove", a.r, a.c, a.term); break;
	default: std::snprintf(buf, sizeof buf, "?"); break;
	}
	return buf;
}

// Canonical encoding.
void encode_node(std::string& out, const Node& n, const int* perm) {
	auto P = [&](int v) { return v < 0 ? 0xff : perm[v]; };
	auto Pm = [&](uint8_t m) { uint8_t r = 0; for (int i = 0; i < cfg.N; ++i) { if (m >> i & 1) { r |= uint8_t(1 << perm[i]); } } return r; };
	out.push_back(char(n.term)); out.push_back(char(P(n.voted))); out.push_back(char(n.len));
	for (int i = 0; i < n.len; ++i) { out.push_back(char(n.log[i].term)); out.push_back(char(n.log[i].val)); }
	out.push_back(char(n.synced)); out.push_back(char(n.forg)); out.push_back(char(n.role)); out.push_back(char(n.commit));
	out.push_back(char(Pm(n.seen))); out.push_back(char(n.smax.term)); out.push_back(char(n.smax.index)); out.push_back(char(n.tfired));
	out.push_back(char(Pm(n.seenf))); out.push_back(char(n.cdur)); out.push_back(char(n.pcdur));
	out.push_back(char(n.has_match));
	if (n.has_match) {
		uint8_t m[MAXN] = {}, sm[MAXN] = {};
		for (int i = 0; i < cfg.N; ++i) { m[perm[i]] = n.match[i]; sm[perm[i]] = n.smatch[i]; }
		for (int i = 0; i < cfg.N; ++i) { out.push_back(char(m[i])); out.push_back(char(sm[i])); }
	}
	out.push_back(char(n.has_rnd));
	if (n.has_rnd) {
		Answer rnd[MAXN];
		for (int i = 0; i < cfg.N; ++i) { rnd[perm[i]] = n.rnd[i]; }
		for (int i = 0; i < cfg.N; ++i) {
			out.push_back(char(rnd[i].present)); out.push_back(char(rnd[i].forg)); out.push_back(char(rnd[i].granted));
		}
	}
}
std::string encode(const State& st, const int* perm) {
	std::string out; out.reserve(320);
	int inv[MAXN];
	for (int i = 0; i < cfg.N; ++i) { inv[perm[i]] = i; }
	for (int pos = 0; pos < cfg.N; ++pos) { out.push_back(char(cfg.cls[inv[pos]])); encode_node(out, st.nodes[inv[pos]], perm); }
	out.push_back(char(st.namem)); for (int k = 0; k < st.namem; ++k) { out.push_back(char(st.amem[k].index)); out.push_back(char(st.amem[k].term)); out.push_back(char(st.amem[k].val)); }
	out.push_back(char(st.nadur)); for (int k = 0; k < st.nadur; ++k) { out.push_back(char(st.adur[k].index)); out.push_back(char(st.adur[k].term)); out.push_back(char(st.adur[k].val)); }
	out.push_back(char(st.nprop)); out.push_back(char(st.nos)); out.push_back(char(st.nr));
	return out;
}
bool symmetry = false;
std::string signature(const Node& n, int self) {
	std::string s;
	// The symmetry class first (nodes the script names are never relabeled into one another or
	// into unnamed ones), then fields that name no other node, plus counts of those that do.
	s.push_back(char(cfg.cls[self]));
	s.push_back(char(n.term)); s.push_back(char(n.voted == -1 ? 0 : n.voted == self ? 1 : 2)); s.push_back(char(n.len));
	for (int i = 0; i < n.len; ++i) { s.push_back(char(n.log[i].term)); s.push_back(char(n.log[i].val)); }
	s.push_back(char(n.synced)); s.push_back(char(n.forg)); s.push_back(char(n.role)); s.push_back(char(n.commit));
	s.push_back(char(popcount(n.seen))); s.push_back(char(n.seen >> self & 1)); s.push_back(char(n.smax.term)); s.push_back(char(n.smax.index));
	s.push_back(char(n.tfired)); s.push_back(char(popcount(n.seenf))); s.push_back(char(n.seenf >> self & 1));
	s.push_back(char(n.cdur)); s.push_back(char(n.pcdur)); s.push_back(char(n.has_match)); s.push_back(char(n.has_rnd));
	return s;
}
std::string canonical(const State& st) {
	int ident[MAXN] = {0, 1, 2, 3, 4, 5};
	if (!symmetry) { return encode(st, ident); }
	const int N = cfg.N;
	std::string sig[MAXN];
	int order[MAXN];
	for (int i = 0; i < N; ++i) { sig[i] = signature(st.nodes[i], i); order[i] = i; }
	std::sort(order, order + N, [&](int a, int b) { return sig[a] < sig[b]; });
	// Tie groups in sorted order; enumerate the orders within each group.
	std::string best; bool have = false;
	int cur[MAXN]; std::memcpy(cur, order, sizeof order);
	std::vector<std::pair<int, int>> groups;
	for (int i = 0; i < N;) { int j = i; while (j < N && sig[order[j]] == sig[order[i]]) { ++j; } groups.push_back({i, j}); i = j; }
	// Recursive enumeration over the groups' permutations.
	auto rec = [&](auto&& self, std::size_t g) -> void {
		if (g == groups.size()) {
			int perm[MAXN];
			for (int pos = 0; pos < N; ++pos) { perm[cur[pos]] = pos; }
			std::string e = encode(st, perm);
			if (!have || e < best) { best = std::move(e); have = true; }
			return;
		}
		auto [lo, hi] = groups[g];
		std::sort(cur + lo, cur + hi);
		do { self(self, g + 1); } while (std::next_permutation(cur + lo, cur + hi));
	};
	rec(rec, 0);
	return best;
}

struct Key { uint64_t a, b; bool operator==(const Key&) const = default; };
uint64_t mix64(uint64_t h) { h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33; return h; }
Key hash_bytes(std::string_view s) {
	uint64_t h1 = 0x9e3779b97f4a7c15ULL ^ s.size(), h2 = 0xc2b2ae3d27d4eb4fULL + s.size();
	std::size_t i = 0;
	for (; i + 8 <= s.size(); i += 8) {
		uint64_t w; std::memcpy(&w, s.data() + i, 8);
		h1 = mix64(h1 ^ w) * 0x9fb21c651e98df25ULL; h2 = (h2 + w) * 0xd6e8feb86659fd93ULL; h2 ^= h2 >> 29;
	}
	uint64_t w = 0; std::memcpy(&w, s.data() + i, s.size() - i);
	h1 = mix64(h1 ^ w ^ 0xa0761d6478bd642fULL); h2 = mix64(h2 + w + 0xe7037ed1a0b428dbULL);
	return {h1, h2};
}

// Open-addressing table: key -> the most remaining depth the state was expanded with.
struct Table {
	std::vector<Key> keys; std::vector<uint8_t> depth; std::size_t count = 0, mask = 0;
	explicit Table(std::size_t log2) : keys(std::size_t(1) << log2, Key{0, 0}), depth(std::size_t(1) << log2, 0), mask((std::size_t(1) << log2) - 1) {}
	// Returns true when the state must be expanded with `remaining` (new, or seen with less).
	bool visit(Key k, uint8_t remaining) {
		if (k.a == 0 && k.b == 0) { k.a = 1; }
		std::size_t i = k.a & mask;
		for (;;) {
			if (keys[i].a == 0 && keys[i].b == 0) {
				if (count * 10 >= keys.size() * 9) { std::fprintf(stderr, "table full at %zu states\n", count); std::exit(3); }
				keys[i] = k; depth[i] = uint8_t(remaining + 1); ++count; return true;
			}
			if (keys[i] == k) {
				if (depth[i] >= remaining + 1) { return false; }
				depth[i] = uint8_t(remaining + 1); return true;
			}
			i = (i + 1) & mask;
		}
	}
};

// The same with 64-bit keys (--key64): 9 bytes per slot, omitting a state with probability
// about n^2 / 2^65.
struct Table64 {
	std::vector<uint64_t> keys; std::vector<uint8_t> depth; std::size_t count = 0, mask = 0;
	explicit Table64(std::size_t log2) : keys(std::size_t(1) << log2, 0), depth(std::size_t(1) << log2, 0), mask((std::size_t(1) << log2) - 1) {}
	bool visit(Key k128, uint8_t remaining) {
		uint64_t k = k128.a ^ (k128.b * 0x9e3779b97f4a7c15ULL);
		if (k == 0) { k = 1; }
		std::size_t i = mix64(k) & mask;
		for (;;) {
			if (keys[i] == 0) {
				if (count * 10 >= keys.size() * 9) { std::fprintf(stderr, "table full at %zu states\n", count); std::exit(3); }
				keys[i] = k; depth[i] = uint8_t(remaining + 1); ++count; return true;
			}
			if (keys[i] == k) {
				if (depth[i] >= remaining + 1) { return false; }
				depth[i] = uint8_t(remaining + 1); return true;
			}
			i = (i + 1) & mask;
		}
	}
};

struct Found { bool any = false; std::vector<Action> trace; int depth = 0; };

bool parse_script(const std::string& text) {
	std::size_t at = 0;
	while (at < text.size()) {
		std::size_t comma = text.find(',', at);
		std::string item = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		std::size_t colon = item.find(':');
		if (colon == std::string::npos || cfg.nscript >= MAXSCRIPT) { return false; }
		std::string op = item.substr(0, colon);
		if (op != "add" && op != "remove") { return false; }
		cfg.op[cfg.nscript] = op[0]; cfg.node[cfg.nscript] = std::stoi(item.substr(colon + 1)); ++cfg.nscript;
		if (comma == std::string::npos) { break; }
		at = comma + 1;
	}
	return true;
}

}  // namespace

int main(int argc, char** argv) {
	int depth = 10, table_log2 = 26; std::string mode = "none"; int fmax = -1;
	uint64_t walks = 0, seed = 1; bool by_kind = false, key64 = false;
	std::string base_text, script_text, schedule_file; int from_step = -1;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&] { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(1); } return std::string(argv[++i]); };
		if (a == "--voters") { cfg.N = std::stoi(next()); }
		else if (a == "--fmax") { fmax = std::stoi(next()); }
		else if (a == "--maxos") { cfg.MAXOS = std::stoi(next()); }
		else if (a == "--maxp") { cfg.MAXP = std::stoi(next()); }
		else if (a == "--maxt") { cfg.MAXT = std::stoi(next()); }
		else if (a == "--depth") { depth = std::stoi(next()); }
		else if (a == "--table") { table_log2 = std::stoi(next()); }
		else if (a == "--compat") { cfg.compat = true; }
		else if (a == "--symmetry") { symmetry = true; }
		else if (a == "--walks") { walks = std::stoull(next()); }
		else if (a == "--seed") { seed = std::stoull(next()); }
		else if (a == "--by-kind") { by_kind = true; }
		else if (a == "--key64") { key64 = true; }
		else if (a == "--restart") { std::string v = next(); cfg.restart = v == "text" ? TEXT : v == "persist" ? PERSIST : NO_RESTART; if (!cfg.restart) { std::fprintf(stderr, "--restart text|persist\n"); return 1; } }
		else if (a == "--maxr") { cfg.MAXR = std::stoi(next()); }
		else if (a == "--torn") { cfg.torn = true; }
		else if (a == "--retry") { cfg.retry = true; }
		else if (a == "--base") { base_text = next(); }
		else if (a == "--script") { script_text = next(); }
		else if (a == "--count") { std::string v = next(); if (v != "f1" && v != "major") { std::fprintf(stderr, "--count f1|major\n"); return 1; } cfg.major = v == "major"; }
		else if (a == "--schedule") { schedule_file = next(); }
		else if (a == "--from-step") { from_step = std::stoi(next()); }
		else if (a[0] != '-') { mode = a; }
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
	}
	int m = -1; for (int k = 0; k < 7; ++k) { if (mode == mode_names[k]) { m = k; } }
	if (!parse_script(script_text)) { std::fprintf(stderr, "--script add:X,remove:Y\n"); return 1; }
	uint8_t base = 0;
	if (base_text.empty()) { base = uint8_t((1 << cfg.N) - 1); }
	else { std::size_t at = 0; while (at < base_text.size()) { std::size_t comma = base_text.find(',', at); base |= uint8_t(1 << std::stoi(base_text.substr(at, comma - at))); if (comma == std::string::npos) { break; } at = comma + 1; } }
	if (m < 0 || cfg.N < 3 || cfg.N > MAXN || (!cfg.nscript && popcount(base) % 2 == 0) || (cfg.nscript && m != ROUND && m != ROUND_HEARD)) {
		std::fprintf(stderr, "usage: forgetful_check MODE [--voters 3..6] ...; membership changes need a round mode\n"); return 1;
	}
	cfg.mode = Mode(m); cfg.M[0] = base; cfg.F = (popcount(base) - 1) / 2; cfg.MAJ = cfg.F + 1; cfg.all = base;
	// Symmetry classes: unnamed founding voters are interchangeable; a node a change names, and a
	// node outside every membership, are each their own.
	for (int x = 0; x < cfg.N; ++x) { if (!(base >> x & 1)) { cfg.cls[x] = uint8_t(1 + MAXSCRIPT); } }
	for (int k = 0; k < cfg.nscript; ++k) {
		cfg.M[k + 1] = cfg.op[k] == 'a' ? uint8_t(cfg.M[k] | (1 << cfg.node[k])) : uint8_t(cfg.M[k] & ~(1 << cfg.node[k]));
		cfg.cls[cfg.node[k]] = uint8_t(1 + k);
	}
	cfg.FMAX = fmax >= 0 ? fmax : cfg.F;
	cfg.flags = cfg.mode == FPLUS || cfg.mode == FPLUS_HEARD || cfg.mode == ROUND || cfg.mode == ROUND_HEARD;
	cfg.timeout = cfg.mode == TIMEOUT || cfg.mode == FPLUS || cfg.mode == ROUND;
	cfg.rounds = cfg.mode == ROUND || cfg.mode == ROUND_HEARD;
	if (cfg.restart && !cfg.MAXR) { cfg.MAXR = 1; }
	if (cfg.compat && (cfg.mode > TIMEOUT || cfg.restart || cfg.nscript)) { std::fprintf(stderr, "--compat covers none, heard, timeout\n"); return 1; }
	int fmin = cfg.F; for (int k = 1; k <= cfg.nscript; ++k) { fmin = std::min(fmin, fof(cfg.M[k])); }
	const bool beyond = cfg.FMAX > fmin;

	auto start = std::chrono::steady_clock::now();
	std::printf("mode=%s N=%d FMAX=%d MAXOS=%d MAXP=%d MAXT=%d%s%s depth %d", mode_names[m], cfg.N, cfg.FMAX, cfg.MAXOS, cfg.MAXP,
		cfg.MAXT, cfg.compat ? " compat" : "", symmetry ? " symmetry" : "", depth);
	if (cfg.restart) { std::printf(" restart=%s maxr=%d%s", cfg.restart == TEXT ? "text" : "persist", cfg.MAXR, cfg.torn ? " torn" : ""); }
	if (cfg.retry) { std::printf(" retry"); }
	if (cfg.nscript) { std::printf(" base=%s script=%s", mask_list(base).c_str(), script_text.c_str()); }
	if (cfg.rounds && (cfg.nscript || cfg.major)) { std::printf(" count=%s", cfg.major ? "major" : "f1"); }
	if (walks) { std::printf(", %llu swarm walks from seed %llu%s", (unsigned long long)walks, (unsigned long long)seed, by_kind ? ", kind first" : ""); }
	std::printf("\n");
	std::fflush(stdout);
	const char* kind_names[4] = {"memory", "quorum", "floor", "fence"};

	State init{};
	if (!schedule_file.empty()) {
		std::ifstream in(schedule_file);
		if (!in) { std::fprintf(stderr, "cannot read %s\n", schedule_file.c_str()); return 1; }
		std::vector<std::string> labels; std::string line;
		while (std::getline(in, line)) { if (!line.empty() && line[0] != '#') { labels.push_back(line); } }
		const int steps = from_step >= 0 ? std::min<int>(from_step, int(labels.size())) : int(labels.size());
		std::vector<Succ> succ; State st{}; std::vector<std::string> hit; int peak = 0;
		for (int i = 0; i < steps; ++i) {
			successors(st, succ);
			const Succ* chosen = nullptr;
			for (const auto& s2 : succ) { if (label(s2.act) == labels[i]) { chosen = &s2; break; } }
			if (!chosen) {
				std::printf("  step %d: '%s' is not enabled here\n", i + 1, labels[i].c_str());
				if (from_step < 0) { return 0; }
				return 5;
			}
			std::string tags;
			if (chosen->check == CK_ELECT) { if (!chosen->ok_mem) { tags += "memory"; } if (!chosen->ok_dur) { tags += tags.empty() ? "quorum" : ", quorum"; } }
			else if (chosen->check == CK_FLOOR) { tags = "floor"; }
			else if (chosen->check == CK_FENCE) { tags = "fence"; }
			if (!tags.empty()) { hit.push_back(tags); }
			st = chosen->s;
			int f = 0; for (int k = 0; k < cfg.N; ++k) { f += st.nodes[k].forg; } peak = std::max(peak, f);
			if (from_step < 0) { std::printf("  %2d. %s%s%s\n", i + 1, labels[i].c_str(), tags.empty() ? "" : "   <- ", tags.c_str()); }
		}
		if (from_step < 0) {
			std::string all; for (const auto& h : hit) { all += (all.empty() ? "" : ", ") + h; }
			std::printf("  result: %s; at most %d voters forgetful at once\n", all.empty() ? "no violation" : all.c_str(), peak);
			return 0;
		}
		std::printf("  searching from step %d of %s\n", steps, schedule_file.c_str());
		init = st;
	}
	if (walks) {
		// Swarm testing (Groce et al., ISSTA 2012): each walk enables a random subset of the
		// optional action kinds, then picks uniformly among the enabled successors.
		uint64_t x = seed * 0x9e3779b97f4a7c15ULL + 1;
		auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
		uint64_t steps = 0, hits[4] = {0, 0, 0, 0}; bool printed[4] = {false, false, false, false};
		std::vector<Succ> succ, pick; std::vector<Action> trace;
		for (uint64_t w = 0; w < walks; ++w) {
			// Elections and writes always; the rest each with probability 1/2 (FAIL_ELECT, PROBE,
			// TIMEOUT_FIRE, SYNC, CRASH, the round's three, restarts, torn tails, retried rounds,
			// configuration changes), REPLICATE and COMMIT with 7/8.
			uint32_t on = (1u << ELECT) | (1u << WRITE) | (1u << DECIDE);
			for (int k : {FAIL_ELECT, PROBE, TIMEOUT_FIRE, SYNC, CRASH, ROUND_START, ASK, ASK_REFUSED}) { if (rnd() & 1) { on |= 1u << k; } }
			if (cfg.restart) { for (int k : {RESTART, TORN}) { if (rnd() & 1) { on |= 1u << k; } } }
			if (cfg.retry && (rnd() & 1)) { on |= 1u << ROUND_AGAIN; }
			if (cfg.nscript && (rnd() & 1)) { on |= 1u << CHANGE; }
			for (int k : {REPLICATE, COMMIT}) { if (rnd() % 8) { on |= 1u << k; } }
			State st = init; trace.clear();
			for (int d = 0; d < depth; ++d) {
				successors(st, succ);
				pick.clear();
				for (auto& s2 : succ) { if (on >> s2.act.kind & 1) { pick.push_back(s2); } }
				if (pick.empty()) { break; }
				std::size_t at = rnd() % pick.size();
				if (by_kind) {
					// A random enabled kind, then a successor of that kind.
					uint32_t kinds = 0; for (auto& p2 : pick) { kinds |= 1u << p2.act.kind; }
					int nk = __builtin_popcount(kinds), want = int(rnd() % unsigned(nk)), kind = -1;
					for (int k = 0; k < 32; ++k) { if (kinds >> k & 1) { if (want-- == 0) { kind = k; break; } } }
					std::size_t count = 0; for (auto& p2 : pick) { count += p2.act.kind == kind; }
					std::size_t nth = rnd() % count;
					for (std::size_t i = 0; i < pick.size(); ++i) { if (pick[i].act.kind == kind && nth-- == 0) { at = i; break; } }
				}
				Succ& sc = pick[at];
				++steps; trace.push_back(sc.act);
				int kinds_hit[2] = {-1, -1};
				if (sc.check == CK_ELECT) { if (!sc.ok_mem) { kinds_hit[0] = 0; } if (!sc.ok_dur) { kinds_hit[1] = 1; } }
				else if (sc.check == CK_FLOOR) { kinds_hit[0] = 2; }
				else if (sc.check == CK_FENCE) { kinds_hit[0] = 3; }
				for (int k : kinds_hit) {
					if (k < 0) { continue; }
					++hits[k];
					if (!printed[k]) {
						printed[k] = true;
						bool allowed = beyond && (k == 0 || k == 3);
						std::printf("  %s (%s) in walk %llu, a trace of %zu steps:\n", allowed ? "expected beyond the model" : "VIOLATION", kind_names[k],
							(unsigned long long)w, trace.size());
						for (const auto& a : trace) { std::printf("    %s\n", label(a).c_str()); }
						std::fflush(stdout);
					}
				}
				st = sc.s;
			}
		}
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("  %llu walks, %llu steps (%.0f s); hits: memory %llu, quorum %llu, floor %llu, fence %llu\n",
			(unsigned long long)walks, (unsigned long long)steps, secs, (unsigned long long)hits[0], (unsigned long long)hits[1],
			(unsigned long long)hits[2], (unsigned long long)hits[3]);
		bool bad = hits[1] || hits[2] || (!beyond && (hits[0] || hits[3]));
		if (!bad) { std::printf("  no violation of the target%s\n", beyond ? " (beyond the model: no quorum or floor loss)" : ": no memory, quorum, floor, or fence"); }
		return bad ? 4 : 0;
	}
	// One table of the two, the other left at one slot.
	Table table{key64 ? 0 : std::size_t(table_log2)};
	Table64 table64{key64 ? std::size_t(table_log2) : 0};
	// Without symmetry the state's own bytes are its canonical encoding: every field is a byte,
	// with no padding, and unused log, match, round, and acknowledgement slots are kept zero.
	static_assert(alignof(State) == 1 && std::has_unique_object_representations_v<State>);
	auto visit = [&](const State& s2, uint8_t remaining) {
		Key k = symmetry ? hash_bytes(canonical(s2)) : hash_bytes(std::string_view(reinterpret_cast<const char*>(&s2), sizeof s2));
		return key64 ? table64.visit(k, remaining) : table.visit(k, remaining);
	};
	Found found[4];  // memory, quorum, floor, fence
	const char* const* kinds = kind_names;
	std::vector<uint64_t> per_depth(std::size_t(depth) + 1, 0);
	uint64_t expansions = 0, transitions = 0;

	struct Frame { State s; std::vector<Succ> succ; std::size_t next = 0; Action via{}; };
	std::vector<Frame> stack; stack.reserve(std::size_t(depth) + 2);
	visit(init, uint8_t(depth));
	stack.push_back(Frame{init, {}, 0, {}});
	successors(stack.back().s, stack.back().succ); ++expansions;
	while (!stack.empty()) {
		Frame& top = stack.back();
		if (top.next >= top.succ.size()) { stack.pop_back(); continue; }
		Succ& sc = top.succ[top.next++];
		++transitions;
		const int d = int(stack.size());  // depth of the successor
		auto record = [&](int k) {
			if (found[k].any && found[k].depth <= d) { return; }
			found[k].any = true; found[k].depth = d; found[k].trace.clear();
			for (std::size_t i = 1; i < stack.size(); ++i) { found[k].trace.push_back(stack[i].via); }
			found[k].trace.push_back(sc.act);
		};
		if (sc.check == CK_ELECT) { if (!sc.ok_mem) { record(0); } if (!sc.ok_dur) { record(1); } }
		else if (sc.check == CK_FLOOR) { record(2); }
		else if (sc.check == CK_FENCE) { record(3); }
		if (d > depth) { continue; }
		const uint8_t remaining = uint8_t(depth - d);
		if (!visit(sc.s, remaining)) { continue; }
		++per_depth[std::size_t(d)];
		if (remaining == 0) { continue; }
		Frame f{sc.s, {}, 0, sc.act};
		successors(f.s, f.succ); ++expansions;
		stack.push_back(std::move(f));
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::printf("  %zu states within depth %d (%llu expansions, %llu transitions, %.0f s)%s\n", key64 ? table64.count : table.count, depth,
		(unsigned long long)expansions, (unsigned long long)transitions, secs, key64 ? "; 64-bit keys" : "");
	bool bad = false;
	for (int k = 0; k < 4; ++k) {
		if (!found[k].any) { continue; }
		bool allowed = beyond && (k == 0 || k == 3);
		bad |= !allowed;
		std::printf("  %s (%s), a trace of %zu steps:\n", allowed ? "expected beyond the model" : "VIOLATION", kinds[k], found[k].trace.size());
		for (const auto& a : found[k].trace) { std::printf("    %s\n", label(a).c_str()); }
	}
	if (!bad) { std::printf("  no violation of the target%s\n", beyond ? " (beyond the model: no quorum or floor loss)" : ": no memory, quorum, floor, or fence"); }
	return bad ? 4 : 0;
}
