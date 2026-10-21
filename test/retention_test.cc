// What manifest retention buys (explore/retention, docs/perf/retention.md): for each policy
// (keep 0, 1, 2 predecessors; trim once settled), what open recovers under power loss at every
// barrier of a merge commit and under damage to the current manifest, the newest segment, and
// a predecessor's segment. An outcome is: the head opens with everything acknowledged (head),
// an older manifest opens (fallback) with everything or with documents missing (lost), or no
// manifest opens (refused: the host needs a replica or a repair).
#include "kv_test.hh"

#include <algorithm>
#include <random>
#include <set>

static const auto uuid = uuid_of("retention");

struct plan {
	std::size_t preds;
	bool trim;
	const char* name;
};
static const plan plans[] = {{0, false, "keep 0"}, {1, false, "keep 1"}, {2, false, "keep 2"}, {2, true, "trim settled"}};

static kv_index::options options_of(const plan& p) {
	kv_index::options o;
	o.key_prefix = "Q";
	o.retained_predecessors = p.preds;
	o.trim_settled = p.trim;
	return o;
}

using doc_set = std::set<std::string>;
static doc_set live_set(const kv_index& ix) {
	doc_set out;
	for (const auto& [docid, vs] : contents(ix)) { out.insert(vs.second); }
	return out;
}

// A batch of 30 operations: new keys, replaced keys, and deletes; each body is unique.
static std::uint64_t counter = 0;
static transaction batch(std::mt19937_64& rng, std::uint64_t& next_key) {
	std::vector<operation> ops;
	for (int i = 0; i < 30; ++i) {
		std::string body = "b" + std::to_string(++counter) + ":";
		for (int j = 0; j < 40; ++j) { body += "0123456789abcdef"[rng() & 15]; }
		const std::uint64_t r = rng() % 10;
		if (r < 6 || next_key < 20) {
			ops.push_back(add("Q" + std::to_string(next_key++), body));
		} else if (r < 9) {
			ops.push_back(replace("Q" + std::to_string(rng() % next_key), body));
		} else {
			ops.push_back(erase("Q" + std::to_string(rng() % next_key)));
		}
	}
	return tx(ops);
}

// Five flush commits, the merge of all of them, then `later` flush commits. The hook runs
// between the merge and its commit. Returns the acknowledged documents.
struct built_t {
	doc_set acked;
	std::uint64_t head = 0;
};
static built_t build_ix(faultfs::io& io, const plan& p, int later, const std::function<void(kv_index&)>& between = {}) {
	counter = 0;
	std::mt19937_64 rng(12345);
	std::uint64_t next_key = 0;
	auto ix = kv_index::create(io, options_of(p), uuid);
	for (int i = 0; i < 5; ++i) {
		ix.apply(batch(rng, next_key), {});
		ix.commit();
	}
	ix.force_merge(1);
	if (between) { between(ix); }
	ix.commit();
	for (int i = 0; i < later; ++i) {
		ix.apply(batch(rng, next_key), {});
		ix.commit();
	}
	return built_t{live_set(ix), ix.manifest().generation};
}

enum kind { full, lost, refused };
struct result {
	kind k = refused;
	std::size_t missing = 0;
	bool fell_back = false;
};

// The host's recovery: open the head and verify everything; when that fails, the older
// manifests newest first, each verified, until one opens.
static result recover(faultfs::filesystem& fs, faultfs::io& io, const plan& p, const doc_set& acked, const doc_set* alt = nullptr) {
	auto verify = [&](kv_index& ix) -> std::optional<doc_set> {
		try {
			(void)ix.check();
			return live_set(ix);
		} catch (const std::exception&) {
		}
		try {
			if (!ix.heal().empty()) { return std::nullopt; }
			(void)ix.check();
			return live_set(ix);
		} catch (const std::exception&) {
			return std::nullopt;
		}
	};
	auto judge = [&](const doc_set& got, bool fb) {
		result r;
		r.fell_back = fb;
		auto missing = [&](const doc_set& want) {
			std::size_t n = 0;
			for (const auto& d : want) { n += !got.count(d); }
			return n;
		};
		r.missing = missing(acked);
		if (alt) { r.missing = std::min(r.missing, missing(*alt)); }
		r.k = r.missing ? lost : full;
		return r;
	};
	std::vector<std::uint64_t> manifests;
	for (const auto& n : fs.list()) {
		if (auto q = index_detail::parse_name(n); q && q->type == index_detail::parsed_name::manifest) { manifests.push_back(q->a); }
	}
	std::sort(manifests.rbegin(), manifests.rend());
	for (std::size_t i = 0; i < manifests.size(); ++i) {
		try {
			auto ix = i == 0 ? kv_index::open(io, options_of(p)) : kv_index::open(io, options_of(p), manifests[i]);
			if (auto got = verify(ix)) { return judge(*got, i != 0); }
		} catch (const std::exception&) {
		}
	}
	return {};
}

