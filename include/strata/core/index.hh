// An index: one shard's segments, live docs, metadata, and manifests in one directory,
// with one writer (Phase 3 design). Transactions apply atomically on the owner core;
// buffered documents flush to segments sorted by docid; commit() makes everything
// applied so far durable through the commit protocol; open() recovers the last durable
// manifest and fails closed when a file it names is missing or differs.
#pragma once

#include "strata/core/borrow.hh"
#include "strata/core/budget.hh"
#include "strata/core/check.hh"
#include "strata/core/command.hh"
#include "strata/core/deleted_bits.hh"
#include "strata/core/executor.hh"
#include "strata/core/files.hh"
#include "strata/core/manifest_log.hh"
#include "strata/core/hazard.hh"
#include "strata/core/index_ledger.hh"
#include "strata/core/merge.hh"
#include "strata/core/merge_policy.hh"
#include "strata/core/owner.hh"
#include "strata/core/rate_limiter.hh"
#include "strata/core/turn_seal.hh"
#include "strata/core/reader.hh"
#include "strata/core/writer.hh"
#include "strata/core/write_buffer.hh"

#include "completion/io.h"
#include "hashes.hh"
#include "varint.hh"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <charconv>
#include <deque>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace strata {

// Apply's answer. A rejected transaction left no trace.
struct verdict {
	// flush_owed, drain_owed and map_owed come only under options::defer_io: nothing was applied; the
	// host runs refresh(), reclaim_for_space() or promote() as a chore and applies the transaction
	// again. Each is answered at most once per transaction (cold-tier D3, CT-R22).
	enum outcome : std::uint8_t { accepted, version_conflict, schema_conflict, invalid, skipped, no_space, fields_conflict, flush_owed, drain_owed, map_owed };
	outcome result = accepted;
	std::size_t op = 0;                  // the rejecting operation
	std::optional<std::size_t> field;    // the rejecting entry of the transaction's fields, when one is
	field_mark current_fields;           // on a fields conflict: the table's size and chain now
	std::uint64_t current_version = 0;   // on a version conflict
	std::vector<std::uint64_t> docids;   // per document operation, as assigned
	std::vector<std::uint64_t> versions;  // per document operation, as written
};

// The writer stopped after a durability failure; reopen to continue (design D2).
struct index_fenced : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// A logged index's replay met a verdict its run did not give (standalone-turn-log D6.5): the
// log or the head is not this index's. Nothing is skipped.
struct replay_diverged : std::runtime_error {
	replay_diverged(files::position p, verdict::outcome why)
		: std::runtime_error("strata: replay diverged at {" + std::to_string(p.index) + ", " + std::to_string(p.applied) + "}: verdict " + std::to_string(int(why))), at(p), result(why) {}
	files::position at;
	verdict::outcome result;
};
// The turn log lacks records the index's head proves were durable (standalone-turn-log D6.4).
struct log_lost : std::runtime_error {
	using std::runtime_error::runtime_error;
};
// The log's group belongs to another index (standalone-turn-log D6.3): its checkpoint record
// names another UUID.
struct log_mismatch : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// The directory holds no committed index: no manifest and no segment, only what an
// interrupted create leaves. create() runs again over it.
struct index_not_created : std::runtime_error {
	using std::runtime_error::runtime_error;
};
// A reader's acquire on an index that closed (shared-reader D11).
struct index_closed : std::runtime_error {
	using std::runtime_error::runtime_error;
};
// A closing index whose views readers still hold past the host's deadline: their records, and
// since when the owner's scans saw each (D6, D11).
struct readers_active : std::runtime_error {
	readers_active(const char* what, std::vector<reader_registry::claim_time> c) : std::runtime_error(what), claims(std::move(c)) {}
	std::vector<reader_registry::claim_time> claims;
};

namespace index_detail {

inline std::string segment_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".sx"; }
inline std::string spill_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".spill"; }
inline std::string forward_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".fwd"; }
inline std::string records_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".rec"; }
inline std::string manifest_name(std::uint64_t n) { return "manifest-" + std::to_string(n); }

// Parses a decimal field: "123" with no sign, no leading zero, no overflow.
inline std::optional<std::uint64_t> number(std::string_view s) {
	if (s.empty() || (s.size() > 1 && s[0] == '0')) { return std::nullopt; }
	std::uint64_t v = 0;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc() || p != s.data() + s.size()) { return std::nullopt; }
	return v;
}

// The kind and numbers of one of strata's file names, or nothing for other names.
struct parsed_name {
	enum kind { segment, manifest, manifest_tmp, spill, scratch } type;
	std::uint64_t a = 0, b = 0;
};
inline std::optional<parsed_name> parse_name(std::string_view n) {
	auto strip = [&](std::string_view prefix, std::string_view suffix) -> std::optional<std::string_view> {
		if (n.size() < prefix.size() + suffix.size() || !n.starts_with(prefix) || !n.ends_with(suffix)) { return std::nullopt; }
		return n.substr(prefix.size(), n.size() - prefix.size() - suffix.size());
	};
	if (auto m = strip("manifest-", ".tmp")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::manifest_tmp, *v}; }
	}
	if (auto m = strip("manifest-", "")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::manifest, *v}; }
	}
	if (auto m = strip("seg-", ".sx")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::segment, *v}; }
	}
	if (auto m = strip("seg-", ".spill")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::spill, *v}; }
	}
	for (const char* suffix : {".fwd", ".rec"}) {
		if (auto m = strip("seg-", suffix)) {
			if (auto v = number(*m)) { return parsed_name{parsed_name::scratch, *v}; }
		}
	}
	return std::nullopt;
}

inline std::string read_all(kronuz::journal::File& f) {
	const std::uint64_t size = f.size();
	std::string out(size, '\0');
	for (std::uint64_t at = 0; at < size;) {
		const std::size_t n = f.read_at(at, std::span<char>(out.data() + at, size - at));
		if (n == 0) { throw corrupt_index("short read"); }
		at += n;
	}
	return out;
}

// The log at `io` holding generation `g`, or the newest without `g`: the greatest snapshot
// generation at or below it among the directory's logs (manifest-log D6.8, D8).
inline std::optional<std::uint64_t> log_holding(kronuz::journal::IO& io, std::optional<std::uint64_t> g = std::nullopt) {
	std::optional<std::uint64_t> holder;
	auto cursor = io.scan_directory();
	while (auto n = cursor->next()) {
		if (auto p = parse_name(*n); p && p->type == parsed_name::manifest && (!g || p->a <= *g) && (!holder || p->a > *holder)) { holder = p->a; }
	}
	return holder;
}
// Generation `g` of the index at `io`, or its newest without `g`, read-only: its log replayed to
// it. generation_not_retained when no log there reaches it whole; a damaged newest log is
// log_damage. Install, tools and tests.
inline files::log_state read_state(kronuz::journal::IO& io, std::optional<std::uint64_t> g = std::nullopt, const files::readable_by& r = {}) {
	const std::optional<std::uint64_t> holder = log_holding(io, g);
	if (!holder) { throw files::generation_not_retained(g.value_or(0)); }
	auto f = io.open_existing(manifest_name(*holder));
	return files::read_log(read_all(*f), *holder, r, g).state;
}

// Writes a file from its start, gathered a MiB at a time.
class file_sink final : public output {
public:
	explicit file_sink(kronuz::journal::File& f) : f_(f) { pending_.reserve(gather); }  // one gather buffer, never regrown
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (pending_.size() + size > gather) { drain(); }
		if (size >= gather) {
			put(std::string_view(data, size));
		} else {
			pending_.append(data, size);
		}
	}
	void finish() { drain(); }
	std::uint64_t size() const noexcept { return at_ + pending_.size(); }

private:
	static constexpr std::size_t gather = 1 << 20;
	void drain() {
		put(pending_);
		pending_.clear();
	}
	void put(std::string_view bytes) {
		while (!bytes.empty()) {
			const std::size_t n = f_.write_at(at_, bytes);
			if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
			at_ += n;
			bytes.remove_prefix(n);
		}
	}
	kronuz::journal::File& f_;
	std::string pending_;
	std::uint64_t at_ = 0;
};

// A flush's held section (the positions beside the postings): gathered a MiB at a time in
// memory, past that written to `name`, a file created on the first drain and read back by
// copy_to(). The file is removed when copied or destroyed; one left by a crash is an orphan
// open removes. Failures throw std::system_error (ENOSPC, EIO).
class io_spill final : public spill {
public:
	io_spill(kronuz::journal::IO& io, std::string name, flush_charge* memory) : io_(io), name_(std::move(name)), memory_(memory) {}
	io_spill(const io_spill&) = delete;
	io_spill& operator=(const io_spill&) = delete;
	~io_spill() override {
		release();
		if (!f_) { return; }
		f_.reset();
		try {
			io_.remove(name_);
		} catch (const std::system_error&) {
		}
	}
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (buf_.size() + size > gather) { drain(); }
		if (size >= gather) {
			put(std::string_view(data, size));
		} else {
			buf_.append(data, size);
			charge();
		}
	}
	void copy_to(output& out) override {
		if (!f_) {
			out.write(buf_);
			release();
			return;
		}
		drain();
		for (std::uint64_t at = 0; at < at_;) {
			buf_.resize(std::min<std::uint64_t>(gather, at_ - at));
			charge();
			const std::size_t n = f_->read_at(at, std::span<char>(buf_.data(), buf_.size()));
			if (n == 0) { throw std::system_error(EIO, std::generic_category(), "strata: short read of a flush's spill"); }
			out.write(buf_.data(), n);
			at += n;
		}
		release();
		f_.reset();
		io_.remove(name_);
	}

private:
	static constexpr std::size_t gather = 1 << 20;
	void charge() {
		if (memory_ && buf_.capacity() > charged_) {
			memory_->grow(buf_.capacity() - charged_);
			charged_ = buf_.capacity();
		}
	}
	void release() {
		std::string().swap(buf_);
		if (memory_) { memory_->shrink(std::exchange(charged_, 0)); }
	}
	void drain() {
		put(buf_);
		buf_.clear();
	}
	void put(std::string_view bytes) {
		if (bytes.empty()) { return; }
		if (!f_) { f_ = io_.create_exclusive(name_); }
		while (!bytes.empty()) {
			const std::size_t n = f_->write_at(at_, bytes);
			if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
			at_ += n;
			bytes.remove_prefix(n);
		}
	}
	kronuz::journal::IO& io_;
	std::string name_, buf_;
	flush_charge* memory_;
	std::uint64_t charged_ = 0, at_ = 0;
	std::unique_ptr<kronuz::journal::File> f_;
};

inline void write_all_at(kronuz::journal::File& f, std::uint64_t at, std::string_view bytes) {
	for (std::uint64_t done = 0; done < bytes.size();) {
		const std::size_t n = f.write_at(at + done, bytes.substr(done));
		if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
		done += n;
	}
}

inline void write_all(kronuz::journal::File& f, std::string_view bytes) {
	for (std::uint64_t at = 0; at < bytes.size();) {
		const std::size_t n = f.write_at(at, bytes.substr(at));
		if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
		at += n;
	}
}

// The version map's hash of a key: wordwise::hash32, the hash's high half (its low half ignores
// a short tail's bytes past the first four). In memory only.
inline std::uint32_t key_hash(std::string_view key) noexcept { return ::wordwise::hash32(key.data(), key.size()); }
// The docid map's hash of a docid: distinct below 2^32; docids past it fold their halves together.
inline std::uint32_t docid_hash(std::uint64_t docid) noexcept { return std::uint32_t(docid ^ (docid >> 32)); }

// 32-bit values by 32-bit hash in one array of 8-byte slots (the hash beside the value), at
// most 7/8 full, probed linearly. Hashes collide, so every lookup takes a test of the value
// that compares the full key; a hash match alone never identifies an entry.
// Values are below 2^32 - 1 (the empty marker). Removal shifts the entries after it back, so
// there are no tombstones.
class hash_slots {
public:
	template <class Eq>
	std::uint32_t* find(std::uint32_t h, Eq&& eq) noexcept {
		const std::size_t i = index_of(h, eq);
		return i == npos ? nullptr : &slots_[i].value;
	}
	template <class Eq>
	const std::uint32_t* find(std::uint32_t h, Eq&& eq) const noexcept {
		const std::size_t i = index_of(h, eq);
		return i == npos ? nullptr : &slots_[i].value;
	}
	// A new entry; the caller found none equal. The value is not the empty marker.
	void insert(std::uint32_t h, std::uint32_t value) {
		assert(value != empty);
		if (8 * (size_ + 1) > 7 * slots_.size()) { grow(); }
		place(h, value);
		++size_;
	}
	template <class Eq>
	bool erase(std::uint32_t h, Eq&& eq) noexcept {
		std::size_t i = index_of(h, eq);
		if (i == npos) { return false; }
		for (std::size_t j = i;;) {
			j = (j + 1) & mask_;
			if (slots_[j].value == empty) { break; }
			const std::size_t k = home(slots_[j].hash);
			// slots_[j] moves back to i unless its home lies cyclically in (i, j].
			if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) { continue; }
			slots_[i] = slots_[j];
			i = j;
		}
		slots_[i].value = empty;
		--size_;
		return true;
	}
	void clear() noexcept {
		std::vector<slot>().swap(slots_);
		size_ = 0, mask_ = 0;
	}
	std::size_t size() const noexcept { return size_; }
	std::uint64_t bytes() const noexcept { return slots_.capacity() * sizeof(slot); }

private:
	static constexpr std::uint32_t empty = ~std::uint32_t(0);
	static constexpr std::size_t npos = ~std::size_t(0);
	struct slot {
		std::uint32_t hash;
		std::uint32_t value;
	};
	template <class Eq>
	std::size_t index_of(std::uint32_t h, Eq& eq) const noexcept {
		if (!size_) { return npos; }
		for (std::size_t i = home(h);; i = (i + 1) & mask_) {
			const slot& s = slots_[i];
			if (s.value == empty) { return npos; }
			if (s.hash == h && eq(s.value)) { return i; }
		}
	}
	std::size_t home(std::uint32_t h) const noexcept { return std::size_t((std::uint64_t(h) * 0x9E3779B97F4A7C15ull) >> 32) & mask_; }
	void place(std::uint32_t h, std::uint32_t value) noexcept {
		std::size_t i = home(h);
		while (slots_[i].value != empty) { i = (i + 1) & mask_; }
		slots_[i] = {h, value};
	}
	void grow() {
		std::vector<slot> old(std::max<std::size_t>(16, 2 * slots_.size()), slot{0, empty});
		old.swap(slots_);
		mask_ = slots_.size() - 1;
		for (const slot& s : old) {
			if (s.value != empty) { place(s.hash, s.value); }
		}
	}
	static_assert(sizeof(slot) == 8);
	std::vector<slot> slots_;
	std::size_t size_ = 0, mask_ = 0;
};

// A get's decode buffer (format-14-kv step 1): one per thread, reused from get to get across
// every index, so a get allocates nothing once warm; a get nested in another's callback takes
// a buffer of its own. A buffer a large record grew past 256 KiB is released after its get.
class lent_frame {
public:
	using stored_frame = stored_plugin::reader::stored_frame;
	lent_frame() : f_(busy() ? &own_ : &shared()) {
		if (f_ != &own_) { busy() = true; }
	}
	~lent_frame() {
		if (f_ == &own_) { return; }
		busy() = false;
		if (f_->buf.capacity() > (std::size_t(256) << 10)) { std::string().swap(f_->buf); }
	}
	lent_frame(const lent_frame&) = delete;
	lent_frame& operator=(const lent_frame&) = delete;
	// The buffer, holding no frame.
	stored_frame& frame() noexcept {
		f_->clear();
		return *f_;
	}

private:
	static stored_frame& shared() noexcept {
		thread_local stored_frame f;
		return f;
	}
	static bool& busy() noexcept {
		thread_local bool b = false;
		return b;
	}
	stored_frame own_;
	stored_frame* f_;
};

// A logged index's open record and its V (standalone-turn-log D3, D4), shared with the turn
// loop's sealer and watcher so that it outlives the index's moves. The owner appends under `mu`,
// and the loop seals and reports durability under it; V is stored with release.
struct log_state {
	struct closed_record {
		std::uint64_t sequence = 0, through = 0;
		std::string payload;
		kronuz::journal::TurnRecordKind kind = kronuz::journal::TurnRecordKind::Batch;
		std::uint64_t covers = 0;  // a checkpoint's: the group's last record its state includes
	};
	std::mutex mu;
	std::uint64_t group = 0, command_bytes = 0;
	std::uint32_t block = 0;
	std::uint64_t next = 1;  // the sequence the next record takes: the group's last staged plus one
	// The open record: its sequence (0: none open), its transactions and their bytes, and V once
	// it is durable.
	std::uint64_t open = 0, count = 0, through = 0;
	std::string body;
	std::deque<closed_record> closed;                            // records the size cap closed, oldest first
	std::deque<std::pair<std::uint64_t, std::uint64_t>> sealed;  // (sequence, V) staged, not yet durable
	std::atomic<std::uint64_t> synced{0};                        // the last durable sequence
	std::atomic<std::uint64_t> handed{0};                        // the last sequence sealed into a turn
	std::atomic<std::uint64_t> visible{0};                       // V
	std::shared_ptr<std::atomic<std::uint64_t>> readers;         // the readers' V, once reader()
	// The owner's: X of the last checkpoint record staged or recovered, and the command bytes
	// joined since it (D5's second trigger).
	files::position checkpointed;
	std::uint64_t logged_bytes = 0;
	bool writing = false;  // counted in the sealer's writers (D2 item 2)
	// The newest checkpoint record staged or recovered, which a leave hands to the table (D7).
	std::uint64_t last_checkpoint = 0;
	std::string checkpoint_payload;

