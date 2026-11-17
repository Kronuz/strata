// The file set shares its index's directory under its prefix (quest group-core A6, D111): strata's
// create, commits, retention, crash recovery and open leave the set's names and bytes alone, and
// the set's recovery deletes only names under its prefix, so the index reopens at its position.
// A group creates its index deferred (D112): nothing is written until the first commit, which
// alone makes the index durable, crash or not, beside the set's files.
#include "blob_app.hh"

#include "strata/core/faultfs.hh"

#include <iostream>
#include <set>
#include <string>

namespace {

namespace ff = strata::faultfs;
namespace sg = strata::group;

int failures = 0;
void check(bool ok, const std::string& what) {
	if (!ok) {
		++failures;
		std::cerr << "FAIL: " << what << '\n';
	}
}

// The directory's names, those under the set's prefix or the rest.
std::set<std::string> names(kronuz::journal::IO& io, bool set) {
	std::set<std::string> out;
	for (auto cursor = io.scan_directory(); auto n = cursor->next();) {
		if (n->starts_with(sg::bundle_detail::prefix) == set) { out.insert(*n); }
	}
	return out;
}

blob::transaction put(std::uint64_t i) {
	blob::transaction t;
	blob::operation op;
	op.type = blob::operation::replace_key;
	op.key = op.doc.key = "k" + std::to_string(i);
	op.doc.stored = blob::locator(0, i, 1);
	t.ops.push_back(op);
	return t;
}

}  // namespace

int main() {
	ff::filesystem disk;
	ff::io io(disk);
	strata::byte_budget storage{std::uint64_t(1) << 30};
	strata::basic_index_options<strata::document_layout> o;
	o.keyed = true;
	o.storage = &storage;
	try {
		// The set's files first: a create over them succeeds, as over an empty directory.
		sg::file_set files(io);
		files.write("vol-0", 0, "hello");
		files.write("up-1", 0, "xy");
		files.seal("up-1");
		files.sync(files.capture());
		const auto set_names = names(io, true);
		check(set_names == std::set<std::string>{"app.up-1", "app.vol-0"}, "the set's names carry its prefix");

		// Commits enough to roll strata's log over and retire its old files, then a crash and an open.
		std::uint64_t last = 0;
		{
			auto ix = blob::index_type::create(io, o, {'f', 's', 'e', 't'});
			for (last = 1; last <= 40; ++last) {
				ix.apply(put(last), strata::files::position{last, 1});
				ix.commit();
				if (last % 10 == 0) { (void)ix.reclaim(); }
			}
			--last;
			ix.close();
		}
		disk.crash(7);
		{
			auto ix = blob::index_type::open(io, o);
			check(ix.applied() == strata::files::position{last, 1}, "strata reopens at its last commit");
			ix.close();
		}
		check(names(io, true) == set_names, "strata's create, commits, retention and open leave the set's names alone");
		check(files.read("vol-0", 0, 5) == "hello" && files.read("up-1", 0, 2) == "xy", "and their bytes");
		const auto strata_names = names(io, false);
		check(!strata_names.empty(), "strata's names are in the directory");

		// The set's recovery to a bundle naming vol-0 at 3 bytes: up-1 goes, strata's names stay.
		sg::file_set again(io);
		sg::file_map named;
		named["vol-0"] = sg::file_entry{3, std::nullopt};
		again.recover(named);
		check(names(io, true) == std::set<std::string>{"app.vol-0"}, "the set's recovery deletes the names under its prefix it does not name");
		check(again.read("vol-0", 0, 3) == "hel", "and truncates the named ones");
		check(names(io, false) == strata_names, "the set's recovery leaves strata's names alone");
		auto ix = blob::index_type::open(io, o);
		check(ix.applied() == strata::files::position{last, 1}, "strata reopens at its position after the set's recovery");
		ix.close();
	} catch (const std::exception& e) {
		check(false, std::string("threw: ") + e.what());
	}
	// A deferred create: no index before its first commit; after it, a power cut at any seed
	// leaves the index at that commit, the set's file beside it.
	for (std::uint64_t seed = 1; seed <= 64; ++seed) {
		ff::filesystem d;
		ff::io dio(d);
		try {
			sg::file_set files(dio);
			files.write("vol-0", 0, "abc");
			{
				auto ix = blob::index_type::create_deferred(dio, o, {'d', 'e', 'f', 'r'});
				ix.apply(put(1), strata::files::position{1, 1});
				ix.apply(put(2), strata::files::position{2, 1});
				check(names(dio, false).empty(), "a deferred create writes nothing before its first commit");
				files.sync(files.capture());
				ix.commit();
				d.power_off_after(0);
			}
			d.crash(seed);
			auto ix = blob::index_type::open(dio, o);
			check(ix.applied() == strata::files::position{2, 1}, "a deferred create's first commit is durable (seed " + std::to_string(seed) + ")");
			check(files.read("vol-0", 0, 3) == "abc", "the set's file beside it");
			ix.close();
		} catch (const std::exception& e) {
			check(false, "seed " + std::to_string(seed) + " threw: " + e.what());
		}
	}
	std::cout << "file set beside strata: " << (failures ? "FAIL" : "ok") << '\n';
	return failures ? 1 : 0;
}
