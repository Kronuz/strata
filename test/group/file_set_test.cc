// The file set shares its index's directory under its prefix (quest group-core A6, D111): strata's
// create, commits, retention, crash recovery and open leave the set's names and bytes alone, and
// the set's recovery deletes only names under its prefix, so the index reopens at its position.
// A group creates its index deferred (D112): nothing is written until the first commit, which
// alone makes the index durable, crash or not, beside the set's files. A sealed file recovers
// only against its CRC32C, and sealing and recovery read a file in bounded pieces.
#include "blob_app.hh"

#include "faultlab/faultfs.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>

namespace {

namespace fl = kronuz::faultlab;
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

// An IO over faultfs that records the largest read asked of any of its files: a seal or a
// recovery that reads a whole file in one call shows here (file-set-seal-streaming A2).
struct read_bound {
	std::size_t largest = 0;
};
class bounded_file final : public kronuz::journal::File {
public:
	bounded_file(std::unique_ptr<kronuz::journal::File> f, read_bound& r) : f_(std::move(f)), r_(&r) {}
	std::uint64_t size() override { return f_->size(); }
	std::size_t read_at(std::uint64_t o, std::span<char> b) override {
		r_->largest = std::max(r_->largest, b.size());
		return f_->read_at(o, b);
	}
	std::size_t write_at(std::uint64_t o, std::string_view b) override { return f_->write_at(o, b); }
	void truncate(std::uint64_t s) override { f_->truncate(s); }
	void sync() override { f_->sync(); }
	void sync_data() override { f_->sync_data(); }

private:
	std::unique_ptr<kronuz::journal::File> f_;
	read_bound* r_;
};
class bounded_io final : public kronuz::journal::IO {
public:
	bounded_io(fl::io& in, read_bound& r) : in_(&in), r_(&r) {}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool c) override { return in_->acquire_owner(c); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view n) override { return std::make_unique<bounded_file>(in_->open_existing(n), *r_); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view n) override { return std::make_unique<bounded_file>(in_->create_exclusive(n), *r_); }
	std::unique_ptr<kronuz::journal::File> open_existing_read_only(std::string_view n) override { return std::make_unique<bounded_file>(in_->open_existing_read_only(n), *r_); }
	void open_existing_read_only_into(std::string_view n, kronuz::journal::File& f) override { in_->open_existing_read_only_into(n, f); }
	std::unique_ptr<kronuz::journal::File> open_reclaim_candidate_read_only(std::string_view n) override { return in_->open_reclaim_candidate_read_only(n); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, kronuz::journal::File& f) override { return in_->open_reclaim_candidate_read_only_into(n, f); }
	void replace(std::string_view s, std::string_view d) override { in_->replace(s, d); }
	void remove(std::string_view n) override { in_->remove(n); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return in_->scan_directory(); }
	void sync_directory() override { in_->sync_directory(); }

private:
	fl::io* in_;
	read_bound* r_;
};

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
	fl::filesystem disk;
	fl::io io(disk);
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
	// A sealed file recovers against its CRC32C: intact it recovers, and one flipped byte at any
	// offset fails the recovery closed.
	for (std::uint64_t at = 0; at < 6; ++at) {
		fl::filesystem d;
		fl::io dio(d);
		try {
			sg::file_set files(dio);
			files.write("vol-0", 0, "sealed");
			files.seal("vol-0");
			files.write("vol-1", 0, "live");
			const sg::file_map named = files.capture();
			files.sync(named);
			check(named.at("vol-0").crc.has_value(), "a sealed file's entry names its CRC32C");
			sg::file_set intact(dio);
			intact.recover(named);
			check(intact.read("vol-0", 0, 6) == "sealed", "an intact sealed file recovers");
			auto f = dio.open_existing(std::string(sg::bundle_detail::prefix) + "vol-0");
			const std::string flipped(1, char("sealed"[at] ^ 0x01));
			f->write_at(at, flipped);
			f->sync();
			bool damaged = false;
			try {
				sg::file_set again(dio);
				again.recover(named);
			} catch (const sg::file_set_damage& e) {
				damaged = std::string(e.what()).find("CRC32C") != std::string::npos;
			}
			check(damaged, "a sealed file with byte " + std::to_string(at) + " flipped fails recovery on its CRC32C");
		} catch (const std::exception& e) {
			check(false, "sealed at " + std::to_string(at) + " threw: " + e.what());
		}
	}
	// Sealing streams: a file sixteen times the read bound, and an empty one, seal to their
	// whole-file CRC32C with no read past the bound, and recover against it the same way.
	{
		constexpr std::size_t bound = 64 * 1024;
		fl::filesystem d;
		fl::io inner(d);
		read_bound r;
		bounded_io dio(inner, r);
		try {
			std::string bytes(16 * bound + 7, '\0');
			for (std::size_t i = 0; i < bytes.size(); ++i) { bytes[i] = char(i * 131 + (i >> 9)); }
			sg::file_set files(dio);
			for (std::size_t at = 0; at < bytes.size(); at += 3 * bound) { files.write("vol-0", at, std::string_view(bytes).substr(at, 3 * bound)); }
			files.write("up-1", 0, "");
			files.seal("vol-0");
			files.seal("up-1");
			const sg::file_map named = files.capture();
			check(named.at("vol-0").length == bytes.size(), "the large file's whole length is written");
			check(named.at("vol-0").crc == kronuz::crc32c::crc32c(bytes), "a large sealed file's CRC32C is its whole bytes'");
			check(named.at("up-1").crc == kronuz::crc32c::crc32c(std::string_view()), "an empty sealed file's CRC32C is the empty bytes'");
			check(r.largest <= bound, "a seal reads at most " + std::to_string(bound) + " bytes at a time, read " + std::to_string(r.largest));
			files.sync(named);
			r.largest = 0;
			sg::file_set again(dio);
			again.recover(named);
			check(r.largest <= bound, "a recovery reads at most " + std::to_string(bound) + " bytes at a time, read " + std::to_string(r.largest));
		} catch (const std::exception& e) {
			check(false, std::string("sealing streams threw: ") + e.what());
		}
	}
	// A deferred create: no index before its first commit; after it, a power cut at any seed
	// leaves the index at that commit, the set's file beside it.
	for (std::uint64_t seed = 1; seed <= 64; ++seed) {
		fl::filesystem d;
		fl::io dio(d);
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
