// Reopening logged indexes at another loop count (rocksdb-parity D10 item 2, task 8): kv_strata's
// engine with four indexes creates at L = 1, then reopens at L = 3 and L = 2, with a power cut
// before each reopen. Every acknowledged write reads back with its value, every index finds its
// group, and an unacknowledged write is either gone or whole, and stays as the first recovery
// found it. Each re-partitioning reopen is also cut at every mutation, under power-loss seeds.
// The host and the indexes share one faultfs filesystem, so one cut orders them all.
// Usage: test_reopen_loops [--seeds N] (power-loss seeds per cut, default 2).
#include "kv_strata.hh"

#include "faultlab/faultfs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fl = kronuz::faultlab;

namespace {

namespace jr = kronuz::journal;

int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

// A directory as the names under its prefix in one shared faultfs; its owner locks are tokens.
class dir_view final : public jr::IO {
public:
	dir_view(fl::io& io, std::string prefix) : io_(io), prefix_(std::move(prefix)) {}
	jr::FilesystemClass directory_filesystem() const override { return io_.directory_filesystem(); }
	jr::DirectoryTrust directory_trust() const override { return io_.directory_trust(); }
	std::unique_ptr<jr::OwnerLock> acquire_owner(bool) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::OwnerLock> acquire_directory_owner() override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::OwnerLock> lock_owner(const jr::OwnerLockOptions&) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::File> open_existing(std::string_view n) override { return io_.open_existing(path(n)); }
	std::unique_ptr<jr::File> open_existing_read_only(std::string_view n) override { return io_.open_existing_read_only(path(n)); }
	std::unique_ptr<jr::File> open_reclaim_candidate_read_only(std::string_view n) override { return io_.open_reclaim_candidate_read_only(path(n)); }
	void open_existing_read_only_into(std::string_view n, jr::File& f) override { io_.open_existing_read_only_into(path(n), f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, jr::File& f) override { return io_.open_reclaim_candidate_read_only_into(path(n), f); }
	std::unique_ptr<jr::File> create_exclusive(std::string_view n) override { return io_.create_exclusive(path(n)); }
	void replace(std::string_view from, std::string_view to) override { io_.replace(path(from), path(to)); }
	void remove(std::string_view n) override { io_.remove(path(n)); }
	std::optional<jr::EntryFootprint> entry_footprint(std::string_view n) override { return io_.entry_footprint(path(n)); }
	void order_files_and_directory(std::span<jr::File* const> files) override { io_.order_files_and_directory(files); }
	void sync_directory() override { io_.sync_directory(); }
	std::unique_ptr<jr::DirectoryCursor> scan_directory() override {
		struct cursor final : jr::DirectoryCursor {
			std::vector<std::string> names;
			std::size_t at = 0;
			std::optional<std::string> next() override { return at < names.size() ? std::optional(names[at++]) : std::nullopt; }
		};
		auto c = std::make_unique<cursor>();
		for (auto all = io_.scan_directory(); auto n = all->next();) {
			if (n->starts_with(prefix_)) { c->names.push_back(n->substr(prefix_.size())); }
		}
		return c;
	}

private:
	std::string path(std::string_view n) const { return prefix_ + std::string(n); }
	fl::io& io_;
	std::string prefix_;
};

constexpr std::uint32_t loop_counts[] = {1, 3, 2};  // step j runs at loop_counts[j]
constexpr unsigned indexes = 4;

struct world {
	fl::filesystem fs;
	fl::io io{fs};
	strata_kv::io_maker maker() {
		return [this](const std::string& dir) -> std::shared_ptr<jr::IO> {
			std::string prefix = dir + "/";
			for (char& ch : prefix) { ch = ch == '/' ? '.' : ch; }
			return std::make_shared<dir_view>(io, prefix);
		};
	}
	std::unique_ptr<strata_kv> open(bool create, std::uint32_t loops) {
		kvbench::config c;
		c.dir = "kv";
		c.dur = kvbench::durability::sync;
		c.indexes = indexes;
		c.memory = 64 << 20;
		c.background = 0;
		return std::make_unique<strata_kv>(c, maker(), strata_kv::opening{create, loops});
	}
};

// What a reopen must read: every acknowledged key with its value, and unacknowledged keys that may
// be absent or hold their value. A recovery settles the unacknowledged ones as it found them.
struct model {
	std::map<std::string, std::string> acked;
	std::map<std::string, std::string> maybe;
	std::vector<std::uint64_t> groups;  // index i's group, from the create
};

std::string key_of(int step, int n) {
	char b[32];
	std::snprintf(b, sizeof b, "s%d-k%04d", step, n);
	return b;
}
std::string value_of(int step, int n, const char* what) { return std::string(what) + "-" + std::to_string(step) + "-" + std::to_string(n) + std::string(40, 'v'); }

// Reads every index whole and compares it with the model, then settles the unacknowledged keys.
bool verify(const strata_kv& e, model& m, std::uint32_t loops, const std::string& label) {
	bool ok = e.loops() == loops;
	if (!ok) { std::fprintf(stderr, "%s: %u loops, not %u\n", label.c_str(), e.loops(), loops); }
	std::map<std::string, std::string> got;
	for (unsigned i = 0; i < indexes; ++i) {
		if (e.group(i) != m.groups[i]) {
			std::fprintf(stderr, "%s: index %u in group %llu, created in %llu\n", label.c_str(), i, (unsigned long long)e.group(i), (unsigned long long)m.groups[i]);
			ok = false;
		}
		e.index(i).scan(std::string_view{}, [&](std::string_view k, std::string_view v, std::uint64_t) {
			if (kvbench::route_hash(k) % indexes != i) {
				std::fprintf(stderr, "%s: %.*s in index %u\n", label.c_str(), int(k.size()), k.data(), i);
				ok = false;
			}
			got.emplace(std::string(k), std::string(v));
			return true;
		});
	}
	for (const auto& [k, v] : m.acked) {
		const auto it = got.find(k);
		if (it == got.end() || it->second != v) {
			std::fprintf(stderr, "%s: acknowledged %s %s\n", label.c_str(), k.c_str(), it == got.end() ? "lost" : ("reads " + it->second).c_str());
			ok = false;
		}
	}
	for (const auto& [k, v] : got) {
		if (m.acked.contains(k)) { continue; }
		const auto it = m.maybe.find(k);
		if (it == m.maybe.end() || it->second != v) {
			std::fprintf(stderr, "%s: %s reads %s, never acknowledged or written\n", label.c_str(), k.c_str(), v.c_str());
			ok = false;
		}
	}
	for (const auto& [k, v] : m.maybe) {
		if (got.contains(k)) { m.acked[k] = v; }
	}
	m.maybe.clear();
	if (!ok) { ++failures; }
	return ok;
}

// Step j's writes: three acknowledged batches, which also overwrite and erase step j - 1's keys,
// then an unacknowledged tail of new keys (ack=none: each applied alone, never awaited).
void write_step(strata_kv& e, model& m, int j) {
	e.on_phase("acked", kvbench::durability::sync);
	for (int b = 0; b < 3; ++b) {
		std::map<std::string, std::optional<std::string>> batch;
		for (int n = 0; n < 8; ++n) {
			batch[key_of(j, b * 8 + n)] = value_of(j, b * 8 + n, "put");
			e.put(0, key_of(j, b * 8 + n), value_of(j, b * 8 + n, "put"));
		}
		if (j) {
			batch[key_of(j - 1, b)] = value_of(j, b, "again");
			e.put(0, key_of(j - 1, b), value_of(j, b, "again"));
			batch[key_of(j - 1, 8 + b)] = std::nullopt;
			e.erase(0, key_of(j - 1, 8 + b));
		}
		e.durable(0);
		for (const auto& [k, v] : batch) {
			if (v) {
				m.acked[k] = *v;
			} else {
				m.acked.erase(k);
			}
		}
	}
	e.on_phase("tail", kvbench::durability::wal);
	for (int n = 0; n < 4; ++n) {
		m.maybe[key_of(j, 100 + n)] = value_of(j, 100 + n, "tail");
		e.put(0, key_of(j, 100 + n), value_of(j, 100 + n, "tail"));
	}
}

// The power goes off with the engine running: nothing it does after reaches the disk, then the
// disk keeps what `seed` picks of its unsynced state.
void cut(world& w, std::unique_ptr<strata_kv>& e, std::uint64_t seed) {
	w.fs.power_off_after(0);
	e.reset();
	w.fs.crash(seed);
}

// Steps 0 to `upto` - 1 with a power cut after each: create at L = 1, then reopen at each next L,
// read everything, write. False when a reopen fails or reads wrong.
bool build(world& w, model& m, int upto, std::uint64_t seed, const std::string& label) {
	try {
		auto e = w.open(true, loop_counts[0]);
		for (unsigned i = 0; i < indexes; ++i) { m.groups.push_back(e->group(i)); }
		write_step(*e, m, 0);
		cut(w, e, seed);
		for (int j = 1; j < upto; ++j) {
			e = w.open(false, loop_counts[j]);
			if (!verify(*e, m, loop_counts[j], label + " step " + std::to_string(j))) { return false; }
			write_step(*e, m, j);
			cut(w, e, seed * 31 + std::uint64_t(j));
		}
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "%s: threw: %s\n", label.c_str(), ex.what());
		++failures;
		return false;
	}
	return true;
}

// L = 1 -> 3 -> 2, a power cut before each reopen, then a last reopen at 2.
void steps(std::uint64_t seed) {
	world w;
	model m;
	const std::string label = "steps seed " + std::to_string(seed);
	if (!build(w, m, 3, seed, label)) { return; }
	try {
		auto e = w.open(false, loop_counts[2]);
		verify(*e, m, loop_counts[2], label + " last");
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "%s: last reopen threw: %s\n", label.c_str(), ex.what());
		++failures;
	}
}