struct tally {
	std::size_t n = 0, head = 0, fb_full = 0, fb_lost = 0, refuse = 0, missing = 0;
	void add(const result& r) {
		++n;
		if (r.k == refused) {
			++refuse;
		} else if (!r.fell_back) {
			r.k == full ? ++head : ++fb_lost;  // a head that opens with documents missing
		} else if (r.k == full) {
			++fb_full;
		} else {
			++fb_lost;
		}
		missing += r.missing;
	}
};
static void row(const std::string& scenario, const plan& p, const tally& t) {
	if (!t.n) {
		std::printf("| %s | %s | n/a | | | | | |\n", scenario.c_str(), p.name);
		return;
	}
	auto pct = [&](std::size_t x) { return 100.0 * double(x) / double(t.n); };
	std::printf("| %s | %s | %zu | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f |\n", scenario.c_str(), p.name, t.n, pct(t.head), pct(t.fb_full), pct(t.fb_lost), pct(t.refuse),
		t.fb_lost ? double(t.missing) / double(t.fb_lost) : 0.0);
	std::fflush(stdout);
}

static std::string newest_segment(const kv_index& ix) {
	std::uint64_t id = 0;
	for (const auto& s : ix.manifest().segments) { id = std::max(id, s.id); }
	return index_detail::segment_name(id);
}
static void flip(faultfs::filesystem& fs, const std::string& name, std::mt19937_64& rng) {
	const std::uint64_t size = fs.view(name).size();
	const std::uint64_t at = rng() % size;
	fs.add_media_fault(name, faultfs::media::bit_flip, at, at + 1, 0);
}

