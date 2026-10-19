// T1 (durability-modes): durability calls to create one tiny index and commit it, step by
// step, through a counting IO over PosixIO; with syncspy loaded, the fsync and F_FULLFSYNC
// syscalls too.  count_syncs ROOT [INDEXES]
#include "completion/posix.h"
#include "counting_io.hh"
#include "strata/index.hh"

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

struct syscalls { unsigned long fsync = 0, fullfsync = 0, barrier = 0; };
using spy_fn = void (*)(unsigned long*, unsigned long*, unsigned long*);
spy_fn spy() { return reinterpret_cast<spy_fn>(::dlsym(RTLD_DEFAULT, "syncspy_counts")); }
syscalls now() {
	syscalls s;
	if (auto f = spy()) { f(&s.fsync, &s.fullfsync, &s.barrier); }
	return s;
}

strata::operation upsert(const std::string& id, const std::string& text) {
	strata::operation op;
	op.type = strata::operation::replace_key;
	op.key = "Q" + id;
	op.doc.terms.resize(1);
	op.doc.terms[0].term = op.key;
	op.doc.terms[0].wdf = 0;
	op.doc.text = text;
	op.doc.split_text = true;
	op.doc.stored = id;
	return op;
}

void row(const char* step, const probe::counts& a, const probe::counts& b, const syscalls& x, const syscalls& y) {
	std::printf("%-14s file_sync %llu  sync_data %llu  dir_sync %llu  sync_files %llu (%llu files)  owner_create %llu  | durability calls %llu",
		step, (unsigned long long)(b.file_sync - a.file_sync), (unsigned long long)(b.file_sync_data - a.file_sync_data),
		(unsigned long long)(b.dir_sync - a.dir_sync), (unsigned long long)(b.sync_files_calls - a.sync_files_calls),
		(unsigned long long)(b.sync_files_files - a.sync_files_files), (unsigned long long)(b.owner_create - a.owner_create),
		(unsigned long long)(b.durability_calls() - a.durability_calls()));
	if (spy()) { std::printf("  | syscalls fsync %lu  F_FULLFSYNC %lu", y.fsync - x.fsync, y.fullfsync - x.fullfsync); }
	std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: count_syncs ROOT [INDEXES]\n"); return 2; }
	const std::filesystem::path root = argv[1];
	const int indexes = argc > 2 ? std::atoi(argv[2]) : 1;
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	probe::counts total;
	const std::array<char, 16> uuid{'d', 'u', 'r', 'a', 'b', 'i', 'l', 'i', 't', 'y', '-', 'p', 'r', 'o', 'b', 'e'};
	syscalls s0 = now();
	for (int i = 0; i < indexes; ++i) {
		const auto dir = root / ("ix" + std::to_string(i));
		std::filesystem::create_directories(dir);
		std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
		kronuz::journal::PosixIO posix(dir.string());
		probe::counts c;
		probe::counting_io io(posix, c);
		strata::index::options o;
		o.key_prefix = "Q";
		const bool show = i == 0;
		probe::counts c0 = c; syscalls x0 = now();
		auto ix = strata::index::create(io, o, uuid);
		probe::counts c1 = c; syscalls x1 = now();
		if (show) { row("create", c0, c1, x0, x1); }
		strata::transaction t;
		t.ops.push_back(upsert("doc-" + std::to_string(i), "a tiny document"));
		if (ix.apply(t, {}).result != strata::verdict::accepted) { std::fprintf(stderr, "rejected\n"); return 1; }
		ix.commit();
		probe::counts c2 = c; syscalls x2 = now();
		if (show) { row("first commit", c1, c2, x1, x2); }
		strata::transaction u;
		u.ops.push_back(upsert("doc-b-" + std::to_string(i), "another tiny document"));
		if (ix.apply(u, {}).result != strata::verdict::accepted) { std::fprintf(stderr, "rejected\n"); return 1; }
		ix.commit();
		probe::counts c3 = c; syscalls x3 = now();
		if (show) {
			row("second commit", c2, c3, x2, x3);
			for (const auto& [k, n] : c3.syncs_by_kind) { std::printf("  file syncs of %-12s %llu\n", k.c_str(), (unsigned long long)n); }
			std::printf("  creates %llu, replaces %llu, removes %llu\n", (unsigned long long)c3.creates, (unsigned long long)c3.replaces, (unsigned long long)c3.removes);
		}
		total.file_sync += c3.file_sync; total.file_sync_data += c3.file_sync_data; total.dir_sync += c3.dir_sync;
		total.sync_files_calls += c3.sync_files_calls; total.sync_files_files += c3.sync_files_files; total.owner_create += c3.owner_create;
	}
	syscalls s1 = now();
	std::printf("%d indexes, create + two commits each: durability calls %llu (%.2f per index)", indexes,
		(unsigned long long)total.durability_calls(), double(total.durability_calls()) / indexes);
	if (spy()) { std::printf("; syscalls fsync %lu, F_FULLFSYNC %lu (%.2f and %.2f per index)", s1.fsync - s0.fsync, s1.fullfsync - s0.fullfsync, double(s1.fsync - s0.fsync) / indexes, double(s1.fullfsync - s0.fullfsync) / indexes); }
	std::printf("\n");
	return 0;
}
