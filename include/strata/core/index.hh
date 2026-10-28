// An index: one shard's segments, live docs, metadata, and manifests in one directory,
// with one writer (Phase 3 design). Transactions apply atomically on the owner core;
// buffered documents flush to segments sorted by docid; commit() makes everything
// applied so far durable through the commit protocol; open() recovers the last durable
// manifest and fails closed when a file it names is missing or differs.
#pragma once

#include "strata/core/borrow.hh"
#include "strata/core/budget.hh"
#include "strata/core/check.hh"
#include "strata/core/deleted_bits.hh"
#include "strata/core/executor.hh"
#include "strata/core/files.hh"
#include "strata/core/hazard.hh"
#include "strata/core/merge.hh"
#include "strata/core/merge_policy.hh"
#include "strata/core/owner.hh"
#include "strata/core/rate_limiter.hh"
#include "strata/core/reader.hh"
#include "strata/core/writer.hh"
#include "strata/core/write_buffer.hh"

#include "completion/io.h"
#include "hashes.hh"

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

template <class Layout>
struct basic_operation {
	enum kind : std::uint8_t { add, replace_key, delete_key, replace_docid, delete_docid, set_meta, delete_meta };
	kind type = add;
	std::uint64_t docid = 0;                       // replace_docid, delete_docid
	std::string key;                               // replace_key, delete_key: the key term
	std::optional<std::uint64_t> expected_version;  // 0: must be absent
	basic_document<Layout> doc;                    // add, replace_*
	std::string meta_key, meta_value;              // set_meta, delete_meta
};

template <class Layout>
struct basic_transaction {
	std::vector<basic_operation<Layout>> ops;
	std::optional<std::uint64_t> expected_schema;  // the schema version it was built against
	std::optional<std::uint64_t> set_schema;       // the schema version it sets
	// The fields its parts name by declaration (`field_ref::declared(i)`), applied in order before its
	// operations: a known name answers its id, an identical declaration too, a new name takes the next id.
	std::vector<field_declaration> fields;
	// The table its `field_ref::id(n)` references were read from; required by any `id` reference.
	std::optional<field_mark> expected_fields;
};

// Apply's answer. A rejected transaction left no trace.
struct verdict {
	enum outcome : std::uint8_t { accepted, version_conflict, schema_conflict, invalid, skipped, no_space, fields_conflict };
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
inline std::string live_name(std::uint64_t id, std::uint64_t gen) { return "seg-" + std::to_string(id) + "." + std::to_string(gen) + ".live"; }
inline std::string spill_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".spill"; }
inline std::string forward_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".fwd"; }
inline std::string records_name(std::uint64_t id) { return "seg-" + std::to_string(id) + ".rec"; }
inline std::string meta_name(std::uint64_t gen) { return "meta-" + std::to_string(gen) + ".kv"; }
inline std::string fields_name(std::uint64_t gen) { return "fields-" + std::to_string(gen) + ".kv"; }
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
	enum kind { segment, live, meta, fields, manifest, manifest_tmp, spill, scratch } type;
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
	if (auto m = strip("meta-", ".kv")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::meta, *v}; }
	}
	if (auto m = strip("fields-", ".kv")) {
		if (auto v = number(*m)) { return parsed_name{parsed_name::fields, *v}; }
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
	if (auto m = strip("seg-", ".live")) {
		const auto dot = m->find('.');
		if (dot != std::string_view::npos) {
			auto id = number(m->substr(0, dot));
			auto gen = number(m->substr(dot + 1));
			if (id && gen) { return parsed_name{parsed_name::live, *id, *gen}; }
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

// Writes a file from its start, gathered a MiB at a time.
class file_sink final : public output {
public:
	explicit file_sink(kronuz::journal::File& f) : f_(f) {}
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

}  // namespace index_detail

namespace detail {
// Whether segments map at open (basic_index_options::eager_segments); STRATA_EAGER_SEGMENTS=0
// turns it off, so a whole test run exercises first-use maps.
inline bool eager_segments_default() noexcept {
	static const bool on = [] {
		const char* e = std::getenv("STRATA_EAGER_SEGMENTS");
		return !(e && *e == '0');
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
	std::string key_prefix;                 // the host's key-term prefix (Xapiand's "Q"); empty: no keys
	// Manifests kept behind the current one (I14). 0 keeps none: damage to the current manifest or
	// newest segment is refused, to be repaired from a replica (or the planned recovery tool). 1 or
	// 2 let a standalone host open(..., to) an older one; its local manifests lost are reported by
	// fallback(), which a host without a journal reports as data loss.
	std::size_t retained_predecessors = 0;
	// The node's indexing budget, if any, and this index's share of it: the buffer flushes
	// when the budget is under pressure, when the buffer passes its share, or when the
	// version map passes a quarter of the share (D17).
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
	// The pool partition's hazard registry (shared-reader D4, D5), shared by the partition's
	// indexes; none: reader() is refused and nothing is published. It outlives the index, its
	// reader handles, and their guards.
	reader_registry* readers = nullptr;
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
	// Maps every segment at open, as an open always did. Off, a segment maps at its first read
	// (cold-tier D2) and the failure rules of D9 apply to it; lazy segments are not the default
	// until apply() rolls back (cold-tier step 1, item 6).
	bool eager_segments = detail::eager_segments_default();
};

// An open that fell back to a manifest older than the newest on disk (open's `to`): the commits
// after `to` are gone from this index's local manifests. `discarded` counts the newer manifests
// found on disk, at most `newest - to` (pinned checkpoints leave gaps); a merge's generation
// holds no new documents, a flush's or delete's does.
struct fallback_report {
	std::uint64_t to = 0;      // the generation opened
	std::uint64_t newest = 0;  // the newest manifest generation found on disk
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
	static basic_index create(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) {
		basic_index ix = create_deferred(io, opts, uuid);
		ix.commit();  // generation 1 (D3-C)
		return ix;
	}

	// Opens the highest durable manifest (or `to`, committed again as the newest), fails
	// closed when a file it names is missing or differs, then deletes orphans. An open at a
	// `to` older than the newest manifest is a fallback and says so in fallback(), never silently.
	static basic_index open(kronuz::journal::IO& io, const options& opts = {}, std::optional<std::uint64_t> to = std::nullopt) {
		basic_index ix(io);
		const lease_scope leased{ix.lease_};
		ix.lock_directory(opts);
		ix.opts_ = opts;
		ix.seed_identities();
		if (opts.budget) { ix.permit_ = opts.budget->open(); }
		if (opts.storage) { ix.storage_permit_ = opts.storage->open(); }
		ix.start_merges();
		ix.recover(to);
		return ix;
	}

	// A moved index keeps its readers' shared state: handles follow it (D11).
	basic_index(basic_index&&) noexcept = default;
	// The target runs its destructor first (its merges' jobs and its views let go under its
	// lease: SR-D5, SR-D2), then takes o's state.
	basic_index& operator=(basic_index&& o) noexcept {
		if (this != &o) {
			std::destroy_at(this);
			std::construct_at(this, std::move(o));
		}
		return *this;
	}
	// Runs close()'s first half (SR-D2): merges ended, the current view retired, released views
	// freed. A view readers still protect is a host bug, since a finished close is its duty: it
	// is reported (an abort in debug builds) and leaks with its segments and the directory lock,
	// so no reader faults and no reopen in this process truncates its files.
	~basic_index() {
		const lease_scope leased{lease_};
		end_merges();
		if (pub_) {
			retire_current();
			free_views();
			if (!pub_->retired.empty()) {
				detail::owner_break("strata: an index destroyed while readers hold its views");
				pub_->retired.leak();
				for (auto& t : pub_->tables) { detail::keep_forever(std::move(t.second)); }  // their readers' tables too
				detail::keep_forever(std::shared_ptr<const kronuz::journal::OwnerLock>(std::move(lock_)));
			}
			pub_.reset();
		}
		segs_.clear();  // the segments drop here, under the lease
	}

	// Releases the owner lock and drops all state; uncommitted work is lost. Background
	// merges queued are dropped and running ones stopped first. The index is fenced
	// afterwards. Never waits for readers (D11): views they still protect keep their segments,
	// files, and the directory lock, and the index is closing() until try_finish_close() frees
	// the last of them. Readers' acquires throw index_closed from here on.
	void close() {
		// Refused inside a flush or a scan's callback before anything is reset, and before the
		// lease: a flush's run thread may call it, and a refusal changes nothing.
		require_buffers_unpinned();
		if (scanning_) { throw std::logic_error("strata: the index was closed inside a scan's callback"); }
		const writer_scope leased{*this};
		if (closing()) { throw index_fenced("strata: the index is closing"); }
		end_merges();
		segs_.clear();
		clear_buffer();
		permit_ = {};
		fenced_ = true;
		if (pub_) {
			retire_current();
			free_views();
			if (!pub_->retired.empty()) {
				pub_->closing = true;
				return;
			}
		}
		storage_permit_ = {};
		lock_.reset();
	}

	// A closing index (D11): frees the views readers released since, and once none is held
	// releases its files' accounting and the directory lock. Past `deadline` (the executor's
	// clock) with views still held, throws readers_active naming their records. True once
	// closed; false while closing.
	bool try_finish_close(std::optional<std::uint64_t> deadline = std::nullopt) {
		const writer_scope leased{*this};
		if (!closing()) {
			if (!fenced_) { throw std::logic_error("strata: try_finish_close() on an open index"); }
			return true;
		}
		free_views();
		if (pub_->retired.empty()) {
			pub_->closing = false;
			storage_permit_ = {};
			lock_.reset();
			return true;
		}
		if (deadline && exec_->now() >= *deadline) {
			std::vector<reader_registry::claim_time> mine;
			for (const auto& c : pub_->rs->registry->claims()) {
				pub_->retired.for_each([&](const view& v) {
					if (c.p == &v) { mine.push_back(c); }
				});
			}
			throw readers_active("strata: readers hold a closing index's views past the deadline", std::move(mine));
		}
		return false;
	}
	bool closing() const noexcept { return pub_ && pub_->closing; }

	// --- writer ---------------------------------------------------------------------------

	// Applies a transaction at a log position. Positions at or below the applied one are
	// skipped (replay). A transaction that stage 1 rejects, or whose stage 1 throws (a segment read
	// that fails, an allocation), is rolled back through the undo log: its slots, pending deletes,
	// records, metadata and storage reservation. The exception is rethrown with the position unmoved.
	verdict apply(const transaction& t, files::position at) {
		const writer_scope leased{*this};
		live();
		// Stage 1 waits for room: the flush happens before this transaction, so a flush
		// failure never loses an accepted transaction's verdict.
		if (needs_flush() || !fits(t) || buffer_size() + t.ops.size() > opts_.buffered_docs_cap) { refresh(); }
		verdict v;
		if (at != files::position{} && at <= m_.applied) {
			v.result = verdict::skipped;
			return v;
		}
		if (t.ops.size() > opts_.buffered_docs_cap || !lane_fits(t)) {
			v.result = verdict::invalid;
			return v;
		}
		// Storage admission: the transaction's estimated bytes (and a segment's fixed bytes
		// when it starts a buffer), then any growth of the commit allowance it causes (a
		// first delete in a segment adds a live-docs file), or no_space.
		const std::uint64_t reserve = storage_permit_ ? estimate_bytes(t) + (buffer_size() == 0 ? segment_overhead : 0) : 0;
		if (reserve && !storage_permit_.try_grow(reserve) && !(drain() && storage_permit_.try_grow(reserve))) {
			v.result = verdict::no_space;
			return v;
		}
		undo_.clear();
		const std::size_t fields_before = fields_.size();
		try {
			v = evaluate(t);
		} catch (...) {
			// Each step is logged before its change can fail, so the undo log covers whatever
			// stage 1 had done when it threw.
			abandon(reserve);
			throw;
		}
		std::uint64_t allowance = allowance_;
		if (v.result == verdict::accepted && storage_permit_) {
			allowance = commit_allowance();
			if (allowance > allowance_ && !storage_permit_.try_grow(allowance - allowance_)) { v.result = verdict::no_space; }
		}
		if (v.result != verdict::accepted) {
			abandon(reserve);
		} else {
			reserved_ += reserve;
			allowance_ = std::max(allowance, allowance_);
			++wb_->transactions;
			visible_ = wb_->base + wb_->transactions;
			wb_->order.publish();
			invert_pending();
			charge();
			if (t.set_schema) { set_schema(*t.set_schema); }
			if (fields_.size() != fields_before) { ++fields_epoch_; }
			++applied_since_commit_;
			changed();
		}
		undo_.clear();
		if (at != files::position{}) {
			m_.applied = at;
			changed();  // a rejected transaction moves the position too (SR-D16)
		}
		// The group's end (D7): the cover first (rule 4), then V, before the writer resumes (rule 3).
		publish_cover();
		if (pub_) { pub_->rs->visible.store(visible_, std::memory_order_release); }
		if (chores_owed_.load(std::memory_order_acquire)) { run_chores(); }
		return v;
	}

	// Undoes a transaction's stage 1: its slots, pending deletes and records, its documents and
	// its storage reservation. Kept rejected records stay counted against the budget.
	void abandon(std::uint64_t reserve) {
		rollback();
		pending_docs_.clear();
		storage_permit_.shrink(reserve);
		charge();
		undo_.clear();
	}

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
	void check_in() noexcept {
		do {
			const chore_scope chores{lease_};
			if (!chores) {
				// Dekker with writer_scope (CR4): a holder that hands off after this store sees it; one
				// that handed off before lets the retry take the lease.
				chores_owed_.store(true, std::memory_order_seq_cst);
				const chore_scope retry{lease_};
				if (!retry) { return; }
				run_chores();
			} else {
				run_chores();
			}
		} while (chores_owed_.load(std::memory_order_seq_cst));  // a check-in that met these chores
	}
	// Whether a check-in found the lease held and left its chores to the holder.
	bool chores_owed() const noexcept { return chores_owed_.load(std::memory_order_acquire); }

	// Writes the buffer as a segment (searchable, not yet durable). ENOSPC leaves the
	// buffer in place to retry once space returns; other failures fence.
	void refresh() {
		const writer_scope leased{*this};
		live();
		guarded([&] { flush(); });
		publish_merges(false, false);
		if (opts_.auto_merge) { maybe_merge(); }
		publish_point();
	}

	// Merges the given segments into one (searchable, not yet durable), waiting for it: their
	// live documents in docid order, deleted ones dropped. Nothing is written when none is
	// live; no room for the output throws, after the merge leaves no trace.
	void merge(const std::vector<std::uint64_t>& ids) {
		const writer_scope leased{*this};
		live();
		apply_pending();  // the merge drops what the buffer deleted (CR5)
		publish_merges(true, false);
		schedule(ids);
		publish_merges(true, true);
	}

	// Merges until at most `max_segments` remain and none holds deletes, by the policy's
	// forced merges. With an executor it waits for every background merge, then for its own.
	// The buffer's pending deletes count as deletes, as in merge().
	void force_merge(std::size_t max_segments = 1) {
		const writer_scope leased{*this};
		apply_pending();
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_merges(std::move(c), max_segments); }, true, true);
	}

	// Merges away the segments whose deleted fraction passes the policy's
	// force_merge_deletes_pct_allowed, waiting like force_merge().
	void force_merge_deletes() {
		const writer_scope leased{*this};
		apply_pending();
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_deletes_merges(std::move(c)); }, true);
	}

	// Runs the policy's merges until it asks for none, or until storage admission has no
	// room for the next; those wait for space. With an executor it hands one round to it,
	// leaving out segments already under merge or damaged, and returns.
	void maybe_merge() {
		const writer_scope leased{*this};
		try {
			run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_merges(std::move(c)); }, false);
		} catch (const map_failure&) {
			// The round is skipped and counted; the next one retries. merge(), force_merge() and
			// force_merge_deletes() rethrow it, as the host asked for those (cold-tier D9).
			load_->skipped.fetch_add(1, std::memory_order_relaxed);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
		}
	}

	// Waits for this index's background merges and publishes them. One that ran out of
	// space is discarded, its sources left for a later merge; any other failure fences.
	void wait_merges() {
		const writer_scope leased{*this};
		live();
		publish_merges(true, false);
	}
	// Background merges handed to the executor and not yet published.
	std::size_t merges_pending() const noexcept { return pending_.size(); }
	// Whether a background merge found a damaged segment, or a first map failed its checks
	// (cold-tier D11); merges skip the first until heal(), and a commit throws corrupt_segment
	// at the second until it.
	bool needs_heal() const noexcept {
		return std::any_of(segs_.begin(), segs_.end(), [](const auto& s) { return s->damaged || s->failed(); });
	}

	// Makes everything applied durable (design D1), then reclaims what retention allows.
	// ENOSPC from a write before that file's sync leaves the index as it was (the partial
	// file removed); any other failure, and any failed sync, rename, or directory sync,
	// fences the writer.
	// With readers, the order is flush, publish, scan and free, retain(), reclaim() (SR-D6): the
	// view this commit retires, which holds the sources of the merges it installed, is freed
	// first when no guard names it, so those sources are queued in the same commit.
	void commit() {
		const writer_scope leased{*this};
		live();
		guarded([&] { flush(); });
		publish_merges(false, false);
		if (opts_.auto_merge) { maybe_merge(); }
		prepare_commit();  // the input maps, outside guarded(): a failure leaves the index as refresh() does
		guarded([&] { commit_files(); });
		publish_point();
		free_views();
		retain();
		reclaim_files(opts_.reclaim_slice);
	}

	// Frees queued obsolete files, about `bytes` of them, each truncated from its end in
	// `reclaim_step` steps and then removed. Returns the bytes freed. Views readers released
	// are freed first, and the files only they held are queued then (D6), so an idle index
	// returns a released view's space here, not at its next commit. With a host executor the
	// files are posted to it, and the bytes returned are the ones posted.
	std::uint64_t reclaim(std::uint64_t bytes = ~std::uint64_t(0)) {
		const writer_scope leased{*this};
		live();
		if (free_views()) { retain(true); }
		return reclaim_files(bytes);
	}
	std::size_t reclaim_pending() const noexcept { return reclaim_.size(); }
	// Owner thread. Waits for the deletes posted to the host's executor and takes their
	// outcomes; a failed one is queued again. It does not sync the directory, so the permit
	// still counts the removed bytes until the next commit or drain(). Without an executor
	// there is nothing to wait for.
	void wait_deletes() {
		const writer_scope leased{*this};
		live();
		finish_deletes();
	}
	// Files posted for deletion whose outcome the owner has not yet collected.
	std::size_t deletes_pending() const noexcept { return posted_.size(); }

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
	std::vector<std::uint64_t> heal() {
		const writer_scope leased{*this};
		live();
		std::vector<std::uint64_t> damaged;
		for (auto& s : segs_) {
			if (s->unsynced) {  // built in memory, never read from disk
				if (s->damaged) { damaged.push_back(s->ref.id); }
				continue;
			}
			if (!s->damaged && !s->failed() && !s->bodied()) { continue; }
			try {
				const segment& sg = s->seg();  // a marked record throws corrupt_segment
				if (s->damaged) {  // a merge found it inconsistent: the checksums alone do not clear it
					basic_check_report<Layout> r;
					check_segment(sg, &s->deleted, r, m_.key_prefix);
				} else {
					sg.verify_all();
				}
				s->damaged = false;
				continue;
			} catch (const corrupt_segment&) {
			}
			try {
				auto fresh = read_checked([&] { return load_segment(*io_, m_, s->ref, true, load_); }, nullptr);
				if (s->damaged) {
					basic_check_report<Layout> r;
					check_segment(fresh->mapped(), &s->deleted, r, m_.key_prefix);
				}
				// The record's own state carries over; the facts are the fresh body's.
				fresh->deleted = std::move(s->deleted);
				fresh->dead = s->dead;
				fresh->deletes_dirty = s->deletes_dirty;
				fresh->ref = s->ref;
				s = std::move(fresh);  // a view keeps the failing object until it is freed (D11)
				load_->retries.fetch_add(1, std::memory_order_relaxed);
				changed();
			} catch (const corrupt_index&) {
				damaged.push_back(s->ref.id);
			} catch (const corrupt_segment&) {
				damaged.push_back(s->ref.id);
			} catch (const std::system_error&) {
				damaged.push_back(s->ref.id);
			}
		}
		publish_point();
		return damaged;
	}

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
	segment_list segments() const {
		lease_.check_read();
		segment_list out;
		out.views_.reserve(segs_.size());
		out.bits_.reserve(segs_.size());
		for (const auto& s : segs_) { out.views_.push_back({s->ref.id, s.get(), s->dead ? &s->deleted : nullptr, s->dead}); }
		if (!wb_ || wb_->pending.empty()) { return out; }
		std::vector<deleted_bits*> own(segs_.size(), nullptr);
		for (const auto& [id, ord] : wb_->pending) {
			const auto it = std::lower_bound(segs_.begin(), segs_.end(), id, [](const auto& x, std::uint64_t y) { return x->ref.id < y; });
			const std::size_t i = std::size_t(it - segs_.begin());
			if (!own[i]) {
				own[i] = &out.bits_.emplace_back((*it)->deleted.clone());
				out.views_[i].deleted = own[i];
			}
			if (!own[i]->test(ord)) {
				own[i]->set(ord);
				++out.views_[i].dead;
			}
		}
		return out;
	}
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
		const std::string& key_prefix() const noexcept { return key_prefix_; }
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
		std::string key_prefix_;
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
		explicit reader_state(reader_registry* r) noexcept : registry(r) {}
		reader_registry* registry;
		std::atomic<const view*> current{nullptr};
		std::atomic<std::uint64_t> seq{0};
		std::atomic<std::uint64_t> at_index{0}, at_applied{0};
		std::atomic<std::uint64_t> visible{0};  // V (write-buffer D5): only the lease holder stores it
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
	// call refreshes, so the handle never starts empty (D1). Needs options::readers.
	reader_handle reader() {
		const writer_scope leased{*this};
		live();
		if (!opts_.readers) { throw std::logic_error("strata: reader() needs index_options::readers"); }
		if (!pub_) {
			pub_ = std::make_unique<publication>();
			pub_->rs = std::make_shared<reader_state>(opts_.readers);
			pub_->rs->visible.store(visible_, std::memory_order_release);
		}
		if (!pub_->current) { refresh(); }
		return reader_handle(pub_->rs);
	}

	// Under the write lease (D9, SR-D20): refreshes only when the current view is behind `p`.
	// Each holder checks after it takes the lease, so of the calls the host's lease order queues
	// behind one that refreshed, none flushes again.
	void publish_through(files::position p) {
		const writer_scope leased{*this};
		live();
		if (!pub_) { throw std::logic_error("strata: publish_through() needs a reader()"); }
		if (pub_->current && pub_->current->position_ >= p) { return; }
		refresh();
	}

	// The views not yet freed (current and retired), and the bytes of the segments only they
	// hold (SR-D7). A view a reader released is freed at the owner's next publication, commit,
	// reclaim(), or try_finish_close().
	struct views_report {
		std::size_t views = 0;
		std::uint64_t bytes = 0;
	};
	views_report views_held() const {
		views_report r;
		if (!pub_) { return r; }
		std::set<const open_segment*> mine, theirs;
		for (const auto& s : segs_) { mine.insert(s.get()); }
		for_each_view([&](const view& v) {
			++r.views;
			for (const auto& s : v.keep_) {
				if (!mine.count(s.get()) && theirs.insert(s.get()).second) { r.bytes += s->ref.size; }
			}
		});
		return r;
	}

	const files::manifest& manifest() const noexcept { return m_; }
	std::uint64_t last_docid() const noexcept { return m_.last_docid; }
	std::uint64_t schema_version() const noexcept { return m_.schema_version; }
	const std::map<std::string, std::string>& metadata() const noexcept { return meta_; }
	// The field table as of the last applied transaction (never an in-flight one: apply runs on the
	// writer's thread), with its mark for `expected_fields` (fields-and-keys D2).
	const field_table& fields() const noexcept { return fields_; }
	field_mark fields_mark() const noexcept { return fields_.mark(); }
	std::size_t buffered_count() const noexcept {
		lease_.check_read();
		return buffer_size();
	}
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
	std::uint64_t digest() const {
		const segment_list views = segments();  // the pending deletes applied, as the freeze applies them (D6)
		std::vector<const buffer*> all;  // oldest first
		for_each_buffer([&](const buffer& b) { all.push_back(&b); });
		// A buffer's record counts when it is its buffer's newest and no newer buffer supersedes it.
		const auto live = [&](std::size_t j, wbuf::ref_t r) {
			if (!newest(*all[j], r)) { return false; }
			const record& x = all[j]->lane.at(r);
			for (std::size_t k = j + 1; k < all.size(); ++k) {
				if (!x.doc().key.empty() && find_key_in(*all[k], x.doc().key) != wbuf::no_ref) { return false; }
				if (all[k]->docids.built() && find_docid_in(*all[k], x.docid) != wbuf::no_ref) { return false; }
			}
			return true;
		};
		return digest_of(views, configured_structures(), m_.last_docid, m_.schema_version, meta_, fields_, [&](auto& document_record, auto& emit) {
			const format::structure_bits configured = configured_structures();
			for (std::size_t j = 0; j < all.size(); ++j) {
				const buffer& b = *all[j];
				if (inverting()) {  // the inverter's buffer: the same records as its documents' (F21)
					if (!b.inv) { continue; }
					std::vector<bool> keep(b.inv->buf.doc_count());
					for (std::size_t i = 0; i < b.order.size(); ++i) {
						const record& x = b.lane.at(b.order[i].ref);
						if (!x.tombstone() && x.ordinal() < keep.size()) { keep[x.ordinal()] = live(j, b.order[i].ref); }
					}
					b.inv->buf.digest(configured, [&](ordinal_t o) { return !keep[o]; }, document_record, emit);
					continue;
				}
				for (std::size_t i = 0; i < b.order.size(); ++i) {
					if (!live(j, b.order[i].ref)) { continue; }
					const record& x = b.lane.at(b.order[i].ref);
					document_record(x.docid, x.version, x.doc().key);
					for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
						if constexpr (requires { typename E::part; E::records(x.docid, static_cast<const typename E::part&>(x.doc()), emit); }) {
							if (configured & E::structure) { E::records(x.docid, static_cast<const typename E::part&>(x.doc()), emit); }
						}
					});
				}
			}
		});
	}

	// The structures this index is configured with, from the manifest (F20, F13-R28). Flushes
	// and merges write them; the digest records them (F21).
	format::structure_bits configured_structures() const noexcept { return m_.configured; }

	// Checks every segment whole (check_segment) and the digest's invariants: one live copy
	// per docid and per key, every docid at or below lastdocid. Throws at the first problem.
	basic_check_report<Layout> check() const {
		basic_check_report<Layout> r;
		for (const auto& s : segs_) { check_segment(s->seg(), &s->deleted, r, m_.key_prefix); }
		if (m_.fields.generation) {  // the file's prefix decodes to the table's durable declarations
			const field_table disk = read_fields(*io_, m_.fields);
			const auto chain = fields_.chain_at(fields_durable_);
			if (disk.size() != fields_durable_ || !chain || disk.mark().chain != *chain) {
				throw corrupt_index("the field table differs from its file");
			}
		}
		(void)digest();
		return r;
	}

	// Commits and pins the manifest; the generation names the checkpoint (design D4).
	std::uint64_t checkpoint() {
		const writer_scope leased{*this};
		live();
		commit();
		const std::uint64_t generation = m_.generation;
		// The pin lives in the next manifest; committing again makes it durable.
		pending_pin_ = generation;
		commit();
		return generation;
	}

	// Unpins a checkpoint; retention reclaims it once nothing else keeps it.
	void release(std::uint64_t generation) {
		const writer_scope leased{*this};
		live();
		pending_unpin_ = generation;
		commit();
	}

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
	void install(kronuz::journal::IO& source, std::uint64_t generation) {
		const writer_scope leased{*this};
		live();
		files::manifest theirs;
		{
			auto f = source.open_existing(index_detail::manifest_name(generation));
			theirs = files::decode_manifest_for(index_detail::read_all(*f), readable());
		}
		if (theirs.generation != generation) { throw corrupt_index("install: manifest generation differs from its name"); }
		check_readable(theirs);
		auto got = load_files(source, theirs, true, load_);
		// The copy, re-encoded live docs (16 bytes for their new names' fields), and the
		// commit's metadata file and manifest.
		std::uint64_t need = theirs.meta.size + theirs.fields.size + 96 + files::encode(theirs).size() + 160 * theirs.segments.size();
		for (const auto& s : got.segments) { need += s->ref.size + (s->ref.live.generation ? s->ref.live.size + 16 : 0); }
		if (storage_permit_ && !storage_permit_.try_grow(need) && !(drain() && storage_permit_.try_grow(need))) {
			throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for an install");
		}
		hold held{this};
		if (storage_permit_) { held.add(need); }
		abandon_merges();  // their sources leave with the old content
		std::vector<files::segment_ref> mines;
		guarded([&] {
			for (auto& s : got.segments) {
				files::segment_ref mine = s->ref;
				mine.id = m_.next_segment++;
				mine.live = {};
				write_new(index_detail::segment_name(mine.id), s->bytes());  // loaded whole: its body is here
				if (s->ref.live.generation) {
					files::live_docs l;
					l.segment = mine.id;
					l.generation = m_.next_live++;
					l.doc_count = s->ref.doc_count;
					for (ordinal_t o = 0; o < s->ref.doc_count; ++o) {
						if ((s->deleted[o >> 6] >> (o & 63)) & 1) { l.deleted.push_back(o); }
					}
					l.live_count = s->ref.live_count;
					l.live_length = s->ref.live_length;
					const std::string lb = files::encode(l);
					write_new(index_detail::live_name(l.segment, l.generation), lb);
					mine.live = {l.generation, lb.size(), files::trailing_crc(lb)};
				}
				mines.push_back(mine);
			}
		});
		// Every copy is written and synced and nothing here has changed, so a host error reopening
		// one (EMFILE, ENOMEM) leaves the index as it was, unfenced; the copies are orphans.
		for (std::size_t i = 0; i < got.segments.size(); ++i) {
			auto& s = got.segments[i];
			if (!opts_.eager_segments) {  // an unmapped record with the facts of the incoming body (cold-tier D9)
				auto mine = std::make_unique<open_segment>(mines[i], load_, vouch_of(theirs));
				mine->structures = s->structures;
				mine->version = s->version;
				mine->total_length = s->total_length;
				mine->facts_known = true;
				mine->deleted = std::move(s->deleted);
				mine->dead = s->dead;
				s = std::move(mine);
			} else if (s->body().mapping) {  // map the copy this index owns: the source may reclaim its file
				s = read_checked([&] { return load_segment(*io_, theirs, mines[i], false, load_); }, &load_->retries);
			} else {
				s->ref = mines[i];
			}
		}
		guarded([&] {
			segs_ = std::move(got.segments);
			clear_buffer();
			reserved_ = 0;  // the discarded buffer's reservations
			if (permit_) { permit_.settle(); }
			meta_ = std::move(got.meta);
			meta_dirty_ = true;
			fields_ = std::move(got.fields);
			fields_rewrite_ = true;
			++fields_epoch_;
			m_.uuid = theirs.uuid;
			m_.last_docid = theirs.last_docid;
			m_.schema_version = theirs.schema_version;
			m_.applied = theirs.applied;
			m_.key_prefix = theirs.key_prefix;
			m_.configured = theirs.configured;
			m_.pinned.clear();
			applied_since_commit_ = 1;
			changed();
			commit_files();
		});
		publish_point();  // earlier views keep the old segments until freed (D11)
		free_views();
		retain();
	}

	// The live document's (docid, version) for a key, if any.
	std::optional<std::pair<std::uint64_t, std::uint64_t>> find_key(std::string_view key) const {
		lease_.check_read();
		if (const copy c = lookup_key(key)) { return std::pair{c.docid, c.version}; }
		return std::nullopt;
	}
	// The live document's stored record for a key, if any: the buffered copy, else the newest
	// segment that holds the key live. For layouts with the `stored` plugin. It allocates the
	// string returned and nothing else (format-14-kv K4).
	std::optional<std::string> get(std::string_view key) const
		requires(segment::template contains<stored_plugin>())
	{
		std::optional<std::string> out;
		get(key, [&](std::string_view v) { out.emplace(v); });
		return out;
	}
	// The same, lending the record to f(std::string_view) for the call, and whether the key
	// is live. A segment's block decodes into this thread's buffer, reused from get to get,
	// so nothing is allocated; a get inside f decodes into a buffer of its own.
	template <class F>
	bool get(std::string_view key, F&& f) const
		requires(segment::template contains<stored_plugin>())
	{
		lease_.check_read();
		// The buffers newest first: the key's newest record answers, a tombstone "absent" (D5, D6).
		const auto in = [&](const buffer& b) -> int {
			const wbuf::ref_t r = find_key_in(b, key);
			if (r == wbuf::no_ref) { return -1; }
			const record& x = b.lane.at(r);
			if (x.tombstone()) { return 0; }
			f(buffered_stored(b, x));
			return 1;
		};
		if (wb_) {
			if (const int got = in(*wb_); got >= 0) { return got; }
		}
		for (std::size_t i = frozen_.size(); i-- > 0;) {
			if (const int got = in(*frozen_[i]); got >= 0) { return got; }
		}
		return get_segments(owned_segments{segs_}, key, f, pending_index());
	}
	// Builds no docid map (WB-23): a buffer without one is walked, newest record first.
	bool has_docid(std::uint64_t docid) const {
		lease_.check_read();
		return bool(locate_docid(docid, false));
	}

	// Whether scan applies to this layout.
	static constexpr bool scannable() {
		return segment::template contains<keys_plugin>() && segment::template contains<stored_plugin>() && std::is_same_v<inverter, detail::no_inverter>;
	}

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
		requires(scannable())
	{
		return scan_keys(from, nullptr, ~std::size_t(0), f);
	}
	template <class F>
	std::size_t scan(std::string_view from, std::size_t n, F&& f) const
		requires(scannable())
	{
		return scan_keys(from, nullptr, n, f);
	}
	// The same for keys in [from, to).
	template <class F>
	std::size_t scan(std::string_view from, std::string_view to, F&& f) const
		requires(scannable())
	{
		return scan_keys(from, &to, ~std::size_t(0), f);
	}
	template <class F>
	std::size_t scan(std::string_view from, std::string_view to, std::size_t n, F&& f) const
		requires(scannable())
	{
		return scan_keys(from, &to, n, f);
	}