int main() {
	int seeds = 150;
	if (const char* e = std::getenv("RETENTION_SEEDS")) { seeds = std::atoi(e); }
	std::printf("| scenario | policy | runs | head opens | fallback, all data | fallback, data lost | refused | docs lost per loss |\n|---|---|---|---|---|---|---|---|\n");
	for (const auto& p : plans) {
		// A. Power loss at every mutation of a merge commit (new documents applied before it).
		{
			std::uint64_t total = 0;
			doc_set before, after;
			{
				faultfs::filesystem fs;
				faultfs::io io(fs);
				counter = 0;
				std::mt19937_64 rng(12345);
				std::uint64_t next_key = 0;
				auto ix = kv_index::create(io, options_of(p), uuid);
				for (int i = 0; i < 5; ++i) {
					ix.apply(batch(rng, next_key), {});
					ix.commit();
				}
				before = live_set(ix);
				const auto base = fs.mutations();
				ix.apply(batch(rng, next_key), {});
				ix.force_merge(1);
				ix.commit();
				total = fs.mutations() - base;
				after = live_set(ix);
			}
			tally t;
			for (std::uint64_t at = 0; at <= total; ++at) {
				for (int seed = 0; seed < 12; ++seed) {
					faultfs::filesystem fs;
					faultfs::io io(fs);
					counter = 0;
					std::mt19937_64 rng(12345);
					std::uint64_t next_key = 0;
					{
						auto ix = kv_index::create(io, options_of(p), uuid);
						for (int i = 0; i < 5; ++i) {
							ix.apply(batch(rng, next_key), {});
							ix.commit();
						}
						fs.power_off_after(at);
						try {
							ix.apply(batch(rng, next_key), {});
							ix.force_merge(1);
							ix.commit();
						} catch (const std::exception&) {
						}
					}
					fs.crash(at * 7919 + seed);
					t.add(recover(fs, io, p, before, &after));
				}
			}
			row("A power loss at each of " + std::to_string(total + 1) + " mutations of a merge commit", p, t);
		}
		for (int later : {0, 2}) {
			const std::string tag = later ? " (+2 commits)" : " (merge is head)";
			// B. The current manifest flipped or torn.
			for (int torn = 0; torn < 2; ++torn) {
				tally t;
				for (int seed = 0; seed < seeds; ++seed) {
					faultfs::filesystem fs;
					faultfs::io io(fs);
					const auto b = build_ix(io, p, later);
					std::mt19937_64 rng(seed * 2654435761u + 1);
					const std::string name = index_detail::manifest_name(b.head);
					if (torn) {
						auto f = io.open_existing(name);
						f->truncate(rng() % fs.view(name).size());
					} else {
						flip(fs, name, rng);
					}
					t.add(recover(fs, io, p, b.acked));
				}
				row(std::string(torn ? "B current manifest torn" : "B current manifest bit-flipped") + tag, p, t);
			}
			// C. The newest segment flipped after its commit.
			{
				tally t;
				for (int seed = 0; seed < seeds; ++seed) {
					faultfs::filesystem fs;
					faultfs::io io(fs);
					const auto b = build_ix(io, p, later);
					std::mt19937_64 rng(seed * 2654435761u + 7);
					std::string name;
					{
						auto ix = kv_index::open(io, options_of(p), std::nullopt);
						name = newest_segment(ix);
					}
					flip(fs, name, rng);
					t.add(recover(fs, io, p, b.acked));
				}
				row("C newest segment bit-flipped" + tag, p, t);
			}
			// D. A segment only a predecessor names flipped; the head intact (D1), and with the
			// newest segment flipped too (D2).
			for (int both = 0; both < 2; ++both) {
				tally t;
				for (int seed = 0; seed < seeds; ++seed) {
					faultfs::filesystem fs;
					faultfs::io io(fs);
					const auto b = build_ix(io, p, later);
					std::mt19937_64 rng(seed * 2654435761u + 13);
					std::set<std::string> head_names;
					std::string newest;
					{
						auto ix = kv_index::open(io, options_of(p), std::nullopt);
						for (const auto& s : ix.manifest().segments) { head_names.insert(index_detail::segment_name(s.id)); }
						newest = newest_segment(ix);
					}
					std::vector<std::string> only_pred;
					for (const auto& n : fs.list()) {
						if (auto q = index_detail::parse_name(n); q && q->type == index_detail::parsed_name::segment && !head_names.count(n)) { only_pred.push_back(n); }
					}
					if (only_pred.empty()) { break; }
					flip(fs, only_pred[rng() % only_pred.size()], rng);
					if (both) { flip(fs, newest, rng); }
					t.add(recover(fs, io, p, b.acked));
				}
				row(std::string(both ? "D2 predecessor's segment and newest segment flipped" : "D1 predecessor's segment flipped") + tag, p, t);
			}
		}
		// E. Damage present when the merge commits (a lost or misdirected write the read-back
		// meets): the merged segment flipped before the commit.
		{
			tally t;
			for (int seed = 0; seed < seeds; ++seed) {
				faultfs::filesystem fs;
				faultfs::io io(fs);
				std::mt19937_64 rng(seed * 2654435761u + 31);
				const auto b = build_ix(io, p, 0, [&](kv_index&) {
					std::string out;
					std::uint64_t best = 0;
					for (const auto& n : fs.list()) {
						if (auto q = index_detail::parse_name(n); q && q->type == index_detail::parsed_name::segment && q->a > best) {
							best = q->a;
							out = n;
						}
					}
					flip(fs, out, rng);
				});
				t.add(recover(fs, io, p, b.acked));
			}
			row("E merged segment damaged as it commits", p, t);
		}
	}
	std::printf("retention: %s\n", failures ? "FAILED" : "done");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
