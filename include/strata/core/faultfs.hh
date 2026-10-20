// faultfs: an in-memory kronuz::journal::IO that models what storage keeps across power
// loss, for crash testing (Phase 3 design, "faultfs"). Each file has current bytes and
// durable bytes; the directory has a current namespace and a durable one. File::sync
// makes a file's bytes durable, sync_directory makes the namespace durable. crash()
// rebuilds the state a power loss could leave:
//
//   - each 512-byte sector written since the file's last sync keeps its old bytes, takes
//     its new ones, or (beyond the durable size) reads as zeros; the size is the durable
//     one or the current one;
//   - each create, rename, and unlink since the last directory sync survives or not,
//     replayed in order, a rename atomically; an op whose source no longer exists drops.
//
// Faults: a rule fails the n-th operation of a kind, before or after its effect, with
// EIO or ENOSPC; a capacity fails writes that grow the total past it with ENOSPC; short
// writes return part of the request. A failed File::sync drops the file's unsynced bytes;
// a later sync succeeds. Handles opened before a crash fail with EIO.
//
// Media faults model unreliable storage on reads: a read overlapping a file's byte range
// fails with EIO (a latent sector error) or returns one bit flipped in that range, the
// next `times` reads (transient) or every read (sticky, times 0). The stored bytes never
// change.
#pragma once

#include "completion/io.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace strata::faultfs {

enum class op { write, sync, sync_directory, create, replace, remove, truncate };

// Fails the `nth` (1-based) operation of `kind` counted from when the rule is added,
// before or after its effect takes place.
struct fault {
	op kind;
	std::uint64_t nth = 1;
	int error = EIO;
	bool after = false;
};

enum class media { read_error, bit_flip };

class filesystem;

namespace detail {

inline constexpr std::size_t sector = 512;

struct inode {
	std::string current;  // what reads return
	std::string durable;  // what survives a crash for certain
	bool dirty = false;
	std::uint64_t mapped = 0;  // the longest live mapping's length (a real one would fault past a cut)
	std::uint64_t mappings = 0;
};

[[noreturn]] inline void fail(int error, const char* what) { throw std::system_error(error, std::generic_category(), what); }

inline void check_name(std::string_view name) {
	if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
		throw std::invalid_argument("faultfs: name must be a single component");
	}
}

}  // namespace detail

// The model one or more IO instances share; it outlives every handle.
class filesystem {
public:
	explicit filesystem(std::uint64_t capacity = ~std::uint64_t(0)) : capacity_(capacity) {
		const char* m = std::getenv("STRATA_FAULTFS_MAP");
		map_files_ = m && *m == '1';
	}

	void add_fault(const fault& f) {
		std::lock_guard lock(mu_);
		rules_.push_back({f, 0});
	}
	void clear_faults() {
		std::lock_guard lock(mu_);
		rules_.clear();
		media_.clear();
	}
	// Reads of `name` overlapping [begin, end) fail or return a flipped bit (media above),
	// `times` times, or always when 0.
	void add_media_fault(const std::string& name, media kind, std::uint64_t begin, std::uint64_t end, std::uint64_t times = 0) {
		std::lock_guard lock(mu_);
		auto it = names_.find(name);
		if (it == names_.end()) { detail::fail(ENOENT, "faultfs media fault"); }
		media_.push_back({inodes_.at(it->second), kind, begin, end, times});
	}
	// What io::directory_filesystem reports (default: a local filesystem named faultfs).
	void classify(kronuz::journal::FilesystemClass fs) {
		std::lock_guard lock(mu_);
		class_ = std::move(fs);
	}
	// What io::directory_trust reports (default: owned and private).
	void set_trust(kronuz::journal::DirectoryTrust trust) {
		std::lock_guard lock(mu_);
		trust_ = trust;
	}
	// Reads that met a media fault so far.
	std::uint64_t media_hits() const {
		std::lock_guard lock(mu_);
		return media_hits_;
	}
	// Whether File::map maps (default: STRATA_FAULTFS_MAP=1). Off, it returns null, so
	// callers read and media faults apply; on, truncating a file below a live mapping
	// aborts, as a real mapping's reader would fault.
	void map_files(bool on) {
		std::lock_guard lock(mu_);
		map_files_ = on;
	}
	// Each write then stores at most `max_bytes` (0: no limit) and returns the count.
	void short_writes(std::size_t max_bytes) {
		std::lock_guard lock(mu_);
		short_ = max_bytes;
	}
	// Power goes off after `n` more mutating operations (writes, truncates, syncs, creates,
	// renames, unlinks): each later one fails with EIO before its effect, until crash().
	void power_off_after(std::uint64_t n) {
		std::lock_guard lock(mu_);
		power_left_ = n;
	}
	// Mutating operations so far, to enumerate crash points.
	std::uint64_t mutations() const {
		std::lock_guard lock(mu_);
		return mutations_;
	}
	void set_capacity(std::uint64_t bytes) {
		std::lock_guard lock(mu_);
		capacity_ = bytes;
	}

