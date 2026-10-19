// An IO decorator that counts durability calls and bytes written, by file kind.
#pragma once
#include "completion/io.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace probe {

struct counts {
	std::uint64_t file_sync = 0, file_sync_data = 0, dir_sync = 0, sync_files_calls = 0, sync_files_files = 0;
	std::uint64_t owner_create = 0, owner_open = 0, creates = 0, opens = 0, replaces = 0, removes = 0;
	std::map<std::string, std::uint64_t> bytes_by_kind, files_by_kind, syncs_by_kind;
	std::uint64_t durability_calls() const { return file_sync + file_sync_data + dir_sync + sync_files_files + owner_create; }
};

// seg-N.sx, manifest-N(.tmp), meta-N.kv, seg-N.live...: the kind is the name without digits.
inline std::string kind_of(std::string_view name) {
	std::string out;
	for (char c : name) { if (c < '0' || c > '9') { out.push_back(c); } }
	return out;
}

class counting_file final : public kronuz::journal::File {
public:
	counting_file(std::unique_ptr<kronuz::journal::File> inner, counts& c, std::string kind, int tee = -1) : inner_(std::move(inner)), c_(c), kind_(std::move(kind)), tee_(tee) {}
	~counting_file() override { if (tee_ >= 0) { ::close(tee_); } }
	kronuz::journal::File& inner() noexcept { return *inner_; }
	std::uint64_t size() override { return inner_->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner_->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
		const auto n = inner_->write_at(offset, bytes);
		c_.bytes_by_kind[kind_] += n;
		if (tee_ >= 0 && ::pwrite(tee_, bytes.data(), n, off_t(offset)) != ssize_t(n)) { throw std::runtime_error("tee write"); }
		return n;
	}
	void truncate(std::uint64_t size) override { inner_->truncate(size); }
	void sync() override { ++c_.file_sync; ++c_.syncs_by_kind[kind_]; inner_->sync(); }
	void sync_data() override { ++c_.file_sync_data; ++c_.syncs_by_kind[kind_]; inner_->sync_data(); }
	kronuz::journal::EntryInspection inspect() override { return inner_->inspect(); }
	bool try_lease(kronuz::journal::LeaseMode mode) override { return inner_->try_lease(mode); }
	void close() override { inner_->close(); }
	std::shared_ptr<const kronuz::journal::Mapping> map() override { return inner_->map(); }
private:
	std::unique_ptr<kronuz::journal::File> inner_;
	counts& c_;
	std::string kind_;
	int tee_;
};

class counting_io final : public kronuz::journal::IO {
public:
	counting_io(kronuz::journal::IO& inner, counts& c) : inner_(inner), c_(c) {}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override {
		++(create ? c_.owner_create : c_.owner_open);
		return inner_.acquire_owner(create);
	}
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override {
		++c_.opens;
		return std::make_unique<counting_file>(inner_.open_existing(name), c_, kind_of(name));
	}
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		++c_.creates;
		const auto kind = kind_of(name);
		++c_.files_by_kind[kind];
		int tee = -1;
		if (!tee_dir.empty() && kind == "seg-.sx") {
			tee = ::open((tee_dir + "/" + std::string(name)).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			if (tee < 0) { throw std::runtime_error("tee open"); }
		}
		return std::make_unique<counting_file>(inner_.create_exclusive(name), c_, kind, tee);
	}
	void replace(std::string_view source, std::string_view destination) override { ++c_.replaces; inner_.replace(source, destination); }
	void remove(std::string_view name) override { ++c_.removes; inner_.remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_.scan_directory(); }
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view name) override { return inner_.entry_footprint(name); }
	std::optional<kronuz::journal::EntryInspection> inspect_entry(std::string_view name) override { return inner_.inspect_entry(name); }
	void sync_directory() override { ++c_.dir_sync; inner_.sync_directory(); }
	// Unwraps, so the backend still batches the device flushes it would batch.
	void sync_files(std::span<kronuz::journal::File* const> files) override {
		++c_.sync_files_calls;
		std::vector<kronuz::journal::File*> inner;
		for (auto* f : files) {
			++c_.sync_files_files;
			if (auto* cf = dynamic_cast<counting_file*>(f)) { inner.push_back(&cf->inner()); } else { inner.push_back(f); }
		}
		inner_.sync_files(inner);
	}
	// When set, every segment file's bytes are also written there as written: what segment
	// mode would ship, merged-away segments included.
	std::string tee_dir;
private:
	kronuz::journal::IO& inner_;
	counts& c_;
};

}  // namespace probe
