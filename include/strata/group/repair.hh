// strata::group's repair (quest group-heal A3; docs/mechanisms/group-install.md, Repair): a
// replica whose strata or set bytes are damaged fences itself locally and reinstalls its leader's
// image through install's path (consensus's Driver::reinstall). The marker `rd` beside the replica
// records the damage durably, so a crash anywhere from the damage to the activation reopens the
// replica damaged, never unfenced over damaged bytes; the install's activation removes it.
#pragma once

#include "strata/core/files.hh"
#include "strata/core/format.hh"
#include "strata/core/index.hh"
#include "strata/group/file_set.hh"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace strata::group {

// The replica's bytes are damaged: it refuses reads, writes and checkpoints while it reinstalls.
struct replica_damaged : std::runtime_error {
	using std::runtime_error::runtime_error;
};

namespace repair_detail {
inline constexpr std::string_view marker = "rd", marker_tmp = "rd.tmp";
inline std::unique_ptr<kronuz::journal::File> open_if(kronuz::journal::IO& io, std::string_view name) {
	try {
		return io.open_existing(name);
	} catch (const std::system_error& e) {
		if (e.code() == std::errc::no_such_file_or_directory) { return nullptr; }
		throw;
	}
}
}  // namespace repair_detail

// The errors that mean bytes the replica holds fail their checks: strata's (its manifest log, a
// segment) and the set's (a missing, short or, sealed, CRC32C-failing file).
inline bool damage(std::exception_ptr e) {
	try {
		std::rethrow_exception(e);
	} catch (const corrupt_index&) {
		return true;
	} catch (const corrupt_segment&) {
		return true;
	} catch (const file_set_damage&) {
		return true;
	} catch (const replica_damaged&) {
		return true;
	} catch (...) {}
	return false;
}

// True when the replica in `dir` is marked damaged.
inline bool marked_damaged(kronuz::journal::IO& dir) { return repair_detail::open_if(dir, repair_detail::marker) != nullptr; }

// Marks the replica damaged, durably (a temporary, a rename, the directory synced); `reason` is
// the marker's text. A replica already marked keeps its marker.
inline void mark_damaged(kronuz::journal::IO& dir, std::string_view reason) {
	namespace d = repair_detail;
	if (marked_damaged(dir)) { return; }
	{
		auto f = d::open_if(dir, d::marker_tmp);
		if (f) {
			f->truncate(0);
		} else {
			f = dir.create_exclusive(d::marker_tmp);
		}
		for (std::size_t done = 0; done < reason.size();) { done += f->write_at(done, reason.substr(done)); }
		f->sync();
	}
	dir.replace(d::marker_tmp, d::marker);
	dir.sync_directory();
}

// The replica holds its reinstalled bytes: the marker goes, durably.
inline void clear_damaged(kronuz::journal::IO& dir) {
	if (!marked_damaged(dir)) { return; }
	dir.remove(repair_detail::marker);
	dir.sync_directory();
}

// Every strata name in `dir` removed (a damaged index's), so a deferred index takes the
// reinstall; staging names (`is.`) are not strata's and stay.
inline void quarantine_strata(kronuz::journal::IO& dir) {
	std::vector<std::string> names;
	for (auto c = dir.scan_directory(); auto n = c->next();) {
		if (index_detail::parse_name(*n)) { names.push_back(*n); }
	}
	for (const auto& n : names) { dir.remove(n); }
	dir.sync_directory();
}

}  // namespace strata::group