	// Power loss: rebuilds the state storage could hold, chosen by `seed`; every open
	// handle becomes stale and the owner locks are released.
	void crash(std::uint64_t seed) {
		std::lock_guard lock(mu_);
		std::mt19937_64 rng(seed);
		auto coin = [&] { return (rng() & 1) != 0; };
		// Data: per inode still reachable from either namespace.
		for (auto& [id, node] : inodes_) {
			if (!node->dirty) { continue; }
			const std::string& old = node->durable;
			const std::string& now = node->current;
			const std::size_t size = coin() ? old.size() : now.size();
			std::string out(size, '\0');
			// Each sector holds its old bytes or its new ones; past the durable size, its
			// new ones or zeros (growth persisting without its data).
			for (std::size_t at = 0; at < size; at += detail::sector) {
				const std::size_t n = std::min(detail::sector, size - at);
				const bool take_new = at < now.size() && coin();
				const std::string& from = take_new ? now : old;
				if (at < from.size()) { std::memcpy(out.data() + at, from.data() + at, std::min(n, from.size() - at)); }
			}
			node->current = out;
			node->durable = std::move(out);
			node->dirty = false;
		}
		// Namespace: the durable map plus a surviving subset of the logged ops, in order.
		auto names = durable_names_;
		for (const auto& e : log_) {
			if (!coin()) { continue; }
			switch (e.kind) {
			case op::create:
				names.try_emplace(e.a, e.id);
				break;
			case op::replace:
				if (auto it = names.find(e.a); it != names.end() && it->second == e.id) {
					names[e.b] = e.id;
					names.erase(e.a);
				}
				break;
			case op::remove:
				if (auto it = names.find(e.a); it != names.end() && it->second == e.id) { names.erase(it); }
				break;
			default:
				break;
			}
		}
		names_ = names;
		durable_names_ = std::move(names);
		log_.clear();
		collect();
		++epoch_;
		owner_ = directory_owner_ = false;
		power_left_.reset();
	}

	// The current bytes of a file, for readers that take a view (valid until the next
	// change to that file or a crash).
	std::string_view view(std::string_view name) const {
		std::lock_guard lock(mu_);
		auto it = names_.find(std::string(name));
		if (it == names_.end()) { detail::fail(ENOENT, "faultfs view"); }
		return inodes_.at(it->second)->current;
	}
	std::vector<std::string> list() const {
		std::lock_guard lock(mu_);
		std::vector<std::string> out;
		for (const auto& [n, _] : names_) { out.push_back(n); }
		return out;
	}
	std::uint64_t used() const {
		std::lock_guard lock(mu_);
		return used_locked();
	}

private:
	friend class io;
	friend class file;