private:
	friend struct index_test_access;  // tests reach invariants no public call can break

	// Runs the merges `find` asks for, asking again after each round, until it asks none.
	// With an executor they run there, on segments not under merge or damaged: one round
	// without `wait`, otherwise every round, each waited for and published.
	template <class Find>
	void run_merges(Find find, bool wait, bool forced = false) {
		live();
		if (wait) { publish_merges(true, false); }
		for (;;) {
			std::vector<merge_policy::candidate> c;
			for (const auto& s : segs_) {
				if (merging(*s) || s->damaged) { continue; }
				std::uint64_t deleted = 0;
				for (auto w : s->deleted) { deleted += std::uint64_t(std::popcount(w)); }
				c.push_back({s->ref.id, s->ref.size, s->ref.doc_count, deleted});
			}
			const auto merges = find(std::move(c));
			if (merges.empty()) { return; }
			for (const auto& ids : merges) { schedule(ids, forced); }
			if (!wait && !own_exec_) { return; }  // the index's own executor runs them now
			publish_merges(true, true);
		}
	}

	explicit basic_index(kronuz::journal::IO& io) : load_(std::make_shared<load_context>(&io)), io_(&io) {}

	// Merges run on the host's executor, or on the index's own inline one, synchronously
	// on the writer's thread, when it passes none: one merge implementation either way.
	void start_merges() {
		exec_ = opts_.background;
		if (!exec_) {
			own_exec_ = std::make_unique<inline_executor>();
			exec_ = own_exec_.get();
		}
		group_ = exec_->open();
		if (!own_exec_) {
			runs_group_ = exec_->open();  // scan runs' builds, beside the merges (D9, D12)
			del_group_ = exec_->open();  // reclaim's deletes, apart from the merges
			del_chan_ = std::make_shared<delete_chan>();
		}
	}

	// The digest over segments (an index's or a view's), then what `buffer` adds: one
	// implementation for both (D8).
	template <class Segs, class Buffer>
	static std::uint64_t digest_of(const Segs& segs, format::structure_bits configured, std::uint64_t last_docid, std::uint64_t schema_version,
		const std::map<std::string, std::string>& meta, const field_table& table, Buffer&& buffer) {
		std::uint64_t sum = 0;
		auto emit = [&](std::string_view rec) { sum += xxh64::hash(rec); };
		std::set<std::uint64_t> docids;
		std::set<std::string> keys;
		auto document_record = [&](std::uint64_t docid, std::uint64_t version, std::string_view key) {
			if (docid == 0 || docid > last_docid || !docids.insert(docid).second) { throw std::logic_error("strata: digest: docid invariant"); }
			if (!key.empty() && !keys.insert(std::string(key)).second) { throw std::logic_error("strata: digest: key invariant"); }
			digest_record rec('D');
			rec.u64(docid);
			rec.u64(version);
			rec.bytes(key);
			emit(rec.data());
		};
		for (std::size_t i = 0; i < segs.size(); ++i) {
			const segment_view s = segs[i];
			auto live = [&](ordinal_t o) { return s.live(o); };
			const segment& sg = *s.seg();
			const core_segment& core = sg.core();
			std::vector<std::string> key_of(core.doc_count());  // walked through the keys reader's checks (L3, MR2)
			sg.template get<keys_plugin>().each_key([&](std::string_view key, ordinal_t o) { key_of[o] = key; });
			for (ordinal_t o = 0; o < core.doc_count(); ++o) {
				if (live(o)) { document_record(core.docid(o), sg.template get<keys_plugin>().version(o), key_of[o]); }
			}
			for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
				if constexpr (requires { E::records(sg.template get<E>(), core, live, emit); }) {
					if (configured & E::structure) { E::records(sg.template get<E>(), core, live, emit); }
				}
			});
		}
		buffer(document_record, emit);
		digest_record last('L');
		last.u64(last_docid);
		emit(last.data());
		digest_record schema('S');
		schema.u64(schema_version);
		emit(schema.data());
		for (const auto& [k, v] : meta) {
			digest_record m('M');
			m.bytes(k);
			m.bytes(v);
			emit(m.data());
		}
		for (std::size_t id = 0; id < table.size(); ++id) {  // the field table, in id order
			digest_record f('F');
			f.u32(std::uint32_t(id));
			f.bytes(table[id].name);
			f.u32(std::uint32_t(table[id].role) | std::uint32_t(table[id].postings) << 8 | std::uint32_t(table[id].norms) << 16);
			emit(f.data());
		}
		return sum;
	}

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
			const int c = std::string_view(p.doc().key).compare(q.doc().key);
			return c != 0 ? c < 0 : p.position != q.position ? p.position > q.position : x.ref > y.ref;
		}
	};
	// Whether an entry's key is at or after `key`, whose prefix is `prefix`: a record read only on a tie.
	static bool entry_from(const buffer& b, const wbuf::pos_entry& e, std::uint64_t prefix, std::string_view key) {
		const std::uint64_t p = e.prefix();
		return p != prefix ? p > prefix : std::string_view(b.lane.at(e.ref).doc().key) >= key;
	}
	// Keyless records sit in the position index with a zero prefix and in no run.
	static bool keyed(const buffer& b, const wbuf::pos_entry& e) { return e.prefix() != 0 || !b.lane.at(e.ref).doc().key.empty(); }

	// The lease holder's (D9): installs the splices finished jobs left, then posts a fresh job when
	// a scan asked and a compaction when the big levels call for one. Without a host executor the
	// jobs run here, as merges do. Whether a run set was replaced or first installed.
	bool maintain_runs() const {
		if constexpr (!scannable()) {
			return false;
		} else {
			bool any = false;
			for (const auto& b : frozen_) { any |= maintain_runs(b); }
			if (wb_) { any |= maintain_runs(wb_); }
			return any;
		}
	}
	bool maintain_runs(const std::shared_ptr<buffer>& sb) const {
		buffer& b = *sb;
		bool installed = take_runs(b, b.fresh);
		installed |= take_runs(b, b.compaction);
		const wbuf::run_set* cur = b.runs.load(std::memory_order_relaxed);
		if (!b.fresh.live && b.wants_runs.load(std::memory_order_relaxed)) {
			const std::size_t n1 = b.order.published(), from = cur ? cur->cover : 0;
			if (n1 > from + wbuf::tail_build) {
				b.wants_runs.store(false, std::memory_order_relaxed);
				std::vector<wbuf::level> lv;
				if (cur) { lv = cur->levels; }
				start_runs(sb, b.fresh, [sb, &j = b.fresh, lv = std::move(lv), from, n1, cap = run_cap_]() mutable {
					finish_job(j, [&] {
						std::vector<wbuf::pos_entry> fresh;
						fresh.reserve(n1 - from);
						for (std::size_t i = from; i < n1; ++i) {
							if (keyed(*sb, sb->order[i])) { fresh.push_back(sb->order[i]); }
						}
						return wbuf::fresh_splice(std::move(lv), std::move(fresh), n1, cap, entry_less{sb.get()});
					});
				});
				if (take_runs(b, b.fresh)) {
					installed = true;
					cur = b.runs.load(std::memory_order_relaxed);
				}
			}
		}
		if (cur && !b.compaction.live) {
			std::size_t nb = 0;  // the leading levels past the fresh jobs' cap
			while (nb < cur->levels.size() && cur->levels[nb]->size() > run_cap_) { ++nb; }
			std::vector<wbuf::level> big(cur->levels.begin(), cur->levels.begin() + std::ptrdiff_t(nb));
			if (wbuf::counter_merges(big)) {
				start_runs(sb, b.compaction, [sb, &j = b.compaction, big = std::move(big)] {
					finish_job(j, [&] { return wbuf::compact_splice(big, entry_less{sb.get()}); });
				});
				installed |= take_runs(b, b.compaction);
			}
		}
		return installed;
	}
	// A job's end on the pool: its splice, or a failed one, then ready with release.
	template <class Build>
	static void finish_job(wbuf::run_job& j, Build&& build) noexcept {
		try {
			j.out = build();
		} catch (...) {
			j.out = {};
			j.out.failed = true;
		}
		j.ready.store(true, std::memory_order_release);
	}
	template <class Job>
	void start_runs(const std::shared_ptr<buffer>& sb, wbuf::run_job& j, Job&& job) const {
		if (!runs_group_) {
			j.live = true;
			job();
			return;
		}
		runs_group_->post([sb, job = std::forward<Job>(job)](const task::cancel_token&) mutable { job(); });
		j.live = true;  // a post that throws leaves it clear, so the buffer can post again
	}
	// Applies a finished job's splice to the installed set and installs the result with release;
	// the replaced set waits for D10's stamp while readers may hold it.
	bool take_runs(buffer& b, wbuf::run_job& j) const {
		if (!j.live || !j.ready.load(std::memory_order_acquire)) { return false; }
		j.live = false;
		j.ready.store(false, std::memory_order_relaxed);
		const wbuf::splice sp = std::exchange(j.out, {});
		const wbuf::run_set* cur = b.runs.load(std::memory_order_relaxed);
		auto next = wbuf::apply_splice(cur, sp);
		if (!next) { return false; }
		b.runs.store(next.release(), std::memory_order_release);
		std::unique_ptr<const wbuf::run_set> old(cur);
		if (old && pub_ && live_reads()) { b.retired_runs.push_back(std::shared_ptr<const void>(std::move(old))); }
		return true;
	}

	template <class F>
	std::size_t scan_keys(std::string_view from, const std::string_view* to, std::size_t n, F& f) const {
		lease_.check_read();
		struct guard {
			std::uint32_t& depth;
			explicit guard(std::uint32_t& d) : depth(d) { ++depth; }
			~guard() { --depth; }
		} in_scan(scanning_);
		if (!n || (to && *to <= from)) { return 0; }
		// The lease holder's scan asks for its own builds and installs what finished first.
		for_each_buffer([&](const buffer& b) {
			const wbuf::run_set* rs = b.runs.load(std::memory_order_relaxed);
			if (b.order.published() > (rs ? rs->cover : 0) + wbuf::tail_build) { b.wants_runs.store(true, std::memory_order_relaxed); }
		});
		maintain_runs();
		std::vector<buffer_part> parts;  // oldest first
		parts.reserve(frozen_.size() + 1);
		for_each_buffer([&](const buffer& b) { parts.push_back(part_at(b, ~std::uint64_t(0), from, to)); });
		return scan_segments(owned_segments{segs_}, std::span<buffer_part>(parts), pending_index(), from, to, n, f);
	}

	// --- reads at a position from any thread (write-buffer D5, D6, D9) ----------------------

	static std::uint64_t position_of(const buffer& b, const record& x) noexcept { return b.base + x.position; }
	// The key's newest record at or below r: its slot, then prev while newer (D5).
	static const record* key_at(const buffer& b, std::string_view key, std::uint64_t r) {
		for (wbuf::ref_t x = find_key_in(b, key); x != wbuf::no_ref;) {
			const record& rec = b.lane.at(x);
			if (position_of(b, rec) <= r) { return &rec; }
			x = rec.prev;
		}
		return nullptr;
	}
	// The docid walk at r (D5): whether b holds a record of the docid at or below r. It follows
	// the docid prev field when it names a record, else prev; another docid ends it.
	static bool docid_at(const buffer& b, std::uint64_t docid, std::uint64_t r) {
		for (wbuf::ref_t x = probe_docid(b, docid); x != wbuf::no_ref;) {
			const record& rec = b.lane.at(x);
			if (rec.docid != docid) { return false; }
			if (position_of(b, rec) <= r) { return true; }
			x = rec.docid_prev() != wbuf::no_ref ? rec.docid_prev() : rec.prev;
		}
		return false;
	}
	// D6's docid check: a segment copy is superseded at r when the view's active buffer holds a
	// record of its docid at or below r; below the first docid-only supersession it is skipped.
	static bool superseded(const buffer* active, const segment_view& s, ordinal_t ord, std::uint64_t r) {
		if (!active || r < active->first_docid_only.load(std::memory_order_acquire)) { return false; }
		return docid_at(*active, s.seg()->core().docid(ord), r);
	}
	template <class F>
	static bool get_at(const view& v, std::string_view key, std::uint64_t r, F& f)
		requires(segment::template contains<stored_plugin>())
	{
		const auto in = [&](const buffer& b) -> int {
			const record* x = key_at(b, key, r);
			if (!x) { return -1; }
			if (x->tombstone()) { return 0; }
			f(std::string_view(static_cast<const typename stored_plugin::part&>(x->doc()).stored));
			return 1;
		};
		if (v.active_) {
			if (const int got = in(*v.active_); got >= 0) { return got; }
		}
		for (std::size_t i = v.frozen_.size(); i-- > 0;) {
			if (const int got = in(*v.frozen_[i]); got >= 0) { return got; }
		}
		const buffer* a = v.active_.get();
		return get_segments(v.views_, key, f, nullptr, [&](const segment_view& s, ordinal_t o) { return superseded(a, s, o, r); });
	}
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
	static buffer_part part_at(const buffer& b, std::uint64_t r, std::string_view from, const std::string_view* to) {
		buffer_part p{&b, r, b.runs.load(std::memory_order_acquire), {}, 0};
		const std::size_t cover = p.runs ? p.runs->cover : 0;
		std::size_t end = b.order.published();
		for (std::size_t lo = cover; lo < end;) {  // positions never decrease along the index
			const std::size_t mid = lo + (end - lo) / 2;
			if (position_of(b, b.lane.at(b.order[mid].ref)) <= r) {
				lo = mid + 1;
			} else {
				end = mid;
			}
		}
		if (end > cover + wbuf::tail_build && !b.wants_runs.load(std::memory_order_relaxed)) { b.wants_runs.store(true, std::memory_order_relaxed); }
		const std::uint64_t pf = wbuf::key_prefix(from), pt = to ? wbuf::key_prefix(*to) : 0;
		for (std::size_t i = cover; i < end; ++i) {
			const wbuf::pos_entry e = b.order[i];
			if (keyed(b, e) && entry_from(b, e, pf, from) && !(to && entry_from(b, e, pt, *to))) { p.tail.push_back(e); }
		}
		const entry_less less{&b};
		if (p.tail.size() > 2 * wbuf::tail_select) {  // a seek and a few nexts need only the smallest
			std::nth_element(p.tail.begin(), p.tail.begin() + wbuf::tail_select, p.tail.end(), less);
			std::sort(p.tail.begin(), p.tail.begin() + wbuf::tail_select, less);
			p.sorted = wbuf::tail_select;
		} else {
			std::sort(p.tail.begin(), p.tail.end(), less);
			p.sorted = p.tail.size();
		}
		return p;
	}
	template <class F>
	static std::size_t scan_at(const view& v, std::uint64_t r, std::string_view from, const std::string_view* to, std::size_t n, F& f)
		requires(snapshot_scannable())
	{
		if (!n || (to && *to <= from)) { return 0; }
		std::vector<buffer_part> parts;  // oldest first
		parts.reserve(v.frozen_.size() + 1);
		for (const auto& b : v.frozen_) { parts.push_back(part_at(*b, r, from, to)); }
		if (v.active_) { parts.push_back(part_at(*v.active_, r, from, to)); }
		const buffer* a = v.active_.get();
		return scan_segments(v.views_, std::span<buffer_part>(parts), nullptr, from, to, n, f, [&](const segment_view& s, ordinal_t o) { return superseded(a, s, o, r); });
	}

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
	static bool get_segments(const Segs& segs, std::string_view key, F& f, const pending_set* pending = nullptr, Gone&& gone = {}) {
		const std::uint64_t hash = xxh64::hash(key);
		for (std::size_t i = segs.size(); i-- > 0;) {
			const segment_view s = segs[i];
			if (const auto ord = s.seg()->template get<keys_plugin>().find(key, live_bits{s.deleted ? s.deleted->data() : nullptr}, hash)) {
				if (pending_has(pending, s.id, *ord) || gone(s, *ord)) { return false; }
				index_detail::lent_frame lent;
				f(s.seg()->template get<stored_plugin>().stored(*ord, lent.frame()));
				return true;
			}
		}
		return false;
	}

	// A scan's k-way merge over the segments, oldest first, and the buffers' parts, oldest buffer
	// first (D9). One pass finds the smallest key, comparing prefixes first, and the sources
	// holding it; only those step. Among a key's sources the newest wins: a buffer's over a
	// segment's, a newer buffer's over an older's, within a buffer the newest record at or below
	// its r. A winning tombstone emits nothing (D6).
	template <class Segs, class F, class Gone = no_check>
	static std::size_t scan_segments(const Segs& segs, std::span<buffer_part> buffers, const pending_set* pending, std::string_view from, const std::string_view* to, std::size_t n, F& f, Gone&& gone = {}) {
		if (!n || (to && *to <= from)) { return 0; }
		struct seg_source {
			segment_view seg;
			keys::reader::range cur;
			typename stored_plugin::reader::stored_frame frame;
		};
		const auto skip_dead = [&](seg_source& s) {
			while (s.cur.valid() && ((s.seg.dead && s.seg.deleted->test(s.cur.ordinal())) || pending_has(pending, s.seg.id, s.cur.ordinal()))) { s.cur.next(); }
		};
		std::vector<seg_source> segsrc;  // by segment, oldest first
		segsrc.reserve(segs.size());
		for (std::size_t i = 0; i < segs.size(); ++i) {
			const segment_view sv = segs[i];
			const auto& k = sv.seg()->template get<keys_plugin>();
			if (!k.key_count() || k.max_key() < from || (to && k.min_key() >= *to)) { continue; }
			segsrc.push_back({sv, k.range(from), {}});
			skip_dead(segsrc.back());
		}
		struct source {
			std::uint64_t pre = 0;
			std::string_view key;
			bool valid = false;
			seg_source* seg = nullptr;
			buffer_part* part = nullptr;  // a buffer's level or tail:
			const std::vector<wbuf::pos_entry>* a = nullptr;
			std::size_t at = 0;
			const record* rec = nullptr;
		};
		const auto settle = [](source& s) {
			if (s.seg) {
				s.valid = s.seg->cur.valid();
				if (s.valid) {
					s.key = s.seg->cur.key();
					s.pre = wbuf::key_prefix(s.key);
				}
				return;
			}
			const buffer& b = *s.part->b;
			for (s.valid = false; s.at < s.a->size(); ++s.at) {
				if (s.a == &s.part->tail && s.at == s.part->sorted) { s.part->finish(); }
				const wbuf::pos_entry& e = (*s.a)[s.at];
				const record& x = b.lane.at(e.ref);
				if (position_of(b, x) <= s.part->r) {  // run entries may be newer than r
					s.valid = true;
					s.key = x.doc().key;
					s.pre = e.prefix();
					s.rec = &x;
					return;
				}
			}
		};
		std::vector<source> src;  // the segments oldest first, then each buffer's levels and tail
		src.reserve(segsrc.size() + 8 * buffers.size());
		for (auto& s : segsrc) {
			src.push_back({.seg = &s});
			settle(src.back());
		}
		const std::uint64_t pf = wbuf::key_prefix(from);
		for (buffer_part& p : buffers) {
			if (p.runs) {
				for (const auto& l : p.runs->levels) {
					const auto it = std::partition_point(l->begin(), l->end(), [&](const wbuf::pos_entry& e) { return !entry_from(*p.b, e, pf, from); });
					src.push_back({.part = &p, .a = l.get(), .at = std::size_t(it - l->begin())});
					settle(src.back());
				}
			}
			src.push_back({.part = &p, .a = &p.tail});
			settle(src.back());
		}
		const auto cmp = [](const source& x, const source& y) { return x.pre != y.pre ? (x.pre < y.pre ? -1 : 1) : x.key.compare(y.key); };
		const auto newer = [](const source& x, const source& y) {  // within one buffer
			return x.rec->position != y.rec->position ? x.rec->position > y.rec->position : (*x.a)[x.at].ref > (*y.a)[y.at].ref;
		};
		std::vector<std::size_t> tied;
		tied.reserve(src.size());
		std::size_t count = 0;
		while (count < n) {
			tied.clear();
			const source* min = nullptr;
			for (std::size_t i = 0; i < src.size(); ++i) {
				if (!src[i].valid) { continue; }
				const int c = min ? cmp(src[i], *min) : -1;
				if (c < 0) {
					min = &src[i];
					tied.clear();
				}
				if (c <= 0) { tied.push_back(i); }
			}
			if (!min || (to && min->key >= *to)) { break; }
			// The newest: the last tied source's buffer or segment; within a buffer, its newest record.
			std::size_t pick = tied.back();
			if (src[pick].part) {
				for (const std::size_t i : tied) {
					if (src[i].part == src[pick].part && newer(src[i], src[pick])) { pick = i; }
				}
			}
			const std::string_view key = min->key;
			const std::uint64_t pre = min->pre;
			bool emit = true;
			std::string_view value;
			std::uint64_t version = 0;
			if (seg_source* s = src[pick].seg) {
				const ordinal_t ord = s->cur.ordinal();
				emit = !gone(s->seg, ord);  // the docid check, emitted or not (D9, WB-22)
				if (emit) {
					value = s->seg.seg()->template get<stored_plugin>().stored(ord, s->frame);
					version = s->seg.seg()->template get<keys_plugin>().version(ord);
				}
			} else {
				const record& x = *src[pick].rec;
				emit = !x.tombstone();
				if (emit) {
					value = static_cast<const typename stored_plugin::part&>(x.doc()).stored;
					version = x.version;
				}
			}
			bool more = true;
			if (emit) {
				++count;
				if constexpr (std::is_void_v<std::invoke_result_t<F&, std::string_view, std::string_view, std::uint64_t>>) {
					f(key, value, version);
				} else {
					more = f(key, value, version);
				}
			}
			if (!more) { break; }
			// Every source holding the key moves past it: the buffers' first, while `key` (perhaps a
			// segment cursor's) is still valid.
			for (const std::size_t i : tied) {
				source& s = src[i];
				if (s.seg) { continue; }
				while (s.valid && s.pre == pre && s.key == key) {
					++s.at;
					settle(s);
				}
			}
			for (const std::size_t i : tied) {
				source& s = src[i];
				if (!s.seg) { continue; }
				s.seg->cur.next();
				skip_dead(*s.seg);
				settle(s);
			}
		}
		return count;
	}

	// --- memory -----------------------------------------------------------------------------

	static std::uint64_t doc_bytes(const document& d) { return sizeof(document) + 96 + d.part_bytes(); }
	// The buffers' maps, position indexes, and scan runs.
	std::uint64_t map_bytes() const {
		std::uint64_t n = 0;
		for_each_buffer([&](const buffer& b) { const wbuf::run_set* rs = b.runs.load(std::memory_order_relaxed);
			n += b.keys.bytes() + b.docids.bytes() + b.order.bytes() + (rs ? rs->bytes() : 0); });
		return n;
	}

	// Brings the permit to what the buffer and version map hold now.
	// The buffer's memory: the documents' estimate, or in an inverting layout the inverter's
	// allocations and the records' lane.
	std::uint64_t buffered_bytes() const noexcept {
		std::uint64_t n = 0;
		for_each_buffer([&](const buffer& b) { n += inverting() ? (b.inv ? b.inv->held.held() : 0) + b.lane.bytes() : b.bytes; });
		return n;
	}
	void charge() {
		if (!permit_) { return; }
		const std::uint64_t want = buffered_bytes() + map_bytes();
		if (want > permit_.bytes()) {
			permit_.grow(want - permit_.bytes());
		} else {
			permit_.shrink(permit_.bytes() - want);
		}
	}
	// A buffer no accepted record references still holds a rejected transaction's records.
	bool holds_rejected() const noexcept { return wb_ && wb_->order.size() == 0 && wb_->bytes != 0; }
	bool needs_flush() const {
		if (buffered_records() == 0 && !holds_rejected()) { return false; }
		if (buffered_bytes() >= opts_.buffer_share || map_bytes() >= opts_.buffer_share / 4) { return true; }
		if (!opts_.budget || !opts_.budget->pressure()) { return false; }
		const std::uint64_t div = opts_.pressure_flush_divisor;
		const std::uint64_t share = std::min(opts_.buffer_share, opts_.budget->capacity());
		return !div || buffered_bytes() + map_bytes() >= share / div;
	}

	// --- in-memory state ------------------------------------------------------------------

	// What a record needs to map its body, shared by an index's records: the index's IO and its
	// counters, which readers' maps write (cold-tier D2). Heap-held so a moved index keeps them.
	struct load_context {
		explicit load_context(kronuz::journal::IO* i) noexcept : io(i) {}
		kronuz::journal::IO* io;
		std::atomic<std::uint64_t> retries{0};  // reads that passed on the second try
		std::atomic<std::uint64_t> skipped{0};  // merge rounds a host error dropped at scheduling
	};
	// The manifest's claim a mapped file must meet (F2, F20): its structures within `mask`, its
	// version within the range. Copied from the manifest the record was made under.
	struct vouch {
		format::structure_bits mask = ~format::structure_bits(0);
		std::uint32_t min_version = 0, max_version = ~std::uint32_t(0);
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
		bool deletes_dirty = false;          // deletes since its live-docs generation
		bool damaged = false;                // a background merge found it corrupt; heal() clears it
		std::unique_ptr<kronuz::journal::File> unsynced;  // flushed, awaiting the commit's sync
		// The facts the manifest needs and the entry lacks. Only the owner writes them (under the
		// lease, or before the record is published): a reader's map leaves them in the body.
		format::structure_bits structures = 0;
		std::uint32_t version = 0;
		std::uint64_t total_length = 0;
		bool facts_known = false;
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
		const segment& fail(const char* what) const {
			const void* expected = nullptr;
			if (body_.compare_exchange_strong(expected, failed_marker(), std::memory_order_acq_rel, std::memory_order_acquire) || expected == failed_marker()) {
				throw corrupt_segment(what);
			}
			return static_cast<const segment_body*>(expected)->seg;
		}
		[[gnu::noinline]] const segment& map_body() const {
			assert(detail::no_input_map() == 0 && "an input map inside guarded() or a chore");
			std::unique_ptr<const segment_body> fresh;
			try {
				fresh = read_checked([&] { return open_body(*ctx->io, ref, claim, false); }, &ctx->retries);
			} catch (const corrupt_index& e) {
				return fail(e.what());
			} catch (const corrupt_segment& e) {
				return fail(e.what());
			}
			const void* expected = nullptr;
			if (body_.compare_exchange_strong(expected, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) { return fresh.release()->seg; }
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

	// A record (D3, WB-21), 8-byte aligned, in the lane's bytes:
	//   0  docid u64, version u64, position u32, prev ref u32    stage 1 fills them
	//  24  flags u8 (tombstone, docid prev, ordinal), 3 B padding
	//  28  the fields the flags announce, 4 bytes each, docid prev first, then the ordinal
	//  then the writer's copy of the document, 8-byte aligned: 32, or 40 with both fields.
	// The producer reserves a field from the operation's type; a document is the key only in a
	// tombstone or an inverting layout, whose inverter holds the document. Nothing is written
	// after the record's map slot is stored (WB-9).
	struct record {
		static constexpr std::uint8_t tombstone_flag = 1, docid_prev_flag = 2, ordinal_flag = 4;
		static constexpr std::size_t flags_at = 24, fields_at = 28;
		std::uint64_t docid, version;
		std::uint32_t position;  // its transaction's place in the buffer, from 1: base + position (D5)
		wbuf::ref_t prev;        // the record its key's slot, else its docid's, named before it
		std::uint8_t flags;
		std::uint8_t padding[3] = {};
		std::uint32_t first_field = 0;  // bytes 28 to 32; the fields run on past the header's end
		static constexpr std::size_t fields_of(std::uint8_t f) noexcept { return std::size_t(std::popcount(unsigned(f & (docid_prev_flag | ordinal_flag)))); }
		static constexpr std::size_t doc_at(std::uint8_t f) noexcept { return (fields_at + 4 * fields_of(f) + 7) & ~std::size_t(7); }
		// The record's bytes in the lane, from its flags.
		static constexpr std::size_t stride_for(std::uint8_t f) noexcept { return (doc_at(f) + sizeof(document) + 7) & ~std::size_t(7); }
		record(std::uint64_t d, std::uint64_t v, std::uint32_t pos, wbuf::ref_t p, std::uint8_t f, wbuf::ref_t dprev, std::uint32_t ord, document dc)
			: docid(d), version(v), position(pos), prev(p), flags(f) {
			std::byte* const at = bytes() + fields_at;
			if (f & docid_prev_flag) { std::memcpy(at, &dprev, 4); }
			if (f & ordinal_flag) { std::memcpy(at + (f & docid_prev_flag ? 4 : 0), &ord, 4); }
			new (bytes() + doc_at(f)) document(std::move(dc));
		}
		record(const record&) = delete;
		record& operator=(const record&) = delete;
		~record() { doc().~document(); }
		bool tombstone() const noexcept { return flags & tombstone_flag; }
		bool has_docid_prev() const noexcept { return flags & docid_prev_flag; }
		// The docid's slot before, when it is not prev; no_ref when the flag is not set (D3).
		wbuf::ref_t docid_prev() const noexcept { return has_docid_prev() ? field(0) : wbuf::no_ref; }
		ordinal_t ordinal() const noexcept { return flags & ordinal_flag ? field(has_docid_prev() ? 1 : 0) : 0; }
		std::size_t stride() const noexcept { return stride_for(flags); }
		document& doc() noexcept { return *std::launder(reinterpret_cast<document*>(bytes() + doc_at(flags))); }
		const document& doc() const noexcept { return const_cast<record*>(this)->doc(); }
		// Appends a record to a lane at the stride its flags give.
		static wbuf::ref_t emplace(wbuf::lane<record>& l, std::uint64_t d, std::uint64_t v, std::uint32_t pos, wbuf::ref_t p, std::uint8_t f, wbuf::ref_t dprev, std::uint32_t ord, document dc) {
			return l.append_n(stride_for(f), d, v, pos, p, f, dprev, ord, std::move(dc));
		}

	private:
		std::byte* bytes() noexcept { return reinterpret_cast<std::byte*>(this); }
		std::uint32_t field(std::size_t i) const noexcept {
			std::uint32_t x;
			std::memcpy(&x, reinterpret_cast<const std::byte*>(this) + fields_at + 4 * i, 4);
			return x;
		}
	};
	static_assert(offsetof(record, flags) == record::flags_at && offsetof(record, first_field) == record::fields_at && sizeof(record) == 32);
	static_assert(alignof(document) <= 8 && record::stride_for(record::docid_prev_flag | record::ordinal_flag) <= wbuf::min_chunk);
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
		std::uint64_t bytes = 0;                 // the documents' estimate (doc_bytes)
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
	void for_each_buffer(F&& f) const {
		for (const auto& b : frozen_) { f(*b); }
		if (wb_) { f(*wb_); }
	}
	const pending_set* pending_index() const noexcept { return wb_ && !wb_->pending.empty() ? &wb_->pending_keys : nullptr; }

	void live() const {
		if (fenced_) { throw index_fenced("strata: the writer is fenced; reopen the index"); }
		if (scanning_) { throw std::logic_error("strata: the index changed inside a scan's callback"); }
	}

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
	void guarded(F&& f) {
		try {
			const detail::no_input_map_scope noinput;
			f();
		} catch (const space_error& e) {
			throw std::system_error(e.code(), e.what());
		} catch (const std::system_error& e) {
			fenced_ = true;
			throw index_fenced(std::string("strata: durability failure: ") + e.what());
		}
	}

	// A segment just streamed to `file`: mapped when the backend maps and the mapping holds
	// what was written, so its bytes leave memory; else read back through the file (and charged
	// to `held`). A map that fails keeps the bytes: an output map never fails on a host error and
	// never fences (cold-tier D9).
	std::unique_ptr<open_segment> streamed_segment(std::unique_ptr<kronuz::journal::File> file, files::segment_ref ref, flush_charge* held = nullptr) {
		auto seg = std::make_unique<open_segment>(ref, load_);
		std::shared_ptr<const kronuz::journal::Mapping> m = map_output(*file);
		if (m && m->bytes().size() == ref.size) {
			seg->ref.footer_crc = format::decode_footer(m->bytes()).toc_crc;
			seg->adopt(std::make_unique<segment_body>(std::move(m)));
		} else {
			std::string bytes = index_detail::read_all(*file);  // a backend that cannot map
			seg->ref.footer_crc = format::decode_footer(bytes).toc_crc;
			if (held) { held->grow(bytes.capacity()); }
			seg->adopt(std::make_unique<segment_body>(std::move(bytes)));
		}
		seg->unsynced = std::move(file);
		return seg;
	}
	// A file this index just wrote, mapped: null when the backend cannot map or the map fails.
	static std::shared_ptr<const kronuz::journal::Mapping> map_output(kronuz::journal::File& f) noexcept {
		try {
			return f.map();
		} catch (...) {
			return nullptr;
		}
	}

	// Creates a file and writes it whole. ENOSPC removes the partial file and throws
	// space_error; the caller has changed nothing yet.
	std::unique_ptr<kronuz::journal::File> write_unsynced(const std::string& name, std::string_view bytes) {
		auto f = io_->create_exclusive(name);
		try {
			index_detail::write_all(*f, bytes);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
			f.reset();
			io_->remove(name);
			sync_dir();  // the partial file's space returns once its removal is durable
			throw space_error(e.code(), e.what());
		}
		track(name, bytes.size());
		return f;
	}

	// A writer call's hold on the lease. After its handoff it re-reads the owed flag and runs a
	// check-in that met the held lease, so no chore stays owed with the lease free (CR4).
	class writer_scope {
	public:
		explicit writer_scope(basic_index& ix) noexcept : ix_(ix), lease_(ix.lease_) {}
		~writer_scope() {
			if (lease_.release() && ix_.chores_owed_.load(std::memory_order_seq_cst)) { ix_.check_in(); }
		}
		writer_scope(const writer_scope&) = delete;
		writer_scope& operator=(const writer_scope&) = delete;

	private:
		basic_index& ix_;
		lease_scope lease_;
	};

	// A check-in's chores, under the lease: merges finished on the pool published, views no
	// reader holds freed and their files queued. They never throw: a failure fences the index,
	// and the next writer call reports it through live() (CR2).
	void run_chores() noexcept {
		chores_owed_.exchange(false, std::memory_order_acq_rel);
		if (fenced_ || closing()) { return; }
		try {
			if (!pending_.empty()) {
				guarded([&] { publish_merges(false, false); });
				publish_point();
			}
			if (free_views()) { retain(true); }
		} catch (...) {
			fenced_ = true;
		}
	}

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
	void changed() noexcept {
		if (pub_) { ++pub_->changes; }
	}

	// A publication point (D1): a new view when the state is publishable, with no document
	// buffered, so no transaction is seen in part (SR-D1), and changed since the current view
	// (SR-D16). Then the registry scan frees what no record names (D6).
	void publish_point() {
		if (!pub_ || fenced_ || buffered_records() != 0) { return; }
		if (pub_->current && pub_->changes == pub_->published && !retire_tables()) { return; }
		publish_view(build_view());
		free_views();
	}

	std::unique_ptr<const view> build_view() {
		auto v = std::make_unique<view>();
		v->keep_.reserve(segs_.size());
		v->bits_.reserve(segs_.size());  // segment_views point into it
		v->views_.reserve(segs_.size());
		for (const auto& s : segs_) {
			v->keep_.push_back(s);
			v->bits_.push_back(s->dead ? s->deleted.share() : deleted_bits{});
			v->views_.push_back({s->ref.id, s.get(), s->dead ? &v->bits_.back() : nullptr, s->dead});
		}
		const view* last = pub_->current.get();
		v->meta_ = last && *last->meta_ == meta_ ? last->meta_ : std::make_shared<const std::map<std::string, std::string>>(meta_);
		if (last && last->fields_epoch_ == fields_epoch_) {
			v->fields_ = last->fields_;
		} else {
			v->fields_ = std::make_shared<const field_table>(fields_);
		}
		v->fields_epoch_ = fields_epoch_;
		v->key_prefix_ = m_.key_prefix;
		v->last_docid_ = m_.last_docid;
		v->schema_version_ = m_.schema_version;
		v->configured_ = m_.configured;
		v->position_ = m_.applied;
		v->generation_ = pub_->generation + 1;
		v->visible_ = visible_;
		if (live_reads()) {
			// An empty active buffer now, so the next write needs no view of its own (D5).
			if (!wb_) { wb_ = std::make_shared<buffer>(visible_, opts_.lane_chunks_cap); }
			v->active_ = wb_;
			v->frozen_.assign(frozen_.begin(), frozen_.end());
		}
		return v;
	}

	// Whether readers on other threads read the buffers: not an inverting layout's (D11).
	bool live_reads() const noexcept { return !inverting(); }

	// D7's rule 4 at a group's end: V never passes the newest view's cover, so a view is
	// published when its buffers are not the index's, or a map table was replaced (D10).
	void publish_cover() {
		maintain_runs();
		const bool stamped = retire_tables();
		if (!pub_ || !pub_->current || fenced_ || !live_reads()) { return; }
		const view& c = *pub_->current;
		bool same = stamped ? false : c.active_ == wb_ && c.frozen_.size() == frozen_.size();
		for (std::size_t i = 0; same && i < frozen_.size(); ++i) { same = c.frozen_[i] == frozen_[i]; }
		if (!same) { publish_view(build_view()); }
		// The registry scan, every 8 groups or past 32 retired objects (D10).
		if (++pub_->groups >= 8 || pub_->retired.size() + pub_->tables.size() > 32) { free_views(); }
	}

	// Moves the map tables growth replaced into the retire list, stamped with the current view's
	// generation (D10); freed at once when no reader reads the buffers. Whether any moved.
	bool retire_tables() {
		std::vector<std::shared_ptr<const void>> old;
		const auto take = [&](buffer& b) {
			b.keys.take_retired(old);
			b.docids.take_retired(old);
			for (auto& r : b.retired_runs) { old.push_back(std::move(r)); }
			b.retired_runs.clear();
		};
		if (wb_) { take(*wb_); }
		for (auto& b : frozen_) { take(*b); }
		if (old.empty() || !pub_ || !live_reads()) { return false; }
		for (auto& t : old) { pub_->tables.push_back({pub_->generation, std::move(t)}); }
		return true;
	}

	// Frees the retired tables no reader can still probe: stamped below every view a record names.
	void free_tables() {
		if (!pub_ || pub_->tables.empty()) { return; }
		std::uint64_t oldest = pub_->current ? pub_->current->generation_ : ~std::uint64_t(0);
		pub_->retired.for_each([&](const view& v) { oldest = std::min(oldest, v.generation_); });
		std::erase_if(pub_->tables, [&](const auto& t) { return t.first < oldest; });
	}

	// The view's pointer first, sequentially consistent so the scan's loads come after it (D5);
	// then its position and generation (SR-D15). The old view is retired.
	void publish_view(std::unique_ptr<const view> next) {
		reader_state& rs = *pub_->rs;
		const std::uint64_t g = next->generation_;
		reader_registry::publish(rs.current, next.get());
		if (rs.probe) { rs.probe(0); }
		rs.seq.store(2 * g - 1, std::memory_order_release);
		rs.at_index.store(next->position_.index, std::memory_order_release);
		if (rs.probe) { rs.probe(1); }
		rs.at_applied.store(next->position_.applied, std::memory_order_release);
		rs.seq.store(2 * g, std::memory_order_release);
		if (rs.probe) { rs.probe(2); }
		if (pub_->current) { pub_->retired.retire(std::move(pub_->current)); }
		pub_->current = std::move(next);
		pub_->published = pub_->changes;
		pub_->generation = g;
	}

	// Closing: readers' acquires find null from here on, and the current view is retired.
	void retire_current() {
		reader_registry::publish(pub_->rs->current, static_cast<const view*>(nullptr));
		if (pub_->current) { pub_->retired.retire(std::move(pub_->current)); }
	}

	// Scans the registry and frees every retired view no record names; whether it freed any.
	// What remains is at most one view per record (D6).
	bool free_views() {
		if (!pub_) { return false; }
		pub_->groups = 0;
		if (pub_->retired.empty()) {
			free_tables();
			return false;
		}
		const std::size_t before = pub_->retired.size();
		const std::size_t left = pub_->retired.reclaim(*pub_->rs->registry, exec_->now());
		assert(left <= pub_->rs->registry->capacity());
		free_tables();
		return left < before;
	}

	template <class F>
	void for_each_view(F&& f) const {
		if (!pub_) { return; }
		if (pub_->current) { f(*pub_->current); }
		pub_->retired.for_each(f);
	}

	// --- background merges (D11, D18) ------------------------------------------------------

	bool merging(const open_segment& s) const {
		for (const auto& job : pending_) {
			for (const auto& src : job->sources) {
				if (src.get() == &s) { return true; }
			}
		}
		return false;
	}

	// Hands the merge of `ids` to the executor. It shares the sources and reads a copy of
	// their deletes so far; storage admission reserves the output's estimate as merge()
	// does, on a permit of the merge's own.
	// `forced`: a merge force_merge() asked for.
	void schedule(const std::vector<std::uint64_t>& ids, bool forced = false) {
		auto job = std::make_shared<merge_job>();
		job->reserved = segment_overhead;
		std::uint64_t live_docs = 0;
		for (const auto& s : segs_) {
			if (std::find(ids.begin(), ids.end(), s->ref.id) == ids.end() || merging(*s) || s->damaged) { continue; }
			// A map point, before anything is reserved (cold-tier D9): a source that fails its checks
			// is marked and left out; a host error drops the round.
			try {
				(void)s->seg();
			} catch (const corrupt_segment&) {
				s->damaged = true;
				continue;
			} catch (const std::system_error& e) {
				throw map_failure(e.code(), e.what());
			}
			const std::uint64_t docs = s->ref.doc_count;
			job->sources.push_back(s);
			job->deleted.emplace_back(s->deleted.begin(), s->deleted.end());
			job->reserved += s->ref.size + 18 * docs + 16 * (docs / format::block + 1);
			live_docs += docs - s->dead;
		}
		using mode = typename options::reorder_mode;
		job->reorder = (opts_.reordering == mode::force_merge && forced) || (opts_.reordering == mode::merges && (forced || live_docs >= opts_.reorder_min_docs));
		if (job->sources.empty()) { return; }
		if (storage_permit_) {
			job->storage = opts_.storage->open();
			if (!job->storage.try_grow(job->reserved) && !(drain() && job->storage.try_grow(job->reserved))) {
				throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for a merge");
			}
		}
		job->id = m_.next_segment++;
		job->header = new_identity();
		pending_.reserve(pending_.size() + 1);
		const merge_env ctx{io_, exec_, opts_.rate, opts_.budget, m_.key_prefix, opts_.reorder_settings, configured_structures(),
			opts_.zstd_merged ? std::optional<format::stored_codec>(format::zstd_codec(1)) : std::nullopt, load_};
		group_->post([job, ctx](const task::cancel_token& stop) { run_merge(*job, ctx, stop); });
		pending_.push_back(std::move(job));
	}

	// What a merge uses besides its job; the index's, never the index.
	struct merge_env {
		kronuz::journal::IO* io;
		executor* exec;
		rate_limiter* rate;
		byte_budget* budget;
		std::string key_prefix;
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
	static void run_merge(merge_job& job, const merge_env& ctx, const task::cancel_token& stop) {
		try {
			if (stop.requested()) { throw task::cancelled{}; }
			std::uint64_t live = 0;
			for (std::size_t k = 0; k < job.sources.size(); ++k) {
				std::uint64_t dead = 0;
				for (auto w : job.deleted[k]) { dead += std::uint64_t(std::popcount(w)); }
				live += job.sources[k]->ref.doc_count - dead;
			}
			if (live == 0) {  // nothing to write: publication drops the sources
				job.done.store(true, std::memory_order_release);
				return;
			}
			job.memory = flush_charge(ctx.budget);
			if (!ctx.exec->checkpoint(stop)) { throw task::cancelled{}; }
			std::vector<basic_merge_source<Layout>> sources;
			for (std::size_t k = 0; k < job.sources.size(); ++k) { sources.push_back({&job.sources[k]->mapped(), &job.deleted[k]}); }  // scheduling mapped them (D9)
			const std::string name = index_detail::segment_name(job.id);
			auto file = ctx.io->create_exclusive(name);
			std::string bytes;
			std::shared_ptr<const kronuz::journal::Mapping> mapped;  // the output, mapped; else read into bytes
			try {
				const std::size_t chunk = ctx.rate ? ctx.rate->chunk() : std::size_t(1) << 20;
				std::uint64_t used = 0;
				merge_file out(*file, ctx, job, stop);
				merge_output sink(out, used, chunk);
				std::vector<merge_ref> order;
				// A key-ordered layout's merges order by key themselves and read each source in
				// order, so they take neither a reordering nor a records scratch (format-14-kv D4).
				constexpr bool by_key = segment_policy<Layout>::key_order;
				if constexpr (order_source<Layout>::exists && !by_key) {
					if (job.reorder) {
						merge_scratch forward(ctx, job, used, stop, chunk, index_detail::forward_name(job.id));
						order = order_source<Layout>::order(sources, forward, ctx.reorder, &stop, &job.memory);
					}
				}
				{
					std::optional<merge_scratch> records;
					if (!by_key && (!order.empty() || (sources.size() == 1 && !sources[0].seg->core().docid_ordered()))) {
						records.emplace(ctx, job, used, stop, chunk, index_detail::records_name(job.id));
					}
					merge_spill positions(ctx, job, used, stop, chunk);
					job.plan = detail::merge_layout(std::span<const basic_merge_source<Layout>>(sources), job.header, sink, positions,
						{.cancel = &stop, .memory = &job.memory, .order = order, .records = records ? &*records : nullptr, .key_prefix = ctx.key_prefix,
							.structures = ctx.structures, .stored_codec = ctx.stored_codec});
				}
				sink.finish();
				job.memory.shrink(job.memory.held());  // the build's buffers are gone
				if (!(mapped = map_output(*file))) {
					bytes = index_detail::read_all(*file);  // through the file it holds: no new descriptor
					job.memory.grow(bytes.capacity());
				}
				if ((mapped ? mapped->bytes().size() : bytes.size()) != out.size()) {
					throw std::system_error(EIO, std::generic_category(), "strata: a merge's output reads back short");
				}
			} catch (...) {
				file.reset();
				ctx.io->remove(name);
				ctx.io->sync_directory();  // the partial file's space returns once its removal is durable
				throw;
			}
			const std::string_view view = mapped ? mapped->bytes() : std::string_view(bytes);
			files::segment_ref ref;
			ref.id = job.id;
			ref.identity = job.header;
			ref.size = view.size();
			ref.footer_crc = format::decode_footer(view).toc_crc;
			ref.doc_count = job.plan.doc_count;
			job.output = std::make_unique<open_segment>(ref, ctx.load);
			job.output->adopt(mapped ? std::make_unique<segment_body>(std::move(mapped)) : std::make_unique<segment_body>(std::move(bytes)));  // facts before it is published
			job.output->unsynced = std::move(file);
		} catch (const std::system_error& e) {
			job.error = e.code().value() == ENOSPC ? std::make_exception_ptr(space_error(e.code(), e.what())) : std::current_exception();
		} catch (...) {
			job.error = std::current_exception();
		}
		job.done.store(true, std::memory_order_release);
	}

	// Publishes the background merges that finished, after waiting for all of them with
	// `wait`. One that ran out of space, or that the executor stopped, is dropped, its
	// sources left for a later merge (and with `space_throws`, ENOSPC is thrown once the
	// rest are published). A corrupt source is marked for heal() and skipped until then.
	// Any other failure fences: an I/O error, or a broken invariant.
	void publish_merges(bool wait, bool space_throws) {
		if (!group_ || pending_.empty()) { return; }
		if (wait) { group_->wait(); }
		obsoleted_ = 0;
		std::optional<std::string> space;  // the first refusal's own message: admission's or the disk's
		for (std::size_t i = 0; i < pending_.size();) {
			if (!pending_[i]->done.load(std::memory_order_acquire)) {
				++i;
				continue;
			}
			const auto job = std::move(pending_[i]);
			pending_.erase(pending_.begin() + std::ptrdiff_t(i));
			const job_drop drop{*job};  // whether installed, dropped, or fencing
			if (job->error) {
				job->storage.settle();
				job->memory.settle();
				try {
					std::rethrow_exception(job->error);
				} catch (const space_error& e) {
					if (!space) { space = e.what(); }
					continue;
				} catch (const task::cancelled&) {
					continue;
				} catch (const corrupt_segment& e) {
					if (mark_damaged(*job)) { continue; }
					fence(std::string("strata: a merge's output is corrupt: ") + e.what());
				} catch (const std::system_error& e) {
					fence(std::string("strata: durability failure: ") + e.what());
				} catch (const std::exception& e) {
					fence(std::string("strata: merge failure: ") + e.what());
				}
			}
			install_merge(*job);
		}
		// Published before retain() when publishable, so the view it retires can be freed first.
		publish_point();
		// What the installs obsoleted is owed; retain() queues it, so a file a pending merge, a
		// kept manifest, or a view names is never queued, and this call pays up to the cap.
		if (obsoleted_) {
			debt_ += std::exchange(obsoleted_, 0);
			retain();
		}
		if (debt_) { reclaim_files(std::min(debt_, opts_.reclaim_cap)); }
		collect_deletes();
		settle_storage();
		if (space && space_throws) { throw space_error(std::make_error_code(std::errc::no_space_on_device), *space); }
	}

	[[noreturn]] void fence(const std::string& why) {
		fenced_ = true;
		throw index_fenced(why);
	}

	// Marks the merge's sources that fail check_segment (checksums, then consistency); false when none does.
	bool mark_damaged(const merge_job& job) {
		bool any = false;
		for (const auto& src : job.sources) {
			if (std::find(segs_.begin(), segs_.end(), src) == segs_.end()) { continue; }
			try {
				basic_check_report<Layout> r;
				check_segment(src->mapped(), &src->deleted, r, m_.key_prefix);
			} catch (const corrupt_segment&) {
				src->damaged = true;
				any = true;
			}
		}
		return any;
	}

	// Publishes a finished merge, on the writer's thread. When its sources are all still
	// here, the output replaces them, and each delete made since the merge read them carries
	// over through its ordinal map, once (D11); otherwise the output's file is reclaimed. A
	// delete that maps to no output document breaks the invariant and fences, changing
	// nothing.
	void install_merge(merge_job& job) {
		std::vector<std::size_t> at;
		for (const auto& src : job.sources) {
			const auto it = std::find(segs_.begin(), segs_.end(), src);
			if (it == segs_.end()) { break; }
			at.push_back(std::size_t(it - segs_.begin()));
		}
		const std::string name = index_detail::segment_name(job.id);
		auto reclaim_output = [&] {
			if (!job.output) { return; }
			track(name, job.output->ref.size);
			obsoleted_ += job.output->ref.size;
			job.output.reset();
			reclaim_.push_back(name);
			queued_.insert(name);
		};
		std::vector<ordinal_t> carried;
		if (at.size() == job.sources.size() && job.output) {
			for (std::size_t k = 0; k < at.size(); ++k) {
				const auto& now = segs_[at[k]]->deleted;
				const auto& then = job.deleted[k];
				const auto& to = job.plan.ordinals[k];
				for (std::size_t w = 0; w < now.size(); ++w) {
					for (std::uint64_t fresh = now[w] & ~then[w]; fresh; fresh &= fresh - 1) {
						const ordinal_t o = to[w * 64 + std::size_t(std::countr_zero(fresh))];
						if (o == no_more_docs) {
							reclaim_output();
							settle_storage();
							job.storage.settle();
							job.memory.settle();
							fence("strata: merge publication: a delete names a document the merge dropped");
						}
						carried.push_back(o);
					}
				}
			}
		}
		if (at.size() != job.sources.size()) {
			reclaim_output();
		} else {
			if (job.output) {
				auto& out = *job.output;
				for (ordinal_t o : carried) { out.deleted.set(o); }
				out.dead = carried.size();
				out.deletes_dirty = !carried.empty();
			}
			carry_pending(job);
			changed();
			std::sort(at.begin(), at.end());
			for (std::size_t k = at.size(); k-- > 0;) {
				obsoleted_ += segs_[at[k]]->ref.size;
				segs_.erase(segs_.begin() + std::ptrdiff_t(at[k]));
			}
			if (job.output) {
				// Its id was taken at scheduling, below any flushed since: it goes in id order,
				// which the manifest requires.
				track(name, job.output->ref.size);
				const auto at_id = std::lower_bound(segs_.begin(), segs_.end(), job.id, [](const auto& s, std::uint64_t id) { return s->ref.id < id; });
				segs_.emplace(at_id, std::move(job.output));
			}
		}
		settle_storage();
		job.storage.settle();
		job.memory.settle();  // published: the segment's bytes are the index's now (D17)
	}

	// A merge's install carries the pending deletes naming its sources to its output (D6, WB-2):
	// each (source, ordinal) through the plan's ordinal map. One the output dropped fences.
	void carry_pending(const merge_job& job) {
		if (!wb_ || wb_->pending.empty()) { return; }
		bool moved = false;
		for (auto& [id, ord] : wb_->pending) {
			for (std::size_t k = 0; k < job.sources.size(); ++k) {
				if (job.sources[k]->ref.id != id) { continue; }
				const ordinal_t o = job.output ? job.plan.ordinals[k][ord] : no_more_docs;
				if (o == no_more_docs) { fence("strata: merge publication: a pending delete names a document the merge dropped"); }
				id = job.id;
				ord = o;
				moved = true;
				break;
			}
		}
		if (!moved) { return; }
		wb_->pending_keys.clear();
		for (const auto& [id, ord] : wb_->pending) { wb_->pending_keys.insert(pending_key(id, ord)); }
	}

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
	void end_merges() noexcept {
		runs_group_.reset();
		group_.reset();
		end_deletes();
		for (const auto& job : pending_) { const job_drop drop{*job}; }
		pending_.clear();
	}

	// Ends every background merge unpublished: queued ones are dropped, running ones
	// stopped, and finished outputs' files reclaimed.
	void abandon_merges() {
		if (!group_) { return; }
		group_->cancel();
		for (const auto& job : pending_) {
			const job_drop drop{*job};
			if (job->done.load(std::memory_order_acquire) && job->output) {
				const std::string name = index_detail::segment_name(job->id);
				track(name, job->output->ref.size);
				job->output.reset();
				reclaim_.push_back(name);
				queued_.insert(name);
			}
			job->storage.settle();
			job->memory.settle();
		}
		pending_.clear();
		settle_storage();
	}

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
	static void run_delete(kronuz::journal::IO& io, delete_chan& ch, const std::string& name, std::uint64_t size, std::uint64_t step, const task::cancel_token& stop) {
		std::uint64_t left = size;
		try {
			if (stop.requested()) { throw std::system_error(ECANCELED, std::generic_category(), "strata: delete stopped"); }
			if (left > step) {
				auto f = io.open_existing(name);
				try {
					while (left > step) {
						if (stop.requested()) { throw std::system_error(ECANCELED, std::generic_category(), "strata: delete stopped"); }
						f->truncate(left - step);
						left -= step;
					}
					f->sync();
				} catch (...) {
					try {
						left = f->size();
					} catch (const std::system_error&) {
					}
					throw;
				}
			}
			io.remove(name);
			ch.report(name, std::nullopt);
		} catch (const std::system_error& e) {
			ch.report(name, e.code().value() == ENOENT ? std::nullopt : std::optional<std::uint64_t>(left));
		} catch (...) {
			ch.report(name, left);
		}
	}

	// Takes the posted deletes' outcomes: a removed file's bytes wait in `unlinked_` for a
	// directory sync; a failed one returns to the files on record at the size left, and to
	// the queue's front, its bytes owed again.
	void collect_deletes() {
		if (!del_chan_ || !del_chan_->ready.load(std::memory_order_acquire)) { return; }
		std::vector<std::string> done;
		std::vector<failed_delete> failed;
		{
			const std::lock_guard lock(del_chan_->mu);
			done.swap(del_chan_->done);
			failed.swap(del_chan_->failed);
			del_chan_->ready.store(0, std::memory_order_relaxed);
		}
		for (const auto& name : done) {
			if (auto it = posted_.find(name); it != posted_.end()) {
				inflight_ -= it->second;
				unlinked_ += it->second;
				posted_.erase(it);
			}
		}
		for (auto it = failed.rbegin(); it != failed.rend(); ++it) { requeue_delete(it->name, it->left); }
	}
	void requeue_delete(const std::string& name, std::uint64_t left) {
		const auto it = posted_.find(name);
		if (it == posted_.end()) { return; }
		inflight_ -= it->second;
		posted_.erase(it);
		track(name, left);
		reclaim_.push_front(name);
		queued_.insert(name);
		debt_ += left;
	}
	// Waits for the posted deletes (it never cancels them) and collects. A delete the executor
	// dropped without running reported nothing: its file is requeued as it was.
	void finish_deletes() {
		if (!del_group_) { return; }
		del_group_->wait();
		collect_deletes();
		const std::vector<std::pair<std::string, std::uint64_t>> dropped(posted_.begin(), posted_.end());
		for (const auto& [name, size] : dropped) { requeue_delete(name, size); }
	}
	// close() and the destructor: the deletes in flight finish; a crash's orphans are the next
	// open's.
	void end_deletes() noexcept {
		if (del_group_) {
			try {
				del_group_->wait();
			} catch (...) {
			}
		}
		del_group_.reset();
	}

	// reclaim_files() with an executor: pops the queue up to `bytes`, does the accounting at
	// once (the file leaves the files on record; its bytes stay in the permit until the
	// removal is synced), and posts the delete.
	std::uint64_t reclaim_posted(std::uint64_t bytes) {
		collect_deletes();
		std::uint64_t freed = 0;
		while (!reclaim_.empty() && freed < bytes) {
			const std::string name = std::move(reclaim_.front());
			reclaim_.pop_front();
			queued_.erase(name);
			std::uint64_t size = 0;
			if (auto it = files_.find(name); it != files_.end()) {
				size = it->second;
				on_disk_ -= size;
				files_.erase(it);
			}
			inflight_ += size;
			posted_[name] = size;
			freed += size;
			try {
				del_group_->post([chan = del_chan_, io = io_, name, size, step = opts_.reclaim_step](const task::cancel_token& stop) { run_delete(*io, *chan, name, size, step, stop); });
			} catch (const std::exception&) {
				requeue_delete(name, size);  // not posted: the file stays queued for a later round
				freed -= size;
				break;
			}
		}
		debt_ -= std::min(debt_, freed);
		if (reclaim_.empty()) { debt_ = 0; }
		settle_storage();
		return freed;
	}

	// reclaim()'s file half: the queue, about `bytes` of it.
	std::uint64_t reclaim_files(std::uint64_t bytes) {
		if (del_group_) { return reclaim_posted(bytes); }
		std::uint64_t freed = 0;
		while (!reclaim_.empty() && freed < bytes) {
			const std::string name = reclaim_.front();
			auto it = files_.find(name);
			std::uint64_t size = it == files_.end() ? 0 : it->second;
			std::unique_ptr<kronuz::journal::File> f;
			std::uint64_t cut = 0;
			try {
				if (size > opts_.reclaim_step) {
					// Truncated bytes count as free once the file's sync makes the truncation durable.
					f = io_->open_existing(name);
					while (size > opts_.reclaim_step && freed + cut < bytes) {
						size -= opts_.reclaim_step;
						f->truncate(size);
						track(name, size);
						truncated_ += opts_.reclaim_step;
						cut += opts_.reclaim_step;
					}
					f->sync();
					truncated_ -= cut;
					freed += cut;
					if (size > opts_.reclaim_step) { break; }
				}
				remove_file(name);
			} catch (const std::system_error& e) {
				// A failed round releases what it counted as truncated; the file's size is
				// whatever the truncations left.
				if (cut) {
					truncated_ -= cut;
					try {
						track(name, f->size());
					} catch (const std::system_error&) {
					}
				}
				// The commits stand; the file stays queued for a later round, unless it is gone.
				if (e.code().value() != ENOENT) { break; }
				forget_file(name);
			}
			freed += size;
			queued_.erase(name);
			reclaim_.pop_front();
		}
		debt_ -= std::min(debt_, freed);
		if (reclaim_.empty()) { debt_ = 0; }
		settle_storage();
		return freed;
	}

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

	void track(const std::string& name, std::uint64_t size) {
		auto& at = files_[name];
		on_disk_ += size - at;
		at = size;
	}
	void remove_file(const std::string& name) {
		io_->remove(name);
		forget_file(name);
	}
	// A removed file's bytes stay counted until a directory sync makes its removal durable:
	// filesystems release the blocks then, not at the unlink.
	void forget_file(const std::string& name) {
		if (auto it = files_.find(name); it != files_.end()) {
			on_disk_ -= it->second;
			unlinked_ += it->second;
			files_.erase(it);
		}
	}
	void sync_dir() {
		io_->sync_directory();
		unlinked_ = 0;
	}
	// A bound on a segment's fixed bytes, apart from its documents and terms: the header,
	// footer, and table of contents, each section's alignment and region checksums, and the
	// term index's and key filter's least sizes. (A one-document segment takes 1,088.)
	static constexpr std::uint64_t segment_overhead = 4096;

	// A bound on the files the next commit writes besides segments: a manifest with one
	// more segment entry, a live-docs file for each segment with deletes since its last
	// one, and a metadata file when the metadata changed. Each index holds it standing, so
	// a commit that frees space can always be written.
	std::uint64_t commit_allowance() const {
		std::uint64_t n = manifest_bytes_ + 256;
		for (const auto& s : segs_) {
			if (s->deletes_dirty) { n += 96 + (s->ref.doc_count + 7) / 8; }
		}
		if (meta_dirty_ || !m_.meta.generation) {
			n += 96;
			for (const auto& [k, v] : meta_) { n += k.size() + v.size() + 20; }
		}
		if (fields_owed()) {  // a block of the new declarations, or the whole table
			const bool whole = fields_rewrite_ || !m_.fields.generation || !fields_clean_;
			n += 32 + 8 * (whole ? fields_.size() : fields_.size() - fields_durable_) + (whole ? fields_.name_bytes() : fields_.name_bytes() - fields_durable_names_);
		}
		return n;
	}

	// Whether the next commit writes the field table: new declarations, or a table that replaced the
	// durable one (install).
	bool fields_owed() const noexcept { return fields_.size() != fields_durable_ || fields_rewrite_; }
	// Whether the table's file ends where the manifest says: a torn tail past it means the next commit
	// writes a new file, so no byte any manifest may name is ever overwritten.
	bool clean_fields_tail() const {
		if (!m_.fields.generation) { return true; }
		const auto it = files_.find(index_detail::fields_name(m_.fields.generation));
		return it != files_.end() && it->second == m_.fields.size;
	}

	// Reclaims every queued file for admission that is short of space; false if none was.
	bool drain() {
		collect_deletes();
		if (reclaim_.empty() && !unlinked_ && !inflight_) { return false; }
		reclaim();
		finish_deletes();
		if (unlinked_) {
			try {
				sync_dir();
			} catch (const std::system_error&) {
				return false;  // the removals stay counted; a later sync frees them
			}
			settle_storage();
		}
		return true;
	}

	// Brings the storage permit to the files' bytes, what admission reserved, and the
	// commit allowance.
	void settle_storage() {
		if (!storage_permit_) { return; }
		manifest_bytes_ = files::encode(m_).size() + 160 * segs_.size();
		allowance_ = commit_allowance();
		const std::uint64_t want = on_disk_ + unlinked_ + inflight_ + truncated_ + reserved_ + allowance_ + held_;
		if (want > storage_permit_.bytes()) {
			storage_permit_.grow(want - storage_permit_.bytes());
		} else {
			storage_permit_.shrink(storage_permit_.bytes() - want);
		}
	}
	// A transaction's bytes on disk, overestimated: stored records, each term with its
	// posting and positions, a per-document overhead, and deletes' live-docs entries.
	static std::uint64_t estimate_bytes(const transaction& t) {
		std::uint64_t n = 64;
		for (const auto& o : t.ops) {
			n += 16 + o.key.size() + o.meta_key.size() + o.meta_value.size();
			n += 64 + o.doc.disk_estimate();  // a per-document overhead, and the parts
		}
		return n;
	}

	// --- lookups ----------------------------------------------------------------------------

	static wbuf::ref_t find_key_in(const buffer& b, std::string_view key) {
		return b.keys.find(index_detail::key_hash(key), [&](wbuf::ref_t r) { return std::string_view(b.lane.at(r).doc().key) == key; });
	}
	// The const probe (WB-23): any thread; no table means no docid-only supersession.
	static wbuf::ref_t probe_docid(const buffer& b, std::uint64_t docid) {
		return b.docids.find(index_detail::docid_hash(docid), [&](wbuf::ref_t r) { return b.lane.at(r).docid == docid; });
	}
	// The lease holder's: builds the map first (D8's exception for a frozen buffer).
	wbuf::ref_t find_docid_in(const buffer& b, std::uint64_t docid) const {
		build_docids(b);
		return probe_docid(b, docid);
	}
	// The docid map, from the position index, the first time stage 1 or a flush needs it (D3,
	// WB-23): the table is published, then filled; every record takes its docid's slot in
	// order, so the newest names each docid. It runs inline under the lease at any size: apply()
	// is synchronous, so a post to the pool would only block the caller (the async seam is step 3).
	void build_docids(const buffer& b) const {
		if (b.docids.built()) { return; }
		b.docids.reserve(b.order.size());
		for (std::size_t i = 0; i < b.order.size(); ++i) { store_docid(b, b.order[i].ref); }
	}
	wbuf::version_map::place store_docid(const buffer& b, wbuf::ref_t r) const {
		const std::uint64_t d = b.lane.at(r).docid;
		const std::uint32_t h = index_detail::docid_hash(d);
		const auto p = b.docids.locate(h, [&](wbuf::ref_t q) { return b.lane.at(q).docid == d; });
		b.docids.store(p, h, r);
		return p;
	}
	// Whether a buffer's record is its key's (else its docid's) newest and not a tombstone: what
	// its flush writes. A frozen buffer keeps no live bits; newer records shadow its copies (WB-16).
	bool newest(const buffer& b, wbuf::ref_t r) const {
		const record& x = b.lane.at(r);
		if (x.tombstone()) { return false; }
		return x.doc().key.empty() ? find_docid_in(b, x.docid) == r : find_key_in(b, x.doc().key) == r;
	}
	copy buffered_copy(const buffer& b, typename copy::where_t w, std::size_t at, wbuf::ref_t r) const {
		const record& x = b.lane.at(r);
		if (x.tombstone()) { return {}; }
		return {w, at, r, 0, x.docid, x.version};
	}

	// A docid's live copy: the buffers newest first, through their docid maps, then the segments
	// newest first with their live bits and the pending deletes.
	// Stage 1 builds a buffer's map on its first docid lookup (D8's exception); `build` false
	// walks a buffer without one instead.
	copy locate_docid(std::uint64_t docid, bool build = true) const {
		const auto find = [&](const buffer& b) {
			if (build || b.docids.built()) { return find_docid_in(b, docid); }
			for (std::size_t i = b.order.size(); i-- > 0;) {
				if (b.lane.at(b.order[i].ref).docid == docid) { return b.order[i].ref; }
			}
			return wbuf::no_ref;
		};
		if (wb_) {
			if (const auto r = find(*wb_); r != wbuf::no_ref) { return buffered_copy(*wb_, copy::active, 0, r); }
		}
		for (std::size_t i = frozen_.size(); i-- > 0;) {
			if (const auto r = find(*frozen_[i]); r != wbuf::no_ref) { return buffered_copy(*frozen_[i], copy::frozen, i, r); }
		}
		const pending_set* pending = pending_index();
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			const segment& sg = s.seg();  // a map point under the lease (cold-tier D9)
			if (auto ord = sg.core().ordinal_of(docid); ord && !((s.deleted[*ord >> 6] >> (*ord & 63)) & 1)) {
				if (pending_has(pending, s.ref.id, *ord)) { return {}; }
				return {copy::segment, i, wbuf::no_ref, *ord, docid, sg.template get<keys_plugin>().version(*ord)};
			}
		}
		return {};
	}

	// A key's live copy: the buffers' key maps newest first, then segments newest first.
	copy lookup_key(std::string_view key) const {
		if (wb_) {
			if (const auto r = find_key_in(*wb_, key); r != wbuf::no_ref) { return buffered_copy(*wb_, copy::active, 0, r); }
		}
		for (std::size_t i = frozen_.size(); i-- > 0;) {
			if (const auto r = find_key_in(*frozen_[i], key); r != wbuf::no_ref) { return buffered_copy(*frozen_[i], copy::frozen, i, r); }
		}
		const pending_set* pending = pending_index();
		const std::uint64_t hash = xxh64::hash(key);  // one hash, one filter probe per segment (CR7)
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			const segment& sg = s.seg();  // a map point under the lease (cold-tier D9)
			const auto& keys = sg.template get<keys_plugin>();
			if (const auto ord = keys.find(key, live_bits{s.deleted.data()}, hash)) {
				if (pending_has(pending, s.ref.id, *ord)) { return {}; }
				return {copy::segment, i, wbuf::no_ref, *ord, sg.core().docid(*ord), keys.version(*ord)};
			}
		}
		return {};
	}
	// A copy's key, when the buffer holds it; a segment's is not recoverable from its ordinal.
	std::string_view key_of_copy(const copy& c) const {
		if (c.where == copy::active) { return wb_->lane.at(c.ref).doc().key; }
		if (c.where == copy::frozen) { return frozen_[c.at]->lane.at(c.ref).doc().key; }
		return {};
	}

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

	void rollback() {
		buffer* b = wb_.get();
		for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
			switch (it->type) {
			case undo_step::key_slot: b->keys.restore(it->place); break;
			case undo_step::docid_slot: b->docids.restore(it->place); break;
			case undo_step::pending:
				b->pending_keys.erase(pending_key(b->pending.back().first, b->pending.back().second));
				b->pending.pop_back();
				break;
			case undo_step::segment_dirty: segs_[it->a]->deletes_dirty = it->prev_dirty; break;
			case undo_step::last_docid: m_.last_docid = it->prev_value; break;
			case undo_step::first_docid_only: b->first_docid_only.store(it->prev_value, std::memory_order_release); break;
			case undo_step::fields: fields_.truncate(it->a); break;
			case undo_step::meta:
				if (it->prev_meta) {
					meta_[it->key] = *it->prev_meta;
				} else {
					meta_.erase(it->key);
				}
				meta_dirty_ = it->prev_dirty;
				break;
			}
		}
		if (b) {
			// With readers, a rejected record may be under a reader that loaded its slot before the
			// restore: it stays in the lane, unreferenced, and its bytes stay counted, so the flush
			// threshold bounds them and the freeze reclaims them (D4, CR1).
			const bool keep = pub_ && live_reads();
			if (!keep) {
				b->lane.truncate(mark_.lane);  // the rejected records and their document copies
				b->bytes = mark_.bytes;
			}
			b->order.rewind(mark_.records);
			b->docs = mark_.docs;
		}
	}

	// Removes a live copy a newer record or tombstone supersedes. In a buffer nothing changes: the
	// newer record shadows it (D6). In a segment it is a pending delete, applied at the freeze.
	void remove_copy(const copy& c) {
		if (c.where != copy::segment) { return; }
		auto& s = *segs_[c.at];
		buffer& b = *wb_;
		assert(s.ref.id < (std::uint64_t(1) << 32));
		reserve_undo(2);  // each step is logged before the change it undoes can fail
		b.pending.push_back({s.ref.id, c.ord});
		undo_.push_back({undo_step::pending});
		b.pending_keys.insert(pending_key(s.ref.id, c.ord));
		undo_.push_back({undo_step::segment_dirty, {}, c.at, {}, 0, {}, s.deletes_dirty});
		s.deletes_dirty = true;  // its live-docs file changes at the next freeze
	}

	// Appends a record and points its key's slot, and its docid's when that map exists, at it:
	// the record is complete before either store (WB-9). A tombstone carries only its key.
	wbuf::ref_t push_record(buffer& b, std::uint64_t docid, std::uint64_t version, const document* doc, std::string_view key, bool tomb, bool by_docid = false) {
		const std::uint32_t kh = key.empty() ? 0 : index_detail::key_hash(key);
		wbuf::version_map::place kp;
		if (!key.empty()) { kp = b.keys.locate(kh, [&](wbuf::ref_t q) { return std::string_view(b.lane.at(q).doc().key) == key; }); }
		wbuf::ref_t prev = wbuf::no_ref;
		if (!key.empty()) {
			prev = wbuf::version_map::ref_of(kp);
		} else if (b.docids.built()) {
			prev = probe_docid(b, docid);
		}
		// A replace_docid reserves the docid prev field; it names the docid's record when that is
		// not prev (D3, WB-21). Its map exists: writable_buffer built it.
		wbuf::ref_t dprev = wbuf::no_ref;
		std::uint8_t flags = tomb ? record::tombstone_flag : 0;
		if (by_docid) {
			flags |= record::docid_prev_flag;
			if (const wbuf::ref_t d = probe_docid(b, docid); d != prev) { dprev = d; }
		}
		std::uint32_t ordinal = 0;
		if (inverting() && !tomb) {
			flags |= record::ordinal_flag;
			ordinal = std::uint32_t((b.inv ? b.inv->buf.doc_count() : 0) + pending_docs_.size());
		}
		document copy_of;
		if (tomb || inverting()) {
			copy_of.key.assign(key);
		} else {
			copy_of = *doc;
			for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
				if constexpr (requires(typename E::part& p) { E::buffered(p); }) { E::buffered(static_cast<typename E::part&>(copy_of)); }
				// A part's references become ids: the buffer stores what the transaction resolved.
				if constexpr (requires(typename E::part& p, const field_resolution& r) { E::resolve(p, r); }) {
					if (fields_res_) { E::resolve(static_cast<typename E::part&>(copy_of), *fields_res_); }
				}
			});
		}
		const wbuf::ref_t r = record::emplace(b.lane, docid, version, b.transactions + 1, prev, flags, dprev, ordinal, std::move(copy_of));
		reserve_undo(2);  // so logging the slots below cannot throw after a store
		if (!key.empty()) {
			b.keys.store(kp, kh, r);
			undo_.push_back({undo_step::key_slot, kp});
		}
		if (b.docids.built()) { undo_.push_back({undo_step::docid_slot, store_docid(b, r)}); }
		b.order.push_back(wbuf::make_entry(b.lane.at(r).doc().key, r));
		if (!tomb) { ++b.docs; }
		b.bytes += tomb || inverting() ? record::stride_for(flags) : doc_bytes(*doc);
		return r;
	}

	// Room for `n` more undo steps, grown geometrically: a push after it cannot throw.
	void reserve_undo(std::size_t n) {
		if (undo_.capacity() - undo_.size() < n) { undo_.reserve(std::max(undo_.capacity() * 2, undo_.size() + n)); }
	}

	void note_docid_only(buffer& b) {
		const std::uint64_t at = b.base + b.transactions + 1, was = b.first_docid_only.load(std::memory_order_relaxed);
		if (was <= at) { return; }
		undo_.push_back({undo_step::first_docid_only, {}, 0, {}, was});
		b.first_docid_only.store(at, std::memory_order_release);
	}

	void bump_last_docid(std::uint64_t docid) {
		if (docid <= m_.last_docid) { return; }
		undo_.push_back({undo_step::last_docid, {}, 0, {}, m_.last_docid});
		m_.last_docid = docid;
	}

	// The core's stage-1 context: the index's key prefix (F8).
	validate_context context() const { return {m_.key_prefix, fields_res_}; }
	std::optional<std::string_view> key_of(const document& d, bool& valid) const { return keys_plugin::key_of(context(), d, valid); }

	// Validation that can reject runs here, before anything changes (design D10).
	bool valid(const document& d) const { return d.valid(context()); }

	// The active buffer, ready for the transaction: created at the first write, its maps sized so
	// no growth moves a slot the undo log names, its docid map built when the transaction
	// addresses a docid or writes a keyless document.
	buffer& writable_buffer(const transaction& t) {
		if (!wb_) { wb_ = std::make_shared<buffer>(visible_, opts_.lane_chunks_cap); }
		buffer& b = *wb_;
		mark_ = {b.order.size(), b.docs, b.bytes, b.lane.tell()};  // before anything that can throw: a rollback restores it
		bool docids = false;
		for (const auto& o : t.ops) {
			const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
			docids = docids || o.type == operation::replace_docid || o.type == operation::delete_docid || (writes && o.doc.key.empty());
		}
		b.keys.reserve(2 * t.ops.size());
		if (docids) { build_docids(b); }
		if (b.docids.built()) { b.docids.reserve(2 * t.ops.size()); }
		return b;
	}

	verdict evaluate(const transaction& t) {
		verdict v;
		buffer& b = writable_buffer(t);
		if (t.expected_schema && *t.expected_schema != m_.schema_version) {
			v.result = verdict::schema_conflict;
			v.current_version = m_.schema_version;
			return v;
		}
		// The fields (D2): the host's mark first, then each declaration by name, then the resolution
		// the parts' references are checked against.
		if (t.expected_fields) {
			const auto chain = fields_.chain_at(t.expected_fields->size);
			if (!chain || *chain != t.expected_fields->chain) {
				v.result = verdict::fields_conflict;
				v.current_fields = fields_.mark();
				return v;
			}
		}
		std::vector<std::uint32_t> declared_ids;
		declared_ids.reserve(t.fields.size());
		bool logged = false;
		for (std::size_t i = 0; i < t.fields.size(); ++i) {
			const field_declaration& d = t.fields[i];
			auto refuse = [&] {
				v.result = verdict::invalid;
				v.field = i;
				return v;
			};
			if (field_problem(d)) { return refuse(); }
			if (const auto id = fields_.find(d.name)) {
				if (fields_[*id] != d) { return refuse(); }  // a different declaration of a known name
				declared_ids.push_back(*id);
				continue;
			}
			// One key field, in a keyed index (D7).
			if (d.role == field_role::key && (m_.key_prefix.empty() || fields_.key_field())) { return refuse(); }
			if (!logged) {
				undo_.push_back({undo_step::fields, {}, fields_.size()});  // logged before the table changes
				logged = true;
			}
			declared_ids.push_back(fields_.append(d));
		}
		const field_resolution resolution{declared_ids, t.expected_fields ? t.expected_fields->size : 0, fields_.key_field()};
		pending_declared_ = declared_ids;
		pending_known_ = resolution.known;
		pending_key_field_ = resolution.key;
		struct resolution_scope {
			const field_resolution*& slot;
			~resolution_scope() { slot = nullptr; }
		} resolution_guard{fields_res_};
		fields_res_ = &resolution;
		for (std::size_t i = 0; i < t.ops.size(); ++i) {
			const operation& o = t.ops[i];
			v.op = i;
			auto reject = [&](verdict::outcome why, std::uint64_t current = 0) {
				v.result = why;
				v.current_version = current;
				return v;
			};
			switch (o.type) {
			case operation::set_meta:
			case operation::delete_meta: {
				if (o.meta_key.starts_with(files::reserved_meta_prefix)) { return reject(verdict::invalid); }  // the engine's (F20)
				undo_step u{undo_step::meta};
				u.key = o.meta_key;
				u.prev_dirty = meta_dirty_;
				if (auto it = meta_.find(o.meta_key); it != meta_.end()) { u.prev_meta = it->second; }
				undo_.push_back(std::move(u));
				if (o.type == operation::set_meta) {
					meta_[o.meta_key] = o.meta_value;
				} else {
					meta_.erase(o.meta_key);
				}
				meta_dirty_ = true;
				continue;
			}
			default:
				break;
			}
			const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
			bool key_valid = true;
			std::optional<std::string_view> doc_key;  // into the operation, which outlives stage 2
			if (writes) {
				if (!valid(o.doc)) { return reject(verdict::invalid); }
				doc_key = key_of(o.doc, key_valid);
				if (!key_valid) { return reject(verdict::invalid); }
			}
			// The key the operation addresses and its live copy.
			std::optional<std::string_view> key;
			if (o.type == operation::replace_key || o.type == operation::delete_key) {
				if (m_.key_prefix.empty() || !o.key.starts_with(m_.key_prefix)) { return reject(verdict::invalid); }
				key = o.key;
				if (writes && doc_key != key) { return reject(verdict::invalid); }
			} else if (writes) {
				key = doc_key;
			}
			copy by_key;
			if (key) { by_key = lookup_key(*key); }
			copy by_docid;
			if (o.type == operation::replace_docid || o.type == operation::delete_docid) {
				if (o.docid == 0) { return reject(verdict::invalid); }
				by_docid = locate_docid(o.docid);
				// A key and a docid naming different live documents reject.
				if (by_key && by_docid && by_key.docid != o.docid) { return reject(verdict::invalid); }
				if (by_key && !by_docid) { return reject(verdict::invalid); }
			}
			// The current version of the addressed document.
			const copy& target = by_key ? by_key : by_docid;
			const std::uint64_t current = target.version;
			const bool exists = bool(target);
			if (o.type == operation::add && exists) { return reject(verdict::version_conflict, current); }
			if (o.expected_version) {
				// A version belongs to a key: a conditional operation without one is invalid.
				if (!key) { return reject(verdict::invalid); }
				if (*o.expected_version == 0 ? exists : (!exists || *o.expected_version != current)) { return reject(verdict::version_conflict, current); }
			}
			// The superseded copy's key, when known: a tombstone for it shadows the copy (D6).
			const std::string_view old_key = by_key ? *key : key_of_copy(target);
			// A segment copy found by docid alone: readers at or above this position check docids (WB-22).
			if (exists && target.where == copy::segment && !by_key) { note_docid_only(b); }
			if (o.type == operation::delete_key || o.type == operation::delete_docid) {
				if (exists) {
					remove_copy(target);
					push_record(b, target.docid, target.version, nullptr, old_key, true);
					v.docids.push_back(target.docid);
				} else {
					v.docids.push_back(0);
				}
				v.versions.push_back(0);
				continue;
			}
			// add or replace: the docid kept, given, or assigned.
			std::uint64_t docid;
			if (o.type == operation::replace_docid) {
				docid = o.docid;
			} else if (by_key) {
				docid = by_key.docid;
			} else {
				docid = m_.last_docid + 1;
			}
			if (exists) {
				remove_copy(target);
				if (!old_key.empty() && (!key || old_key != *key)) { push_record(b, target.docid, target.version, nullptr, old_key, true); }
			}
			// A version belongs to a key: a document without one has version 0 (F9).
			const std::uint64_t version = !key ? 0 : exists ? current + 1 : 1;
			bump_last_docid(docid);
			const std::string_view new_key = key ? std::string_view(o.doc.key) : std::string_view();
			push_record(b, docid, version, &o.doc, new_key, false, o.type == operation::replace_docid);
			if (inverting()) { pending_docs_.push_back({docid, version, new_key, &o.doc}); }
			v.docids.push_back(docid);
			v.versions.push_back(version);
		}
		v.op = 0;
		return v;
	}

	void set_schema(std::uint64_t version) { m_.schema_version = version; }

	// --- segment identity ----------------------------------------------------------------

	// A new segment's physical identity (F6, F13-R41): the producing term, then a base drawn
	// at open plus a count, through splitmix64's bijective finalizer, so ids never repeat
	// within an open and collide across producers only by chance (Lucene's randomId: a random
	// base, incremented per id).
	std::array<std::uint8_t, 16> new_identity() {
		std::uint64_t z = identity_base_ + identities_++;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		z ^= z >> 31;
		std::array<std::uint8_t, 16> id{};
		std::memcpy(id.data(), &producer_term_, 8);
		std::memcpy(id.data() + 8, &z, 8);
		return id;
	}
	void seed_identities() {
		if (opts_.identity_seed) {
			identity_base_ = *opts_.identity_seed;
		} else {
			std::random_device rd;
			identity_base_ = std::uint64_t(rd()) << 32 ^ rd();
		}
		identities_ = 0;
	}

	// --- the buffer -------------------------------------------------------------------------

	bool inverting() const noexcept { return !std::is_same_v<inverter, detail::no_inverter> && opts_.flush_threads <= 1; }
	// Buffered documents (tombstones not counted), and every buffered record.
	std::size_t buffer_size() const noexcept {
		std::size_t n = 0;
		for_each_buffer([&](const buffer& b) { n += b.docs; });
		return n;
	}
	std::size_t buffered_records() const noexcept {
		std::size_t n = 0;
		for_each_buffer([&](const buffer& b) { n += b.order.size(); });
		return n;
	}
	std::string_view buffered_stored(const buffer& b, const record& x) const
		requires(segment::template contains<stored_plugin>())
	{
		if (!inverting()) { return static_cast<const typename stored_plugin::part&>(x.doc()).stored; }
		const std::size_t n = b.inv ? b.inv->buf.doc_count() : 0;
		return x.ordinal() < n ? b.inv->buf.stored(x.ordinal()) : std::string_view(static_cast<const typename stored_plugin::part&>(*pending_docs_[x.ordinal() - n].doc).stored);
	}

	// Stage 2 for an accepted transaction: its documents into the active writer. Validation
	// ran in stage 1 (D10), so only memory can fail here, and that fences.
	void invert_pending() {
		if (pending_docs_.empty()) { return; }
		try {
			auto& inv = wb_->inv;
			if (!inv) {
				inv = std::make_unique<active_buffer>();
				inv->buf.max_run_bytes = max_run_bytes_;
			}
			auto& a = *inv;
			for (const pending_doc& p : pending_docs_) {
				// An inverter that reads field references takes the transaction's resolution.
				const field_resolution res{pending_declared_, pending_known_, pending_key_field_};
				ordinal_t ord;
				if constexpr (requires { a.buf.add(p.docid, p.version, p.key, *p.doc, res); }) {
					ord = a.buf.add(p.docid, p.version, p.key, *p.doc, res);
				} else {
					ord = a.buf.add(p.docid, p.version, p.key, *p.doc);
				}
				if (ord && p.docid <= a.prev_docid) { a.ordered = false; }
				a.prev_docid = p.docid;
			}
		} catch (...) {
			pending_docs_.clear();
			fenced_ = true;
			throw;
		}
		pending_docs_.clear();
	}

	// Whether the transaction's records fit the active buffer's lane, or an empty one: at most
	// two per operation (a tombstone and the record), each at most the largest header (CR9).
	bool lane_fits(const transaction& t) const noexcept {
		constexpr std::size_t widest = record::stride_for(record::docid_prev_flag | record::ordinal_flag);
		const std::size_t records = 2 * t.ops.size();
		return wb_ ? wb_->lane.room_for(records, widest) : wbuf::lane<record>(opts_.lane_chunks_cap).room_for(records, widest);
	}
	// Whether the transaction's documents fit the active buffer's 32-bit structures.
	bool fits(const transaction& t) const noexcept {
		if (!lane_fits(t)) { return false; }  // a full chunk directory freezes (D3)
		if (!wb_ || !wb_->inv) { return true; }
		return wb_->inv->buf.fits(t);
	}

	// Nothing may change a buffer a flush borrows from (core/borrow.hh).
	void require_buffers_unpinned() const {
		for_each_buffer([](const buffer& b) { b.lane.require_unpinned(); });
	}
	void clear_buffer() {
		require_buffers_unpinned();
		wb_.reset();
		frozen_.clear();
		pending_docs_.clear();
	}

	// --- freeze and flush (D8) --------------------------------------------------------------

	// Seals the active buffer at a transaction boundary: it becomes the newest frozen buffer, and
	// its pending deletes go to the segments' live bits, copied on write (D6). O(1) in records.
	void freeze() {
		if (holds_rejected()) { wb_.reset(); }  // views that hold it keep it alive
		if (!wb_ || wb_->order.size() == 0) { return; }
		assert(pending_docs_.empty());
		apply_pending();
		frozen_.push_back(std::move(wb_));
	}
	// The active buffer's pending deletes set in their segments' bits (D6, D8).
	void apply_pending() {
		if (!wb_) { return; }
		buffer& b = *wb_;
		for (const auto& [id, ord] : b.pending) {
			const auto it = std::lower_bound(segs_.begin(), segs_.end(), id, [](const auto& s, std::uint64_t x) { return s->ref.id < x; });
			if (it == segs_.end() || (*it)->ref.id != id) { fence("strata: freeze: a pending delete names no segment"); }
			auto& s = **it;
			if (!s.deleted.test(ord)) {
				s.deleted.set(ord);  // a clone when a view shares them (shared-reader D3)
				++s.dead;
			}
			s.deletes_dirty = true;
		}
		b.pending.clear();
		b.pending_keys.clear();
	}

	// Writes the buffers as new segments: a freeze, then each frozen buffer oldest first, each
	// installed as it is written. Each run's memory is charged to the budget on its own permit,
	// settled when its segment is published (D17); the sort's on another, settled when the flush
	// ends. A failure keeps the frozen buffers unwritten for the next flush.
	void flush() {
		freeze();
		while (!frozen_.empty()) {
			const std::size_t first = segs_.size();
			if (inverting()) {
				flush_active(*frozen_.front());
			} else {
				flush_documents(*frozen_.front());
			}
			install_flushed(first);
		}
		if (permit_) { permit_.settle(); }
		reserved_ = 0;  // the buffer's bytes are on disk now
		settle_storage();
	}

	// A flush's install (D6, WB-16): the oldest frozen buffer leaves, and the new segments (from
	// segs_[first] on) lose every copy a newer buffer supersedes, by key through its key map, and
	// a keyless document by docid through its docid map.
	void install_flushed(std::size_t first) {
		require_buffers_unpinned();
		frozen_.erase(frozen_.begin());
		if (!frozen_.empty() || (wb_ && wb_->order.size())) {
			for (std::size_t i = first; i < segs_.size(); ++i) {
				auto& s = *segs_[i];
				const auto& core = s.mapped().core();  // the flush's own segment: its body is here
				const auto& keys = s.mapped().template get<keys_plugin>();
				const auto drop = [&](ordinal_t o) {
					if (s.deleted.test(o)) { return; }
					s.deleted.set(o);
					++s.dead;
					s.deletes_dirty = true;
				};
				for (auto r = keys.range({}); r.valid(); r.next()) {
					bool gone = false;
					for_each_buffer([&](const buffer& b) { gone = gone || find_key_in(b, r.key()) != wbuf::no_ref; });
					if (gone) { drop(r.ordinal()); }
				}
				// A newer record with the docid replaced or deleted the document, keyless or not.
				for (ordinal_t o = 0; o < core.doc_count(); ++o) {
					bool gone = false;
					for_each_buffer([&](const buffer& b) { gone = gone || (b.docids.built() && find_docid_in(b, core.docid(o)) != wbuf::no_ref); });
					if (gone) { drop(o); }
				}
			}
		}
		changed();
	}

	// One segment from an inverting buffer: its postings as they are when its docids increase
	// and every document is its docid's newest, else rebuilt in docid order without the others.
	void flush_active(const buffer& b) {
		if (!b.inv || !b.inv->buf.doc_count()) { return; }
		auto& a = *b.inv;
		flush_charge run(opts_.budget);  // the flush's sort, scratch, and output, until published
		std::vector<ordinal_t> order;
		for (std::size_t i = 0; i < b.order.size(); ++i) {
			if (newest(b, b.order[i].ref)) { order.push_back(b.lane.at(b.order[i].ref).ordinal()); }
		}
		const bool as_is = order.size() == a.buf.doc_count() && a.ordered;
		if (as_is) {
			order.clear();
		} else {
			std::sort(order.begin(), order.end(), [&](ordinal_t x, ordinal_t y) { return a.buf.docid(x) < a.buf.docid(y); });
			run.grow(order.capacity() * sizeof(ordinal_t));
			if (order.empty()) { return; }
		}
		const std::uint64_t id = m_.next_segment++;  // never reused, even after a failure
		const std::array<std::uint8_t, 16> header = new_identity();
		// The segment goes to its file as it is written. ENOSPC removes the partial file and
		// keeps the buffer, as write_unsynced does.
		const std::string name = index_detail::segment_name(id);
		auto file = io_->create_exclusive(name);
		index_detail::file_sink sink(*file);
		try {
			detail::chunked_spill positions(&run);
			a.buf.flush(header, order, builder_settings{&run, nullptr, m_.key_prefix, configured_structures(), segment_policy<Layout>::stored_block, segment_policy<Layout>::stored_codec}, sink, positions);
			sink.finish();
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
			file.reset();
			io_->remove(name);
			sync_dir();
			throw space_error(e.code(), e.what());
		}
		track(name, sink.size());
		files::segment_ref ref;
		ref.id = id;
		ref.identity = header;
		ref.size = sink.size();
		ref.doc_count = as_is ? a.buf.doc_count() : order.size();
		segs_.emplace_back(streamed_segment(std::move(file), ref));
	}

	// More than one flush thread, or a layout without an inverter: a frozen buffer's newest
	// records in contiguous runs, one segment each, inverted and encoded in parallel (D18). Runs
	// are of docids, or of keys when the segments are key-ordered (format-14-kv D1a): keyed
	// documents by key, then keyless ones by docid.
	void flush_documents(buffer& b) {
		// The owner's lease from before `docs` points into the buffer to the end of the flush.
		const auto owner = b.lane.pin();
		flush_charge sorting(opts_.budget);
		std::vector<const record*> docs;
		for (std::size_t i = 0; i < b.order.size(); ++i) {
			if (newest(b, b.order[i].ref)) { docs.push_back(&b.lane.at(b.order[i].ref)); }
		}
		sorting.grow(docs.capacity() * sizeof(docs[0]));
		if (!docs.empty()) {
			if (configured_structures() & format::bit::index_sort) {
				std::sort(docs.begin(), docs.end(), [](const record* a, const record* c) {
					const std::string_view x = a->doc().key, y = c->doc().key;
					if (x.empty() != y.empty()) { return y.empty(); }
					return x.empty() ? a->docid < c->docid : x < y;
				});
			} else {
				std::sort(docs.begin(), docs.end(), [](const record* a, const record* c) { return a->docid < c->docid; });
			}
			// Contiguous runs, one segment each, inverted and encoded in parallel (D18).
			const std::size_t runs = std::max<std::size_t>(1, std::min(opts_.flush_threads, docs.size() / std::max<std::size_t>(1, opts_.flush_min_docs)));
			std::vector<std::uint64_t> ids(runs);
			std::vector<std::array<std::uint8_t, 16>> identities(runs);
			for (std::size_t k = 0; k < runs; ++k) {
				ids[k] = m_.next_segment++;  // never reused, even after a failure
				identities[k] = new_identity();
			}
			// Each run streams to a file of its own, created here and written by the run's thread.
			std::vector<std::string> names(runs);
			std::vector<std::unique_ptr<kronuz::journal::File>> files(runs);
			std::vector<std::uint64_t> sizes(runs);
			std::vector<std::exception_ptr> errors(runs);
			std::vector<flush_charge> charges(runs);  // each run's, held until its segment is published
			auto stream = [&](std::size_t k) {
				try {
					const std::size_t begin = docs.size() * k / runs, end = docs.size() * (k + 1) / runs;
					flush_charge charge(opts_.budget);  // on the run's own thread
					{
						// The writer's views into the buffer (stored records, keys) live as long as it does,
						// and the lease, declared before it, outlives it: the buffer cannot change under them.
						auto lease = b.lane.pin();
						basic_segment_writer<Layout> w;
						if (opts_.budget) { w.memory = &charge; }
						w.key_prefix = m_.key_prefix;
						w.structures = configured_structures();
						w.borrow_stored(lease);
						w.reserve(end - begin, 0);  // the stored bytes stay in the buffer
						for (std::size_t i = begin; i < end; ++i) {
							const record* x = docs[i];
							w.add({x->docid, x->version, x->doc().key}, x->doc(), true);
						}
						index_detail::file_sink sink(*files[k]);
						detail::chunked_spill positions(&charge);
						w.flush(identities[k], sink, positions);
						sink.finish();
						sizes[k] = sink.size();
					}
					charge.shrink(charge.held());  // the writer's buffers are gone
					charges[k] = std::move(charge);
				} catch (...) { errors[k] = std::current_exception(); }
			};
			// Every run's file is written before any segment is published: a write that fails
			// (ENOSPC) publishes none and removes the files written, so the retry has their
			// space and the buffer it keeps holds no published document.
			std::exception_ptr failure;
			try {
				for (std::size_t k = 0; k < runs; ++k) {
					names[k] = index_detail::segment_name(ids[k]);
					files[k] = io_->create_exclusive(names[k]);
				}
				std::vector<std::thread> threads;
				for (std::size_t k = 1; k < runs; ++k) { threads.emplace_back(stream, k); }
				stream(0);
				for (auto& t : threads) { t.join(); }
				for (auto& e : errors) { if (e) { std::rethrow_exception(e); } }
			} catch (...) {
				failure = std::current_exception();
			}
			if (failure) {
				bool removed = false;
				for (std::size_t k = 0; k < runs; ++k) {
					if (!files[k]) { continue; }
					files[k].reset();
					try { remove_file(names[k]); removed = true; } catch (...) {}  // best effort: the failure is the news
				}
				if (removed) { try { sync_dir(); } catch (...) {} }
				try {
					std::rethrow_exception(failure);
				} catch (const std::system_error& e) {
					if (e.code().value() != ENOSPC) { throw; }
					throw space_error(e.code(), e.what());
				}
			}
			for (std::size_t k = 0; k < runs; ++k) { track(names[k], sizes[k]); }
			for (std::size_t k = 0; k < runs; ++k) {
				files::segment_ref ref;
				ref.id = ids[k];
				ref.identity = identities[k];
				ref.size = sizes[k];
				ref.doc_count = docs.size() * (k + 1) / runs - docs.size() * k / runs;
				segs_.emplace_back(streamed_segment(std::move(files[k]), ref, &charges[k]));
				charges[k].settle();
			}
		}
	}

	// --- commit -----------------------------------------------------------------------------

	// The commit's input maps (cold-tier D9): every record gets the facts the manifest needs, and a
	// record with new deletes gets its body for the live length. A host error, or a record whose
	// first map failed its checks (corrupt_segment, until heal()), leaves the index as refresh()
	// leaves it: flushed segments and installed merges in segs_, nothing durable, nothing fenced.
	// The facts and bodies installed so far are true and stay.
	void prepare_commit() {
		for (const auto& s : segs_) {
			if (s->dead == s->ref.doc_count) { continue; }  // commit_files drops it
			if (!s->facts_known) {
				s->learn_facts();
			} else if (s->deletes_dirty || s->failed()) {
				(void)s->seg();
			}
		}
	}

	void commit_files() {
		// A segment whose documents are all deleted leaves without a merge; retention frees
		// its files.
		if (std::erase_if(segs_, [](const auto& s) { return s->dead == s->ref.doc_count; })) { changed(); }
		for (const auto& s : segs_) {
			if (!s->facts_known) { fence("strata: commit: a segment's facts are unknown"); }
		}
		// The manifest lists segments by increasing id, and open() refuses any other order:
		// a regression fences before anything is written, whichever caller commits.
		for (std::size_t i = 1; i < segs_.size(); ++i) {
			if (segs_[i]->ref.id <= segs_[i - 1]->ref.id) { fence("strata: commit: segments out of id order"); }
		}
		// 1. New data files, synced together: flushed segments, live-docs generations,
		// metadata. Their generations come from the counters directly, so a failed commit
		// never reuses a name.
		// Nothing changes until every file is durable, so a failed write or sync leaves the
		// next commit to write them again under new names.
		std::vector<kronuz::journal::File*> files;
		std::vector<std::unique_ptr<kronuz::journal::File>> written;
		std::vector<std::pair<open_segment*, files::live_docs>> lives;
		std::optional<files::metadata> md;
		for (auto& s : segs_) {
			if (s->unsynced) { files.push_back(s->unsynced.get()); }
			if (s->deletes_dirty) {
				files::live_docs l;
				l.segment = s->ref.id;
				l.generation = m_.next_live++;
				l.doc_count = s->ref.doc_count;
				for (ordinal_t o = 0; o < s->ref.doc_count; ++o) {
					if ((s->deleted[o >> 6] >> (o & 63)) & 1) { l.deleted.push_back(o); }
				}
				l.live_count = l.doc_count - l.deleted.size();
				l.live_length = live_length(*s);
				written.push_back(write_unsynced(index_detail::live_name(l.segment, l.generation), files::encode(l)));
				lives.emplace_back(s.get(), std::move(l));
			}
		}
		if (meta_dirty_ || !m_.meta.generation) {
			md.emplace();
			md->generation = m_.next_meta++;
			md->entries = meta_;
			written.push_back(write_unsynced(index_detail::meta_name(md->generation), files::encode(*md)));
		}
		std::optional<files::file_ref> fields_ref;  // the table's file after this commit (empty ref: none)
		if (fields_owed()) {
			if (fields_.size() == 0) {
				fields_ref.emplace();
			} else if (!fields_rewrite_ && m_.fields.generation && fields_clean_) {
				// Append one block after the committed prefix; the file is clean until the manifest names it.
				const std::string name = index_detail::fields_name(m_.fields.generation);
				const std::string block = files::encode_fields_block(fields_, fields_durable_, fields_.size());
				auto f = io_->open_existing(name);
				fields_clean_ = false;
				try {
					index_detail::write_all_at(*f, m_.fields.size, block);
				} catch (const std::system_error& e) {
					if (e.code().value() != ENOSPC) { throw; }
					throw space_error(e.code(), e.what());
				}
				track(name, m_.fields.size + block.size());
				fields_ref = files::file_ref{m_.fields.generation, m_.fields.size + block.size(), files::trailing_crc(block)};
				written.push_back(std::move(f));
			} else {
				const std::uint64_t gen = m_.next_fields++;
				const std::string bytes = files::encode_fields(fields_);
				written.push_back(write_unsynced(index_detail::fields_name(gen), bytes));
				fields_ref = files::file_ref{gen, bytes.size(), files::trailing_crc(bytes)};
			}
		}
		for (const auto& f : written) { files.push_back(f.get()); }
		io_->sync_files(files);
		for (auto& s : segs_) { s->unsynced.reset(); }
		for (auto& [s, l] : lives) {
			const std::string bytes = files::encode(l);
			s->ref.live = {l.generation, bytes.size(), files::trailing_crc(bytes)};
			s->ref.live_count = l.live_count;
			s->ref.live_length = l.live_length;
			s->deletes_dirty = false;
		}
		for (auto& s : segs_) {
			if (!s->ref.live.generation) {
				s->ref.live_count = s->ref.doc_count;
				s->ref.live_length = s->total_length;
			}
		}
		if (md) {
			const std::string bytes = files::encode(*md);
			m_.meta = {md->generation, bytes.size(), files::trailing_crc(bytes)};
			meta_dirty_ = false;
		}
		if (fields_ref) {
			m_.fields = *fields_ref;
			fields_durable_ = fields_.size();
			fields_durable_names_ = fields_.name_bytes();
			fields_rewrite_ = false;
			fields_clean_ = true;
		}
		files::manifest next = m_;
		next.generation = next_manifest_;
		if (pending_pin_) { next.pinned.push_back(*pending_pin_); }
		if (pending_unpin_) { std::erase(next.pinned, *pending_unpin_); }
		if (applied_since_commit_) { ++next.revision; }
		next.segments.clear();
		next.doc_count = next.total_length = next.live_count = next.live_length = 0;
		next.mask = next.configured;
		next.min_version = next.max_version = 0;
		for (const auto& s : segs_) {
			next.mask |= s->structures;
			const std::uint32_t v = s->version;
			next.min_version = next.min_version ? std::min(next.min_version, v) : v;
			next.max_version = std::max(next.max_version, v);
			next.segments.push_back(s->ref);
			next.doc_count += s->ref.doc_count;
			next.total_length += s->total_length;
			next.live_count += s->ref.live_count;
			next.live_length += s->ref.live_length;
		}
		// 2-5. Directory, the manifest under a temporary name, the rename, the directory.
		sync_dir();
		const std::string tmp = index_detail::manifest_name(next.generation) + ".tmp";
		const std::string bytes = files::encode(next);
		++next_manifest_;  // a failed attempt's name is never reused
		write_new(tmp, bytes);
		io_->replace(tmp, index_detail::manifest_name(next.generation));
		files_.erase(tmp);
		files_[index_detail::manifest_name(next.generation)] = bytes.size();
		sync_dir();
		m_ = next;
		applied_since_commit_ = 0;
		pending_pin_.reset();
		pending_unpin_.reset();
	}

	void write_new(const std::string& name, std::string_view bytes) { write_unsynced(name, bytes)->sync(); }

	std::uint64_t live_length(const open_segment& s) const {
		std::uint64_t n = 0;
		const segment& sg = s.mapped();  // prepare_commit mapped it (D9)
		for (ordinal_t o = 0; o < s.ref.doc_count; ++o) {
			if (!((s.deleted[o >> 6] >> (o & 63)) & 1)) { n += sg.length(o); }
		}
		return n;
	}

	// --- retention --------------------------------------------------------------------------

	// Keeps the current manifest, the pinned ones, and the retained predecessors. Older
	// manifests are removed first, durably; then every file of strata's that no kept
	// manifest names is queued for reclaim(). Works from the files on record, with no
	// directory scan.
	// With `data_only` (reclaim()'s pass, D6), it removes no manifest: it queues data files only
	// when no obsolete manifest is on record, as between commits.
	void retain(bool data_only = false) {
		retention_blocked_.reset();
		std::set<std::uint64_t> keep(m_.pinned.begin(), m_.pinned.end());
		keep.insert(m_.generation);
		std::vector<std::uint64_t> manifests;
		for (const auto& [n, size] : files_) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { manifests.push_back(p->a); }
		}
		std::sort(manifests.rbegin(), manifests.rend());
		std::size_t predecessors = 0;
		for (std::uint64_t g : manifests) {
			if (g < m_.generation && predecessors < opts_.retained_predecessors) {
				keep.insert(g);
				++predecessors;
			}
		}
		std::erase_if(manifest_files_, [&](const auto& e) { return !keep.count(e.first); });
		std::set<std::string> referenced;
		for (std::uint64_t g : keep) {
			if (g != m_.generation && !files_.count(index_detail::manifest_name(g))) { continue; }
			const auto* names = manifest_files(g);
			if (!names) {  // a kept manifest it cannot read: remove nothing this round
				retention_blocked_ = g;
				return;
			}
			referenced.insert(names->begin(), names->end());
		}
		// Flushed segments awaiting a commit are on disk but not yet named by a manifest.
		for (const auto& s : segs_) { referenced.insert(index_detail::segment_name(s->ref.id)); }
		// A pending merge's sources stay mapped until it publishes: none is truncated before then.
		for (const auto& job : pending_) {
			for (const auto& src : job->sources) { referenced.insert(index_detail::segment_name(src->ref.id)); }
		}
		// So do a view's until it is freed (D7): a file a reader maps is never truncated.
		for_each_view([&](const view& v) {
			for (const auto& sv : v.views_) { referenced.insert(index_detail::segment_name(sv.id)); }
		});
		// Manifests leave first, durably, then the data files only they referenced: a crash
		// between the two never leaves a manifest naming a removed file.
		std::vector<std::string> obsolete;
		for (const auto& [n, size] : files_) {
			if (!referenced.count(n) && !queued_.count(n)) { obsolete.push_back(n); }
		}
		if (data_only) {
			const bool manifests = std::any_of(obsolete.begin(), obsolete.end(), [](const std::string& n) {
				auto p = index_detail::parse_name(n);
				return p && p->type == index_detail::parsed_name::manifest;
			});
			if (manifests || sync_owed_) { return; }
		}
		// A failure here leaves the commits standing: nothing is queued until a later round
		// removes the manifests and syncs the directory.
		try {
			for (const auto& n : obsolete) {
				if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) {
					sync_owed_ = true;
					remove_file(n);  // IO::remove: a missing file is a success
				}
			}
			if (sync_owed_) {
				sync_dir();
				sync_owed_ = false;
			}
		} catch (const std::system_error&) {
			return;
		}
		for (const auto& n : obsolete) {
			if (auto p = index_detail::parse_name(n); p && p->type != index_detail::parsed_name::manifest) {
				reclaim_.push_back(n);
				queued_.insert(n);
			}
		}
	}
	// The files manifest `g` names (itself included), read once from disk for an older one;
	// null when it cannot be read, and only a successful read is cached.
	const std::vector<std::string>* manifest_files(std::uint64_t g) {
		if (g == m_.generation) {
			auto& names = manifest_files_[g];
			names = names_of(m_);
			return &names;
		}
		if (auto it = manifest_files_.find(g); it != manifest_files_.end()) { return &it->second; }
		try {
			auto f = io_->open_existing(index_detail::manifest_name(g));
			return &(manifest_files_[g] = names_of(files::decode_manifest(index_detail::read_all(*f))));
		} catch (const std::exception&) {
			return nullptr;
		}
	}
	static std::vector<std::string> names_of(const files::manifest& mf) {
		std::vector<std::string> names{index_detail::manifest_name(mf.generation)};
		if (mf.meta.generation) { names.push_back(index_detail::meta_name(mf.meta.generation)); }
		if (mf.fields.generation) { names.push_back(index_detail::fields_name(mf.fields.generation)); }
		for (const auto& s : mf.segments) {
			names.push_back(index_detail::segment_name(s.id));
			if (s.live.generation) { names.push_back(index_detail::live_name(s.id, s.live.generation)); }
		}
		return names;
	}

	// --- recovery ---------------------------------------------------------------------------

	// A create that writes nothing until its first commit (D3-C): the directory lock, an
	// interrupted create's residue cleared, the state of an empty index in memory.
	static basic_index create_deferred(kronuz::journal::IO& io, const options& opts, const std::array<char, 16>& uuid) {
		basic_index ix(io);
		const lease_scope leased{ix.lease_};
		ix.lock_directory(opts);
		ix.clear_uncreated();
		ix.m_.uuid = uuid;
		ix.m_.key_prefix = opts.key_prefix;
		// Every structure the index's layout builds, keys only with a key prefix (F4, F20), and
		// index_sort's key order with keys in a key-ordered layout (format-14-kv D1a).
		ix.m_.configured = segment::readable & ~(opts.key_prefix.empty() ? format::bit::keys : 0) & ~format::bit::index_sort;
		if (segment_policy<Layout>::key_order && (ix.m_.configured & format::bit::keys)) { ix.m_.configured |= format::bit::index_sort; }
		ix.opts_ = opts;
		ix.seed_identities();
		if (opts.budget) { ix.permit_ = opts.budget->open(); }
		if (opts.storage) { ix.storage_permit_ = opts.storage->open(); }
		ix.start_merges();
		return ix;
	}

	// Whether names are only what an interrupted create leaves: the first metadata file and
	// the first manifest's temporary.
	static bool uncreated(const std::vector<std::string>& names) {
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (p && !(p->a == 1 && (p->type == index_detail::parsed_name::meta || p->type == index_detail::parsed_name::manifest_tmp))) { return false; }
		}
		return true;
	}
	// A directory the IO admitted though another user owns it or others can write it warns or
	// refuses as options::directory_trust says; an IO that does not say is not trusted. Before
	// the lock, so a refusal writes nothing.
	void check_directory_trust(const options& opts) {
		const auto trust = io_->directory_trust();
		if (trust.trusted()) { return; }
		if (opts.directory_trust == options::trust_policy::refuse) {
			throw std::system_error(std::make_error_code(std::errc::permission_denied),
				!trust.known ? "strata: the directory's ownership and mode are unknown"
				: trust.foreign_owner ? "strata: the directory is owned by another user" : "strata: the directory is writable by group or others");
		}
		trust_warning_ = strata::directory_trust_warning{trust.known, trust.foreign_owner, trust.shared_writable};
	}
	// The owner lock is the directory's own (D2), writing nothing. Busy (EWOULDBLOCK) and
	// other lock errors refuse as they are. A filesystem not known local (an IO that does not
	// classify is not), or a lock it does not support, warns or refuses as options::nonlocal
	// says. A failed classification warns as an unknown filesystem, or refuses under refuse.
	void lock_directory(const options& opts) {
		check_directory_trust(opts);
		const bool refuse = opts.nonlocal == options::nonlocal_policy::refuse;
		kronuz::journal::FilesystemClass fs;
		try {
			fs = io_->directory_filesystem();
		} catch (const std::system_error& e) {
			if (refuse) { throw std::system_error(e.code(), std::string("strata: the directory's filesystem is unknown: ") + e.what()); }
			fs = {false, std::string("unknown (") + e.what() + ")"};
		}
		const bool fuse = fs.name.starts_with("fuse") || fs.name.starts_with("macfuse") || fs.name.starts_with("osxfuse");
		if (!fs.local && refuse && !fuse) {
			throw std::system_error(std::make_error_code(std::errc::not_supported), "strata: " + fs.name + " is not a known-local filesystem");
		}
		try {
			lock_ = io_->acquire_directory_owner();
		} catch (const std::system_error& e) {
			if (e.code() != std::errc::not_supported || refuse) { throw; }  // unlocked: refuse's to stop
			fs_warning_ = strata::filesystem_warning{fs.name, fs.local, false};
			return;
		}
		if (!fs.local) { fs_warning_ = strata::filesystem_warning{fs.name, false, true}; }
	}

	// Removes an interrupted create's files, durably, under the owner lock; throws when the
	// directory holds a committed index or a segment. Syncs only when it removed something.
	void clear_uncreated() {
		std::vector<std::string> names;
		{
			auto cursor = io_->scan_directory();
			while (auto n = cursor->next()) { names.push_back(*n); }
		}
		if (!uncreated(names)) { throw std::system_error(std::make_error_code(std::errc::file_exists), "strata: the directory holds an index"); }
		bool removed = false;
		for (const auto& n : names) {
			if (index_detail::parse_name(n)) {
				io_->remove(n);
				removed = true;
			}
		}
		if (removed) { sync_dir(); }
	}

	void recover(std::optional<std::uint64_t> to) {
		std::vector<std::string> names;
		{
			auto cursor = io_->scan_directory();
			while (auto n = cursor->next()) { names.push_back(*n); }
		}
		std::uint64_t top = 0, max_segment = 0, max_live = 0, max_meta = 0, max_fields = 0;
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (!p) { continue; }
			track(n, read_checked([&] { return io_->open_existing(n)->size(); }, &load_->retries));  // an EIO twice is damage
			switch (p->type) {
			case index_detail::parsed_name::manifest: top = std::max(top, p->a); break;
			case index_detail::parsed_name::manifest_tmp: top = std::max(top, p->a); break;
			case index_detail::parsed_name::segment:
			case index_detail::parsed_name::spill:
			case index_detail::parsed_name::scratch: max_segment = std::max(max_segment, p->a); break;
			case index_detail::parsed_name::live:
				max_segment = std::max(max_segment, p->a);
				max_live = std::max(max_live, p->b);
				break;
			case index_detail::parsed_name::meta: max_meta = std::max(max_meta, p->a); break;
			case index_detail::parsed_name::fields: max_fields = std::max(max_fields, p->a); break;
			}
		}
		std::vector<std::uint64_t> manifests;
		for (const auto& n : names) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { manifests.push_back(p->a); }
		}
		if (manifests.empty()) {
			if (uncreated(names)) { throw index_not_created("strata: no index was committed here"); }
			throw corrupt_index("no manifest");
		}
		std::sort(manifests.begin(), manifests.end());
		std::uint64_t chosen = manifests.back();
		if (to) {
			if (!std::binary_search(manifests.begin(), manifests.end(), *to)) { throw corrupt_index("no such manifest"); }
			chosen = *to;
		}
		m_ = read_checked([&] { return files::decode_manifest_for(index_detail::read_all(*io_->open_existing(index_detail::manifest_name(chosen))), readable()); }, &load_->retries);
		if (m_.generation != chosen) { throw corrupt_index("manifest generation differs from its name"); }
		// Every file the manifest names must exist and agree with its entry.
		check_readable(m_);
		// Lazy: the names and sizes the scan recorded stand for the segment files (cold-tier D11).
		auto files_of = load_files(*io_, m_, false, load_, opts_.eager_segments ? nullptr : &files_);
		segs_ = std::move(files_of.segments);
		meta_ = std::move(files_of.meta);
		fields_ = std::move(files_of.fields);
		fields_durable_ = fields_.size();
		fields_durable_names_ = fields_.name_bytes();
		fields_clean_ = clean_fields_tail();
		if (!opts_.key_prefix.empty() && opts_.key_prefix != m_.key_prefix) { throw std::invalid_argument("strata: the index's key prefix differs"); }
		opts_.key_prefix = m_.key_prefix;
		// Counters start above every name on disk, so no name is ever reused (D3).
		m_.next_segment = std::max(m_.next_segment, max_segment + 1);
		m_.next_live = std::max(m_.next_live, max_live + 1);
		m_.next_meta = std::max(m_.next_meta, max_meta + 1);
		m_.next_fields = std::max(m_.next_fields, max_fields + 1);
		next_manifest_ = std::max(m_.generation, top) + 1;
		// An older manifest becomes the head by committing its content as a new generation,
		// so power loss leaves the old head or the chosen one, never a manifest between (D3).
		if (chosen != manifests.back()) {
			fallback_ = fallback_report{chosen, manifests.back(), std::uint64_t(manifests.end() - std::upper_bound(manifests.begin(), manifests.end(), chosen))};
			prepare_commit();  // an open's failure to map fails the open
			commit_files();
		}
		// Orphans leave only after a successful open, under the owner lock.
		for (const auto& n : names) {
			if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest_tmp) { remove_file(n); }
		}
		retain();
		reclaim(opts_.reclaim_slice);
	}

	static std::string read_named(kronuz::journal::IO& io, const std::string& name, std::uint64_t size) {
		std::unique_ptr<kronuz::journal::File> f;
		try {
			f = io.open_existing(name);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOENT) { throw; }  // read_checked tells the medium's errors from the host's
			throw corrupt_index("missing file: " + name);
		}
		std::string bytes = index_detail::read_all(*f);
		if (bytes.size() != size) { throw corrupt_index("file size differs from the manifest: " + name); }
		return bytes;
	}

	// Every file a manifest names, read from io and checked against its entry: each
	// segment's size, footer checksum, and document count (and every byte, when `whole`);
	// each live-docs file's checksum, segment, generation, document count, live count, and
	// live length; the metadata file's checksum and generation. recover and install share
	// it, so a checkpoint is held to what an open is. Throws corrupt_index (or
	// corrupt_segment) at the first file that disagrees; changes nothing.
	struct loaded_files {
		std::vector<std::shared_ptr<open_segment>> segments;
		std::map<std::string, std::string> meta;
		field_table fields;
	};
	// Runs a read and its checks, and once more if either fails: an unreliable read (a
	// transient media fault) passes the second time; damage on the medium fails both, and
	// an EIO then reports as corrupt_index, while other errors stay what they are.
	template <class F>
	static auto read_checked(F&& f, std::atomic<std::uint64_t>* retries) -> decltype(f()) {
		try {
			return f();
		} catch (const corrupt_index&) {
			if (retries) { retries->fetch_add(1, std::memory_order_relaxed); }
		} catch (const corrupt_segment&) {
			if (retries) { retries->fetch_add(1, std::memory_order_relaxed); }
		} catch (const std::system_error& e) {
			if (retries && e.code().value() == EIO) { retries->fetch_add(1, std::memory_order_relaxed); }  // the host's errors are not the medium's
		}
		try {
			return f();
		} catch (const std::system_error& e) {
			// Only a read error is the medium's; a lack of descriptors, memory, or permission is
			// the host's, and a healthy shard must not read as damaged.
			if (e.code().value() != EIO) { throw; }
			throw corrupt_index(std::string("unreadable: ") + e.what());
		}
	}
	// What this binary reads, against which decode_manifest_for classifies a manifest before any
	// segment opens (F20, F13-R42).
	static files::readable_by readable() { return {segment::readable, format::min_read_version, format::version}; }
	// Refuses configured bits no flush could write, as damage (CR5): each bit's owner set
	// (F4), and keys set exactly when the index has a key prefix (F8).
	static void check_readable(const files::manifest& m) {
		for (format::structure_bits b = m.configured; b; b &= b - 1) {
			const format::structure_bits owner = format::requires_of(b & -b);
			if (owner && !(m.configured & owner)) { throw corrupt_index("configured structure bit " + std::to_string(std::countr_zero(b)) + " without its owner"); }
		}
		if (m.key_prefix.empty() == bool(m.configured & format::bit::keys)) { throw corrupt_index("the key prefix and the configured keys bit disagree"); }
	}
	static vouch vouch_of(const files::manifest& m) noexcept { return {m.mask, m.min_version, m.max_version}; }
	// `sizes` non-null: the records only, their files not opened. Each named file's existence and
	// size are checked against the names and sizes the directory scan recorded, with no system
	// call, and the first map checks the rest (cold-tier D11). Live-docs files are read here.
	static loaded_files load_files(kronuz::journal::IO& io, const files::manifest& m, bool whole, const std::shared_ptr<load_context>& load,
		const std::map<std::string, std::uint64_t>* sizes = nullptr) {
		loaded_files out;
		for (const auto& r : m.segments) {
			if (sizes) {
				out.segments.emplace_back(read_checked([&] { return load_record(io, m, r, *sizes, load); }, &load->retries));
			} else {
				out.segments.emplace_back(read_checked([&] { return load_segment(io, m, r, whole, load); }, &load->retries));
			}
		}
		if (m.meta.generation) {
			out.meta = read_checked([&] {
				const std::string mb = read_named(io, index_detail::meta_name(m.meta.generation), m.meta.size);
				if (files::trailing_crc(mb) != m.meta.crc) { throw corrupt_index("metadata differs from the manifest"); }
				const files::metadata md = files::decode_metadata(mb);
				if (md.generation != m.meta.generation) { throw corrupt_index("metadata generation"); }
				return md.entries;
			}, &load->retries);
		}
		if (m.fields.generation) {
			out.fields = read_checked([&] { return read_fields(io, m.fields); }, &load->retries);
		}
		return out;
	}
	// The table a manifest's reference reads: the file's first `size` bytes, whose last block's CRC
	// is the reference's. A tail past them (a torn append) is ignored.
	static field_table read_fields(kronuz::journal::IO& io, const files::file_ref& ref) {
		std::unique_ptr<kronuz::journal::File> f;
		try {
			f = io.open_existing(index_detail::fields_name(ref.generation));
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOENT) { throw; }
			throw corrupt_index("missing file: " + index_detail::fields_name(ref.generation));
		}
		if (f->size() < ref.size) { throw corrupt_index("fields file shorter than the manifest says"); }
		std::string bytes(ref.size, '\0');
		for (std::uint64_t at = 0; at < ref.size;) {
			const std::size_t n = f->read_at(at, std::span<char>(bytes.data() + at, ref.size - at));
			if (n == 0) { throw corrupt_index("short read"); }
			at += n;
		}
		field_table t;
		const std::uint32_t last = files::decode_fields(bytes, t);
		if (last != ref.crc || t.size() == 0) { throw corrupt_index("fields differ from the manifest"); }
		return t;
	}
	// The manifest's mask and version range passed check_readable, so a segment it names that
	// this binary cannot read disagrees with its manifest: damage (F2, F20).
	static std::unique_ptr<open_segment> load_segment(kronuz::journal::IO& io, const files::manifest& m, const files::segment_ref& r, bool whole,
		const std::shared_ptr<load_context>& load) {
		auto s = std::make_unique<open_segment>(r, load, vouch_of(m));
		s->adopt(open_body(io, r, s->claim, whole));
		read_live(io, r, *s);
		return s;
	}
	// A record whose segment file is named and sized as the manifest says (cold-tier D11).
	static std::unique_ptr<open_segment> load_record(kronuz::journal::IO& io, const files::manifest& m, const files::segment_ref& r,
		const std::map<std::string, std::uint64_t>& sizes, const std::shared_ptr<load_context>& load) {
		const std::string name = index_detail::segment_name(r.id);
		const auto it = sizes.find(name);
		if (it == sizes.end()) { throw corrupt_index("missing file: " + name); }
		if (it->second != r.size) { throw corrupt_index("file size differs from the manifest: " + name); }
		auto s = std::make_unique<open_segment>(r, load, vouch_of(m));
		read_live(io, r, *s);
		return s;
	}
	static std::unique_ptr<segment_body> open_body(kronuz::journal::IO& io, const files::segment_ref& r, const vouch& v, bool whole) {
		try {
			return open_body_of(io, r, v, whole);
		} catch (const unsupported_segment& e) {
			throw corrupt_segment(e.what());
		}
	}
	// Opens, maps and checks a segment file against its manifest entry: size, footer, identity,
	// document count, and structures and versions against `v`; every byte when `whole`. A map that
	// fails for a reason of the host throws unchanged; the caller tells it from damage.
	static std::unique_ptr<segment_body> open_body_of(kronuz::journal::IO& io, const files::segment_ref& r, const vouch& v, bool whole) {
		const std::string name = index_detail::segment_name(r.id);
		std::unique_ptr<kronuz::journal::File> f;
		try {
			f = io.open_existing(name);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOENT) { throw; }
			throw corrupt_index("missing file: " + name);
		}
		auto mapped = f->map();
		std::string bytes;
		if (!mapped) { bytes = index_detail::read_all(*f); }
		const std::string_view view = mapped ? mapped->bytes() : std::string_view(bytes);
		if (view.size() != r.size) { throw corrupt_index("file size differs from the manifest: " + name); }
		(void)format::read_version(view);
		if (format::decode_footer(view).toc_crc != r.footer_crc) { throw corrupt_index("segment differs from its manifest entry"); }
		// The header's identity is the entry's (I13, F13-R41): a file of another producer under
		// this name is never taken for the one the manifest names.
		if (std::memcmp(view.data() + format::identity_offset, r.identity.data(), 16) != 0) { throw corrupt_index("segment identity differs from its manifest entry"); }
		auto b = mapped ? std::make_unique<segment_body>(std::move(mapped)) : std::make_unique<segment_body>(std::move(bytes));
		if (b->seg.core().doc_count() != r.doc_count) { throw corrupt_index("segment document count"); }
		// The manifest vouches for its segments: one outside its mask or version range is damage.
		const std::uint32_t ver = b->seg.core().version();
		if ((b->seg.core().structures() & ~v.mask) || ver < v.min_version || ver > v.max_version) {
			throw corrupt_segment("segment outside its manifest's structures or versions");
		}
		if (whole) { b->seg.verify_all(); }
		return b;
	}
	// The entry's live-docs file, read into the record's bits and checked against the entry.
	static void read_live(kronuz::journal::IO& io, const files::segment_ref& r, open_segment& s) {
		if (!r.live.generation) { return; }
		const std::string lb = read_named(io, index_detail::live_name(r.id, r.live.generation), r.live.size);
		if (files::trailing_crc(lb) != r.live.crc) { throw corrupt_index("live docs differ from the manifest"); }
		const files::live_docs l = files::decode_live_docs(lb);
		if (l.segment != r.id || l.generation != r.live.generation || l.doc_count != r.doc_count || l.live_count != r.live_count ||
			l.live_length != r.live_length) {
			throw corrupt_index("live docs disagree with the manifest");
		}
		std::uint64_t* bits = s.deleted.mutable_words();
		for (std::uint64_t o : l.deleted) { bits[o >> 6] |= std::uint64_t(1) << (o & 63); }
		s.dead = 0;
		for (std::uint64_t w : s.deleted) { s.dead += std::uint64_t(std::popcount(w)); }
	}

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
	files::manifest m_;
	std::vector<std::shared_ptr<open_segment>> segs_;  // by id, as the manifest lists them; a background merge shares its sources
	std::vector<std::shared_ptr<merge_job>> pending_;  // handed to the executor, oldest first
	// The buffers (D18's stage 2, write-buffer D3, D8): the active one takes writes; frozen ones,
	// oldest first, wait for their flush. In an inverting layout each holds its inverter.
	// Views hold them too (write-buffer D5, shared-reader D12).
	std::shared_ptr<buffer> wb_;
	std::vector<std::shared_ptr<buffer>> frozen_;
	std::uint64_t visible_ = 0;  // V (D5): the accepted prefix; readers load the published copy
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
	std::map<std::string, std::uint64_t> files_;  // the index's files and their sizes
	std::map<std::uint64_t, std::vector<std::string>> manifest_files_;  // a retained manifest's files
	std::deque<std::string> reclaim_;  // obsolete data files, oldest first
	std::set<std::string> queued_;
	bool sync_owed_ = false;  // manifests removed since the last directory sync
	std::optional<std::uint64_t> retention_blocked_;
	std::optional<fallback_report> fallback_;
	std::optional<strata::filesystem_warning> fs_warning_;
	std::optional<strata::directory_trust_warning> trust_warning_;
	std::uint64_t on_disk_ = 0, reserved_ = 0;
	std::uint64_t unlinked_ = 0;  // removed, awaiting a directory sync to be freed
	std::uint64_t inflight_ = 0;  // posted for deletion, not yet removed: counted as on disk
	std::map<std::string, std::uint64_t> posted_;  // those files and their sizes
	std::uint64_t obsoleted_ = 0;  // bytes the merges published in this call made obsolete
	std::uint64_t debt_ = 0;       // obsoleted bytes not yet reclaimed: publishes and commits pay it
	std::uint64_t truncated_ = 0;  // truncated in this reclaim step, awaiting the file's sync
	std::uint64_t held_ = 0;      // reserved by a merge or install while it writes
	std::uint64_t allowance_ = 0, manifest_bytes_ = 0;  // the commit allowance, and its manifest's part
	std::uint64_t applied_since_commit_ = 0;
	std::uint64_t next_manifest_ = 1;
	std::optional<std::uint64_t> pending_pin_, pending_unpin_;
	bool meta_dirty_ = false;
	// The field table (fields-and-keys D2) and its file: the declarations the file's prefix holds
	// (and their name bytes), whether the next commit writes a new file rather than appending, and
	// whether the file ends where the manifest says. The epoch moves with the table's content.
	field_table fields_;
	std::size_t fields_durable_ = 0;
	std::uint64_t fields_durable_names_ = 0;
	bool fields_rewrite_ = false, fields_clean_ = true;
	std::uint64_t fields_epoch_ = 0;
	const field_resolution* fields_res_ = nullptr;  // the transaction in stage 1
	bool fenced_ = false;
	std::uint64_t producer_term_ = 0, identity_base_ = 0, identities_ = 0;
	std::unique_ptr<publication> pub_;  // with a reader(): views published to its handles
	write_lease lease_;  // the holder's token (owner.hh)
	// A check-in found the lease held (D2); movable with the index.
	struct owed_flag : std::atomic<bool> {
		owed_flag() noexcept : std::atomic<bool>(false) {}
		owed_flag(owed_flag&& o) noexcept : std::atomic<bool>(o.load()) {}
	} chores_owed_;
};

// The shared reader's names (docs/design/shared-reader.md D1, D4).
template <class Layout>
using basic_view = typename basic_index<Layout>::view;
template <class Layout>
using basic_reader = typename basic_index<Layout>::reader_handle;

}  // namespace strata
