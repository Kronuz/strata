// faultfs against its model: synced data and namespace survive a crash; unsynced data
// and namespace changes survive or not, per sector and per op, both outcomes reachable;
// faults fire before or after their effect; a failed sync drops unsynced bytes; capacity
// and short writes; stale handles; the owner locks.
#include "strata/core/faultfs.hh"

#include <cstdio>
#include <cstdlib>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using strata::faultfs::filesystem;
using strata::faultfs::io;
using strata::faultfs::op;

static int error_of(auto&& f) {
	try {
		f();
	} catch (const std::system_error& e) {
		return e.code().value();
	}
	return 0;
}

static bool exists(filesystem& fs, const std::string& name) {
	for (const auto& n : fs.list()) {
		if (n == name) { return true; }
	}
	return false;
}

int main() {
	// Synced data and a synced directory survive every crash.
	for (std::uint64_t seed = 0; seed < 50; ++seed) {
		filesystem fs;
		io d(fs);
		auto f = d.create_exclusive("a");
		CHECK(f->write_at(0, std::string(2000, 'x')) == 2000);
		f->sync();
		d.sync_directory();
		fs.crash(seed);
		CHECK(exists(fs, "a") && fs.view("a") == std::string(2000, 'x'));
	}
	// Unsynced bytes: per sector, old or new; past the durable size, new or zeros. Over
	// seeds, every outcome appears and nothing else does.
	{
		std::set<std::string> outcomes;
		for (std::uint64_t seed = 0; seed < 400; ++seed) {
			filesystem fs;
			io d(fs);
			auto f = d.create_exclusive("a");
			f->write_at(0, std::string(512, 'o'));
			f->sync();
			d.sync_directory();
			f->write_at(0, std::string(1024, 'n'));
			fs.crash(seed);
			const std::string v(fs.view("a"));
			CHECK(v.size() == 512 || v.size() == 1024);
			CHECK(v.substr(0, 512) == std::string(512, 'o') || v.substr(0, 512) == std::string(512, 'n'));
			if (v.size() == 1024) { CHECK(v.substr(512) == std::string(512, 'n') || v.substr(512) == std::string(512, '\0')); }
			outcomes.insert(v.size() == 512 ? std::string(1, v[0]) : std::string(1, v[0]) + (v[600] ? "n" : "0"));
		}
		CHECK(outcomes.size() == 6);  // at 512: o, n; at 1024: o or n, then n or zeros
	}
	// Unsynced creates may vanish; a synced directory keeps them.
	{
		bool kept = false, lost = false;
		for (std::uint64_t seed = 0; seed < 100; ++seed) {
			filesystem fs;
			io d(fs);
			d.create_exclusive("new")->sync();
			fs.crash(seed);
			(exists(fs, "new") ? kept : lost) = true;
		}
		CHECK(kept && lost);
	}
	// A rename is atomic: the destination holds the old file or the new one, never
	// neither, when both were durable.
	{
		bool old_seen = false, new_seen = false;
		for (std::uint64_t seed = 0; seed < 100; ++seed) {
			filesystem fs;
			io d(fs);
			auto a = d.create_exclusive("m");
			a->write_at(0, "old");
			a->sync();
			auto b = d.create_exclusive("m.tmp");
			b->write_at(0, "new");
			b->sync();
			d.sync_directory();
			d.replace("m.tmp", "m");
			fs.crash(seed);
			CHECK(exists(fs, "m"));
			const std::string v(fs.view("m"));
			CHECK(v == "old" || v == "new");
			(v == "old" ? old_seen : new_seen) = true;
			if (v == "new") { CHECK(!exists(fs, "m.tmp")); }
		}
		CHECK(old_seen && new_seen);
	}
	// Faults before and after their effect.
	{
		filesystem fs;
		io d(fs);
		fs.add_fault({op::create, 1, EIO, false});
		CHECK(error_of([&] { d.create_exclusive("x"); }) == EIO);
		CHECK(!exists(fs, "x"));
		fs.add_fault({op::create, 1, EIO, true});
		CHECK(error_of([&] { d.create_exclusive("y"); }) == EIO);
		CHECK(exists(fs, "y"));
		fs.clear_faults();
		auto f = d.open_existing("y");
		f->write_at(0, "data");
		fs.add_fault({op::sync, 1, EIO, false});
		CHECK(error_of([&] { f->sync(); }) == EIO);
		CHECK(f->size() == 0);  // the unsynced bytes are gone
		f->write_at(0, "again");
		f->sync();  // a later sync succeeds
		CHECK(fs.view("y") == "again");
		fs.add_fault({op::sync_directory, 1, EIO, false});
		CHECK(error_of([&] { d.sync_directory(); }) == EIO);
	}
	// A failed sync with `keep` (Linux's writeback error): its pages read as written but are clean
	// and not durable, so a later sync skips them unless a write or truncate dirties them again.
	for (std::uint64_t seed = 0; seed < 8; ++seed) {
		filesystem fs;
		io d(fs);
		auto f = d.create_exclusive("k");
		f->write_at(0, std::string(100, 'a'));
		f->sync();
		d.sync_directory();
		f->write_at(100, std::string(3 * 4096, 'b'));  // pages 0 to 3
		fs.add_fault({op::sync, 1, EIO, false, true});
		CHECK(error_of([&] { f->sync(); }) == EIO);
		CHECK(fs.view("k").size() == 100 + 3 * 4096);  // still cached
		f->write_at(3 * 4096 + 100, "c");  // dirties page 3 only
		f->sync();
		fs.crash(seed);
		const std::string after(fs.view("k"));
		CHECK(after.size() == 3 * 4096 + 101);
		CHECK(after.substr(0, 100) == std::string(100, 'a') && after.substr(100, 3 * 4096 - 100) == std::string(3 * 4096 - 100, '\0'));
		CHECK(after.substr(3 * 4096) == std::string(100, 'b') + "c");
		// A truncate below the lost pages drops them: the next sync writes what reads return.
		auto g = d.open_existing("k");
		g->write_at(0, std::string(8192, 'd'));
		fs.add_fault({op::sync, 1, EIO, false, true});
		CHECK(error_of([&] { g->sync(); }) == EIO);
		g->truncate(50);
		g->sync();
		fs.crash(seed);
		CHECK(fs.view("k") == std::string(50, 'd'));
	}
	// Capacity and short writes.
	{
		filesystem fs(1000);
		io d(fs);
		auto f = d.create_exclusive("c");
		CHECK(f->write_at(0, std::string(800, 'a')) == 800);
		CHECK(error_of([&] { f->write_at(800, std::string(300, 'b')); }) == ENOSPC);
		CHECK(f->write_at(100, std::string(300, 'b')) == 300);  // overwriting needs no space
		fs.short_writes(7);
		CHECK(f->write_at(0, std::string(100, 'z')) == 7);
	}
	// Stale handles and the owner lock.
	{
		filesystem fs;
		io d(fs);
		auto lock = d.acquire_owner(true);
		CHECK(error_of([&] { d.acquire_owner(false); }) == EAGAIN);
		auto f = d.create_exclusive("s");
		d.sync_directory();
		fs.crash(1);
		CHECK(error_of([&] { f->size(); }) == EIO);
		auto again = d.acquire_owner(false);  // the crash released it
		CHECK(error_of([&] { d.open_existing("missing"); }) == ENOENT);
		CHECK(error_of([&] { d.create_exclusive("s"); }) == EEXIST);
		d.remove("missing");  // idempotent
	}
	// The directory lock: writes nothing, busy while held, released by its lock and by a
	// crash, apart from owner.lock's.
	{
		filesystem fs;
		io d(fs);
		const auto before = fs.mutations();
		{
			auto lock = d.acquire_directory_owner();
			CHECK(error_of([&] { d.acquire_directory_owner(); }) == EWOULDBLOCK);
			CHECK(fs.list().empty() && fs.mutations() == before);
		}
		auto lock = d.acquire_directory_owner();
		fs.crash(2);
		auto again = d.acquire_directory_owner();  // the crash released it
	}
	// An ordering point: a rename made after order_files_and_directory survives a crash only
	// with every staged byte and name; both outcomes occur; the next sync_directory makes
	// the staged bytes durable.
	{
		int renamed = 0, lost = 0, kept_staged_only = 0;
		for (std::uint64_t seed = 0; seed < 400; ++seed) {
			filesystem fs;
			io d(fs);
			auto data = d.create_exclusive("data");
			CHECK(data->write_at(0, std::string(3000, 'd')) == 3000);
			auto tmp = d.create_exclusive("m.tmp");
			CHECK(tmp->write_at(0, std::string(700, 'm')) == 700);
			std::vector<kronuz::journal::File*> staged{data.get(), tmp.get()};
			d.order_files_and_directory(staged);
			d.replace("m.tmp", "m");
			fs.crash(seed);
			if (exists(fs, "m")) {
				++renamed;
				CHECK(exists(fs, "data") && fs.view("data") == std::string(3000, 'd'));
				CHECK(fs.view("m") == std::string(700, 'm'));
			} else if (exists(fs, "data")) {
				++kept_staged_only;
			} else {
				++lost;
			}
		}
		CHECK(renamed > 0 && lost > 0 && kept_staged_only > 0);
		for (std::uint64_t seed = 0; seed < 50; ++seed) {
			filesystem fs;
			io d(fs);
			auto data = d.create_exclusive("data");
			CHECK(data->write_at(0, std::string(3000, 'd')) == 3000);
			std::vector<kronuz::journal::File*> staged{data.get()};
			d.order_files_and_directory(staged);
			d.sync_directory();
			fs.crash(seed);
			CHECK(exists(fs, "data") && fs.view("data") == std::string(3000, 'd'));
		}
	}
	// The barrier's second phase: a file synced after the ordering point (the log's appended
	// record) makes every staged byte and name durable with it; a change after the point does not.
	for (std::uint64_t seed = 0; seed < 100; ++seed) {
		filesystem fs;
		io d(fs);
		auto log = d.create_exclusive("log");
		d.sync_directory();
		auto data = d.create_exclusive("data");
		CHECK(data->write_at(0, std::string(3000, 'd')) == 3000);
		std::vector<kronuz::journal::File*> staged{data.get()};
		d.order_files_and_directory(staged);
		auto later = d.create_exclusive("later");
		CHECK(log->write_at(0, std::string(100, 'e')) == 100);
		log->sync_data();
		fs.crash(seed);
		CHECK(fs.view("log") == std::string(100, 'e'));
		CHECK(exists(fs, "data") && fs.view("data") == std::string(3000, 'd'));
		(void)later;
	}
	// The same when the log's sync fails after its effect: its bytes and the staged ones are durable.
	for (std::uint64_t seed = 0; seed < 50; ++seed) {
		filesystem fs;
		io d(fs);
		auto log = d.create_exclusive("log");
		d.sync_directory();
		auto data = d.create_exclusive("data");
		CHECK(data->write_at(0, std::string(3000, 'd')) == 3000);
		std::vector<kronuz::journal::File*> staged{data.get()};
		d.order_files_and_directory(staged);
		CHECK(log->write_at(0, std::string(100, 'e')) == 100);
		fs.add_fault({op::sync, 1, EIO, true});
		CHECK(error_of([&] { log->sync_data(); }) == EIO);
		fs.crash(seed);
		CHECK(fs.view("log") == std::string(100, 'e'));
		CHECK(exists(fs, "data") && fs.view("data") == std::string(3000, 'd'));
	}
	// A write to a staged file after the ordering point is not ordered by it (CB-5): a crash keeps
	// each of its sectors or the staged one, and neither sync_directory nor another file's sync
	// makes it durable; only the file's own sync does.
	{
		// Two sectors, so the staged bytes alone (a quarter of the surviving crashes) are reachable.
		const std::string staged_bytes(1024, 'a'), later_bytes(1024, 'b');
		// 0: lost; else bit 1 for a staged sector, bit 2 for a later one.
		auto sectors = [&](filesystem& fs) {
			if (!exists(fs, "data")) { return 0; }
			const std::string v(fs.view("data"));
			if (v.empty()) { return 0; }
			CHECK(v.size() == 1024);
			int seen = 0;
			for (std::size_t at = 0; at + 512 <= v.size(); at += 512) {
				const std::string s = v.substr(at, 512);
				if (s == staged_bytes.substr(0, 512)) {
					seen |= 1;
				} else if (s == later_bytes.substr(0, 512)) {
					seen |= 2;
				} else {
					CHECK(s == std::string(512, '\0'));  // a sector the staged bytes never reached
					seen |= 4;
				}
			}
			return seen;
		};
		enum settle { none, directory, other_file, own };
		for (settle how : {none, directory, other_file, own}) {
			std::set<int> outcomes;
			for (std::uint64_t seed = 0; seed < 200; ++seed) {
				filesystem fs;
				io d(fs);
				auto log = d.create_exclusive("log");
				d.sync_directory();
				auto data = d.create_exclusive("data");
				CHECK(data->write_at(0, staged_bytes) == 1024);
				std::vector<kronuz::journal::File*> staged{data.get()};
				d.order_files_and_directory(staged);
				CHECK(data->write_at(0, later_bytes) == 1024);
				if (how == directory) { d.sync_directory(); }
				if (how == other_file) {
					CHECK(log->write_at(0, "e") == 1);
					log->sync();
				}
				if (how == own) { data->sync(); }
				fs.crash(seed);
				const int seen = sectors(fs);
				outcomes.insert(seen);
				if (how == own) { CHECK(seen == 2); }
				if (how == directory || how == other_file) { CHECK((seen & 3) && !(seen & 4)); }
			}
			if (how != own) {
				CHECK(outcomes.count(1) && outcomes.count(3));  // the staged bytes alone, and mixed with later ones
			}
		}
	}
	std::printf("faultfs: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