	struct logged {
		op kind;
		std::string a, b;
		std::uint64_t id;
	};
	struct rule {
		fault f;
		std::uint64_t seen;
	};

	// Counts an operation against the rules; returns the fault that fires, if any. With
	// the power off, every mutating operation fails before its effect.
	const fault* check(op kind) {
		++mutations_;
		if (power_left_) {
			if (*power_left_ == 0) {
				static const fault off{op::write, 1, EIO, false};
				return &off;
			}
			--*power_left_;
		}
		for (auto& r : rules_) {
			if (r.f.kind == kind && ++r.seen == r.f.nth) { return &r.f; }
		}
		return nullptr;
	}
	std::uint64_t used_locked() const {
		std::uint64_t n = 0;
		for (const auto& [_, node] : inodes_) { n += std::max(node->current.size(), node->durable.size()); }
		return n;
	}
	// Drops inodes no namespace names and no handle holds.
	void collect() {
		for (auto it = inodes_.begin(); it != inodes_.end();) {
			bool named = false;
			for (const auto& [_, id] : names_) { named = named || id == it->first; }
			for (const auto& [_, id] : durable_names_) { named = named || id == it->first; }
			for (const auto& e : log_) { named = named || e.id == it->first; }
			if (!named && it->second.use_count() == 1) {
				it = inodes_.erase(it);
			} else {
				++it;
			}
		}
	}

	mutable std::mutex mu_;
	std::map<std::uint64_t, std::shared_ptr<detail::inode>> inodes_;
	std::map<std::string, std::uint64_t> names_, durable_names_;
	std::vector<logged> log_;
	std::vector<rule> rules_;
	struct media_rule {
		std::shared_ptr<detail::inode> node;
		media kind;
		std::uint64_t begin, end, left;
		bool sticky = left == 0;
	};
	std::vector<media_rule> media_;
	std::uint64_t media_hits_ = 0;
	bool map_files_ = false;
	std::uint64_t next_id_ = 1, epoch_ = 0, capacity_;
	std::size_t short_ = 0;
	bool owner_ = false, directory_owner_ = false;
	kronuz::journal::FilesystemClass class_{true, "faultfs"};
	kronuz::journal::DirectoryTrust trust_{true, false, false, false};
	std::uint64_t mutations_ = 0;
	std::optional<std::uint64_t> power_left_;
};

class file final : public kronuz::journal::File {
public:
	file(filesystem& fs, std::shared_ptr<detail::inode> node) : fs_(&fs), node_(std::move(node)), epoch_(fs.epoch_) {}