	// Records of a group one turn seals: seal() hands one, the driver's repeat calls get nothing.
	static constexpr std::size_t turn_records = 1;
	struct joined {
		bool opened = false;    // it opened a record, so the loop is woken
		std::uint64_t full = 0;  // not joined: the open record to wait for, until it is sealed
	};
	// The append (D3, F3), in one critical section: the position {s, j} taken, the command
	// appended, and `applied` set to it. A record that the command would take past the command
	// size closes first, unless the unsealed records already fill a turn: the window of applied
	// transactions not durable stays one barrier plus one write (D4), and their bytes bounded.
	joined join(std::string_view cmd, std::uint64_t v, files::position& applied) {
		const std::lock_guard lock(mu);
		if (count && command::batch_bytes(count + 1, body.size() + command::detail::leb_bytes(cmd.size()) + cmd.size()) > command_bytes) {
			if (closed.size() + 1 >= turn_records) { return {false, open}; }
			closed.push_back({open, through, command::batch_payload(count, body)});
			body.clear();
			count = 0;
		}
		const bool opened = count == 0;
		if (opened) { open = next++; }
		command::add_transaction(body, cmd);
		logged_bytes += cmd.size();
		++count;
		through = v;
		const files::position at{open, count};
		assert(at > applied);  // the applied position never moves back
		applied = at;
		return {opened, 0};
	}
	// A chunk of a transaction past the command size (rocksdb-parity A3): a record of its own,
	// after the open batch closes, with the same turn bound. Only the last moves `applied`, and
	// `v` is the V its durability gives, so V passes the transaction once every chunk is durable.
	joined join_chunk(std::string_view bytes, command::chunk_place p, std::uint64_t v, files::position& applied) {
		const std::lock_guard lock(mu);
		if (count) {
			if (closed.size() + 1 >= turn_records) { return {false, open}; }
			closed.push_back({open, through, command::batch_payload(count, body)});
			body.clear();
			count = 0;
		}
		if (closed.size() >= turn_records) { return {false, closed.back().sequence}; }
		const std::uint64_t s = next++;
		closed.push_back({s, v, command::chunk_payload(p, bytes)});
		logged_bytes += bytes.size();
		if (p == command::chunk_place::last) {
			assert((files::position{s, 1} > applied));
			applied = {s, 1};
		}
		return {true, 0};
	}
	// The sealer (D3), called by the driver each turn: the next whole record, oldest first, if it
	// frames within `room` (turn_framed_bytes bounds the driver's record_cost), and none while the
	// last one handed is not durable, so a turn takes turn_records of the group (D4's window).
	std::optional<kronuz::journal::TurnRecord> seal(std::uint64_t room, bool first) {
		const std::lock_guard lock(mu);
		if (handed.load(std::memory_order_relaxed) > synced.load(std::memory_order_relaxed)) { return std::nullopt; }
		kronuz::journal::TurnRecord r;
		r.group = group;
		r.kind = kronuz::journal::TurnRecordKind::Batch;
		if (!closed.empty()) {
			closed_record& c = closed.front();
			if (!first && turn_framed_bytes(c.payload.size(), block) > room) { return std::nullopt; }
			r.sequence = c.sequence;
			r.kind = c.kind;
			r.covers = c.covers;
			r.payload = std::move(c.payload);
			sealed.emplace_back(c.sequence, c.through);
			closed.pop_front();
			handed.store(r.sequence, std::memory_order_release);
			return r;
		}
		if (!count || (!first && turn_framed_bytes(command::batch_bytes(count, body.size()), block) > room)) { return std::nullopt; }
		r.sequence = open;
		r.payload = command::batch_payload(count, body);
		sealed.emplace_back(open, through);
		body.clear();
		count = 0;
		open = 0;
		handed.store(r.sequence, std::memory_order_release);
		return r;
	}
	// The checkpoint record (D5 step 4) at the next sequence, with every batch durable, so it
	// supersedes only records the head at X covers: the owner holds the lease from the seal on.
	void checkpoint(std::string payload, files::position x, std::uint64_t v) {
		const std::lock_guard lock(mu);
		if (open || !closed.empty() || x.index + 1 != next) { throw std::logic_error("strata: a checkpoint record with batches not yet durable"); }
		checkpoint_payload = payload;
		last_checkpoint = next;
		closed.push_back({next++, v, std::move(payload), kronuz::journal::TurnRecordKind::Checkpoint, x.index});
		checkpointed = x;
		logged_bytes = 0;
	}
	// The watcher's step (D4): the records through `s` are durable, so V covers their
	// transactions. Takes no lease.
	void durable_through(std::uint64_t s) {
		const std::lock_guard lock(mu);
		std::uint64_t v = visible.load(std::memory_order_relaxed);
		while (!sealed.empty() && sealed.front().first <= s) {
			v = sealed.front().second;
			sealed.pop_front();
		}
		if (s > synced.load(std::memory_order_relaxed)) { synced.store(s, std::memory_order_release); }
		publish(v);
	}
	// Replay applied what the log made durable: V follows it.
	void replayed(std::uint64_t v) {
		const std::lock_guard lock(mu);
		publish(v);
	}
	void attach_readers(std::shared_ptr<std::atomic<std::uint64_t>> r) {
		const std::lock_guard lock(mu);
		readers = std::move(r);
		readers->store(visible.load(std::memory_order_relaxed), std::memory_order_release);
	}

private:
	void publish(std::uint64_t v) {
		visible.store(v, std::memory_order_release);
		if (readers) { readers->store(v, std::memory_order_release); }
	}
};

// The turn_group index attribute (D7): the host's identity, then the group, big-endian.
inline files::attribute turn_group_attribute(const kronuz::journal::Identity& host, std::uint64_t group) {
	files::attribute a;
	a.tag = files::turn_group_tag;
	a.bytes.assign(host.begin(), host.end());
	for (int i = 7; i >= 0; --i) { a.bytes.push_back(char(group >> (8 * i))); }
	return a;
}
struct turn_member {
	kronuz::journal::Identity host{};
	std::uint64_t group = 0;
};
inline std::optional<turn_member> turn_group_of(const std::vector<files::attribute>& as) {
	for (const auto& a : as) {
		if (a.tag != files::turn_group_tag) { continue; }
		if (a.bytes.size() != 24) { throw corrupt_index("strata: a turn_group attribute of another size"); }
		turn_member m;
		std::copy(a.bytes.begin(), a.bytes.begin() + 16, m.host.begin());
		for (std::size_t i = 16; i < 24; ++i) { m.group = m.group << 8 | std::uint8_t(a.bytes[i]); }
		return m;
	}
	return std::nullopt;
}

}  // namespace index_detail

namespace detail {
// Whether segments map at open (basic_index_options::eager_segments): off, unless
// STRATA_EAGER_SEGMENTS=1 turns it on, so a whole test run exercises open-time maps.
inline bool eager_segments_default() noexcept {
	static const bool on = [] {
		const char* e = std::getenv("STRATA_EAGER_SEGMENTS");
		return e && *e == '1';
	}();
	return on;
}
// Debug builds: a thread inside guarded() or a noexcept chore maps no input (cold-tier D9).
inline unsigned& no_input_map() noexcept {
	thread_local unsigned depth = 0;
	return depth;
}
struct no_input_map_scope {
	no_input_map_scope() noexcept { ++no_input_map(); }
	~no_input_map_scope() { --no_input_map(); }
	no_input_map_scope(const no_input_map_scope&) = delete;
	no_input_map_scope& operator=(const no_input_map_scope&) = delete;
};
// The buffer of a layout with no inverter: documents wait whole in `buffer_`, whatever the flush
// threads (layers.md L10). Its members exist so the index compiles one way; none runs.
struct no_inverter {
	static constexpr std::uint64_t run_cap = ~std::uint64_t(0);
	flush_charge* memory = nullptr;
	std::uint64_t max_run_bytes = run_cap;
	std::size_t doc_count() const noexcept { return 0; }
	std::uint64_t docid(ordinal_t) const noexcept { return 0; }
	std::uint64_t version(ordinal_t) const noexcept { return 0; }
	std::string_view key(ordinal_t) const noexcept { return {}; }
	std::string_view stored(ordinal_t) const noexcept { return {}; }
	template <class Doc>
	ordinal_t add(std::uint64_t, std::uint64_t, std::string_view, const Doc&) { return 0; }
	template <class Transaction>
	bool fits(const Transaction&) const noexcept { return true; }
	template <class Sink, class Spill>
	void flush(const std::array<std::uint8_t, 16>&, std::span<const ordinal_t>, const builder_settings&, Sink&, Spill&) {}
	template <class Dead, class DocumentRecord, class Emit>
	void digest(format::structure_bits, Dead&&, DocumentRecord&&, Emit&&) const {}
};
template <class E>
concept has_inverter = requires { typename E::inverter; };
template <class E>
struct inverter_of_entry {
	using type = typename E::inverter;
};
// The first layout entry's `inverter`, the buffer that inverts documents as they arrive at one
// flush thread; none: no_inverter.
template <class... Entries>
struct first_inverter {
	using type = no_inverter;
};
template <class E, class... Entries>
struct first_inverter<E, Entries...> {
	using type = typename std::conditional_t<has_inverter<E>, inverter_of_entry<E>, first_inverter<Entries...>>::type;
};
template <class Layout>
struct layout_inverter;
template <class... Entries>
struct layout_inverter<layout<Entries...>> : first_inverter<Entries...> {};
}  // namespace detail

template <class Layout>
struct basic_index_options {
	bool keyed = false;                     // the index holds keys (any bytes, 1 to max_term_bytes); fixed at creation
	// Manifests kept behind the current one (I14). 0 keeps none: damage to the current manifest or
	// newest segment is refused, to be repaired from a replica (or the planned recovery tool). 1 or
	// 2 let a standalone host open(..., to) an older one; its local manifests lost are reported by
	// fallback(), which a host without a journal reports as data loss.
	std::size_t retained_predecessors = 0;
	// The node's indexing budget, if any, and this index's share of it: the buffer flushes
	// when the budget is under pressure, when the buffer passes its share (with a budget, at
	// most the capacity less the flush's working set), or when the version map passes a
	// quarter of the share (D17).
	byte_budget* budget = nullptr;
	std::uint64_t buffer_share = ~std::uint64_t(0);
	// Under pressure the buffer flushes only once it and its version map reach min(buffer_share, budget capacity) / this; 0: on any pressure.
	std::uint64_t pressure_flush_divisor = 4;
	// Buffered entries (dead ones included) a flush waits for: 2^32 - 2, a segment's documents
	// (limits.md), and under the version and docid maps' 32-bit indexes (empty is 2^32 - 1). A
	// transaction with more operations than this is invalid. Tests lower it.
	std::uint64_t buffered_docs_cap = 0xFFFFFFFEull;
	// Chunks a buffer's record lane may hold (at most wbuf::max_chunks, about 32 GiB of record
	// headers). A transaction too large for an empty lane is invalid. Tests lower it.
	std::size_t lane_chunks_cap = wbuf::max_chunks;
	// The node's storage, if any (T5): the index holds its files' bytes and reserves each
	// transaction's estimated bytes at admission and each merge's sources' live bytes
	// before it starts; a transaction that does not fit is rejected with no_space.
	// Both budgets must outlive every index that draws on them.
	byte_budget* storage = nullptr;
	// Merges after each flush by `policy` when set; otherwise only merge() and force_merge().
	bool auto_merge = false;
	// Background merges (D11, D18): the node's executor, if any, runs the policy's merges,
	// which publish at the next refresh or commit; without one they run on the caller's
	// thread. `rate` limits their writes. A merge never waits for room on `budget`: it
	// charges its working memory there as it builds, past the capacity if it must, as a
	// flush does; the pressure that reports makes the buffers flush sooner.
	// Both must outlive every index that uses them.
	executor* background = nullptr;
	rate_limiter* rate = nullptr;
	// A hazard registry (shared-reader D4, D5) the host shares among indexes, which outlives the
	// index and its reader handles. None: reader() gives the index a registry of its own, empty
	// until the first acquire, that grows in chunks to `reader_records` and lives as long as the
	// index or its last handle.
	reader_registry* readers = nullptr;
	// The most records (views held at once) an index-owned registry holds; an acquire past it
	// throws too_many_readers.
	std::size_t reader_records = 4096;
	// Writer threads for inversion (D18): a flush splits the buffer's docid-sorted documents
	// into up to this many contiguous runs and inverts and encodes each on its own thread,
	// one segment per run. Files are created, and ids and accounting kept, on the owner thread;
	// each run's thread then calls File::write_at on its own file, so with flush_threads > 1 an
	// IO's File must allow writes from several threads on distinct files (POSIX and faultfs do).
	std::size_t flush_threads = 1;
	std::size_t flush_min_docs = 4096;  // per run: smaller buffers flush as one segment
	// Obsolete data files leave in steps: truncated from the end `reclaim_step` bytes at a
	// time, then removed, so no one syscall frees more than a step. A commit reclaims about
	// `reclaim_slice` bytes, reclaim() the rest, and admission drains them when space is short.
	// A published merge adds the bytes it obsoleted to a debt, and each publish pays at most
	// `reclaim_cap` of it (a step more at most): one owner call frees at most that, and the
	// queue stays bounded while the merges a call publishes obsolete no more than the cap on
	// average. A larger merge's excess waits as debt that later publishes and commits pay,
	// oldest file first (docs/design/limits.md). With a host executor the owner posts whole
	// files to it (truncate in steps, sync, unlink) and returns; without one they run inline.
	std::uint64_t reclaim_step = 16 << 20;
	std::uint64_t reclaim_slice = 64 << 20;
	std::uint64_t reclaim_cap = 64 << 20;
	merge_policy policy;
	// Reordering at merge (docs/design/reordering.md D4): `force_merge` reorders the merges
	// force_merge() runs, as Lucene's BP reorderer does; `merges` also every merge whose
	// output holds at least `reorder_min_docs` live documents.
	enum class reorder_mode { off, force_merge, merges };
	reorder_mode reordering = reorder_mode::force_merge;
	std::uint64_t reorder_min_docs = 262144;
	typename order_source<Layout>::settings reorder_settings;  // the layout's order source's, if it has one
	// The stored blocks' disk preset (format-14-kv Q2): merged output in zstd level 1, flushes
	// in the layout's codec (LZ4 for the document layout). Off: every block in the layout's.
	bool zstd_merged = false;
	// The seed of the segments' identities (F13-R41); none: a random one per open. Tests that
	// compare two indexes' files give both the same seed.
	std::optional<std::uint64_t> identity_seed;
	// A directory on a filesystem not known local (a network or FUSE mount), or one whose
	// lock is unsupported: `warn` opens it and reports it (basic_index::filesystem_warning);
	// `refuse` refuses with errc::not_supported, except a FUSE mount's type (F20). The
	// filesystem is IO::directory_filesystem's: an IO that does not classify is not local,
	// so a wrapper must forward it for `refuse` to see the real filesystem.
	enum class nonlocal_policy { warn, refuse };
	nonlocal_policy nonlocal = nonlocal_policy::warn;
	// A directory the IO admitted though another user owns it or group or others can write it
	// (PosixIOOptions::allow_shared_directory): `warn` proceeds and reports it
	// (basic_index::directory_trust_warning); `refuse` refuses with errc::permission_denied.
	// A symlinked root is neither and proceeds silently. IO::directory_trust is the source: an
	// IO that does not inspect its directory is not trusted, so a wrapper must forward it.
	enum class trust_policy { warn, refuse };
	trust_policy directory_trust = trust_policy::warn;
	// Maps every segment at open. Off (the default), a segment maps at its first read (cold-tier
	// D2) and the failure rules of D9 apply to it. Tools that read every segment, check() and
	// installs may set it (Q3). defer_io forces it on.
	bool eager_segments = detail::eager_segments_default();
	// Host mode (docs/mechanisms/defer-io.md): apply() makes no I/O call. Where it would flush the
	// buffer or drain reclaimable files, it answers flush_owed or drain_owed with nothing applied;
	// the host runs refresh() or reclaim_for_space() (a chore, off the thread that applies) and
	// applies again. Chores a check-in left owed stay owed (chores_owed()) for the host's
	// check_in(). An owed answer needs a flush or a drain that can change the outcome, and the
	// retry after a drain gets no_space instead of a second drain_owed. It covers apply() only, and
	// it forces eager_segments on: a segment maps at open, never at a first read inside apply(). The
	// host retries an owed transaction before it applies any later position, or the retry is skipped.
	// A demoted index (demote()) answers map_owed until promote() maps its bodies again.
	bool defer_io = false;
	// Standalone durability (docs/design/standalone-turn-log.md): the host's turn log and this
	// index's group in it; the log outlives the index. Set, the index is logged: a transaction
	// applied with no position joins the group's open record at the position it takes (D3), and
	// readers see it once its turn's barrier returns (D4). A layout whose parts lack the command
	// codec's hook cannot set it.
	std::conditional_t<command::loggable<Layout>, turn_sealer*, std::nullptr_t> log = nullptr;
	std::uint64_t log_group = 0;
	// A logged record's payload cap (D3): a transaction whose command passes it streams as chunk
	// records of at most this many bytes, all or nothing across a crash (rocksdb-parity A3).
	std::uint64_t command_bytes = command::max_bytes;
	// A logged index checkpoints (commit()) when the commands logged since its last checkpoint
	// pass its share of the log's window, turn_sealer::window_bytes() over its writers, and at
	// least this floor (D5's second trigger, rocksdb-parity D2 item 2; not under defer_io).
	std::uint64_t checkpoint_floor_bytes = 1 << 20;
	// Tests: runs between a logged apply's stage 1 and its append (T12).
	std::function<void()> log_seam;
};

// An open that fell back to a generation older than the newest on disk (open's `to`): the commits
// after `to` are gone from this index's logs. `newest` is the newest generation the logs hold, or
// the first damaged one; `discarded` is `newest - to`. A merge's generation holds no new documents,
// a flush's or delete's does.
struct fallback_report {
	std::uint64_t to = 0;      // the generation opened
	std::uint64_t newest = 0;  // the newest generation found on disk, or the damaged one
	std::uint64_t discarded = 0;
};

// Why an index's directory lock may not exclude every other writer: its filesystem is not
// known local, so the lock excludes local writers only, or it could not be locked at all.
struct filesystem_warning {
	std::string filesystem;
	bool local = false;
	bool locked = false;  // false: two writers are not excluded
};

// Why an index's directory may be written by someone else: another user owns it, group or
// others can write it, or the IO did not say (`known` false, the flags then unset).
struct directory_trust_warning {
	bool known = true;
	bool foreign_owner = false;
	bool shared_writable = false;
};

