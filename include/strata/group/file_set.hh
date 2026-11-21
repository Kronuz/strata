// A group's application file set (cluster-release D12, D16): the files apply writes beside
// strata, in the index's directory under the prefix `app.` (strata reads and removes only its
// own names). Apply writes positionally, rotates and retires; it never unlinks. A checkpoint
// captures the live files' lengths, syncs them and the directory, and after its bundle publishes
// unlinks what the bundle omits. Recovery truncates each named file to its length and deletes
// every other name under the prefix. docs/mechanisms/replication-group.md.
#pragma once

#include "strata/group/bundle.hh"

#include <completion/io.h>
#include <crc32c/crc32c.hh>

#include <cerrno>
#include <cstdint>
#include <map>
#include <algorithm>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace strata::group {

// A file the set cannot serve as named: recovery fails closed on it.
struct file_set_damage : std::runtime_error {
	using std::runtime_error::runtime_error;
};

class file_set {
public:
	explicit file_set(kronuz::journal::IO& io) : io_(&io) {}

	// --- apply ----------------------------------------------------------------------------

	// Writes `bytes` at `offset`, creating the file when absent. Never appends: a retry
	// rewrites the same bytes. ENOSPC (whole or partway) throws std::system_error.
	void write(std::string_view name, std::uint64_t offset, std::string_view bytes) {
		auto [it, fresh] = live_.try_emplace(std::string(name));
		if (fresh && retired(name)) { throw std::logic_error("strata::group: a retired file written again"); }
		if (it->second.crc) { throw std::logic_error("strata::group: an immutable file written"); }
		auto f = open(name, true);
		for (std::size_t done = 0; done < bytes.size();) { done += f->write_at(offset + done, bytes.substr(done)); }
		it->second.length = std::max(it->second.length, offset + bytes.size());
	}
	std::string read(std::string_view name, std::uint64_t offset, std::size_t n) {
		std::string out(n, '\0');
		auto f = open(name, false);
		for (std::size_t done = 0; done < n;) {
			const auto got = f->read_at(offset + done, std::span<char>(out.data() + done, n - done));
			if (!got) { throw file_set_damage("strata::group: file-set read past the end of " + std::string(name)); }
			done += got;
		}
		return out;
	}
	std::uint64_t length(std::string_view name) const {
		auto it = live_.find(name);
		return it == live_.end() ? 0 : it->second.length;
	}
	bool live(std::string_view name) const { return live_.find(name) != live_.end(); }
	// Rotation, only ever at an entry: the file is immutable from here, its CRC32C named.
	void seal(std::string_view name) {
		auto it = live_.find(name);
		if (it == live_.end() || it->second.crc) { return; }
		it->second.crc = kronuz::crc32c::crc32c(read(name, 0, std::size_t(it->second.length)));
	}
	// Retires a file at entry `at`: it leaves the live set now and is unlinked only after a
	// bundle that omits it publishes. Retiring an absent file does nothing (replay).
	void retire(std::string_view name, std::uint64_t at) {
		auto it = live_.find(name);
		if (it == live_.end()) { return; }
		live_.erase(it);
		retired_.emplace_back(std::string(name), at);
	}

	// --- checkpoint -----------------------------------------------------------------------

	// The live files and their lengths, at the capture's position.
	const file_map& capture() const noexcept { return live_; }
	// Makes each named file's bytes durable, then the directory's names. A failure throws and
	// fences the replica: a retried sync can succeed with the bytes lost.
	void sync(const file_map& named) {
		for (const auto& [name, f] : named) {
			if (f.crc && synced_.count(name)) { continue; }  // immutable, synced once
			open(name, false)->sync();
			if (f.crc) { synced_.insert(name); }
		}
		io_->sync_directory();
	}
	// After a bundle at `through` publishes: unlinks each file retired at or below it.
	void unlink_retired(std::uint64_t through) {
		bool removed = false;
		for (auto it = retired_.begin(); it != retired_.end();) {
			if (it->second > through) {
				++it;
				continue;
			}
			io_->remove(path(it->first));
			synced_.erase(it->first);
			it = retired_.erase(it);
			removed = true;
		}
		if (removed) { io_->sync_directory(); }
	}
	std::size_t retired_count() const noexcept { return retired_.size(); }

	// --- recovery -------------------------------------------------------------------------

	// The set as the bundle names it: each named file truncated to its length (missing, short or,
	// sealed, failing its CRC32C fails closed), every other name deleted, the directory synced.
	// Repeats converge.
	void recover(const file_map& named) {
		std::vector<std::string> names;
		for (auto cursor = io_->scan_directory(); auto name = cursor->next();) {
			if (name->starts_with(bundle_detail::prefix)) { names.push_back(name->substr(bundle_detail::prefix.size())); }
		}
		for (const auto& [name, f] : named) {
			auto file = open_named(name);
			if (!file) { throw file_set_damage("strata::group: the bundle names a missing file: " + name); }
			const auto size = file->size();
			if (size < f.length) { throw file_set_damage("strata::group: a file shorter than the bundle names: " + name); }
			if (size > f.length) {
				file->truncate(f.length);
				file->sync();
			}
			if (f.crc && crc32c_of(*file, f.length) != *f.crc) { throw file_set_damage("strata::group: a sealed file fails its CRC32C: " + name); }
		}
		for (const auto& name : names) {
			if (!named.count(name)) { io_->remove(path(name)); }
		}
		io_->sync_directory();
		live_ = named;
		retired_.clear();
		synced_.clear();
	}
	// The group closed: the set holds nothing, and its directory goes with the group.
	void close() noexcept {
		io_ = nullptr;
		file_map().swap(live_);
		decltype(retired_)().swap(retired_);
		synced_.clear();
	}

private:
	static std::string path(std::string_view name) { return std::string(bundle_detail::prefix) + std::string(name); }
	static std::uint32_t crc32c_of(kronuz::journal::File& file, std::uint64_t length) {
		kronuz::crc32c::Crc32c crc;
		std::string chunk(std::size_t(std::min<std::uint64_t>(length, 65536)), '\0');
		for (std::uint64_t at = 0; at < length;) {
			const auto got = file.read_at(at, std::span<char>(chunk.data(), std::size_t(std::min<std::uint64_t>(length - at, chunk.size()))));
			if (!got) { throw file_set_damage("strata::group: a sealed file ends before its length"); }
			crc.update(chunk.data(), got);
			at += got;
		}
		return crc.value();
	}
	std::unique_ptr<kronuz::journal::File> open_named(const std::string& name) {
		try {
			return io_->open_existing(path(name));
		} catch (const std::system_error& e) {
			if (e.code() == std::errc::no_such_file_or_directory) { return nullptr; }
			throw;
		}
	}
	std::unique_ptr<kronuz::journal::File> open(std::string_view name, bool create) {
		const std::string n(name);
		if (auto f = open_named(n)) { return f; }
		if (!create) { throw file_set_damage("strata::group: a file-set file is missing: " + n); }
		return io_->create_exclusive(path(n));
	}
	bool retired(std::string_view name) const {
		for (const auto& [n, at] : retired_) {
			if (n == name) { return true; }
		}
		return false;
	}

	kronuz::journal::IO* io_;
	file_map live_;
	std::vector<std::pair<std::string, std::uint64_t>> retired_;  // name, the entry that retired it
	std::set<std::string, std::less<>> synced_;  // immutable files already synced
};

}  // namespace strata::group