	std::uint64_t size() override {
		std::lock_guard lock(fs_->mu_);
		live();
		return node_->current.size();
	}
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override {
		std::lock_guard lock(fs_->mu_);
		live();
		const std::string& d = node_->current;
		if (offset >= d.size()) { return 0; }
		const std::size_t n = std::min<std::uint64_t>(bytes.size(), d.size() - offset);
		std::memcpy(bytes.data(), d.data() + offset, n);
		for (auto& m : fs_->media_) {
			if (m.node != node_ || m.end <= offset || m.begin >= offset + n || (!m.sticky && m.left == 0)) { continue; }
			if (!m.sticky) { --m.left; }
			++fs_->media_hits_;
			if (m.kind == media::read_error) { detail::fail(EIO, "faultfs read"); }
			const std::uint64_t at = std::max(m.begin, offset);
			bytes[at - offset] = char(bytes[at - offset] ^ (1 << (at % 8)));
		}
		return n;
	}
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
		std::lock_guard lock(fs_->mu_);
		live();
		const fault* f = fs_->check(op::write);
		if (f && !f->after) { detail::fail(f->error, "faultfs write"); }
		std::size_t n = bytes.size();
		if (fs_->short_ && n > fs_->short_) { n = fs_->short_; }
		const std::uint64_t end = offset + n;
		const std::uint64_t held = std::max(node_->current.size(), node_->durable.size());
		if (end > held && fs_->used_locked() + (end - held) > fs_->capacity_) { detail::fail(ENOSPC, "faultfs write"); }
		if (end > node_->current.size()) { node_->current.resize(end, '\0'); }
		std::memcpy(node_->current.data() + offset, bytes.data(), n);
		node_->dirty = true;
		if (f) { detail::fail(f->error, "faultfs write"); }
		return n;
	}
	void truncate(std::uint64_t size) override {
		std::lock_guard lock(fs_->mu_);
		live();
		const fault* f = fs_->check(op::truncate);
		if (f && !f->after) { detail::fail(f->error, "faultfs truncate"); }
		if (node_->mappings && size < node_->mapped) {
			std::fprintf(stderr, "faultfs: a file truncated below a live mapping\n");
			std::abort();
		}
		node_->current.resize(size, '\0');
		node_->dirty = true;
		if (f) { detail::fail(f->error, "faultfs truncate"); }
	}
	// A copy of the file's bytes, registered on the inode while it lives.
	std::shared_ptr<const kronuz::journal::Mapping> map() override {
		std::lock_guard lock(fs_->mu_);
		live();
		if (!fs_->map_files_) { return nullptr; }
		return std::make_shared<mapping>(*fs_, node_);
	}
	void sync() override {
		std::lock_guard lock(fs_->mu_);
		live();
		if (const fault* f = fs_->check(op::sync)) {
			// A failed fsync loses the unsynced bytes; reads see the durable ones.
			if (f->after) { node_->durable = node_->current; }
			node_->current = node_->durable;
			node_->dirty = false;
			detail::fail(f->error, "faultfs sync");
		}
		node_->durable = node_->current;
		node_->dirty = false;
	}

private:
	class mapping final : public kronuz::journal::Mapping {
	public:
		mapping(filesystem& fs, std::shared_ptr<detail::inode> node) : fs_(&fs), node_(std::move(node)), copy_(node_->current) {
			bytes_ = copy_;
			++node_->mappings;
			node_->mapped = std::max<std::uint64_t>(node_->mapped, copy_.size());
		}
		~mapping() override {
			std::lock_guard lock(fs_->mu_);
			if (--node_->mappings == 0) { node_->mapped = 0; }
		}

	private:
		filesystem* fs_;
		std::shared_ptr<detail::inode> node_;
		std::string copy_;
	};
	void live() const {
		if (epoch_ != fs_->epoch_) { detail::fail(EIO, "faultfs handle from before a crash"); }
	}
	filesystem* fs_;
	std::shared_ptr<detail::inode> node_;
	std::uint64_t epoch_;
};