// An index of a layout's segments (layers.md L2); prospect's `index` is its search layout's.
template <class Layout>
class basic_index {
public:
	using options = basic_index_options<Layout>;
	using document = basic_document<Layout>;
	using operation = basic_operation<Layout>;
	using transaction = basic_transaction<Layout>;
	using segment = basic_segment<Layout>;
	// The buffer that inverts documents as they arrive, if the layout has one (prospect's does).
	using inverter = typename detail::layout_inverter<Layout>::type;
	static_assert(!segment_policy<Layout>::key_order || std::is_same_v<inverter, detail::no_inverter>, "a key-ordered layout flushes its whole documents by key");

	// Creates an empty index and commits its first manifest, in an empty directory or over
	// what an interrupted create left (removed first); any other strata file is refused.
	// The owner lock is the directory's own (D2, lock_directory): busy is EWOULDBLOCK, and
	// a refusal writes nothing. The directory's entry in its parent is the host's
	// to make durable; one parent sync can cover many creates (SI-R21).
	static basic_index create(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid);

	// A create that writes nothing until its first commit (D3-C): the directory lock, an
	// interrupted create's residue cleared, the state of an empty index in memory. Until that
	// commit an open finds no index here (strata::group recreates it from its log, D112).
	static basic_index create_deferred(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid);

	// Opens the highest durable manifest (or `to`, committed again as the newest), fails
	// closed when a file it names is missing or differs, then deletes orphans. An open at a
	// `to` older than the newest manifest is a fallback and says so in fallback(), never silently.
	static basic_index open(kronuz::journal::IO& io, const options& opts = {}, std::optional<std::uint64_t> to = std::nullopt);

	// A moved index keeps its readers' shared state: handles follow it (D11).
	basic_index(basic_index&&) noexcept = default;
	// The target runs its destructor first (its merges' jobs and its views let go under its
	// lease: SR-D5, SR-D2), then takes o's state.
	basic_index& operator=(basic_index&& o) noexcept;
	// Runs close()'s first half (SR-D2): merges ended, the current view retired, released views
	// freed. A view readers still protect is a host bug, since a finished close is its duty: it
	// is reported (an abort in debug builds) and leaks with its segments and the directory lock,
	// so no reader faults and no reopen in this process truncates its files.
	~basic_index();

	// Releases the owner lock and drops all state; uncommitted work is lost, except that a logged
	// index first leaves its group (D7), so its head covers every logged batch. Background
	// merges queued are dropped and running ones stopped first. The index is fenced
	// afterwards. Never waits for readers (D11): views they still protect keep their segments,
	// files, and the directory lock, and the index is closing() until try_finish_close() frees
	// the last of them. Readers' acquires throw index_closed from here on.
	void close();

	// A closing index (D11): frees the views readers released since, and once none is held
	// releases its files' accounting and the directory lock. Past `deadline` (the executor's
	// clock) with views still held, throws readers_active naming their records. True once
	// closed; false while closing.
	bool try_finish_close(std::optional<std::uint64_t> deadline = std::nullopt);
	bool closing() const noexcept { return pub_ && pub_->closing; }

	// --- writer ---------------------------------------------------------------------------

	// Applies a transaction at a log position. Positions at or below the applied one are
	// skipped (replay). A transaction that stage 1 rejects, or whose stage 1 throws (a segment read
	// that fails, an allocation), is rolled back through the undo log: its slots, pending deletes,
	// records, metadata and storage reservation. The exception is rethrown with the position unmoved.
	// A logged index (options::log) given no position logs an accepted transaction (standalone-turn-log
	// D2, D3): its command joins the group's open record, or past options::command_bytes streams as
	// chunk records (rocksdb-parity A3), and the position it takes there becomes the applied one.
	verdict apply(const transaction& t, files::position at);

	// A logged index's V (D4): what its log made durable, and its group's last durable sequence.
	std::uint64_t logged_visible() const noexcept { return logged_ ? logged_->visible.load(std::memory_order_acquire) : visible_; }
	std::uint64_t log_synced() const noexcept { return logged_ ? logged_->synced.load(std::memory_order_acquire) : 0; }
	bool logged() const noexcept { return bool(logged_); }
	// Any thread: returns once the log is durable through the position apply() gave, the
	// acknowledgement of a logged transaction (D4). A failed turn throws outcome_unknown (D9).
	// A wait whose index closes first (its group leaves the log), or that names a position of
	// the group's life before a close and reopen, throws index_closed (journal's group_closed):
	// a close that left (D7) made every accepted transaction durable in the head, so the
	// transaction is durable; after a close that could not leave, an open's recovery tells.
	// Reads only the options, so a close on the owner thread does not race it.
	void await_durable(files::position at) const;

	// Undoes a transaction's stage 1: its slots, pending deletes and records, its documents and
	// its storage reservation. Kept rejected records stay counted against the budget.
	void abandon(std::uint64_t reserve);

	// A client's hold on the index for one request (write-buffer D2): never exclusive, never
	// waits. Its end checks the index in (check_in(), with its thread condition). It names the
	// index by address: the index must outlive it and not move while it lives.
	class checkout {
	public:
		checkout(checkout&& o) noexcept : ix_(std::exchange(o.ix_, nullptr)) {}
		checkout& operator=(checkout&&) = delete;
		~checkout() {
			if (ix_) { ix_->check_in(); }
		}
		basic_index& index() const noexcept { return *ix_; }

	private:
		friend class basic_index;
		explicit checkout(basic_index& ix) noexcept : ix_(&ix) {}
		basic_index* ix_;
	};
	checkout check_out() noexcept { return checkout(*this); }

	// Runs the chores owed while no writer runs (D2's writerless group): finished merges are
	// published and retired views freed, under the lease when it is free. When a writer holds it,
	// the chores stay owed and that writer's call runs them. Never throws: a failed chore fences
	// the index, and the next writer call reports it. Off the owner's thread it is safe only while
	// no thread reads the index outside a writer call (get, scan, find_key, has_docid, segments,
	// digest, buffered_count take no lease; debug builds report the overlap); reads off the
	// owner's thread go through views (reader(), D5), which no check-in races (CR3).
	void check_in() noexcept;
	// Whether a check-in found the lease held and left its chores to the holder.
	bool chores_owed() const noexcept { return chores_owed_.load(std::memory_order_acquire); }
	// Whether a durability failure fenced the writer: every writer call throws index_fenced until the host
	// reopens the index. Read under the lease (a chore or a holder's call).
	bool fenced() const noexcept { return fenced_; }
	// Whether a published merge's output awaits the next commit's sync with nothing else uncommitted: warm
	// (demote(), close_ready()) cannot hold until a commit makes it durable. Read under the lease.
	bool merge_output_unsynced() const noexcept;
	// A host that owns the index's lifetime (the catalog) registers the word that says whether a caller
	// holds it: in debug builds every writer call checks `held(*word)` and reports a breach (CT-R15).
	using call_guard = bool (*)(std::uint64_t) noexcept;
	void guard_calls(const std::atomic<std::uint64_t>* word, call_guard held) noexcept { guard_word_ = word, guard_held_ = held; }

	// Writes the buffer as a segment (searchable, not yet durable). ENOSPC leaves the
	// buffer in place to retry once space returns; other failures fence. A deferred index's first
	// refresh with documents syncs: it commits the empty first generation (commit_empty()).
	void refresh();

	// Merges the given segments into one (searchable, not yet durable), waiting for it: their
	// live documents in docid order, deleted ones dropped. Nothing is written when none is
	// live; no room for the output throws, after the merge leaves no trace.
	void merge(const std::vector<std::uint64_t>& ids);

	// Merges until at most `max_segments` remain and none holds deletes, by the policy's
	// forced merges. With an executor it waits for every background merge, then for its own.
	// The buffer's pending deletes count as deletes, as in merge().
	void force_merge(std::size_t max_segments = 1);

	// Merges away the segments whose deleted fraction passes the policy's
	// force_merge_deletes_pct_allowed, waiting like force_merge().
	void force_merge_deletes();

	// Runs the policy's merges until it asks for none, or until storage admission has no
	// room for the next; those wait for space. With an executor it hands one round to it,
	// leaving out segments already under merge or damaged, and returns.
	void maybe_merge();

	// Waits for this index's background merges and publishes them. One that ran out of
	// space is discarded, its sources left for a later merge; any other failure fences.
	void wait_merges();
	// Background merges handed to the executor and not yet published.
	std::size_t merges_pending() const noexcept { return pending_.size(); }
	// Whether a background merge found a damaged segment, or a first map failed its checks
	// (cold-tier D11); merges skip the first until heal(), and a commit throws corrupt_segment
	// at the second until it.
	bool needs_heal() const noexcept;

	// Makes everything applied durable (design D1), then reclaims what retention allows.
	// ENOSPC from a write before that file's sync leaves the index as it was (the partial
	// file removed); any other failure, and any failed sync, rename, or directory sync,
	// fences the writer.
	// With readers, the order is flush, publish, scan and free, retain(), reclaim() (SR-D6): the
	// view this commit retires, which holds the sources of the merges it installed, is freed
	// first when no guard names it, so those sources are queued in the same commit.
	// A logged index's commit is its checkpoint (standalone-turn-log D5): the log durable through
	// the applied position X before the edit (I1), then the checkpoint record staged once the edit
	// is durable (I2), all under one lease, so no batch record lands between X and the record.
	void commit();

	// Frees queued obsolete files, about `bytes` of them, each truncated from its end in
	// `reclaim_step` steps and then removed. Returns the bytes freed. Views readers released
	// are freed first, and the files only they held are queued then (D6), so an idle index
	// returns a released view's space here, not at its next commit. With a host executor the
	// files are posted to it, and the bytes returned are the ones posted.
	std::uint64_t reclaim(std::uint64_t bytes = ~std::uint64_t(0));
	std::size_t reclaim_pending() const noexcept { return reclaim_.size(); }
	// The chore behind drain_owed (options::defer_io): reclaims every queued file, waits for the
	// posted deletes and syncs the directory, as storage admission does when space is short. True
	// when something was reclaimed or removed. Answering drain_owed ends with this call: the next
	// apply then answers no_space if the room is still short.
	bool reclaim_for_space();
	// Owner thread. Waits for the deletes posted to the host's executor and takes their
	// outcomes; a failed one is queued again. It does not sync the directory, so the permit
	// still counts the removed bytes until the next commit or drain(). Without an executor
	// there is nothing to wait for.
	void wait_deletes();
	// Files posted for deletion whose outcome the owner has not yet collected.
	std::size_t deletes_pending() const noexcept { return ledger_.posted().size(); }

	// The kept manifest (a pin or a retained predecessor) whose read failed in the last
	// retention round, which then removed nothing; retention resumes once it reads or the
	// pin is released. A host reports it: a damaged pinned checkpoint stops reclamation.
	std::optional<std::uint64_t> retention_blocked() const noexcept { return retention_blocked_; }

	// Set when this open fell back to a manifest older than the newest on disk: the generation
	// opened, and the newer manifests (`discarded`, those found on disk) dropped. A standalone host
	// reports it as data loss; under a journal that replays above `to`, nothing is lost. In memory only.
	const std::optional<fallback_report>& fallback() const noexcept { return fallback_; }

	// Set when the directory's lock may not exclude every other writer: under `warn`, a
	// filesystem not known local, its classification failed, or its lock unsupported; under
	// `refuse`, a FUSE mount (never refused by its type). A host reports it.
	const std::optional<strata::filesystem_warning>& filesystem_warning() const noexcept { return fs_warning_; }
	// Set when the directory is not trusted and options::directory_trust is warn (a symlinked root
	// alone sets nothing); read as filesystem_warning is.
	const std::optional<strata::directory_trust_warning>& directory_trust_warning() const noexcept { return trust_warning_; }

	// Reads that failed or failed their checks once and passed when read again.
	std::uint64_t media_retries() const noexcept { return load_->retries.load(std::memory_order_relaxed); }
	// Merge rounds maybe_merge() dropped because mapping a source failed for a reason of the host
	// (EMFILE, ENOMEM, ...): each leaves its segments to pile up until a round runs (cold-tier D9).
	std::uint64_t merges_skipped() const noexcept { return load_->skipped.load(std::memory_order_relaxed); }

	// After a search meets corrupt_segment: verifies every committed segment in memory, and
	// reads one that fails again from its file, verified whole, in its place. Returns the ids
	// whose file is damaged too, for the host to fence and repair from a checkpoint or a
	// replica. Segments a background merge found damaged, or whose first map failed its checks,
	// are among those checked, and merges take them again once they pass. A segment never read
	// has nothing to check (cold-tier D9). Like a merge, it invalidates segment views.
	std::vector<std::uint64_t> heal();

	// --- warm (cold-tier D3, D10) -------------------------------------------------------------

	// Demotion to warm, a chore under the lease: every record becomes a twin with no body, a
	// publication replaces the view over the old records, and the views no reader holds are freed,
	// so the index keeps no mapping once the last guard releases. Nothing is written: warm needs no
	// commit. False, and nothing changed (not the flag nor the bit), when a record is buffered, a
	// merge is pending, a segment is unsynced, or a map_owed turn is outstanding (CT-R22). Under
	// defer_io a twin that can still map sets the flag: apply answers map_owed until promote().
	bool demote();
	// Whether close() would lose nothing and leave nothing unaccounted (cold-tier D10, CT-R4), a
	// chore under the lease: warm's predicate, nothing uncommitted, and, after a non-blocking
	// collect of the posted deletes' outcomes, no posted delete, an empty reclaim queue and no
	// debt, so that close() releases the storage permit only for bytes no queue still owes.
	// False asks the host to commit, reclaim() or collect and ask again later; it changes nothing
	// but that collect, and never waits.
	bool close_ready();
	// The wake chore of a demoted defer_io index: maps every record without a body, so the next
	// apply makes no device call. A record whose checks fail takes the marker and counts as done
	// (D9); a host error (EMFILE, ENOMEM) rethrows unchanged with the bodies mapped so far kept, the
	// flag set and nothing fenced. It writes and publishes nothing. With the flag clear it does
	// nothing, and makes no device call.
	void promote();
	// Whether anything applied since the last commit is still uncommitted, a chore under the lease
	// (the catalog commits before it closes a slot at the wake's bound, cold-tier D5).
	bool uncommitted() const noexcept { return applied_since_commit_ != 0; }
	// Whether apply answers map_owed: set by demote() under defer_io, cleared by promote(). Readable
	// without the lease (a stale read is harmless: apply answers map_owed anyway).
	bool promote_owed() const noexcept { return promote_owed_.load(std::memory_order_acquire); }
	// The write lease, for the catalog's chores (cold-tier D4): a chore takes a chore_scope on it, and
	// a chore pin asks held_here() whether its thread holds the lease.
	const write_lease& lease() const noexcept { return lease_; }
	// Input maps made on a thread that did not hold this index's lease: a reader's, a lease-less
	// read's, or one made inside another index's chore (I5-N3). A host that reads only after
	// promote() sees it stay still (cold-tier CT-R23).
	std::uint64_t unleased_maps() const noexcept { return load_->unleased.load(std::memory_order_relaxed); }
	// Maps whose body lost the install race to another thread's (cold-tier D2): the loser's is destroyed.
	std::uint64_t map_losses() const noexcept { return load_->lost.load(std::memory_order_relaxed); }

	// --- reading --------------------------------------------------------------------------

private:
	struct open_segment;

public:
	// A segment of a list or a view: its record, whose body maps on the first call of seg()
	// (cold-tier D2). seg() throws corrupt_segment for a segment that failed its checks and the
	// host's error (EMFILE, ENOMEM, ...) unchanged when the map fails for a reason of the host.
	struct segment_view {
		std::uint64_t id;
		const open_segment* rec;
		const deleted_bits* deleted;  // bit per ordinal; null when none is deleted
		std::uint64_t dead = 0;                      // bits set in deleted
		const segment* seg() const { return &rec->seg(); }
		bool live(ordinal_t ord) const { return !deleted || !deleted->test(ord); }
	};
	// The segments as one list of views, holding the bits it made for the pending deletes.
	class segment_list {
	public:
		using value_type = segment_view;
		const segment_view* begin() const noexcept { return views_.data(); }
		const segment_view* end() const noexcept { return views_.data() + views_.size(); }
		const segment_view* data() const noexcept { return views_.data(); }
		std::size_t size() const noexcept { return views_.size(); }
		bool empty() const noexcept { return views_.empty(); }
		const segment_view& operator[](std::size_t i) const noexcept { return views_[i]; }
		const segment_view& at(std::size_t i) const { return views_.at(i); }

	private:
		friend class basic_index;
		std::vector<segment_view> views_;
		std::vector<deleted_bits> bits_;  // reserved whole first: views_ point into it
	};
	// Null bitmaps for segments without deletes, so searches skip the live check without
	// scanning the bitmap. The pending deletes are applied, as the index's own reads apply them
	// (write-buffer D6): a segment that has one gets a private copy of its bits.
	segment_list segments() const;
	// A snapshot holds no buffer, so its scan needs only the keys and the records.
	static constexpr bool snapshot_scannable() { return segment::template contains<keys_plugin>() && segment::template contains<stored_plugin>(); }
	// Segments as of one moment, oldest first, as segments() lists them and a shared reader's view
	// holds them (docs/design/shared-reader.md D8): get and scan here run the index's own segment
	// code with no buffer. It reads each segment's bytes and the bits and dead count its
	// segment_view gives, nothing of the index, and is valid while those are.
	class snapshot {
	public:
		explicit snapshot(std::span<const segment_view> segs) noexcept : segs_(segs) {}
		std::span<const segment_view> segments() const noexcept { return segs_; }
		std::optional<std::string> get(std::string_view key) const
			requires(segment::template contains<stored_plugin>())
		{
			std::optional<std::string> out;
			get(key, [&](std::string_view v) { out.emplace(v); });
			return out;
		}
		template <class F>
		bool get(std::string_view key, F&& f) const
			requires(segment::template contains<stored_plugin>())
		{
			return get_segments(segs_, key, f);
		}
		template <class F>
		std::size_t scan(std::string_view from, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_segments(segs_, {}, nullptr, from, nullptr, ~std::size_t(0), f);
		}
		template <class F>
		std::size_t scan(std::string_view from, std::size_t n, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_segments(segs_, {}, nullptr, from, nullptr, n, f);
		}
		template <class F>
		std::size_t scan(std::string_view from, std::string_view to, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_segments(segs_, {}, nullptr, from, &to, ~std::size_t(0), f);
		}
		template <class F>
		std::size_t scan(std::string_view from, std::string_view to, std::size_t n, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_segments(segs_, {}, nullptr, from, &to, n, f);
		}