struct tally {
	std::size_t cases = 0, cut_open = 0;
};

// Step s's reopen at loop_counts[s], cut at its k-th mutation (none past its last), and an
// acknowledged batch after it; a recovery then reads every acknowledged write, writes again, and
// a second cut and recovery keep that too. Returns the reopen's mutations.
std::uint64_t cut_case(int s, std::optional<std::uint64_t> k, std::uint64_t seed, tally& t) {
	world w;
	model m;
	const std::string label = "step " + std::to_string(s) + " cut " + (k ? std::to_string(*k) : "none") + " seed " + std::to_string(seed);
	if (!build(w, m, s, seed, label)) { return 0; }
	const std::uint32_t loops = loop_counts[s];
	const std::uint64_t start = w.fs.mutations();
	bool opened = false;
	if (k) { w.fs.power_off_after(*k); }
	try {
		auto e = w.open(false, loops);
		opened = true;
		for (int n = 0; n < 8; ++n) { m.maybe[key_of(s, 200 + n)] = value_of(s, 200 + n, "cut"); }
		e->on_phase("acked", kvbench::durability::sync);
		for (int n = 0; n < 8; ++n) { e->put(0, key_of(s, 200 + n), value_of(s, 200 + n, "cut")); }
		e->durable(0);
		for (int n = 0; n < 8; ++n) {
			m.acked[key_of(s, 200 + n)] = m.maybe[key_of(s, 200 + n)];
			m.maybe.erase(key_of(s, 200 + n));
		}
	} catch (const std::exception&) {
	}
	const std::uint64_t used = w.fs.mutations() - start;
	t.cut_open += k && !opened;
	w.fs.crash(seed * 7919 + k.value_or(0));
	try {
		auto e = w.open(false, loops);
		if (!verify(*e, m, loops, label + " recovery")) { return used; }
		write_step(*e, m, s);
		cut(w, e, seed * 7919 + k.value_or(0) + 1);
		e = w.open(false, loops);
		verify(*e, m, loops, label + " second recovery");
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "%s: recovery threw: %s\n", label.c_str(), ex.what());
		++failures;
	}
	++t.cases;
	return used;
}

}  // namespace

int main(int argc, char** argv) {
	int seeds = 2;
	for (int i = 1; i < argc; ++i) {
		if (std::string_view(argv[i]) == "--seeds" && i + 1 < argc) { seeds = std::max(1, std::atoi(argv[++i])); }
	}
	for (int seed = 0; seed < seeds; ++seed) { steps(std::uint64_t(seed)); }
	for (int s = 1; s < 3; ++s) {
		tally t;
		const std::uint64_t total = cut_case(s, std::nullopt, 0, t);
		CHECK(total > 0);
		for (std::uint64_t k = 0; k <= total; ++k) {
			for (int seed = 0; seed < seeds; ++seed) { cut_case(s, k, std::uint64_t(seed) + 131 * k, t); }
		}
		CHECK(t.cut_open > 0);  // some cuts fell inside the re-partitioning open
		std::printf("reopen at L %u -> %u: %zu cases over %llu mutations, %zu cut inside the open\n", loop_counts[s - 1], loop_counts[s], t.cases, (unsigned long long)total,
			t.cut_open);
	}
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
