// counted_io: a kronuz::journal::IO that reserves on a resource_budget before each system call
// that opens a descriptor or makes a mapping, and holds the reservation until the kernel object
// is gone (cold-tier D6; CT-R8, CT-R32). A file holds one descriptor and one vnode; a mapping holds
// one map and shares its file's vnode, which lives until the file and every mapping of it have
// gone; an owner lock file or a scan cursor holds a descriptor. The directory descriptor the
// inner IO opens when it is built is reserved first (counted_io::make) and released when this IO
// is destroyed. A reservation past a hard cap throws too_many_open, never reaching the kernel; a
// call that fails, or an exception after a call that succeeded, returns the reservation and the
// kernel object together.
//
// The inner IO is reached only through this one: native_directory_handle() and the like are not
// forwarded, so a backend that opens files past the wrapper is not admitted.
#pragma once

#include "strata/core/resource_budget.hh"

#include "completion/io.h"

#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace strata {

// Where a reservation is made: the process's budget, and the counts of the catalog and slot
// that own the IO (either may be null). A host that opens files beside its index (a WAL, blobs)
// follows counted_io's rule with these two helpers: reserve a descriptor, and on macOS a vnode,
// before each file it opens, run the call under guarded(), and let the reservations go with the file.
struct io_counts {
	resource_budget* budget = nullptr;
	resource_counts* catalog = nullptr;
	resource_counts* slot = nullptr;

	// One unit of `r`, held until the reservation goes; past a hard cap it throws too_many_open.
	resource_reservation reserve(resource r) const { return budget->reserve(r, 1, catalog, slot); }

	// Runs the system call `f` that `res` covers; the kernel's EMFILE, ENFILE and ENOMEM are noted as
	// external pressure before the error goes on.
	template <class F>
	decltype(auto) guarded(const resource_reservation& res, F&& f) const {
		try {
			return f();
		} catch (const std::system_error& e) {
			budget->note_failure(res, e.code().value());
			throw;
		}
	}
};

namespace counted_detail {

using kronuz::journal::DirectoryCursor;
using kronuz::journal::File;
using kronuz::journal::IO;
using kronuz::journal::Mapping;
using kronuz::journal::OwnerLock;

inline resource_reservation take(const io_counts& c, resource r) { return c.reserve(r); }

template <class F>
decltype(auto) guarded(const io_counts& c, const resource_reservation& res, F&& f) {
	return c.guarded(res, std::forward<F>(f));
}

// close() on a held file or cursor: the descriptor is gone whether it succeeded or failed, unless
// the backend has no explicit close (its logic_error), which leaves everything as it was.
template <class Inner>
void close_inner(Inner& inner, resource_reservation& desc) {
	try {
		inner.close();
	} catch (const std::logic_error&) {
		throw;
	} catch (...) {
		desc.release();
		throw;
	}
	desc.release();
}

class mapping final : public Mapping {
public:
	mapping(std::shared_ptr<const Mapping> inner, resource_reservation map, std::shared_ptr<resource_reservation> vnode)
		: map_(std::move(map)), vnode_(std::move(vnode)), inner_(std::move(inner)) {
		bytes_ = inner_->bytes();
	}

private:
	resource_reservation map_;
	std::shared_ptr<resource_reservation> vnode_;
	std::shared_ptr<const Mapping> inner_;  // last: the mapping goes before its reservations
};

class file final : public File {
public:
	file(std::unique_ptr<File> inner, io_counts c, resource_reservation desc, std::shared_ptr<resource_reservation> vnode)
		: c_(c), desc_(std::move(desc)), vnode_(std::move(vnode)), inner_(std::move(inner)) {}

	std::uint64_t size() override { return inner_->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner_->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { return inner_->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { inner_->truncate(size); }
	void sync() override { inner_->sync(); }
	void sync_data() override { inner_->sync_data(); }
	kronuz::journal::EntryInspection inspect() override { return inner_->inspect(); }
	bool try_lease(kronuz::journal::LeaseMode m) override { return inner_->try_lease(m); }
	void close() override {
		close_inner(*inner_, desc_);
		vnode_.reset();
	}
	std::shared_ptr<const Mapping> map() override {
		resource_reservation res = take(c_, resource::maps);
		std::shared_ptr<const Mapping> m = guarded(c_, res, [&] { return inner_->map(); });
		if (!m) { return nullptr; }
		return std::make_shared<mapping>(std::move(m), std::move(res), vnode_);
	}
	File* inner() noexcept { return inner_.get(); }

private:
	io_counts c_;
	resource_reservation desc_;
	std::shared_ptr<resource_reservation> vnode_;
	std::unique_ptr<File> inner_;  // last: the descriptor closes before its reservation returns
};

class owner final : public OwnerLock {
public:
	owner(std::unique_ptr<OwnerLock> inner, resource_reservation desc, resource_reservation vnode)
		: desc_(std::move(desc)), vnode_(std::move(vnode)), inner_(std::move(inner)) {}

private:
	resource_reservation desc_, vnode_;
	std::unique_ptr<OwnerLock> inner_;
};

class cursor final : public DirectoryCursor {
public:
	cursor(std::unique_ptr<DirectoryCursor> inner, resource_reservation desc) : desc_(std::move(desc)), inner_(std::move(inner)) {}
	std::optional<std::string> next() override { return inner_->next(); }
	std::optional<std::size_t> next_into(std::span<char> buffer) override { return inner_->next_into(buffer); }
	void close() override { close_inner(*inner_, desc_); }

private:
	resource_reservation desc_;
	std::unique_ptr<DirectoryCursor> inner_;
};

}  // namespace counted_detail

class counted_io final : public kronuz::journal::IO {
public:
	using kronuz::journal::IO::acquire_owner;