	private:
		std::span<const segment_view> segs_;
	};
	// --- shared readers (docs/design/shared-reader.md D1, D4, D6, D8, D9) ----------------------

private:
	struct record;
	struct buffer;

public:
	// One published generation (D1): the segments, their live bits frozen (D3), and the fields
	// readers need from the manifest. Built whole under the write lease, never written again,
	// freed under the lease once no hazard record names it (D6). Its reads are a snapshot's.
	class view;
	// A view read at a position r at or above its own (write-buffer D5): the buffers' records at
	// or below r, then the segments with the view's bits, each copy also checked by docid
	// against the active buffer (D6). Valid while its view is.
	class reading {
	public:
		reading(const view& v, std::uint64_t r) noexcept : v_(&v), r_(r) {}
		std::uint64_t position() const noexcept { return r_; }
		std::optional<std::string> get(std::string_view key) const
			requires(segment::template contains<stored_plugin>())
		{
			std::optional<std::string> out;
			get(key, [&](std::string_view v) { out.emplace(v); });
			return out;
		}
		template <class F>
		bool get(std::string_view key, F&& f) const
			requires(segment::template contains<stored_plugin>())
		{
			return get_at(*v_, key, r_, f);
		}
		template <class F>
		std::size_t scan(std::string_view from, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_at(*v_, r_, from, nullptr, ~std::size_t(0), f);
		}
		template <class F>
		std::size_t scan(std::string_view from, std::size_t n, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_at(*v_, r_, from, nullptr, n, f);
		}
		template <class F>
		std::size_t scan(std::string_view from, std::string_view to, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_at(*v_, r_, from, &to, ~std::size_t(0), f);
		}
		template <class F>
		std::size_t scan(std::string_view from, std::string_view to, std::size_t n, F&& f) const
			requires(snapshot_scannable())
		{
			return scan_at(*v_, r_, from, &to, n, f);
		}

	private:
		const view* v_;
		std::uint64_t r_;
	};

	class view {
	public:
		std::span<const segment_view> segments() const noexcept { return views_; }
		// Reads at the view's own position (D5).
		reading at() const noexcept { return reading(*this, visible_); }
		// Reads at r, at or above the view's position: what a live reader passes (D5).
		reading at(std::uint64_t r) const noexcept { return reading(*this, std::max(r, visible_)); }
		std::optional<std::string> get(std::string_view key) const
			requires(segment::template contains<stored_plugin>())
		{
			return at().get(key);
		}
		template <class F>
		bool get(std::string_view key, F&& f) const
			requires(segment::template contains<stored_plugin>())
		{
			return at().get(key, f);
		}
		template <class... A>
		std::size_t scan(A&&... a) const
			requires(snapshot_scannable())
		{
			return at().scan(std::forward<A>(a)...);
		}
		// The visible position the view reflects (D5).
		std::uint64_t visible() const noexcept { return visible_; }
		const std::map<std::string, std::string>& metadata() const noexcept { return *meta_; }
		std::uint64_t last_docid() const noexcept { return last_docid_; }
		std::uint64_t schema_version() const noexcept { return schema_version_; }
		format::structure_bits configured_structures() const noexcept { return configured_; }
		bool keyed() const noexcept { return configured_ & format::bit::keys; }
		std::uint64_t generation() const noexcept { return generation_; }
		files::position position() const noexcept { return position_; }
		// The index's digest at the moment of publication, when its buffer was empty.
		std::uint64_t digest() const { return digest_of(views_, configured_, last_docid_, schema_version_, *meta_, *fields_, [](auto&, auto&) {}); }
		// The field table as of the view's publication (fields-and-keys D2).
		const field_table& fields() const noexcept { return *fields_; }

	private:
		friend class basic_index;
		std::vector<std::shared_ptr<const open_segment>> keep_;
		std::vector<deleted_bits> bits_;  // shares of the segments' bits; empty where none is deleted
		std::vector<segment_view> views_;
		std::shared_ptr<const std::map<std::string, std::string>> meta_;  // shared until the owner changes it
		std::shared_ptr<const field_table> fields_;                         // likewise
		std::uint64_t fields_epoch_ = 0;
		std::shared_ptr<const buffer> active_;               // null in an inverting layout (D11)
		std::vector<std::shared_ptr<const buffer>> frozen_;  // oldest first
		std::uint64_t visible_ = 0;                          // its position (D5)
		std::uint64_t last_docid_ = 0, schema_version_ = 0, generation_ = 0;
		format::structure_bits configured_ = 0;
		files::position position_;
	};
	// A view a reader protects; release it on any thread, by destruction or release().
	using view_guard = reader_registry::guard<view>;

private:
	// What reader handles and the index share (D4), so moving the index moves nothing a reader
	// points at. The owner stores the view's pointer, then the position and generation, all with
	// release, the position under a sequence: `seq` is twice the generation, odd while it stores.
	struct reader_state {
		reader_state(reader_registry* r, std::unique_ptr<reader_registry> o) noexcept : registry(r), own(std::move(o)) {}
		reader_registry* registry;
		std::unique_ptr<reader_registry> own;  // the index's registry, when the host gave none
		std::atomic<const view*> current{nullptr};
		std::atomic<std::uint64_t> seq{0};
		std::atomic<std::uint64_t> at_index{0}, at_applied{0};
		std::atomic<std::uint64_t> visible{0};  // V (write-buffer D5): the lease holder stores it, a logged index's watcher
		std::function<void(int)> probe;  // tests: called between the publication's stores
	};

public:
	// A shared reader (D4): usable from any thread. Copying one is a read-modify-write on the
	// shared state's count, so a host keeps one per index and lends it by reference; acquires
	// never touch the count.
	class reader_handle {
	public:
		reader_handle() noexcept = default;
		// The current view, protected: throws index_closed once the index closed, and
		// too_many_readers when every record of the registry is claimed. Never blocks.
		view_guard acquire() const {
			view_guard g = s_->registry->acquire(s_->current);
			if (!g) { throw index_closed("strata: the index is closed"); }
			return g;
		}
		// The position of the newest view published, with no acquire: a view acquired after
		// this returns reflects it or a later one (SR-D15).
		files::position published_position() const noexcept {
			for (unsigned spins = 0;; ++spins) {
				if (const auto p = try_published_position()) { return *p; }
				if (spins >= 64) { std::this_thread::yield(); }
			}
		}
		// The same, or none while the owner is storing it.
		std::optional<files::position> try_published_position() const noexcept {
			const std::uint64_t s1 = s_->seq.load(std::memory_order_acquire);
			if (s1 & 1) { return std::nullopt; }
			const files::position p{s_->at_index.load(std::memory_order_acquire), s_->at_applied.load(std::memory_order_acquire)};
			if (s_->seq.load(std::memory_order_acquire) != s1) { return std::nullopt; }
			return p;
		}
		// The newest generation published.
		std::uint64_t generation() const noexcept { return s_->seq.load(std::memory_order_acquire) >> 1; }
		// V, with acquire: every transaction at or below it is visible whole (write-buffer D5).
		std::uint64_t visible() const noexcept { return s_->visible.load(std::memory_order_acquire); }
		// A live read (D5): V loaded first, then a view acquired, read at max(V, its position).
		struct live_view {
			view_guard guard;
			reading at;
		};
		live_view live() const {
			const std::uint64_t v = visible();
			view_guard g = acquire();
			const reading r = g->at(v);
			return {std::move(g), r};
		}
		explicit operator bool() const noexcept { return bool(s_); }

	private:
		friend class basic_index;
		explicit reader_handle(std::shared_ptr<reader_state> s) noexcept : s_(std::move(s)) {}
		std::shared_ptr<reader_state> s_;
	};

	// Starts publishing views, under the write lease (D4), and returns a handle on them. The first
	// call refreshes, so the handle never starts empty (D1). Uses options::readers, or a registry of its own.
	reader_handle reader();

	// Under the write lease (D9, SR-D20): refreshes only when the current view is behind `p`.
	// Each holder checks after it takes the lease, so of the calls the host's lease order queues
	// behind one that refreshed, none flushes again.
	void publish_through(files::position p);

	// The views not yet freed (current and retired), and the bytes of the segments only they
	// hold (SR-D7). A view a reader released is freed at the owner's next publication, commit,
	// reclaim(), or try_finish_close().
	struct views_report {
		std::size_t views = 0;
		std::uint64_t bytes = 0;
	};
	views_report views_held() const;

	// The durable head: the state the manifest log's last record describes (manifest-log D1).
	const files::log_state& manifest() const noexcept { return m_; }
	std::uint64_t last_docid() const noexcept { return cur_.last_docid; }
	// The applied position, committed or not; the head's is manifest().applied().
	files::position applied() const noexcept { return cur_.applied; }
	std::uint64_t schema_version() const noexcept { return cur_.schema_version; }
	const std::map<std::string, std::string>& metadata() const noexcept { return meta_; }
	// The field table as of the last applied transaction (never an in-flight one: apply runs on the
	// writer's thread), with its mark for `expected_fields` (fields-and-keys D2).
	const field_table& fields() const noexcept { return fields_; }
	field_mark fields_mark() const noexcept { return fields_.mark(); }
	std::size_t buffered_count() const noexcept;
	// The replicated log's term that produces this replica's segments from now on (F6,
	// F13-R41; durability D8): the host sets it when its leader's term changes; 0 standalone.
	void set_producer_term(std::uint64_t term) noexcept { producer_term_ = term; }

	// --- deterministic core -------------------------------------------------------------

	// The logical-content digest (design D12; format 13's split, L8 and F21): the sum modulo
	// 2^64 of xxHash64 over records. The core's D record per live document holds its docid,
	// version, and key; each configured plugin adds its own records; the core adds
	// `lastdocid`, the schema version, and each metadata entry. Equal content gives equal
	// digests whatever its segment layout. It checks at most one live copy per docid and per
	// key, every docid at or below `lastdocid`, and throws std::logic_error otherwise.
	std::uint64_t digest() const;

	// The structures this index is configured with, from the manifest (F20, F13-R28). Flushes
	// and merges write them; the digest records them (F21).
	format::structure_bits configured_structures() const noexcept { return cur_.configured; }

	// Checks every segment whole (check_segment) and the digest's invariants: one live copy
	// per docid and per key, every docid at or below lastdocid. Throws at the first problem.
	basic_check_report<Layout> check() const;

	// Commits and pins the manifest; the generation names the checkpoint (design D4).
	std::uint64_t checkpoint();

	// Unpins a checkpoint; retention reclaims it once nothing else keeps it.
	void release(std::uint64_t generation);

	static files::log_state read_generation(kronuz::journal::IO& io, std::uint64_t g) { return index_detail::read_state(io, g, readable()); }

	// Replaces this index's content with the checkpoint `generation` of the index at
	// `source` (design D5). The checkpoint is read and checked whole, as an open checks its
	// head, before anything here changes: a bad checkpoint or a failure reading it throws
	// and leaves this index as it was, unfenced. Storage admission reserves the copy
	// (ENOSPC otherwise). The files are written under fresh local names and commit as a
	// manifest above every existing generation; the buffer, pending deletes, and version
	// map are discarded, and the checkpoint's last docid, schema version, applied position,
	// and UUID are adopted. A crash at any point opens the old content or the installed one.
	// A failure reopening a written copy (EIO as corrupt_index, or a host error as itself)
	// leaves the index as it was, unfenced, the copies orphaned for retention to remove.
	void install(kronuz::journal::IO& source, std::uint64_t generation);

	// The live document's (docid, version) for a key, if any.
	std::optional<std::pair<std::uint64_t, std::uint64_t>> find_key(std::string_view key) const;
	// The live document's stored record for a key, if any: the buffered copy, else the newest
	// segment that holds the key live. For layouts with the `stored` plugin. It allocates the
	// string returned and nothing else (format-14-kv K4).
	std::optional<std::string> get(std::string_view key) const
		requires(segment::template contains<stored_plugin>());
	// The same, lending the record to f(std::string_view) for the call, and whether the key
	// is live. A segment's block decodes into this thread's buffer, reused from get to get,
	// so nothing is allocated; a get inside f decodes into a buffer of its own.
	template <class F>
	bool get(std::string_view key, F&& f) const
		requires(segment::template contains<stored_plugin>());
	// The live document's version for a docid, if any: the buffers and the segments, with no
	// flush. It reads as apply() does, so the active buffer's docid map answers it (WB-23).
	std::optional<std::uint64_t> find_docid(std::uint64_t docid) const;
	// Builds no docid map (WB-23): a buffer without one is walked, newest record first.
	bool has_docid(std::uint64_t docid) const;

	// Whether scan applies to this layout.
	static constexpr bool scannable();

	// The live records with keys >= from, in key order: the buffer's and each segment's, merged
	// by key. f(key, value, version) gets views valid for the call and may return false to stop
	// (a void f runs to the end). Returns how many calls it made, the one that returned false
	// included. The forms with n visit at most n records, and n == 0 visits none; the forms
	// without n run to the end. To resume after a stop, scan from the last key plus '\0'.
	// For layouts with the `keys` and `stored` plugins and no inverter (the buffer's view of an
	// inverting layout waits for a scan test of its own).
	//
	// The scan sees the index as it stands when it starts, and nothing can change it before it
	// ends: an apply, refresh, merge, close, or any other writer call from f throws
	// std::logic_error (the owner core's model: one writer and its readers, no apply between two
	// steps; a scan that survives writes is a view's, D8). A get from f is fine.
	//
	// The lease holder's only, like writes, and so is get: both read the buffer and the segment
	// list, which every writer call changes (a flush appends, a merge replaces, a delete sets a
	// live bit in place), and scan also builds the buffer's sorted view on demand and counts itself
	// in `scanning_`. Other threads read a view (reader()) through the same segment code (D8).
	//
	// At most one live copy of a key exists (format-13 F8), so the merge needs no tombstones:
	// deleted copies are skipped by their live bits and flags. Should a key be live in several
	// sources, the newest wins, the buffer's before the newest segment's, and the older copies
	// are skipped with it.
	template <class F>
	std::size_t scan(std::string_view from, F&& f) const
		requires(scannable());
	template <class F>
	std::size_t scan(std::string_view from, std::size_t n, F&& f) const
		requires(scannable());
	// The same for keys in [from, to).
	template <class F>
	std::size_t scan(std::string_view from, std::string_view to, F&& f) const
		requires(scannable());
	template <class F>
	std::size_t scan(std::string_view from, std::string_view to, std::size_t n, F&& f) const
		requires(scannable());

private:
	friend struct index_test_access;  // tests reach invariants no public call can break

	// Runs the merges `find` asks for, asking again after each round, until it asks none.
	// With an executor they run there, on segments not under merge or damaged: one round
	// without `wait`, otherwise every round, each waited for and published.
	template <class Find>
	void run_merges(Find find, bool wait, bool forced = false);

	explicit basic_index(kronuz::journal::IO& io) : load_(std::make_shared<load_context>(&io)), io_(&io) {}

	// Merges run on the host's executor, or on the index's own inline one, synchronously
	// on the writer's thread, when it passes none: one merge implementation either way.
	void start_merges();

	// The digest over segments (an index's or a view's), then what `buffer` adds: one
	// implementation for both (D8).
	template <class Segs, class Buffer>
	static std::uint64_t digest_of(const Segs& segs, format::structure_bits configured, std::uint64_t last_docid, std::uint64_t schema_version,
		const std::map<std::string, std::string>& meta, const field_table& table, Buffer&& buffer);

	// --- scans (D9) ------------------------------------------------------------------------

	// A buffer's entry order: the key's prefix, the key on a prefix tie, and among one key's
	// records the newest first (position, then reference: one lane's grow with appends).
	struct entry_less {
		const buffer* b;
		bool operator()(const wbuf::pos_entry& x, const wbuf::pos_entry& y) const noexcept {
			if (x.hi != y.hi) { return x.hi < y.hi; }
			if (x.lo != y.lo) { return x.lo < y.lo; }
			if (x.ref == y.ref) { return false; }
			const record &p = b->lane.at(x.ref), &q = b->lane.at(y.ref);
			const int c = std::string_view(p.key()).compare(q.key());
			return c != 0 ? c < 0 : p.position != q.position ? p.position > q.position : x.ref > y.ref;
		}
	};
	// Whether an entry's key is at or after `key`, whose prefix is `prefix`: a record read only on a tie.
	static bool entry_from(const buffer& b, const wbuf::pos_entry& e, std::uint64_t prefix, std::string_view key);
	// Keyless records sit in the position index with a zero prefix and in no run.
	static bool keyed(const buffer& b, const wbuf::pos_entry& e) { return e.prefix() != 0 || !b.lane.at(e.ref).key().empty(); }

	// The lease holder's (D9): installs the splices finished jobs left, then posts a fresh job when
	// a scan asked and a compaction when the big levels call for one. Without a host executor the
	// jobs run here, as merges do. Whether a run set was replaced or first installed.
	bool maintain_runs() const;
	bool maintain_runs(const std::shared_ptr<buffer>& sb) const;
	// A job's end on the pool: its splice, or a failed one, then ready with release.
	template <class Build>
	static void finish_job(wbuf::run_job& j, Build&& build) noexcept;
	template <class Job>
	void start_runs(const std::shared_ptr<buffer>& sb, wbuf::run_job& j, Job&& job) const;
	// Applies a finished job's splice to the installed set and installs the result with release;
	// the replaced set waits for D10's stamp while readers may hold it.
	bool take_runs(buffer& b, wbuf::run_job& j) const;

	template <class F>
	std::size_t scan_keys(std::string_view from, const std::string_view* to, std::size_t n, F& f) const;

	// --- reads at a position from any thread (write-buffer D5, D6, D9) ----------------------