class io final : public kronuz::journal::IO {
public:
	explicit io(filesystem& fs) : fs_(&fs) {}

	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override {
		std::lock_guard lock(fs_->mu_);
		if (fs_->owner_) { detail::fail(EAGAIN, "faultfs owner lock held"); }
		const bool exists = fs_->names_.count("owner.lock") != 0;
		if (create == exists) { detail::fail(create ? EEXIST : ENOENT, "faultfs owner lock"); }
		if (create) { make_locked("owner.lock"); }
		fs_->owner_ = true;
		return std::make_unique<owner>(*fs_, fs_->owner_);
	}
	// The lock on the directory itself, apart from owner.lock's: writes nothing.
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override {
		std::lock_guard lock(fs_->mu_);
		if (fs_->directory_owner_) { detail::fail(EWOULDBLOCK, "faultfs directory lock held"); }
		fs_->directory_owner_ = true;
		return std::make_unique<owner>(*fs_, fs_->directory_owner_);
	}
	kronuz::journal::FilesystemClass directory_filesystem() const override {
		std::lock_guard lock(fs_->mu_);
		return fs_->class_;
	}
	kronuz::journal::DirectoryTrust directory_trust() const override {
		std::lock_guard lock(fs_->mu_);
		return fs_->trust_;
	}
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		auto it = fs_->names_.find(std::string(name));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs open"); }
		return std::make_unique<file>(*fs_, fs_->inodes_.at(it->second));
	}
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		const fault* f = fs_->check(op::create);
		if (f && !f->after) { detail::fail(f->error, "faultfs create"); }
		if (fs_->names_.count(std::string(name))) { detail::fail(EEXIST, "faultfs create"); }
		auto node = make_locked(name);
		if (f) { detail::fail(f->error, "faultfs create"); }
		return std::make_unique<file>(*fs_, std::move(node));
	}
	void replace(std::string_view source, std::string_view destination) override {
		detail::check_name(source);
		detail::check_name(destination);
		std::lock_guard lock(fs_->mu_);
		const fault* f = fs_->check(op::replace);
		if (f && !f->after) { detail::fail(f->error, "faultfs replace"); }
		auto it = fs_->names_.find(std::string(source));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs replace"); }
		const std::uint64_t id = it->second;
		fs_->names_.erase(it);
		fs_->names_[std::string(destination)] = id;
		fs_->log_.push_back({op::replace, std::string(source), std::string(destination), id});
		fs_->collect();
		if (f) { detail::fail(f->error, "faultfs replace"); }
	}
	void remove(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		const fault* f = fs_->check(op::remove);
		if (f && !f->after) { detail::fail(f->error, "faultfs remove"); }
		if (auto it = fs_->names_.find(std::string(name)); it != fs_->names_.end()) {
			fs_->log_.push_back({op::remove, std::string(name), {}, it->second});
			fs_->names_.erase(it);
			fs_->collect();
		}
		if (f) { detail::fail(f->error, "faultfs remove"); }
	}
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override {
		std::lock_guard lock(fs_->mu_);
		std::vector<std::string> names;
		for (const auto& [n, _] : fs_->names_) { names.push_back(n); }
		return std::make_unique<cursor>(std::move(names));
	}
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		auto it = fs_->names_.find(std::string(name));
		if (it == fs_->names_.end()) { return std::nullopt; }
		return kronuz::journal::EntryFootprint{kronuz::journal::EntryKind::Regular, fs_->inodes_.at(it->second)->current.size(), std::nullopt};
	}
	void sync_directory() override {
		std::lock_guard lock(fs_->mu_);
		const fault* f = fs_->check(op::sync_directory);
		if (f && !f->after) { detail::fail(f->error, "faultfs sync_directory"); }
		fs_->durable_names_ = fs_->names_;
		fs_->log_.clear();
		fs_->collect();
		if (f) { detail::fail(f->error, "faultfs sync_directory"); }
	}

private:
	struct owner final : kronuz::journal::OwnerLock {
		owner(filesystem& fs, bool& held) : fs_(&fs), held_(&held), epoch_(fs.epoch_) {}
		~owner() override {
			std::lock_guard lock(fs_->mu_);
			if (epoch_ == fs_->epoch_) { *held_ = false; }
		}
		filesystem* fs_;
		bool* held_;
		std::uint64_t epoch_;
	};
	struct cursor final : kronuz::journal::DirectoryCursor {
		explicit cursor(std::vector<std::string> names) : names_(std::move(names)) {}
		std::optional<std::string> next() override {
			if (at_ == names_.size()) { return std::nullopt; }
			return names_[at_++];
		}
		std::vector<std::string> names_;
		std::size_t at_ = 0;
	};
	std::shared_ptr<detail::inode> make_locked(std::string_view name) {
		const std::uint64_t id = fs_->next_id_++;
		auto node = std::make_shared<detail::inode>();
		fs_->inodes_[id] = node;
		fs_->names_[std::string(name)] = id;
		fs_->log_.push_back({op::create, std::string(name), {}, id});
		return node;
	}
	filesystem* fs_;
};

}  // namespace strata::faultfs
