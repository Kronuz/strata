// basic_index's file ledger (docs/mechanisms/index-layout.md): each file's bytes, counted once as
// on disk, in flight (its delete posted) or unlinked (removed, awaiting a directory sync).
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace strata::index_detail {

// The invariant, checked by consistent(): on_disk() is the sum of files()' sizes, in_flight() the
// sum of posted()'s, and no name is in both. Every transition below keeps it.
class file_ledger {
public:
	using sizes = std::map<std::string, std::uint64_t>;

	// A file on disk at `size`: a new name, or one on record grown or truncated.
	void track(const std::string& name, std::uint64_t size) {
		auto& at = files_[name];
		on_disk_ += size - at;
		at = size;
	}

	// A durable rename: the same bytes under the new name, replacing whatever it named.
	void rename(const std::string& from, const std::string& to) {
		const std::uint64_t size = size_of(from);
		files_.erase(from);
		on_disk_ -= size;
		track(to, size);
	}

	// A removed file: its bytes stay counted, as unlinked, until a directory sync.
	void forget(const std::string& name) {
		if (auto it = files_.find(name); it != files_.end()) {
			on_disk_ -= it->second;
			unlinked_ += it->second;
			files_.erase(it);
		}
	}

	// A delete posted: the file leaves the record and its bytes go in flight. Returns its size, 0
	// for a name not on record.
	std::uint64_t post(const std::string& name) {
		std::uint64_t size = 0;
		if (auto it = files_.find(name); it != files_.end()) {
			size = it->second;
			on_disk_ -= size;
			files_.erase(it);
		}
		in_flight_ += size;
		posted_[name] = size;
		return size;
	}

	// A posted delete that removed its file: in flight, then unlinked.
	void removed(const std::string& name) {
		if (auto it = posted_.find(name); it != posted_.end()) {
			in_flight_ -= it->second;
			unlinked_ += it->second;
			posted_.erase(it);
		}
	}

	// A posted delete undone: its bytes leave flight, and the caller tracks the file again at the
	// size left. False when `name` was not posted.
	bool unpost(const std::string& name) {
		const auto it = posted_.find(name);
		if (it == posted_.end()) { return false; }
		in_flight_ -= it->second;
		posted_.erase(it);
		return true;
	}

	// Removed bytes never on record (a failed merge's output), unlinked until the next sync.
	void add_unlinked(std::uint64_t bytes) { unlinked_ += bytes; }

	// A directory sync made every removal durable: the unlinked bytes are free.
	void synced() { unlinked_ = 0; }

	std::uint64_t size_of(const std::string& name) const {
		const auto it = files_.find(name);
		return it == files_.end() ? 0 : it->second;
	}
	const sizes& files() const noexcept { return files_; }
	const sizes& posted() const noexcept { return posted_; }
	std::uint64_t on_disk() const noexcept { return on_disk_; }
	std::uint64_t in_flight() const noexcept { return in_flight_; }
	std::uint64_t unlinked() const noexcept { return unlinked_; }
	// The bytes the filesystem still holds for the index's files.
	std::uint64_t counted() const noexcept { return on_disk_ + unlinked_ + in_flight_; }

	bool consistent() const {
		std::uint64_t disk = 0, flight = 0;
		for (const auto& [name, size] : files_) {
			if (posted_.count(name)) { return false; }
			disk += size;
		}
		for (const auto& [name, size] : posted_) { flight += size; }
		return disk == on_disk_ && flight == in_flight_;
	}

private:
	sizes files_;   // the files on disk and their sizes
	sizes posted_;  // the files whose delete is posted, and their sizes
	std::uint64_t on_disk_ = 0;
	std::uint64_t in_flight_ = 0;  // posted, not yet removed: the filesystem still holds them
	std::uint64_t unlinked_ = 0;   // removed, awaiting a directory sync to be freed
};

}  // namespace strata::index_detail