	static std::uint64_t position_of(const buffer& b, const record& x) noexcept { return b.base + x.position; }
	// The key's newest record at or below r: its slot, then prev while newer (D5).
	static const record* key_at(const buffer& b, std::string_view key, std::uint64_t r);
	// The docid walk at r (D5): whether b holds a record of the docid at or below r. It follows
	// the docid prev field when it names a record, else prev; another docid ends it.
	static bool docid_at(const buffer& b, std::uint64_t docid, std::uint64_t r);
	// D6's docid check: a segment copy is superseded at r when the view's active buffer holds a
	// record of its docid at or below r; below the first docid-only supersession it is skipped.
	static bool superseded(const buffer* active, const segment_view& s, ordinal_t ord, std::uint64_t r);
	template <class F>
	static bool get_at(const view& v, std::string_view key, std::uint64_t r, F& f)
		requires(segment::template contains<stored_plugin>());
	// One buffer's part of a scan at r (D9): its run set, loaded once, and a private tail, the
	// position-index entries past that set's cover, at or below r, in [from, to). Only the tail's
	// tail_select smallest entries are sorted until the scan passes them.
	struct buffer_part {
		const buffer* b;
		std::uint64_t r;
		const wbuf::run_set* runs;
		std::vector<wbuf::pos_entry> tail;
		std::size_t sorted = 0;
		void finish() {  // sorts the rest of the tail
			std::sort(tail.begin() + std::ptrdiff_t(sorted), tail.end(), entry_less{b});
			sorted = tail.size();
		}
	};
	static buffer_part part_at(const buffer& b, std::uint64_t r, std::string_view from, const std::string_view* to);
	template <class F>
	static std::size_t scan_at(const view& v, std::uint64_t r, std::string_view from, const std::string_view* to, std::size_t n, F& f)
		requires(snapshot_scannable());

	// --- the segment reads (D8): one implementation for the index and its snapshots -----------

	// The pending deletes by (segment id, ordinal): copies stage 1 removed, applied at the freeze.
	using pending_set = std::unordered_set<std::uint64_t>;
	static std::uint64_t pending_key(std::uint64_t segment, ordinal_t ord) noexcept { return segment << 32 | ord; }
	static bool pending_has(const pending_set* p, std::uint64_t segment, ordinal_t ord) { return p && !p->empty() && p->count(pending_key(segment, ord)); }

	// A get's segment half: the segments newest first, each through the key range, the filter, and
	// the dictionary with its live bits; f borrows the record for the call. A copy a pending delete
	// names is absent (D6).
	struct no_check {
		bool operator()(const segment_view&, ordinal_t) const noexcept { return false; }
	};
	template <class Segs, class F, class Gone = no_check>
	static bool get_segments(const Segs& segs, std::string_view key, F& f, const pending_set* pending = nullptr, Gone&& gone = {});

	// A scan's k-way merge over the segments, oldest first, and the buffers' parts, oldest buffer
	// first (D9). One pass finds the smallest key, comparing prefixes first, and the sources
	// holding it; only those step. Among a key's sources the newest wins: a buffer's over a
	// segment's, a newer buffer's over an older's, within a buffer the newest record at or below
	// its r. A winning tombstone emits nothing (D6).
	template <class Segs, class F, class Gone = no_check>
	static std::size_t scan_segments(const Segs& segs, std::span<buffer_part> buffers, const pending_set* pending, std::string_view from, const std::string_view* to, std::size_t n, F& f, Gone&& gone = {});

	// --- memory -----------------------------------------------------------------------------

	// A document's stored bytes: none in a layout without a stored plugin.
	static std::string_view stored_bytes(const document& d) noexcept;
	// Whether plugin E's part may be buffered as its stored bytes alone: the stored plugin's own part,
	// or a part whose plugin opts in with `inline_part` and keeps the stored plugin's builder, so the
	// flush's add_stored_view does all its add would (writer.hh). Any other part keeps its documents
	// whole, so no plugin's add is skipped.
	template <class E>
	static constexpr bool inline_kind() noexcept;
	static constexpr bool inline_layout() noexcept;
	// Whether the record can hold this document as its key and stored bytes: an inline layout, and
	// each opted-in plugin's `inline_part` says the writer reads nothing else in its part.
	static bool inline_parts(const document& d) noexcept;
	// Whether a plugin rewrites a part on its way into the buffer.
	static constexpr bool has_buffer_hooks() noexcept;
	static constexpr std::uint64_t doc_overhead = 96;  // a document's allocations beyond its object and parts
	static std::uint64_t doc_bytes(const document& d) { return sizeof(document) + doc_overhead + d.part_bytes(); }
	// The buffers' maps, position indexes, and scan runs.
	std::uint64_t map_bytes() const;

	// Brings the permit to what the buffer and version map hold now.
	// The buffer's memory: the lane's chunks as they are taken (D3), and beyond it the whole
	// records' estimate, or in an inverting layout the inverter's allocations.
	std::uint64_t buffered_bytes() const noexcept;
	void charge();
	// A buffer no accepted record references still holds a rejected transaction's records.
	bool holds_rejected() const noexcept { return wb_ && wb_->order.size() == 0 && (wb_->bytes != 0 || wb_->lane.tell().chunks != 0); }
	// The flush's working set past the buffer, in percent of the buffer: RSS grew 26% of the
	// buffer's charge over prospect's 1M keyed flush (docs/perf/flush-spill.md).
	static constexpr std::uint64_t flush_working_pct = 26;
	// The buffer's limit: its share, and under a budget the part of the capacity that leaves
	// the flush its working set.
	std::uint64_t buffer_limit() const noexcept;
	bool needs_flush() const;

	// --- in-memory state ------------------------------------------------------------------

	// What a record needs to map its body, shared by an index's records: the index's IO and its
	// counters, which readers' maps write (cold-tier D2). Heap-held so a moved index keeps them.
	struct load_context {
		explicit load_context(kronuz::journal::IO* i) noexcept : io(i) {}
		kronuz::journal::IO* io;
		std::atomic<std::uint64_t> retries{0};  // reads that passed on the second try
		std::atomic<std::uint64_t> skipped{0};  // merge rounds a host error dropped at scheduling
		std::atomic<std::uint64_t> unleased{0}; // input maps made on a thread not holding this index's lease (CT-R23, I5-N3)
		write_lease::word lease;                // the index's write-lease holder word (its lease_ uses it)
		std::atomic<std::uint64_t> lost{0};     // maps whose body lost the install race and was destroyed
	};
	// The manifest's claim a mapped file must meet (F2, F20): its structures within `mask`, its
	// version within the range, its field ids within a table of `fields` and none `key_field`
	// (docs/mechanisms/field-check.md). Copied from the manifest the record was made under; a
	// segment this process wrote holds ids stage 1 checked, so its vouch bounds no field.
	struct vouch {
		static constexpr std::uint64_t any_fields = ~std::uint64_t(0);
		format::structure_bits mask = ~format::structure_bits(0);
		std::uint32_t min_version = 0, max_version = ~std::uint32_t(0);
		std::uint64_t fields = any_fields;
		std::optional<std::uint32_t> key_field;
		// Throws corrupt_segment when the decoded segment breaks the claim: run at every map.
		void check(const segment& sg) const {
			const std::uint32_t ver = sg.core().version();
			if ((sg.core().structures() & ~mask) || ver < min_version || ver > max_version) {
				throw corrupt_segment("segment outside its manifest's structures or versions");
			}
			if (fields != any_fields) { sg.check_fields(fields, key_field); }
		}
	};
	// A segment file mapped (or its bytes, when the backend cannot map) and decoded. Immutable,
	// installed on a record once and never replaced or cleared before the record dies (I1). It
	// carries no owner check: a reader that loses the install race destroys its own on its thread.
	struct segment_body {
		std::shared_ptr<const kronuz::journal::Mapping> mapping;  // the file, mapped (ARCHITECTURE, I/O)
		std::string owned;                                         // or its bytes, when the backend cannot map
		segment seg;
		explicit segment_body(std::string b) : owned(std::move(b)), seg(std::string_view(owned)) {}
		explicit segment_body(std::shared_ptr<const kronuz::journal::Mapping> m) : mapping(std::move(m)), seg(mapping->bytes()) {}
		segment_body(const segment_body&) = delete;
		segment_body& operator=(const segment_body&) = delete;
		std::string_view bytes() const noexcept { return mapping ? mapping->bytes() : std::string_view(owned); }
	};
	// A segment of the index (cold-tier D2): the record, which carries the owner-written state, and
	// its body, which maps at most once. The body is null until the first read maps it, the static
	// failed marker when its checks failed, else a body: a reader installs it by compare-exchange,
	// and the loser destroys only its own and uses the winner's.
	struct open_segment {
		files::segment_ref ref;
		deleted_bits deleted;   // bit per ordinal, copied on write (shared-reader D3); sized from ref.doc_count
		std::uint64_t dead = 0;  // bits set in deleted
		std::unique_ptr<kronuz::journal::File> unsynced;  // flushed, awaiting the commit's sync
		// The facts the manifest needs and the entry lacks. Only the owner writes them (under the
		// lease, or before the record is published): a reader's map leaves them in the body.
		format::structure_bits structures = 0;
		std::uint64_t total_length = 0;
		// The small members share one word: a record per segment of every open index.
		std::uint32_t version = 0;
		bool facts_known = false;
		bool deletes_dirty = false;          // deletes since the head (manifest-log D3)
		bool damaged = false;                // a background merge found it corrupt; heal() clears it
		std::shared_ptr<load_context> ctx;
		vouch claim;
		open_segment(files::segment_ref r, std::shared_ptr<load_context> c, vouch v = {})
			: ref(r), deleted((r.doc_count + 63) / 64), ctx(std::move(c)), claim(v) {}
		open_segment(const open_segment&) = delete;
		open_segment& operator=(const open_segment&) = delete;
		// Dropped under the write lease, and unmapped there (D4, D6; SR-D5). The marker is static.
		~open_segment() {
			detail::owner_only();
			if (const void* b = body_.load(std::memory_order_relaxed); b && b != failed_marker()) { delete static_cast<const segment_body*>(b); }
		}
		// Installs the body this path just made or mapped, before the record is shared, and records
		// the facts from it.
		void adopt(std::unique_ptr<const segment_body> b) {
			assert(!body_.load(std::memory_order_relaxed));
			const segment& sg = b->seg;
			structures = sg.core().structures();
			version = sg.core().version();
			total_length = sg.total_length();
			facts_known = true;
			body_.store(b.release(), std::memory_order_release);
		}
		// The owner records the facts of a body a reader mapped, or maps it (a prepare, D9).
		void learn_facts() {
			if (facts_known) { return; }
			const segment& sg = seg();
			structures = sg.core().structures();
			version = sg.core().version();
			total_length = sg.total_length();
			facts_known = true;
		}
		// The record that stands for this one with no body (cold-tier D3): the same file, bits (shared),
		// deletes, facts, vouch and failed marker. The owner's, under the lease: a body a reader mapped
		// before its facts were learned gives them up here. A reader that maps the original later
		// installs on the original, which the retired views hold.
		std::unique_ptr<open_segment> twin() {
			if (!facts_known && bodied()) { learn_facts(); }
			auto t = std::make_unique<open_segment>(ref, ctx, claim);
			t->deleted = deleted.share();
			t->dead = dead;
			t->deletes_dirty = deletes_dirty;
			t->damaged = damaged;
			t->structures = structures;
			t->version = version;
			t->total_length = total_length;
			t->facts_known = facts_known;
			if (failed()) { t->body_.store(failed_marker(), std::memory_order_relaxed); }
			return t;
		}
		bool bodied() const noexcept {
			const void* b = body_.load(std::memory_order_acquire);
			return b && b != failed_marker();
		}
		// A reader's first map failed its checks (D11); heal() reports and replaces the record.
		bool failed() const noexcept { return body_.load(std::memory_order_acquire) == failed_marker(); }
		// The decoded segment, mapped on the first call (a map point, D9). Throws corrupt_segment when
		// the checks fail twice, and the host's error unchanged when mapping fails for its reasons.
		const segment& seg() const {
			const void* b = body_.load(std::memory_order_acquire);
			if (b && b != failed_marker()) { return static_cast<const segment_body*>(b)->seg; }
			return b ? throw_failed() : map_body();
		}
		// A body this path created or mapped earlier: never maps (D9's body kind).
		const segment& mapped() const noexcept {
			const void* b = body_.load(std::memory_order_acquire);
			assert(b && b != failed_marker());
			return static_cast<const segment_body*>(b)->seg;
		}
		const segment_body& body() const noexcept {
			assert(bodied());
			return *static_cast<const segment_body*>(body_.load(std::memory_order_acquire));
		}
		std::string_view bytes() const noexcept { return body().bytes(); }

	private:
		static const void* failed_marker() noexcept {
			static const char marker = 0;
			return &marker;
		}
		[[noreturn]] static const segment& throw_failed() { throw corrupt_segment("the segment failed its checks"); }
		// Marks the record failed, unless a map that passed got there first.
		// Fences the segment and throws: a corrupt_index as a corrupt_segment, a corrupt_segment as it is
		// (rethrown from the catch, so its prefix is not doubled).
		const segment& fail(const char* what, bool rewrap) const {
			const void* expected = nullptr;
			if (body_.compare_exchange_strong(expected, failed_marker(), std::memory_order_acq_rel, std::memory_order_acquire) || expected == failed_marker()) {
				if (rewrap) { throw corrupt_segment(what); }
				throw;
			}
			return static_cast<const segment_body*>(expected)->seg;
		}
		[[gnu::noinline]] const segment& map_body() const {
			assert(detail::no_input_map() == 0 && "an input map inside guarded() or a chore");
			if (!write_lease::held_by_this_thread(ctx->lease)) { ctx->unleased.fetch_add(1, std::memory_order_relaxed); }
			std::unique_ptr<const segment_body> fresh;
			try {
				fresh = read_checked([&] { return open_body(*ctx->io, ref, claim, false); }, &ctx->retries);
			} catch (const corrupt_index& e) {
				return fail(e.what(), true);
			} catch (const corrupt_segment& e) {
				return fail(e.what(), false);
			}
			const void* expected = nullptr;
			if (body_.compare_exchange_strong(expected, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) { return fresh.release()->seg; }
			ctx->lost.fetch_add(1, std::memory_order_relaxed);
			if (expected == failed_marker()) { throw_failed(); }
			return static_cast<const segment_body*>(expected)->seg;  // the loser's body goes at scope end
		}
		mutable std::atomic<const void*> body_{nullptr};
	};
	// segs_ as segment_views, made as they are read, so the index's reads allocate nothing.
	struct owned_segments {
		const std::vector<std::shared_ptr<open_segment>>& segs;
		std::size_t size() const noexcept { return segs.size(); }
		segment_view operator[](std::size_t i) const noexcept {
			const open_segment& s = *segs[i];
			return {s.ref.id, &s, s.dead ? &s.deleted : nullptr, s.dead};
		}
	};
	// A background merge. The writer sets everything above `plan` before handing it to the
	// executor; the merge grows `storage` and `reserved`, sets the rest, then `done`, and the
	// writer touches none of it again before `done`.
	struct merge_job {
		std::uint64_t id = 0;  // the output segment's
		std::array<std::uint8_t, 16> header{};
		std::vector<std::shared_ptr<open_segment>> sources;
		std::vector<std::vector<std::uint64_t>> deleted;  // the sources' deletes the merge reads
		bool reorder = false;                             // a reordering merge (D4)
		std::uint64_t reserved = 0;                       // on `storage`: the output's estimate, then what it wrote
		byte_budget::permit storage;
		flush_charge memory;  // the merge's working memory, then its output's, until published
		merge_plan plan;
		std::unique_ptr<open_segment> output;  // none when no document was live
		std::exception_ptr error;
		std::uint64_t removed = 0;  // a failed output's bytes, removed, awaiting the writer's directory sync
		std::atomic<bool> done{false};
	};
	// Documents inverted on arrival, and what the flush needs to write them in docid order.
	struct active_buffer {
		flush_charge held;  // what the buffer allocated: charge() draws it on permit_ (D17)
		inverter buf;
		std::uint64_t prev_docid = 0;
		bool ordered = true;  // docids increase with ordinals
		active_buffer() {
			buf.memory = &held;
			held.grow(sizeof(active_buffer));  // the object too (C11)
		}
	};
	struct pending_doc {
		std::uint64_t docid, version;
		std::string_view key;  // in the transaction
		const document* doc;
	};

	// --- the write buffer (docs/design/write-buffer.md D3, D4, D6, D8, D9) ----------------------