	// Reserves the directory's descriptor and vnode, then builds the inner IO with `make_inner`
	// (which opens the directory); both go back if it throws, and when the IO is destroyed.
	template <class Make>
	static std::unique_ptr<counted_io> make(io_counts c, Make&& make_inner) {
		resource_reservation desc = counted_detail::take(c, resource::descriptors);
		resource_reservation vnode = counted_detail::take(c, resource::vnodes);
		std::unique_ptr<kronuz::journal::IO> inner = counted_detail::guarded(c, desc, [&] { return std::unique_ptr<kronuz::journal::IO>(make_inner()); });
		return std::unique_ptr<counted_io>(new counted_io(c, std::move(desc), std::move(vnode), std::move(inner)));
	}

	kronuz::journal::FilesystemClass directory_filesystem() const override { return inner_->directory_filesystem(); }
	kronuz::journal::DirectoryTrust directory_trust() const override { return inner_->directory_trust(); }

	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override {
		return lock([&] { return inner_->acquire_owner(create); });
	}
	// The lock on the directory handle itself opens no descriptor.
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_->acquire_directory_owner(); }

	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override {
		return open([&] { return inner_->open_existing(name); });
	}
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		return open([&] { return inner_->create_exclusive(name); });
	}
	std::unique_ptr<kronuz::journal::File> open_reclaim_candidate(std::string_view name) override {
		return open([&] { return inner_->open_reclaim_candidate(name); });
	}
	void replace(std::string_view source, std::string_view destination) override { inner_->replace(source, destination); }
	void remove(std::string_view name) override { inner_->remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override {
		resource_reservation desc = counted_detail::take(c_, resource::descriptors);
		auto cur = counted_detail::guarded(c_, desc, [&] { return inner_->scan_directory(); });
		return std::make_unique<counted_detail::cursor>(std::move(cur), std::move(desc));
	}
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view name) override { return inner_->entry_footprint(name); }
	std::optional<kronuz::journal::EntryInspection> inspect_entry(std::string_view name) override { return inner_->inspect_entry(name); }
	void sync_directory() override { inner_->sync_directory(); }
	void sync_files(std::span<kronuz::journal::File* const> files) override {
		auto inner = unwrap(files);
		inner_->sync_files(inner);
	}
	void sync_files_and_directory(std::span<kronuz::journal::File* const> files) override {
		auto inner = unwrap(files);
		inner_->sync_files_and_directory(inner);
	}
	void order_files_and_directory(std::span<kronuz::journal::File* const> files) override {
		auto inner = unwrap(files);
		inner_->order_files_and_directory(inner);
	}

protected:
	std::unique_ptr<kronuz::journal::OwnerLock> lock_owner(const kronuz::journal::OwnerLockOptions& o) override {
		if (o.mode == kronuz::journal::OwnerLockMode::directory) { return inner_->acquire_owner(o); }
		return lock([&] { return inner_->acquire_owner(o); });
	}

private:
	counted_io(io_counts c, resource_reservation desc, resource_reservation vnode, std::unique_ptr<kronuz::journal::IO> inner)
		: c_(c), desc_(std::move(desc)), vnode_(std::move(vnode)), inner_(std::move(inner)) {}

	template <class F>
	std::unique_ptr<kronuz::journal::File> open(F&& f) {
		resource_reservation desc = counted_detail::take(c_, resource::descriptors);
		auto vnode = std::make_shared<resource_reservation>(counted_detail::take(c_, resource::vnodes));
		std::unique_ptr<kronuz::journal::File> file = counted_detail::guarded(c_, desc, f);
		if (!file) { return nullptr; }
		return std::make_unique<counted_detail::file>(std::move(file), c_, std::move(desc), std::move(vnode));
	}
	template <class F>
	std::unique_ptr<kronuz::journal::OwnerLock> lock(F&& f) {
		resource_reservation desc = counted_detail::take(c_, resource::descriptors);
		resource_reservation vnode = counted_detail::take(c_, resource::vnodes);
		auto l = counted_detail::guarded(c_, desc, f);
		return std::make_unique<counted_detail::owner>(std::move(l), std::move(desc), std::move(vnode));
	}
	static std::vector<kronuz::journal::File*> unwrap(std::span<kronuz::journal::File* const> files) {
		std::vector<kronuz::journal::File*> out;
		out.reserve(files.size());
		for (auto* f : files) {
			auto* own = dynamic_cast<counted_detail::file*>(f);
			out.push_back(own ? own->inner() : f);
		}
		return out;
	}

	io_counts c_;
	resource_reservation desc_, vnode_;
	std::unique_ptr<kronuz::journal::IO> inner_;  // last: the directory closes before its reservations return
};

}  // namespace strata
