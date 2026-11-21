// faultfs as a journal host's directory: the host takes its owner lock through lock_owner, which
// faultfs::io leaves to the base; this forwards everything else to it.
#pragma once

#include "strata/core/faultfs.hh"

#include <memory>

namespace group_test {

namespace j = kronuz::journal;

class host_io final : public j::IO {
public:
	explicit host_io(strata::faultfs::filesystem& fs) : io_(fs) {}

	j::FilesystemClass directory_filesystem() const override { return io_.directory_filesystem(); }
	j::DirectoryTrust directory_trust() const override { return io_.directory_trust(); }
	std::unique_ptr<j::OwnerLock> acquire_owner(bool create) override { return io_.acquire_owner(create); }
	std::unique_ptr<j::OwnerLock> acquire_directory_owner() override { return io_.acquire_directory_owner(); }
	void open_existing_read_only_into(std::string_view n, j::File& f) override { io_.open_existing_read_only_into(n, f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, j::File& f) override { return io_.open_reclaim_candidate_read_only_into(n, f); }
	std::unique_ptr<j::File> open_reclaim_candidate_read_only(std::string_view n) override { return io_.open_reclaim_candidate_read_only(n); }
	std::unique_ptr<j::File> open_existing(std::string_view n) override { return io_.open_existing(n); }
	std::unique_ptr<j::File> open_existing_read_only(std::string_view n) override { return io_.open_existing_read_only(n); }
	std::unique_ptr<j::File> create_exclusive(std::string_view n) override { return io_.create_exclusive(n); }
	void replace(std::string_view a, std::string_view b) override { io_.replace(a, b); }
	void remove(std::string_view n) override { io_.remove(n); }
	std::unique_ptr<j::DirectoryCursor> scan_directory() override { return io_.scan_directory(); }
	std::optional<j::EntryFootprint> entry_footprint(std::string_view n) override { return io_.entry_footprint(n); }
	void sync_directory() override { io_.sync_directory(); }
	void order_files_and_directory(std::span<j::File* const> files) override { io_.order_files_and_directory(files); }

protected:
	std::unique_ptr<j::OwnerLock> lock_owner(const j::OwnerLockOptions& o) override {
		switch (o.mode) {
		case j::OwnerLockMode::directory:
			return io_.acquire_directory_owner();
		case j::OwnerLockMode::file:
			return io_.acquire_owner(o.owner_file == j::OwnerFile::create_exclusive);
		case j::OwnerLockMode::both:
			break;
		}
		struct both final : j::OwnerLock {
			std::unique_ptr<j::OwnerLock> directory, file;
		};
		auto lock = std::make_unique<both>();
		lock->directory = io_.acquire_directory_owner();
		lock->file = io_.acquire_owner(o.owner_file == j::OwnerFile::create_exclusive);
		return lock;
	}

private:
	strata::faultfs::io io_;
};

}  // namespace group_test