	using stored_part = typename stored_plugin::part;
	// A record (D3, WB-21), 8-byte aligned, in the lane's bytes:
	//   0  docid u64, version u64, position u32, prev ref u32    stage 1 fills them
	//  24  flags u8 (tombstone, docid prev, ordinal, whole), 3 B padding
	//  28  the fields the flags announce, 4 bytes each, docid prev first, then the ordinal
	//  then, inline: [key length varint][stored length varint][key][stored], padded to 8 bytes;
	//      or, whole, a `document` object at the next 8-byte boundary (32, or 40 with both fields).
	// The key and the stored bytes lie in the record whatever the layout, so a record is one
	// block of memory. A whole record is the fallback for a document the inline form cannot hold:
	// one past a chunk, or one with a part the segment writer reads beyond the stored bytes.
	// The producer reserves a field from the operation's type; a document is the key only in a
	// tombstone or an inverting layout, whose inverter holds the document. Nothing is written
	// after the record's map slot is stored (WB-9).
	struct record {
		static constexpr std::uint8_t tombstone_flag = 1, docid_prev_flag = 2, ordinal_flag = 4, whole_flag = 8;
		static constexpr std::size_t flags_at = 24, fields_at = 28;
		std::uint64_t docid, version;
		std::uint32_t position;  // its transaction's place in the buffer, from 1: base + position (D5)
		wbuf::ref_t prev;        // the record its key's slot, else its docid's, named before it
		std::uint8_t flags;
		std::uint8_t padding[3] = {};
		std::uint32_t first_field = 0;  // bytes 28 to 32; the fields run on past the header's end
		static constexpr std::size_t fields_of(std::uint8_t f) noexcept { return std::size_t(std::popcount(unsigned(f & (docid_prev_flag | ordinal_flag)))); }
		// Where the inline lengths start, and where a whole record's document lies.
		static constexpr std::size_t body_at(std::uint8_t f) noexcept { return fields_at + 4 * fields_of(f); }
		static constexpr std::size_t doc_at(std::uint8_t f) noexcept { return (body_at(f) + 7) & ~std::size_t(7); }
		static constexpr std::size_t widest_flags = docid_prev_flag | ordinal_flag;
		static constexpr std::size_t varint_bytes(std::size_t n) noexcept { return (std::size_t(std::bit_width(n | 1)) + 6) / 7; }
		// The record's bytes in the lane: with an inline key and stored bytes, or whole.
		static constexpr std::size_t inline_stride(std::uint8_t f, std::size_t key, std::size_t stored) noexcept {
			return (body_at(f) + varint_bytes(key) + varint_bytes(stored) + key + stored + 7) & ~std::size_t(7);
		}
		static constexpr std::size_t whole_stride(std::uint8_t f) noexcept { return (doc_at(f) + sizeof(document) + 7) & ~std::size_t(7); }
		record(const record&) = delete;
		record& operator=(const record&) = delete;
		~record() {
			if (flags & whole_flag) { whole().~document(); }
		}
		std::string_view key() const noexcept {
			if (flags & whole_flag) { return whole().key; }
			const body b = lengths();
			return {b.at, b.key};
		}
		std::string_view stored() const noexcept {
			if (flags & whole_flag) {
				if constexpr (kept_stored) { return static_cast<const stored_part&>(whole()).stored; }
				return {};
			}
			const body b = lengths();
			return {b.at + b.key, b.stored};
		}
		// The document the record holds, whole: a copy of its own, or one built from the inline bytes.
		document materialize() const {
			if (flags & whole_flag) { return whole(); }
			document d;
			d.key.assign(key());
			if constexpr (kept_stored) { static_cast<stored_part&>(d).stored.assign(stored()); }
			return d;
		}
		bool is_whole() const noexcept { return flags & whole_flag; }
		const document& whole_document() const noexcept { return whole(); }
		bool tombstone() const noexcept { return flags & tombstone_flag; }
		bool has_docid_prev() const noexcept { return flags & docid_prev_flag; }
		// The docid's slot before, when it is not prev; no_ref when the flag is not set (D3).
		wbuf::ref_t docid_prev() const noexcept { return has_docid_prev() ? field(0) : wbuf::no_ref; }
		ordinal_t ordinal() const noexcept { return flags & ordinal_flag ? field(has_docid_prev() ? 1 : 0) : 0; }
		std::size_t stride() const noexcept {
			if (flags & whole_flag) { return whole_stride(flags); }
			const body b = lengths();
			return inline_stride(flags, b.key, b.stored);
		}
		// Appends a record of key `k` and stored bytes `s`: inline when `inline_ok` and the record fits a
		// chunk, else whole, holding `whole_of()`. Every record is at most a chunk (wbuf::min_chunk).
		template <class Whole>
		static wbuf::ref_t emplace(wbuf::lane<record>& l, std::uint64_t d, std::uint64_t v, std::uint32_t pos, wbuf::ref_t p, std::uint8_t f, wbuf::ref_t dprev, std::uint32_t ord, bool inline_ok, std::string_view k, std::string_view s, Whole&& whole_of) {
			const std::size_t n = inline_stride(f, k.size(), s.size());
			if (inline_ok && n <= wbuf::min_chunk) { return l.append_n(n, d, v, pos, p, f, dprev, ord, k, s); }
			f |= whole_flag;
			return l.append_n(whole_stride(f), d, v, pos, p, f, dprev, ord, whole_of());
		}
		// The record of an inline document (for wbuf::lane::append_n): `f` has no whole_flag.
		record(std::uint64_t d, std::uint64_t v, std::uint32_t pos, wbuf::ref_t p, std::uint8_t f, wbuf::ref_t dprev, std::uint32_t ord, std::string_view k, std::string_view s)
			: docid(d), version(v), position(pos), prev(p), flags(f) {
			put_fields(dprev, ord);
			char* at = reinterpret_cast<char*>(bytes() + body_at(f));
			at = varint::leb128::encode(at, k.size());
			at = varint::leb128::encode(at, s.size());
			if (!k.empty()) { std::memcpy(at, k.data(), k.size()); }
			if (!s.empty()) { std::memcpy(at + k.size(), s.data(), s.size()); }
		}
		// The record of a whole document: `f` has whole_flag.
		record(std::uint64_t d, std::uint64_t v, std::uint32_t pos, wbuf::ref_t p, std::uint8_t f, wbuf::ref_t dprev, std::uint32_t ord, document dc)
			: docid(d), version(v), position(pos), prev(p), flags(f) {
			put_fields(dprev, ord);
			new (bytes() + doc_at(f)) document(std::move(dc));
		}

	private:
		static constexpr bool kept_stored = std::is_base_of_v<stored_part, document>;
		struct body {
			const char* at;  // the key, then the stored bytes
			std::size_t key, stored;
		};
		// An inline record's lengths, each at most two bytes since the record fits a chunk.
		body lengths() const noexcept {
			const char* p = reinterpret_cast<const char*>(bytes_c() + body_at(flags));
			const char* const end = p + 2 * varint::leb128::max_bytes;
			body b{};
			varint::leb128::decode(&p, end, b.key);
			varint::leb128::decode(&p, end, b.stored);
			b.at = p;
			return b;
		}
		document& whole() noexcept { return *std::launder(reinterpret_cast<document*>(bytes() + doc_at(flags))); }
		const document& whole() const noexcept { return *std::launder(reinterpret_cast<const document*>(bytes_c() + doc_at(flags))); }
		void put_fields(wbuf::ref_t dprev, std::uint32_t ord) noexcept {
			std::byte* const at = bytes() + fields_at;
			if (flags & docid_prev_flag) { std::memcpy(at, &dprev, 4); }
			if (flags & ordinal_flag) { std::memcpy(at + (flags & docid_prev_flag ? 4 : 0), &ord, 4); }
		}
		std::byte* bytes() noexcept { return reinterpret_cast<std::byte*>(this); }
		const std::byte* bytes_c() const noexcept { return reinterpret_cast<const std::byte*>(this); }
		std::uint32_t field(std::size_t i) const noexcept {
			std::uint32_t x;
			std::memcpy(&x, bytes_c() + fields_at + 4 * i, 4);
			return x;
		}
	};
	static_assert(offsetof(record, flags) == record::flags_at && offsetof(record, first_field) == record::fields_at && sizeof(record) == 32);
	static_assert(alignof(document) <= 8 && record::whole_stride(record::widest_flags) <= wbuf::min_chunk);
	// A whole record's stride is within its doc_bytes, so the charge beyond the lane cannot wrap.
	static_assert(record::whole_stride(record::widest_flags) <= sizeof(document) + doc_overhead);
	// A buffer: active while it takes writes, then frozen (D8) until its flush installs. Its
	// records sit in one lane (one writer, D14's mode of D3); the key map and the docid map name
	// each key's and docid's newest record; the position index lists accepted records in order.
	struct buffer {
		static constexpr std::uint64_t none = ~std::uint64_t(0);
		buffer(std::uint64_t at, std::size_t chunks) noexcept : lane(chunks), base(at) {}
		wbuf::lane<record> lane;
		wbuf::version_map keys;
		// Built under the lease once a docid-addressed operation or a keyless document needs it
		// (D3, WB-23); readers probe it and never build it.
		mutable wbuf::version_map docids;
		wbuf::stable_array<wbuf::pos_entry> order;  // the position index (D9)
		const std::uint64_t base;               // V when it became active: its records' positions follow (D5)
		// The position of its first docid-only supersession, or none (D6, WB-22): stored with
		// release before V passes it, restored on a rejection.
		std::atomic<std::uint64_t> first_docid_only{none};
		std::uint32_t transactions = 0;          // accepted, the last one's position
		std::size_t docs = 0;                    // records that are not tombstones
		std::uint64_t bytes = 0;                 // what whole records hold beyond the lane (doc_bytes less their stride)
		std::vector<std::pair<std::uint64_t, ordinal_t>> pending;  // segment copies removed, applied at the freeze (D6)
		pending_set pending_keys;
		std::unique_ptr<active_buffer> inv;  // an inverting layout's documents
		// The runs (D9): installed with release by the lease holder only; a replaced set waits in
		// retired_runs for D10's stamp. A scan whose tail passes tail_build sets wants_runs.
		std::atomic<const wbuf::run_set*> runs{nullptr};
		mutable std::atomic<bool> wants_runs{false};
		wbuf::run_job fresh, compaction;
		std::vector<std::shared_ptr<const void>> retired_runs;
		~buffer() { delete runs.load(std::memory_order_relaxed); }
	};
	// A live copy: in the active buffer, a frozen one, or a segment.
	struct copy {
		enum where_t : std::uint8_t { none, active, frozen, segment } where = none;
		std::size_t at = 0;  // frozen_'s or segs_'s index
		wbuf::ref_t ref = wbuf::no_ref;
		ordinal_t ord = 0;
		std::uint64_t docid = 0, version = 0;
		explicit operator bool() const noexcept { return where != none; }
	};
	template <class F>
	void for_each_buffer(F&& f) const;
	const pending_set* pending_index() const noexcept { return wb_ && !wb_->pending.empty() ? &wb_->pending_keys : nullptr; }

	void live() const;

	// A host error mapping a merge's source at scheduling: the round is dropped, nothing reserved.
	struct map_failure : std::system_error {
		using std::system_error::system_error;
	};
	// Space errors before a sync stay recoverable (write_unsynced throws space_error);
	// every other I/O error fences.
	struct space_error : std::system_error {
		using std::system_error::system_error;
	};
	template <class F>
	void guarded(F&& f);

	// A segment just streamed to `file`: mapped when the backend maps and the mapping holds
	// what was written, so its bytes leave memory; else read back through the file (and charged
	// to `held`). A map that fails keeps the bytes: an output map never fails on a host error and
	// never fences (cold-tier D9).
	std::unique_ptr<open_segment> streamed_segment(std::unique_ptr<kronuz::journal::File> file, files::segment_ref ref, flush_charge* held = nullptr);
	// A file this index just wrote, mapped: null when the backend cannot map or the map fails.
	static std::shared_ptr<const kronuz::journal::Mapping> map_output(kronuz::journal::File& f) noexcept;

	// Creates a file and writes it whole. ENOSPC removes the partial file and throws
	// space_error; the caller has changed nothing yet.
	std::unique_ptr<kronuz::journal::File> write_unsynced(const std::string& name, std::string_view bytes);

	// An apply's turn under defer_io (cold-tier D3): the map_owed bit holds through the owed answers
	// of one transaction and ends with any other outcome, a throw included.
	struct turn_guard {
		bool& bit;
		bool owed = false;
		explicit turn_guard(bool& b) noexcept : bit(b) {}
		~turn_guard() {
			if (!owed) { bit = false; }
		}
		turn_guard(const turn_guard&) = delete;
		turn_guard& operator=(const turn_guard&) = delete;
	};

	// A writer call's hold on the lease. After its handoff it re-reads the owed flag and runs a
	// check-in that met the held lease, so no chore stays owed with the lease free (CR4).
	// `deferred` (options::defer_io's apply): the handoff leaves the chores owed for the host's check_in().
	class writer_scope {
	public:
		explicit writer_scope(basic_index& ix, bool deferred = false) noexcept : ix_(ix), lease_(ix.lease_), deferred_(deferred) {}
		~writer_scope() {
			if (lease_.release() && !deferred_ && ix_.chores_owed_.load(std::memory_order_seq_cst)) { ix_.check_in(); }
		}
		writer_scope(const writer_scope&) = delete;
		writer_scope& operator=(const writer_scope&) = delete;

	private:
		basic_index& ix_;
		lease_scope lease_;
		bool deferred_;
	};

	// A check-in's chores, under the lease: merges finished on the pool published, views no
	// reader holds freed and their files queued. They never throw: a failure fences the index,
	// and the next writer call reports it through live() (CR2).
	void run_chores() noexcept;

	// --- publication (shared-reader D1, D4, D6) -----------------------------------------------

	// An index's readers: absent until reader(), so a readerless index pays one pointer (D10).
	struct publication {
		std::shared_ptr<reader_state> rs;
		std::unique_ptr<const view> current;
		basic_retired<view> retired;
		std::uint64_t changes = 0, published = 0;  // state changes (SR-D16), and as of `current`
		std::uint64_t generation = 0;
		bool closing = false;  // closed with views protected (D11)
		std::vector<std::pair<std::uint64_t, std::shared_ptr<const void>>> tables;  // retired map tables by generation (D10)
		std::uint32_t groups = 0;  // groups since the last registry scan
	};

	// The state changed in a way readers see: an accepted transaction, segments replaced, an install.
	void changed() noexcept;

	// A publication point (D1): a new view when the state is publishable, with no document
	// buffered, so no transaction is seen in part (SR-D1), and changed since the current view
	// (SR-D16). Then the registry scan frees what no record names (D6).
	void publish_point();

	std::unique_ptr<const view> build_view();

	// Whether readers on other threads read the buffers: not an inverting layout's (D11).
	bool live_reads() const noexcept { return !inverting(); }

	// --- the turn log (docs/design/standalone-turn-log.md) ---------------------------------------

	// Takes the options' group (D6, D7). The head's turn_group attribute must name this host and
	// group, and an index with one opens only on its host (log_mismatch). By the table's status:
	// lost is log_lost; none joins (the group created, the attribute's edit, then the first record,
	// a checkpoint at the head); closed reopens and stages the table's record again as base + 1;
	// stored replays the log's records; bounded (damage to the log, journal's lost-bound.md) replays
	// the records after the bound when the head holds the group through it (a commit seals the
	// head's whole batch record), and otherwise loses the group. A head applied past the log's last durable
	// record, or older than its checkpoint record, names records the log lost (D6.4); a checkpoint
	// record of another UUID is another index's group. `created`: create_deferred's, whose first
	// commit joins.
	void attach_log(bool created = false);
	// Registers the group's sealer and watcher; records follow the group's last staged one, and
	// the log is durable through it, or through `durable` when a staged record is not yet.
	std::uint64_t seal_log(std::uint64_t durable)
		requires(command::loggable<Layout>);
	// D7's join: the group created in the table, durable; the edit that sets the head's turn_group
	// attribute (now, or at create's first commit); then the first record, a checkpoint at the
	// head. A crash before that record's barrier leaves the group none, and the next open joins
	// again. An index the attribute names already took the edit.
	void join(bool member, bool created)
		requires(command::loggable<Layout>);
	// D7's leave, at close(): a checkpoint covering every batch with nothing after it, durable,
	// then the table takes it as the group's base and the log forgets the group.
	void leave();
	// Replays the group's recovered records (D6.5): each logged transaction applies at its position
	// {s, i}, skipped at or below the head's. A chunk run (rocksdb-parity A3) applies whole at its
	// last chunk; one that a later record or the log's end interrupts was never acknowledged, and
	// is dropped, moving the applied position past it so a checkpoint can cover it. A middle or
	// last chunk with no run before it is log_lost above the head and skipped at or below it,
	// where the head holds its run (a bound may fall inside one). An owed verdict runs its chore
	// and applies again; no_space fails the open; any other verdict throws replay_diverged, naming
	// the position. No flush is a checkpoint until the group is released.
	void replay_log(const std::vector<kronuz::journal::TurnRecord>& records)
		requires(command::loggable<Layout>);
	void replay_one(const transaction& t, files::position at)
		requires(command::loggable<Layout>);
	// A write-buffer flush (D5's first trigger): a logged index's is a checkpoint, except during
	// replay, whose flushes publish segments and stage nothing (D6.5).
	void flush_point();
	// D5's second trigger (rocksdb-parity D2 item 2): the commands logged since the last checkpoint
	// pass the group's share of the log's window, and the floor.
	bool log_due() const noexcept;
	// The group's count in the sealer's writers: in at its first join after a checkpoint, out at the
	// next checkpoint or its detach.
	void count_writing(bool on) noexcept;
	// D5's step 4, after commit_files() made the edit durable: the checkpoint record {UUID, g, X},
	// unless nothing was logged since the last one and no join or leave owes one (`force`).
	void stage_checkpoint(bool force = false);
	void detach_log() noexcept;
	// The logged append (D3): the test seam, then the critical section. A failure there leaves an
	// accepted transaction the log lacks, so the index fences.
	void join_log(std::string_view cmd);
	// A command past the record cap (rocksdb-parity A3), as chunks of at most command_bytes each,
	// joined in order under the lease that stage 1 took: no checkpoint lands inside the run, and
	// only the last chunk's record moves the applied position and V.
	void join_chunks(std::string_view cmd)
		requires(command::loggable<Layout>);
	// Until the loop hands record `s` to a turn: the unsealed records then leave room.
	void await_sealed(std::uint64_t s)
		requires(command::loggable<Layout>);
	// No flush publishes above V (D4, F5): a logged index's flush, and its pending deletes' move
	// into segment bits, wait first until the log is durable through the applied position.
	void await_log();
	void await_log_through(std::uint64_t s);
	// Whether the applied state is ahead of the log: a logged index's views then keep their
	// predecessor's index-level state (metadata, fields, schema, last docid, position), which V's
	// prefix may not include yet.
	bool ahead_of_log() const noexcept { return logged_ && logged_->synced.load(std::memory_order_acquire) < cur_.applied.index; }

	// D7's rule 4 at a group's end: V never passes the newest view's cover, so a view is
	// published when its buffers are not the index's, or a map table was replaced (D10).
	void publish_cover();

	// Moves the map tables growth replaced into the retire list, stamped with the current view's
	// generation (D10); freed at once when no reader reads the buffers. Whether any moved.
	bool retire_tables();

	// Frees the retired tables no reader can still probe: stamped below every view a record names.
	void free_tables();

	// The view's pointer first, sequentially consistent so the scan's loads come after it (D5);
	// then its position and generation (SR-D15). The old view is retired into room made before the
	// publish, so nothing after it can fail.
	void publish_view(std::unique_ptr<const view> next);

	// Closing: readers' acquires find null from here on, and the current view is retired.
	void retire_current();

	// Scans the registry and frees every retired view no record names; whether it freed any.
	// What remains is at most one view per record (D6).
	bool free_views();

	template <class F>
	void for_each_view(F&& f) const;

	// --- background merges (D11, D18) ------------------------------------------------------

	bool merging(const open_segment& s) const;

	// Hands the merge of `ids` to the executor. It shares the sources and reads a copy of
	// their deletes so far; storage admission reserves the output's estimate as merge()
	// does, on a permit of the merge's own.
	// `forced`: a merge force_merge() asked for.
	void schedule(const std::vector<std::uint64_t>& ids, bool forced = false);

	// What a merge uses besides its job; the index's, never the index.
	struct merge_env {
		kronuz::journal::IO* io;
		executor* exec;
		rate_limiter* rate;
		byte_budget* budget;
		typename order_source<Layout>::settings reorder;
		format::structure_bits structures;  // the index's configured bits (F4)
		std::optional<format::stored_codec> stored_codec;  // the output's; none: the layout's
		std::shared_ptr<load_context> load;  // the output's record
	};

	// A merge's writes to one file, at increasing offsets: before each chunk an executor
	// checkpoint and the rate limiter's admission, then a draw on the job's storage
	// reservation past what it holds.
	class merge_file {
	public:
		merge_file(kronuz::journal::File& f, const merge_env& ctx, merge_job& job, const task::cancel_token& stop) : f_(f), ctx_(ctx), job_(job), stop_(stop) {}
		void write(std::string_view bytes, std::uint64_t& used) {
			const std::size_t chunk = ctx_.rate ? ctx_.rate->chunk() : std::size_t(1) << 20;
			while (!bytes.empty()) {
				const std::size_t n = std::min(bytes.size(), chunk);
				if (!ctx_.exec->checkpoint(stop_) || (ctx_.rate && !ctx_.rate->admit(*ctx_.exec, n, stop_))) { throw task::cancelled{}; }
				used += n;
				if (job_.storage && used > job_.reserved) {
					if (!job_.storage.try_grow(used - job_.reserved)) {
						throw space_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for a merge's output");
					}
					job_.reserved = used;
				}
				for (std::size_t done = 0; done < n;) {
					const std::size_t w = f_.write_at(at_ + done, bytes.substr(done, n - done));
					if (w == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
					done += w;
				}
				at_ += n;
				bytes.remove_prefix(n);
			}
		}
		std::uint64_t size() const noexcept { return at_; }

	private:
		kronuz::journal::File& f_;
		const merge_env& ctx_;
		merge_job& job_;
		const task::cancel_token& stop_;
		std::uint64_t at_ = 0;
	};

	// The output segment, streamed to its file a chunk at a time.
	class merge_output final : public output {
	public:
		merge_output(merge_file& f, std::uint64_t& used, std::size_t chunk) : f_(f), used_(used), chunk_(chunk) {}
		using output::write;
		void write(const char* data, std::size_t size) override {
			buf_.append(data, size);
			if (buf_.size() >= chunk_) { finish(); }
		}
		void finish() {
			f_.write(buf_, used_);
			buf_.clear();
		}

	private:
		merge_file& f_;
		std::uint64_t& used_;
		std::size_t chunk_;
		std::string buf_;
	};

	// The positions section, spilled to `seg-<id>.spill` until the postings end, then
	// copied into the output and removed. A spill left by a crash is an orphan open removes.
	class merge_spill final : public spill {
	public:
		merge_spill(const merge_env& ctx, merge_job& job, std::uint64_t& used, const task::cancel_token& stop, std::size_t chunk)
			: ctx_(ctx), job_(job), used_(used), stop_(stop), chunk_(chunk), name_(index_detail::spill_name(job.id)) {}
		merge_spill(const merge_spill&) = delete;
		merge_spill& operator=(const merge_spill&) = delete;
		~merge_spill() override {
			if (!f_) { return; }
			f_.reset();
			try {
				ctx_.io->remove(name_);
			} catch (const std::system_error&) {
			}
		}
		using output::write;
		void write(const char* data, std::size_t size) override {
			buf_.append(data, size);
			if (buf_.size() >= chunk_) { drain(); }
		}
		void copy_to(output& out) override {
			drain();
			if (!f_) { return; }
			const auto in = ctx_.io->open_existing(name_);
			std::string block;
			for (std::uint64_t at = 0; at < file_->size();) {
				if (stop_.requested()) { throw task::cancelled{}; }
				block.resize(std::min<std::uint64_t>(std::uint64_t(1) << 20, file_->size() - at));
				const std::size_t n = in->read_at(at, std::span<char>(block.data(), block.size()));
				if (n == 0) { throw std::system_error(EIO, std::generic_category(), "strata: short read of a merge's spill"); }
				out.write(block.data(), n);
				at += n;
			}
			const std::uint64_t spilled = file_->size();
			file_.reset();
			f_.reset();
			ctx_.io->remove(name_);
			used_ -= std::min(used_, spilled);
		}

	private:
		void drain() {
			if (buf_.empty()) { return; }
			if (!f_) {
				f_ = ctx_.io->create_exclusive(name_);
				file_ = std::make_unique<merge_file>(*f_, ctx_, job_, stop_);
			}
			file_->write(buf_, used_);
			buf_.clear();
		}
		const merge_env& ctx_;
		merge_job& job_;
		std::uint64_t& used_;
		const task::cancel_token& stop_;
		std::size_t chunk_;
		std::string name_, buf_;
		std::unique_ptr<kronuz::journal::File> f_;
		std::unique_ptr<merge_file> file_;
	};

	// A reordering merge's temporary file: its forward index (`seg-<id>.fwd`), mapped back
	// for bisection, or its stored records (`seg-<id>.rec`), mapped back as the output is
	// written. Written through the merge's file writes (checkpoints, rate limit, storage)
	// and removed after use; one left by a crash is an orphan open removes.
	class merge_scratch final : public strata::scratch {
	public:
		merge_scratch(const merge_env& ctx, merge_job& job, std::uint64_t& used, const task::cancel_token& stop, std::size_t chunk, std::string name)
			: ctx_(ctx), used_(used), name_(std::move(name)), f_(ctx.io->create_exclusive(name_)), file_(*f_, ctx, job, stop), out_(file_, used, chunk) {}
		merge_scratch(const merge_scratch&) = delete;
		merge_scratch& operator=(const merge_scratch&) = delete;
		~merge_scratch() override {
			mapped_.reset();
			f_.reset();
			used_ -= std::min(used_, file_.size());
			try {
				ctx_.io->remove(name_);
			} catch (const std::system_error&) {
			}
		}
		output& out() override { return out_; }
		std::string_view bytes() override {
			out_.finish();
			if ((mapped_ = f_->map())) { return mapped_->bytes(); }
			owned_ = index_detail::read_all(*ctx_.io->open_existing(name_));
			return owned_;
		}

	private:
		const merge_env& ctx_;
		std::uint64_t& used_;
		std::string name_;
		std::unique_ptr<kronuz::journal::File> f_;
		merge_file file_;
		merge_output out_;
		std::shared_ptr<const kronuz::journal::Mapping> mapped_;
		std::string owned_;
	};

	// On the executor: reads the sources' segments, which never change, and the job alone.
	// It charges its working memory to the indexing budget (never waiting for room), streams
	// the output to its file unsynced (as a flush leaves it) under the rate limit with
	// positions spilled to a file, then reads it back as the segment. A failed or stopped
	// build removes its files.
	static void run_merge(merge_job& job, const merge_env& ctx, const task::cancel_token& stop);

	// Publishes the background merges that finished, after waiting for all of them with
	// `wait`. One that ran out of space, or that the executor stopped, is dropped, its
	// sources left for a later merge (and with `space_throws`, ENOSPC is thrown once the
	// rest are published). A corrupt source is marked for heal() and skipped until then.
	// Any other failure fences: an I/O error, or a broken invariant.
	void publish_merges(bool wait, bool space_throws);

	[[noreturn]] void fence(const std::string& why);

	// Marks the merge's sources that fail check_segment (checksums, then consistency); false when none does.
	bool mark_damaged(const merge_job& job);

	// Publishes a finished merge, on the writer's thread. When its sources are all still
	// here, the output replaces them, and each delete made since the merge read them carries
	// over through its ordinal map, once (D11); otherwise the output's file is reclaimed. A
	// delete that maps to no output document breaks the invariant and fences, changing
	// nothing.
	void install_merge(merge_job& job);

	// A merge's install carries the pending deletes naming its sources to its output (D6, WB-2):
	// each (source, ordinal) through the plan's ordinal map. One the output dropped fences.
	void carry_pending(const merge_job& job);

	// The owner lets go of a job's segments itself: the executor may hold the job after `done`,
	// and the last reference to a source a merge replaced must not drop on its thread (SR-D5).
	struct job_drop {
		merge_job& job;
		~job_drop() {
			job.sources.clear();
			job.deleted.clear();
			job.output.reset();
		}
	};

	// Ends the merges' group, then lets go of every job's segments on this thread.
	void end_merges() noexcept;

	// Ends every background merge unpublished: queued ones are dropped, running ones
	// stopped, and finished outputs' files reclaimed.
	void abandon_merges();

	// What a posted delete reports back, for the owner to collect: the files it removed, and
	// the ones it could not, with the size left on disk.
	struct failed_delete {
		std::string name;
		std::uint64_t left;
	};
	struct delete_chan {
		std::atomic<std::size_t> ready{0};
		std::mutex mu;
		std::vector<std::string> done;  // guarded by mu
		std::vector<failed_delete> failed;  // guarded by mu
		void report(std::string name, std::optional<std::uint64_t> left) {
			const std::lock_guard lock(mu);
			if (left) {
				failed.push_back({std::move(name), *left});
			} else {
				done.push_back(std::move(name));
			}
			ready.fetch_add(1, std::memory_order_release);
		}
	};

	// A delete's body, on an executor thread: it touches no index state, only the IO and the
	// channel. A file already gone counts as removed; any other failure reports the size left.
	static void run_delete(kronuz::journal::IO& io, delete_chan& ch, const std::string& name, std::uint64_t size, std::uint64_t step, const task::cancel_token& stop);

	// Takes the posted deletes' outcomes: a removed file's bytes wait as unlinked for a
	// directory sync; a failed one returns to the files on record at the size left, and to
	// the queue's front, its bytes owed again.
	void collect_deletes();
	void requeue_delete(const std::string& name, std::uint64_t left);
	// Waits for the posted deletes (it never cancels them) and collects. A delete the executor
	// dropped without running reported nothing: its file is requeued as it was.
	void finish_deletes();
	// close() and the destructor: the deletes in flight finish; a crash's orphans are the next
	// open's.
	void end_deletes() noexcept;

	// reclaim_files() with an executor: pops the queue up to `bytes`, does the accounting at
	// once (the file leaves the files on record; its bytes stay in the permit until the
	// removal is synced), and posts the delete.
	std::uint64_t reclaim_posted(std::uint64_t bytes);

	// reclaim()'s file half: the queue, about `bytes` of it.
	std::uint64_t reclaim_files(std::uint64_t bytes);

	// --- storage accounting (T5) -------------------------------------------------------------

	// Bytes a merge or install reserved for what it writes: settle_storage() counts them, so
	// a drain while it runs keeps them; they go when it ends.
	struct hold {
		basic_index* ix;
		std::uint64_t n = 0;
		void add(std::uint64_t bytes) {
			n += bytes;
			ix->held_ += bytes;
		}
		~hold() {
			ix->held_ -= n;
			ix->settle_storage();
		}
	};

	// A removed file's bytes stay counted until a directory sync makes its removal durable:
	// filesystems release the blocks then, not at the unlink.
	void remove_file(const std::string& name);
	// While the open's head is owed its sync, every directory sync is that sync, so no
	// writeback error is taken outside it (sync-at-first-commit D4).
	void sync_dir();
	// The open's head made durable: the log and the directory under one device flush. A failure
	// fences: after a failed fsync a retry proves nothing.
	void sync_inherited();
	// A file an earlier process's generation may name: a log, or a segment on disk at open.
	bool inherited(const std::string& name) const;
	// A bound on a segment's fixed bytes, apart from its documents and terms: the header,
	// footer, and table of contents, each section's alignment and region checksums, and the
	// term index's and key filter's least sizes. (A one-document segment takes 1,088.)
	static constexpr std::uint64_t segment_overhead = 4096;

	// A bound on what the next commit writes besides segments (manifest-log D8, C5): the largest
	// edit the pending changes make (an entry per segment born since the head, the change list's
	// ordinals, the keys it touched, the new declarations, framed), and a snapshot of the state
	// only while a rollover is owed: D7's bounds give way under ENOSPC, so S is not held against
	// every commit. Each index holds it standing, so a commit that frees space can always be written.
	std::uint64_t commit_allowance() const;

	// Whether drain() has files to reclaim, delete results to wait for or removals to sync.
	bool drain_pending();

	// Reclaims every queued file for admission that is short of space; false if none was.
	bool drain();

	// Brings the storage permit to the files' bytes, what admission reserved, and the
	// commit allowance.
	void settle_storage();
	// A transaction's bytes on disk, overestimated: stored records, each term with its
	// posting and positions, a per-document overhead, and deletes' entries in the edit.
	static std::uint64_t estimate_bytes(const transaction& t);

	// --- lookups ----------------------------------------------------------------------------

	static wbuf::ref_t find_key_in(const buffer& b, std::string_view key);
	// The const probe (WB-23): any thread; no table means no docid-only supersession.
	static wbuf::ref_t probe_docid(const buffer& b, std::uint64_t docid);
	// The lease holder's: builds the map first (D8's exception for a frozen buffer).
	wbuf::ref_t find_docid_in(const buffer& b, std::uint64_t docid) const;
	// The docid map, from the position index, the first time stage 1 or a flush needs it (D3,
	// WB-23): the table is published, then filled; every record takes its docid's slot in
	// order, so the newest names each docid. It runs inline under the lease at any size: apply()
	// is synchronous, so a post to the pool would only block the caller (the async seam is step 3).
	void build_docids(const buffer& b) const;
	wbuf::version_map::place store_docid(const buffer& b, wbuf::ref_t r) const;
	// Whether a buffer's record is its key's (else its docid's) newest and not a tombstone: what
	// its flush writes. A frozen buffer keeps no live bits; newer records shadow its copies (WB-16).
	bool newest(const buffer& b, wbuf::ref_t r) const;
	copy buffered_copy(const buffer& b, typename copy::where_t w, std::size_t at, wbuf::ref_t r) const;

	// A docid's live copy: the buffers newest first, through their docid maps, then the segments
	// newest first with their live bits and the pending deletes.
	// Stage 1 builds a buffer's map on its first docid lookup (D8's exception); `build` false
	// walks a buffer without one instead.
	copy locate_docid(std::uint64_t docid, bool build = true) const;

	// A key's live copy: the buffers' key maps newest first, then segments newest first.
	copy lookup_key(std::string_view key) const;
	// A copy's key, when the buffer holds it; a segment's is not recoverable from its ordinal.
	std::string_view key_of_copy(const copy& c) const;

	// --- transactions -----------------------------------------------------------------------

	// Each mutation records how to undo it; a rejection unwinds them in reverse. Records stay in
	// the lane, unreferenced; the position index rewinds to its count before the transaction.
	struct undo_step {
		enum kind { key_slot, docid_slot, pending, segment_dirty, last_docid, meta, first_docid_only, fields } type;
		wbuf::version_map::place place{};
		std::size_t a = 0;
		std::string key;
		std::uint64_t prev_value = 0;
		std::optional<std::string> prev_meta;
		bool prev_dirty = false;
	};
	struct buffer_mark {
		std::size_t records = 0, docs = 0;
		std::uint64_t bytes = 0;
		wbuf::lane<record>::mark lane;
	};

	void rollback();

	// Removes a live copy a newer record or tombstone supersedes. In a buffer nothing changes: the
	// newer record shadows it (D6). In a segment it is a pending delete, applied at the freeze.
	void remove_copy(const copy& c);

	// Appends a record and points its key's slot, and its docid's when that map exists, at it:
	// the record is complete before either store (WB-9). A tombstone carries only its key.
	wbuf::ref_t push_record(buffer& b, std::uint64_t docid, std::uint64_t version, const document* doc, std::string_view key, bool tomb, bool by_docid = false);

	// Room for `n` more undo steps, grown geometrically: a push after it cannot throw.
	void reserve_undo(std::size_t n);

	void note_docid_only(buffer& b);

	void bump_last_docid(std::uint64_t docid);

	// The core's stage-1 context: whether the index is keyed, and the transaction's field resolution.
	validate_context context() const { return {bool(cur_.configured & format::bit::keys), fields_res_}; }
	std::optional<std::string_view> key_of(const document& d, bool& valid) const { return keys_plugin::key_of(context(), d, valid); }

	// Validation that can reject runs here, before anything changes (design D10).
	bool valid(const document& d) const { return d.valid(context()); }

	// The active buffer, ready for the transaction: created at the first write, its maps sized so
	// no growth moves a slot the undo log names, its docid map built when the transaction
	// addresses a docid or writes a keyless document.
	buffer& writable_buffer(const transaction& t);

	verdict evaluate(const transaction& t);

	void set_schema(std::uint64_t version) { cur_.schema_version = version; }

	// --- segment identity ----------------------------------------------------------------

	// A new segment's physical identity (F6, F13-R41): the producing term, then a base drawn
	// at open plus a count, through splitmix64's bijective finalizer, so ids never repeat
	// within an open and collide across producers only by chance (Lucene's randomId: a random
	// base, incremented per id).
	std::array<std::uint8_t, 16> new_identity();
	// A seed gives each open its own range, the seed plus 2^32 per generation committed before it,
	// so a seeded index's reopen never repeats an identity its log holds (I13).
	void seed_identities();

	// --- the buffer -------------------------------------------------------------------------

	bool inverting() const noexcept { return !std::is_same_v<inverter, detail::no_inverter> && opts_.flush_threads <= 1; }
	// Buffered documents (tombstones not counted), and every buffered record.
	std::size_t buffer_size() const noexcept;
	// Warm's predicate (D10), under the lease: no buffered record, no pending merge, no unsynced
	// segment, no map_owed turn outstanding.
	bool warm_quiescent() const noexcept;
	std::size_t buffered_records() const noexcept;
	std::string_view buffered_stored(const buffer& b, const record& x) const
		requires(segment::template contains<stored_plugin>());

	// Stage 2 for an accepted transaction: its documents into the active writer. Validation
	// ran in stage 1 (D10), so only memory can fail here, and that fences.
	void invert_pending();

	// Whether the transaction's records fit the active buffer's lane, or an empty one: at most
	// two per operation (a tombstone and the record), each at the widest stride any of them can take (CR9).
	bool lane_fits(const transaction& t) const noexcept;
	// Whether the transaction's documents fit the active buffer's 32-bit structures.
	bool fits(const transaction& t) const noexcept;

	// Nothing may change a buffer a flush borrows from (core/borrow.hh).
	void require_buffers_unpinned() const;
	void clear_buffer();

	// --- freeze and flush (D8) --------------------------------------------------------------

	// Seals the active buffer at a transaction boundary: it becomes the newest frozen buffer, and
	// its pending deletes go to the segments' live bits, copied on write (D6). O(1) in records.
	void freeze();
	// The active buffer's pending deletes set in their segments' bits (D6, D8).
	void apply_pending();

	// Writes the buffers as new segments: a freeze, then each frozen buffer oldest first, each
	// installed as it is written. Each run's memory is charged to the budget on its own permit,
	// settled when its segment is published (D17); the sort's on another, settled when the flush
	// ends. A failure keeps the frozen buffers unwritten for the next flush.
	void flush();

	// A flush's install (D6, WB-16): the oldest frozen buffer leaves, and the new segments (from
	// segs_[first] on) lose every copy a newer buffer supersedes, by key through its key map, and
	// a keyless document by docid through its docid map.
	void install_flushed(std::size_t first);

	// One segment from an inverting buffer: its postings as they are when its docids increase
	// and every document is its docid's newest, else rebuilt in docid order without the others.
	void flush_active(const buffer& b);

	// More than one flush thread, or a layout without an inverter: a frozen buffer's newest
	// records in contiguous runs, one segment each, inverted and encoded in parallel (D18). Runs
	// are of docids, or of keys when the segments are key-ordered (format-14-kv D1a): keyed
	// documents by key, then keyless ones by docid.
	void flush_documents(buffer& b);

	// --- commit -----------------------------------------------------------------------------

	// The commit's input maps (cold-tier D9): every record gets the facts the manifest needs, and a
	// record with new deletes gets its body for the live length. A host error, or a record whose
	// first map failed its checks (corrupt_segment, until heal()), leaves the index as refresh()
	// leaves it: flushed segments and installed merges in segs_, nothing durable, nothing fenced.
	// The facts and bodies installed so far are true and stay.
	void prepare_commit();

	// A segment's live figures at the commit's target, and the ordinals the change list newly
	// deleted in it when the head holds it (D3).
	struct live_figures {
		std::uint64_t count = 0, length = 0;
		std::vector<std::uint64_t> fresh;
	};
	// The head's figures less the change list's ordinals for a segment the head holds (costing
	// those ordinals), else the record's own for a segment born since the head, whose entry
	// carries its whole deleted set.
	std::vector<live_figures> figures_now();
	// The scalars the commit makes durable.
	files::log_detail::scalar_values target_scalars(const std::vector<live_figures>& fig) const;
	std::vector<std::uint64_t> target_pins() const;
	// A segment's entry: its record's facts, the figures, and its deleted set from its bits (D42).
	static files::log_segment entry_of(const open_segment& s, const live_figures& f);
	// The edit from the head to the current state (D3): scalars that differ, pins, segments removed
	// and added, the change list's deletes in segments the head holds, the keys it touched whose
	// value differs from the head's, and the declarations past the head's table.
	files::log_edit edit_now(const files::log_detail::scalar_values& target, const std::vector<std::uint64_t>& pins, const std::vector<live_figures>& fig) const;
	// The current state whole, as a rollover's snapshot at generation `g`.
	files::log_state state_now(std::uint64_t g, const files::log_detail::scalar_values& target, std::vector<std::uint64_t> pins, const std::vector<live_figures>& fig) const;

	// Makes the current state durable as the next generation (manifest-log D4, D5, D7). The new
	// segment files and the directory are ordered first, only when the commit wrote one; then the
	// edit from the head is appended in one write and synced, or, when a rollover is owed (the
	// first commit, a dirty log, install, open(..., to)) or the log would pass D7's bounds, the
	// state is written as a fresh log. An unforced rollover whose temporary meets ENOSPC appends
	// instead and owes the rollover to the next commit (D8), and never past the reader's 2x bound.
	// ENOSPC leaves the index as it was; any other failure fences.
	void commit_files();

	// A step past which the log's durable state is unknown or ahead of memory: any exception fences,
	// not only a system error (an allocation failing in a barrier, a rename or the head's replay).
	template <class F>
	void durable(F&& f);

	// D5: the state as a snapshot under manifest-<g>.tmp, synced at the commit's ordering point with
	// its new segments, renamed, and the directory synced; the old log stays for retention (D8).
	// False when an unforced rollover's temporary meets ENOSPC: the next commit owes it.
	bool roll_over(std::vector<kronuz::journal::File*> files, files::log_state c, bool forced);

	// A deferred index's first manifest, written before its first data file so that no segment is
	// ever on disk without a log beside it: the empty state create() commits, nothing applied, so a
	// crash before the next commit opens it empty. A logged index applies nothing before its first
	// commit (D7's join), so the join's attributes are never owed here.
	void commit_empty();

	// D4: the edit appended at the log's end in one write and synced, after the commit's new
	// segment files and the directory are ordered. A failed write is truncated back first, so
	// ENOSPC leaves the index as it was; a failed truncate-back or sync fences, whatever it threw.
	// The head then replays the edit: any failure there, now that the edit is durable, fences
	// (SR4's writer half).
	void append_edit(std::vector<kronuz::journal::File*>& files, const files::log_edit& e, const std::string& record);

	// The log's descriptor, open again after a demotion closed it (cold-tier D10).
	void reopen_log();

	void write_new(const std::string& name, std::string_view bytes) { write_unsynced(name, bytes)->sync(); }

	std::uint64_t live_length(const open_segment& s) const;

	// --- retention --------------------------------------------------------------------------

	// Keeps the head, the pinned generations and the `retained_predecessors` generations before
	// the head (manifest-log D8): each names its segments and the log holding it. Every other file
	// of strata's on record, logs included, joins the reclaim queue, with no order between logs and
	// data files and no directory sync: nothing promises an obsolete log's generations open. A kept
	// generation whose files cannot be read blocks the round, which then queues nothing. With
	// `data_only` (reclaim()'s pass, D6) it is the same pass.
	void retain(bool = false);
	// The files generation `g` names, the log holding it included: cached for a generation this
	// process made durable, else read from the log on record with the greatest snapshot at or below
	// `g`, replayed to `g` (D6.8, L3), and cached. Null when no log on record holds it, or when its
	// log ends before a predecessor (a generation a torn tail skipped); any other failure, or a pin
	// its log does not reach, sets `blocked`.
	const std::vector<std::string>* kept_files(std::uint64_t g, bool pinned, bool& blocked);
	static std::vector<std::string> names_of(const files::log_state& s, std::uint64_t log);

	// --- recovery ---------------------------------------------------------------------------

	// Whether names are only what an interrupted create leaves: the first log's temporary.
	static bool uncreated(const std::vector<std::string>& names);
	// A directory the IO admitted though another user owns it or others can write it warns or
	// refuses as options::directory_trust says; an IO that does not say is not trusted. Before
	// the lock, so a refusal writes nothing.
	void check_directory_trust(const options& opts);
	// The owner lock is the directory's own (D2), writing nothing. Busy (EWOULDBLOCK) and
	// other lock errors refuse as they are. A filesystem not known local (an IO that does not
	// classify is not), or a lock it does not support, warns or refuses as options::nonlocal
	// says. A failed classification warns as an unknown filesystem, or refuses under refuse.
	void lock_directory(const options& opts);

	// Removes an interrupted create's files, durably, under the owner lock; throws when the
	// directory holds a committed index or a segment. Syncs only when it removed something.
	void clear_uncreated();

	// Opens the highest log, or the one holding `to` (manifest-log D6): replays it and finds every
	// segment it names with its size. It syncs nothing: what a crashed writer left unsynced is owed
	// to this handle's first directory sync (sync-at-first-commit D1). The open creates
	// nothing; it unlinks `.tmp` orphans without a directory sync and leaves retention and reclaim
	// to the first commit or reclaim(). A dirty tail owes a rollover, and its generation is skipped
	// (D6.7). An open at a `to` below the newest generation commits that state again as the newest
	// generation, by rollover, and says so in fallback() (D6.8).
	void recover(std::optional<std::uint64_t> to);

	// Runs a read and its checks, and once more if either fails: an unreliable read (a
	// transient media fault) passes the second time; damage on the medium fails both, and
	// an EIO then reports as corrupt_index, while other errors stay what they are.
	template <class F>
	static auto read_checked(F&& f, std::atomic<std::uint64_t>* retries) -> decltype(f());
	// What this binary reads, against which the log's reader classifies a snapshot or edit before
	// any segment opens (F20, F13-R42).
	static files::readable_by readable() { return {segment::readable, format::min_read_version, format::version}; }
	// Refuses configured bits no flush could write, as damage (CR5): each bit's owner set
	// (F4), and the keys bit set exactly when the index is keyed (F8).
	static void check_readable(std::uint64_t configured);
	static vouch vouch_of(const files::log_state& m) noexcept {
		return {m.mask, std::uint32_t(m.min_version), std::uint32_t(m.max_version), m.fields.size(), m.fields.key_field()};
	}
	static files::segment_ref ref_of(const files::log_segment& s);
	// The segments a state names, checked against its entries: each segment's size, footer
	// checksum, and document count (and every byte, when `whole`), its deleted set converted to the
	// record's bits after its size check (D42). recover and install share it, so a checkpoint is
	// held to what an open is. `sizes` non-null: the records only, their files not opened, each
	// file's existence and size checked against the names and sizes the directory scan recorded
	// (cold-tier D11). Throws corrupt_index (or corrupt_segment) at the first that disagrees.
	static std::vector<std::shared_ptr<open_segment>> load_files(kronuz::journal::IO& io, const files::log_state& m, bool whole,
		const std::shared_ptr<load_context>& load, const std::map<std::string, std::uint64_t>* sizes = nullptr);
	static void fill_deleted(const files::log_deleted& d, open_segment& s);
	// The log's mask and version range passed the reader's readability checks, so a segment it
	// names that this binary cannot read disagrees with its log: damage (F2, F20).
	static std::unique_ptr<open_segment> load_segment(kronuz::journal::IO& io, const vouch& v, const files::segment_ref& r, bool whole, const std::shared_ptr<load_context>& load);
	// A record whose segment file is named and sized as the log says (cold-tier D11).
	static std::unique_ptr<open_segment> load_record(const files::segment_ref& r, const vouch& v, const std::map<std::string, std::uint64_t>& sizes,
		const std::shared_ptr<load_context>& load);
	static std::unique_ptr<segment_body> open_body(kronuz::journal::IO& io, const files::segment_ref& r, const vouch& v, bool whole);
	// Opens, maps and checks a segment file against its manifest entry: size, footer, identity,
	// document count, and the claim `v` (structures, versions, field ids); every byte when
	// `whole`. A map that fails for a reason of the host throws unchanged; the caller tells it
	// from damage.
	static std::unique_ptr<segment_body> open_body_of(kronuz::journal::IO& io, const files::segment_ref& r, const vouch& v, bool whole);

	// Shared with every record: the IO and the counters their maps write.
	std::shared_ptr<load_context> load_;
	std::unique_ptr<executor::group> group_;  // first: assigned first and ended first, in ~basic_index()
	std::unique_ptr<executor::group> runs_group_;  // run builds on the host's executor; inline without one
	std::unique_ptr<executor::group> del_group_;  // reclaim's deletes on the host's executor; inline without one
	std::shared_ptr<delete_chan> del_chan_;
	std::unique_ptr<inline_executor> own_exec_;  // merges' executor when the host passes none
	executor* exec_ = nullptr;
	kronuz::journal::IO* io_;
	std::unique_ptr<kronuz::journal::OwnerLock> lock_;
	options opts_;
	files::log_state m_;  // the durable head: the state the log's last record describes
	// The writer's scalars, ahead of the head until a commit carries them.
	struct live_scalars {
		std::array<char, 16> uuid{};
		std::uint64_t last_docid = 0, next_segment = 1, schema_version = 0, configured = 0;
		files::position applied;
	} cur_;
	// The current log (manifest-log D1, D4): its descriptor while hot, its appender, the generation
	// of its snapshot (0: none yet), its end and the edits it holds; a rollover is owed by the first
	// commit, a dirty tail, install and open(..., to).
	std::unique_ptr<kronuz::journal::File> log_;
	std::optional<files::log_appender> appender_;
	std::uint64_t log_g_ = 0, log_end_ = 0, log_edits_ = 0;
	bool rollover_owed_ = true, drop_pins_ = false;
	std::uint64_t next_generation_ = 1;
	// The change list (D3): ordinals newly deleted in segments since the head, and the metadata keys
	// transactions touched since it.
	std::vector<std::pair<std::uint64_t, std::uint64_t>> deleted_since_;
	std::set<std::string> meta_since_;
	std::vector<std::shared_ptr<open_segment>> segs_;  // by id, as the manifest lists them; a background merge shares its sources
	std::vector<std::shared_ptr<merge_job>> pending_;  // handed to the executor, oldest first
	// The buffers (D18's stage 2, write-buffer D3, D8): the active one takes writes; frozen ones,
	// oldest first, wait for their flush. In an inverting layout each holds its inverter.
	// Views hold them too (write-buffer D5, shared-reader D12).
	std::shared_ptr<buffer> wb_;
	std::vector<std::shared_ptr<buffer>> frozen_;
	std::uint64_t visible_ = 0;  // V (D5): the accepted prefix; readers load the published copy
	std::shared_ptr<index_detail::log_state> logged_;
	std::optional<std::vector<files::attribute>> attributes_owed_;  // the index's attributes the next commit writes (D7's join)
	bool join_owed_ = false;  // the join's first record waits for the attribute's edit (D7)  // a logged index's open record and V (standalone-turn-log D3)
	bool replaying_ = false;  // a logged index's open replays its log (D6.5)
	std::vector<pending_doc> pending_docs_;  // the transaction's documents, inverted once it is accepted
	std::vector<std::uint32_t> pending_declared_;  // their transaction's field resolution (fields-and-keys D2)
	std::uint64_t pending_known_ = 0;
	std::optional<std::uint32_t> pending_key_field_;
	std::uint64_t max_run_bytes_ = inverter::run_cap;  // a run's bytes at most; tests lower it (index_test_access)
	std::size_t run_cap_ = wbuf::fresh_cap;  // a fresh job's level cap (D9); tests lower it (index_test_access)
	mutable std::uint32_t scanning_ = 0;      // scans running; writer calls throw while it is set
	buffer_mark mark_;                         // the active buffer before the transaction stage 1 runs
	std::map<std::string, std::string> meta_;
	std::vector<undo_step> undo_;
	byte_budget::permit permit_;
	byte_budget::permit storage_permit_;
	index_detail::file_ledger ledger_;  // the index's files and their sizes, until each is freed
	std::map<std::uint64_t, std::vector<std::string>> kept_files_;  // a kept generation's files, its log included
	bool retain_owed_ = false;  // an open's: retention waits for the first commit, reclaim() or a drain (D6.6)
	bool inherited_owed_ = false;  // the open's head awaits its sync (sync-at-first-commit D1)
	std::uint64_t inherited_below_ = 0;  // segments below this id were on disk at open
	std::deque<std::string> reclaim_;  // obsolete data files, oldest first
	std::set<std::string> queued_;
	std::optional<std::uint64_t> retention_blocked_;
	std::optional<fallback_report> fallback_;
	std::optional<strata::filesystem_warning> fs_warning_;
	std::optional<strata::directory_trust_warning> trust_warning_;
	std::uint64_t reserved_ = 0;
	std::uint64_t obsoleted_ = 0;  // bytes the merges published in this call made obsolete
	std::uint64_t debt_ = 0;       // obsoleted bytes not yet reclaimed: publishes and commits pay it
	std::uint64_t truncated_ = 0;  // truncated in this reclaim step, awaiting the file's sync
	std::uint64_t held_ = 0;      // reserved by a merge or install while it writes
	std::uint64_t allowance_ = 0;  // the commit allowance
	std::uint64_t applied_since_commit_ = 0;
	std::optional<std::uint64_t> pending_pin_, pending_unpin_;
	// The field table (fields-and-keys D2): the head's is its durable prefix. The epoch moves with
	// the table's content.
	field_table fields_;
	std::uint64_t fields_epoch_ = 0;
	const field_resolution* fields_res_ = nullptr;  // the transaction in stage 1
	bool fenced_ = false;
	const std::atomic<std::uint64_t>* guard_word_ = nullptr;  // the catalog slot's word (guard_calls)
	call_guard guard_held_ = nullptr;
	std::uint64_t producer_term_ = 0, identity_base_ = 0, identities_ = 0;
	std::unique_ptr<publication> pub_;  // with a reader(): views published to its handles
	write_lease lease_{load_->lease};  // the holder's token (owner.hh), in the records' shared context
	// A check-in found the lease held (D2); movable with the index.
	struct owed_flag : std::atomic<bool> {
		owed_flag() noexcept : std::atomic<bool>(false) {}
		owed_flag(owed_flag&& o) noexcept : std::atomic<bool>(o.load()) {}
	} chores_owed_;
	// defer_io: apply answered drain_owed and the host has not run reclaim_for_space() since;
	// that call ran after an answer, and the next apply spends it (answers no_space, not owed).
	bool drain_asked_ = false, drain_ran_ = false;
	// defer_io, cold-tier D3: demote() left records without a body, and apply answers map_owed until
	// promote() maps them (an atomic so that a wake can read it without the lease; CT-R26). The bit
	// is set by that answer and lives until an apply ends the turn: demote() waits for it (CT-R22).
	owed_flag promote_owed_;
	bool map_asked_ = false;
};

// The shared reader's names (docs/design/shared-reader.md D1, D4).
template <class Layout>
using basic_view = typename basic_index<Layout>::view;
template <class Layout>
using basic_reader = typename basic_index<Layout>::reader_handle;

}  // namespace strata

// The member definitions, one header per mechanism (docs/mechanisms/index-layout.md).
#include "strata/core/index_open.hh"
#include "strata/core/index_apply.hh"
#include "strata/core/index_buffer.hh"
#include "strata/core/index_flush.hh"
#include "strata/core/index_turns.hh"
#include "strata/core/index_commit.hh"
#include "strata/core/index_checkpoint.hh"
#include "strata/core/index_storage.hh"
#include "strata/core/index_merge.hh"
#include "strata/core/index_read.hh"
#include "strata/core/index_publish.hh"
