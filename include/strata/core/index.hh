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
#include <charconv>
#include <deque>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
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
};

// Apply's answer. A rejected transaction left no trace.
struct verdict {
	enum outcome : std::uint8_t { accepted, version_conflict, schema_conflict, invalid, skipped, no_space };
	outcome result = accepted;
	std::size_t op = 0;                  // the rejecting operation
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
	enum kind { segment, live, meta, manifest, manifest_tmp, spill, scratch } type;
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

inline void write_all(kronuz::journal::File& f, std::string_view bytes) {
	for (std::uint64_t at = 0; at < bytes.size();) {
		const std::size_t n = f.write_at(at, bytes.substr(at));
		if (n == 0) { throw std::system_error(EIO, std::generic_category(), "write made no progress"); }
		at += n;
	}
}

// The version map's hash of a key: the wordwise hash's high half (its low half ignores a short
// tail's bytes past the first four).
inline std::uint32_t key_hash(std::string_view key) noexcept { return std::uint32_t(::wordwise::hash(key.data(), key.size()) >> 32); }
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
	// oldest file first (docs/design/limits.md).
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
		const lease_scope leased{lease_};
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
		const lease_scope leased{lease_};
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
	// skipped (replay); a rejected transaction changes nothing.
	verdict apply(const transaction& t, files::position at) {
		const lease_scope leased{lease_};
		live();
		// Stage 1 waits for room: the flush happens before this transaction, so a flush
		// failure never loses an accepted transaction's verdict.
		if (needs_flush() || !fits(t) || buffer_size() + t.ops.size() > opts_.buffered_docs_cap) { refresh(); }
		verdict v;
		if (at != files::position{} && at <= m_.applied) {
			v.result = verdict::skipped;
			return v;
		}
		if (t.ops.size() > opts_.buffered_docs_cap) {
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
		v = evaluate(t);
		std::uint64_t allowance = allowance_;
		if (v.result == verdict::accepted && storage_permit_) {
			allowance = commit_allowance();
			if (allowance > allowance_ && !storage_permit_.try_grow(allowance - allowance_)) { v.result = verdict::no_space; }
		}
		if (v.result != verdict::accepted) {
			rollback();
			pending_docs_.clear();
			storage_permit_.shrink(reserve);
		} else {
			reserved_ += reserve;
			allowance_ = std::max(allowance, allowance_);
			++wb_->transactions;
			invert_pending();
			charge();
			if (t.set_schema) { set_schema(*t.set_schema); }
			++applied_since_commit_;
			changed();
		}
		undo_.clear();
		if (at != files::position{}) {
			m_.applied = at;
			changed();  // a rejected transaction moves the position too (SR-D16)
		}
		if (chores_owed_.load(std::memory_order_acquire)) { run_chores(); }
		return v;
	}

	// A client's hold on the index for one request (write-buffer D2): never exclusive, never
	// waits. Its end checks the index in.
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
	// the index, and the next writer call reports it. Callable from any thread.
	void check_in() noexcept {
		const chore_scope chores{lease_};
		if (!chores) {
			chores_owed_.store(true, std::memory_order_release);
			return;
		}
		run_chores();
	}
	// Whether a check-in found the lease held and left its chores to the holder.
	bool chores_owed() const noexcept { return chores_owed_.load(std::memory_order_acquire); }

	// Writes the buffer as a segment (searchable, not yet durable). ENOSPC leaves the
	// buffer in place to retry once space returns; other failures fence.
	void refresh() {
		const lease_scope leased{lease_};
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
		const lease_scope leased{lease_};
		live();
		publish_merges(true, false);
		schedule(ids);
		publish_merges(true, true);
	}

	// Merges until at most `max_segments` remain and none holds deletes, by the policy's
	// forced merges. With an executor it waits for every background merge, then for its own.
	void force_merge(std::size_t max_segments = 1) {
		const lease_scope leased{lease_};
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_merges(std::move(c), max_segments); }, true, true);
	}

	// Merges away the segments whose deleted fraction passes the policy's
	// force_merge_deletes_pct_allowed, waiting like force_merge().
	void force_merge_deletes() {
		const lease_scope leased{lease_};
		run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_forced_deletes_merges(std::move(c)); }, true);
	}

	// Runs the policy's merges until it asks for none, or until storage admission has no
	// room for the next; those wait for space. With an executor it hands one round to it,
	// leaving out segments already under merge or damaged, and returns.
	void maybe_merge() {
		const lease_scope leased{lease_};
		try {
			run_merges([&](std::vector<merge_policy::candidate> c) { return opts_.policy.find_merges(std::move(c)); }, false);
		} catch (const std::system_error& e) {
			if (e.code().value() != ENOSPC) { throw; }
		}
	}

	// Waits for this index's background merges and publishes them. One that ran out of
	// space is discarded, its sources left for a later merge; any other failure fences.
	void wait_merges() {
		const lease_scope leased{lease_};
		live();
		publish_merges(true, false);
	}
	// Background merges handed to the executor and not yet published.
	std::size_t merges_pending() const noexcept { return pending_.size(); }
	// Whether a background merge found a damaged segment; merges skip it until heal().
	bool needs_heal() const noexcept {
		return std::any_of(segs_.begin(), segs_.end(), [](const auto& s) { return s->damaged; });
	}

	// Makes everything applied durable (design D1), then reclaims what retention allows.
	// ENOSPC from a write before that file's sync leaves the index as it was (the partial
	// file removed); any other failure, and any failed sync, rename, or directory sync,
	// fences the writer.
	// With readers, the order is flush, publish, scan and free, retain(), reclaim() (SR-D6): the
	// view this commit retires, which holds the sources of the merges it installed, is freed
	// first when no guard names it, so those sources are queued in the same commit.
	void commit() {
		const lease_scope leased{lease_};
		live();
		guarded([&] { flush(); });
		publish_merges(false, false);
		if (opts_.auto_merge) { maybe_merge(); }
		guarded([&] { commit_files(); });
		publish_point();
		free_views();
		retain();
		reclaim_files(opts_.reclaim_slice);
	}

	// Frees queued obsolete files, about `bytes` of them, each truncated from its end in
	// `reclaim_step` steps and then removed. Returns the bytes freed. Views readers released
	// are freed first, and the files only they held are queued then (D6), so an idle index
	// returns a released view's space here, not at its next commit.
	std::uint64_t reclaim(std::uint64_t bytes = ~std::uint64_t(0)) {
		const lease_scope leased{lease_};
		live();
		if (free_views()) { retain(true); }
		return reclaim_files(bytes);
	}
	std::size_t reclaim_pending() const noexcept { return reclaim_.size(); }

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
	std::uint64_t media_retries() const noexcept { return media_retries_; }

	// After a search meets corrupt_segment: verifies every committed segment in memory, and
	// reads one that fails again from its file, verified whole, in its place. Returns the
	// ids whose file is damaged too, for the host to fence and repair from a checkpoint or
	// a replica. Segments a background merge found damaged are among those checked, and
	// merges take them again once they pass. Like a merge, it invalidates segment views.
	std::vector<std::uint64_t> heal() {
		const lease_scope leased{lease_};
		live();
		std::vector<std::uint64_t> damaged;
		for (auto& s : segs_) {
			if (s->unsynced) {  // built in memory, never read from disk
				if (s->damaged) { damaged.push_back(s->ref.id); }
				continue;
			}
			try {
				if (s->damaged) {  // a merge found it inconsistent: the checksums alone do not clear it
					basic_check_report<Layout> r;
					check_segment(s->seg, &s->deleted, r, m_.key_prefix);
				} else {
					s->seg.verify_all();
				}
				s->damaged = false;
				continue;
			} catch (const corrupt_segment&) {
			}
			try {
				auto fresh = read_checked([&] { return load_segment(*io_, m_, s->ref, true); }, nullptr);
				if (s->damaged) {
					basic_check_report<Layout> r;
					check_segment(fresh->seg, &s->deleted, r, m_.key_prefix);
				}
				fresh->deleted = std::move(s->deleted);
				fresh->dead = s->dead;
				fresh->deletes_dirty = s->deletes_dirty;
				fresh->ref = s->ref;
				s = std::move(fresh);  // a view keeps the failing object until it is freed (D11)
				++media_retries_;
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

	struct segment_view {
		std::uint64_t id;
		const segment* seg;
		const deleted_bits* deleted;  // bit per ordinal; null when none is deleted
		std::uint64_t dead = 0;                      // bits set in deleted
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
		segment_list out;
		out.views_.reserve(segs_.size());
		out.bits_.reserve(segs_.size());
		for (const auto& s : segs_) { out.views_.push_back({s->ref.id, &s->seg, s->dead ? &s->deleted : nullptr, s->dead}); }
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
	struct open_segment;

public:
	// One published generation (D1): the segments, their live bits frozen (D3), and the fields
	// readers need from the manifest. Built whole under the write lease, never written again,
	// freed under the lease once no hazard record names it (D6). Its reads are a snapshot's.
	class view {
	public:
		std::span<const segment_view> segments() const noexcept { return views_; }
		std::optional<std::string> get(std::string_view key) const
			requires(segment::template contains<stored_plugin>())
		{
			return snapshot(views_).get(key);
		}
		template <class F>
		bool get(std::string_view key, F&& f) const
			requires(segment::template contains<stored_plugin>())
		{
			return snapshot(views_).get(key, f);
		}
		template <class... A>
		std::size_t scan(A&&... a) const
			requires(snapshot_scannable())
		{
			return snapshot(views_).scan(std::forward<A>(a)...);
		}
		const std::map<std::string, std::string>& metadata() const noexcept { return *meta_; }
		std::uint64_t last_docid() const noexcept { return last_docid_; }
		std::uint64_t schema_version() const noexcept { return schema_version_; }
		format::structure_bits configured_structures() const noexcept { return configured_; }
		const std::string& key_prefix() const noexcept { return key_prefix_; }
		std::uint64_t generation() const noexcept { return generation_; }
		files::position position() const noexcept { return position_; }
		// The index's digest at the moment of publication, when its buffer was empty.
		std::uint64_t digest() const { return digest_of(views_, configured_, last_docid_, schema_version_, *meta_, [](auto&, auto&) {}); }

	private:
		friend class basic_index;
		std::vector<std::shared_ptr<const open_segment>> keep_;
		std::vector<deleted_bits> bits_;  // shares of the segments' bits; empty where none is deleted
		std::vector<segment_view> views_;
		std::shared_ptr<const std::map<std::string, std::string>> meta_;  // shared until the owner changes it
		std::string key_prefix_;
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
		explicit operator bool() const noexcept { return bool(s_); }

	private:
		friend class basic_index;
		explicit reader_handle(std::shared_ptr<reader_state> s) noexcept : s_(std::move(s)) {}
		std::shared_ptr<reader_state> s_;
	};

	// Starts publishing views, under the write lease (D4), and returns a handle on them. The first
	// call refreshes, so the handle never starts empty (D1). Needs options::readers.
	reader_handle reader() {
		const lease_scope leased{lease_};
		live();
		if (!opts_.readers) { throw std::logic_error("strata: reader() needs index_options::readers"); }
		if (!pub_) {
			pub_ = std::make_unique<publication>();
			pub_->rs = std::make_shared<reader_state>(opts_.readers);
		}
		if (!pub_->current) { refresh(); }
		return reader_handle(pub_->rs);
	}

	// Under the write lease (D9, SR-D20): refreshes only when the current view is behind `p`.
	// Each holder checks after it takes the lease, so of the calls the host's lease order queues
	// behind one that refreshed, none flushes again.
	void publish_through(files::position p) {
		const lease_scope leased{lease_};
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
	std::size_t buffered_count() const noexcept { return buffer_size(); }
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
				if (!x.doc.key.empty() && find_key_in(*all[k], x.doc.key) != wbuf::no_ref) { return false; }
				if (all[k]->docids_built && find_docid_in(*all[k], x.docid) != wbuf::no_ref) { return false; }
			}
			return true;
		};
		return digest_of(views, configured_structures(), m_.last_docid, m_.schema_version, meta_, [&](auto& document_record, auto& emit) {
			const format::structure_bits configured = configured_structures();
			for (std::size_t j = 0; j < all.size(); ++j) {
				const buffer& b = *all[j];
				if (inverting()) {  // the inverter's buffer: the same records as its documents' (F21)
					if (!b.inv) { continue; }
					std::vector<bool> keep(b.inv->buf.doc_count());
					for (std::size_t i = 0; i < b.order.size(); ++i) {
						const record& x = b.lane.at(b.order[i]);
						if (!x.tombstone() && x.ordinal() < keep.size()) { keep[x.ordinal()] = live(j, b.order[i]); }
					}
					b.inv->buf.digest(configured, [&](ordinal_t o) { return !keep[o]; }, document_record, emit);
					continue;
				}
				for (std::size_t i = 0; i < b.order.size(); ++i) {
					if (!live(j, b.order[i])) { continue; }
					const record& x = b.lane.at(b.order[i]);
					document_record(x.docid, x.version, x.doc.key);
					for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
						if constexpr (requires { typename E::part; E::records(x.docid, static_cast<const typename E::part&>(x.doc), emit); }) {
							if (configured & E::structure) { E::records(x.docid, static_cast<const typename E::part&>(x.doc), emit); }
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
		for (const auto& s : segs_) { check_segment(s->seg, &s->deleted, r, m_.key_prefix); }
		(void)digest();
		return r;
	}

	// Commits and pins the manifest; the generation names the checkpoint (design D4).
	std::uint64_t checkpoint() {
		const lease_scope leased{lease_};
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
		const lease_scope leased{lease_};
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
	void install(kronuz::journal::IO& source, std::uint64_t generation) {
		const lease_scope leased{lease_};
		live();
		files::manifest theirs;
		{
			auto f = source.open_existing(index_detail::manifest_name(generation));
			theirs = files::decode_manifest_for(index_detail::read_all(*f), readable());
		}
		if (theirs.generation != generation) { throw corrupt_index("install: manifest generation differs from its name"); }
		check_readable(theirs);
		auto got = load_files(source, theirs, true, &media_retries_);
		// The copy, re-encoded live docs (16 bytes for their new names' fields), and the
		// commit's metadata file and manifest.
		std::uint64_t need = theirs.meta.size + 96 + files::encode(theirs).size() + 160 * theirs.segments.size();
		for (const auto& s : got.segments) { need += s->ref.size + (s->ref.live.generation ? s->ref.live.size + 16 : 0); }
		if (storage_permit_ && !storage_permit_.try_grow(need) && !(drain() && storage_permit_.try_grow(need))) {
			throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: no storage reserved for an install");
		}
		hold held{this};
		if (storage_permit_) { held.add(need); }
		abandon_merges();  // their sources leave with the old content
		guarded([&] {
			for (auto& s : got.segments) {
				files::segment_ref mine = s->ref;
				mine.id = m_.next_segment++;
				mine.live = {};
				write_new(index_detail::segment_name(mine.id), s->bytes());
				if (s->ref.live.generation) {
					files::live_docs l;
					l.segment = mine.id;
					l.generation = m_.next_live++;
					l.doc_count = s->seg.core().doc_count();
					for (ordinal_t o = 0; o < s->seg.core().doc_count(); ++o) {
						if ((s->deleted[o >> 6] >> (o & 63)) & 1) { l.deleted.push_back(o); }
					}
					l.live_count = s->ref.live_count;
					l.live_length = s->ref.live_length;
					const std::string lb = files::encode(l);
					write_new(index_detail::live_name(l.segment, l.generation), lb);
					mine.live = {l.generation, lb.size(), files::trailing_crc(lb)};
				}
				s->ref = mine;
			}
			segs_ = std::move(got.segments);
			clear_buffer();
			reserved_ = 0;  // the discarded buffer's reservations
			if (permit_) { permit_.settle(); }
			meta_ = std::move(got.meta);
			meta_dirty_ = true;
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
	bool has_docid(std::uint64_t docid) const { return bool(locate_docid(docid)); }

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
				c.push_back({s->ref.id, s->ref.size, s->seg.core().doc_count(), deleted});
			}
			const auto merges = find(std::move(c));
			if (merges.empty()) { return; }
			for (const auto& ids : merges) { schedule(ids, forced); }
			if (!wait && !own_exec_) { return; }  // the index's own executor runs them now
			publish_merges(true, true);
		}
	}

	explicit basic_index(kronuz::journal::IO& io) : io_(&io) {}

	// Merges run on the host's executor, or on the index's own inline one, synchronously
	// on the writer's thread, when it passes none: one merge implementation either way.
	void start_merges() {
		exec_ = opts_.background;
		if (!exec_) {
			own_exec_ = std::make_unique<inline_executor>();
			exec_ = own_exec_.get();
		}
		group_ = exec_->open();
	}

	// The digest over segments (an index's or a view's), then what `buffer` adds: one
	// implementation for both (D8).
	template <class Segs, class Buffer>
	static std::uint64_t digest_of(const Segs& segs, format::structure_bits configured, std::uint64_t last_docid, std::uint64_t schema_version,
		const std::map<std::string, std::string>& meta, Buffer&& buffer) {
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
			const core_segment& core = s.seg->core();
			std::vector<std::string> key_of(core.doc_count());  // walked through the keys reader's checks (L3, MR2)
			s.seg->template get<keys_plugin>().each_key([&](std::string_view key, ordinal_t o) { key_of[o] = key; });
			for (ordinal_t o = 0; o < core.doc_count(); ++o) {
				if (live(o)) { document_record(core.docid(o), s.seg->template get<keys_plugin>().version(o), key_of[o]); }
			}
			for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
				if constexpr (requires { E::records(s.seg->template get<E>(), core, live, emit); }) {
					if (configured & E::structure) { E::records(s.seg->template get<E>(), core, live, emit); }
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
		return sum;
	}

	// --- scans ------------------------------------------------------------------------------

	struct record;
	struct buffer;

	// A buffer's records by key (format-14-kv D3): references in two sorted runs, a main run and
	// a delta of at most delta_cap records that folds into it when full, each by key and, within
	// a key, newest first. Records appended since the last scan are sorted into the delta; a fold
	// keeps only the records the key map still names.
	struct buffer_view {
		static constexpr std::size_t delta_cap = 4096;
		std::vector<std::uint32_t> main, delta;
		std::size_t covered = 0;  // position-index slots below this are in a run
		void reset() noexcept {
			std::vector<std::uint32_t>().swap(main);
			std::vector<std::uint32_t>().swap(delta);
			covered = 0;
		}
		std::uint64_t bytes() const noexcept { return (main.capacity() + delta.capacity()) * sizeof(std::uint32_t); }
	};
	void sync_view(const buffer& b) const {
		auto& v = b.runs;
		const std::size_t size = b.order.size();
		if (v.covered > size) { v.reset(); }
		if (v.covered == size) { return; }
		const auto by_key = [&](wbuf::ref_t x, wbuf::ref_t y) {
			const record &p = b.lane.at(x), &q = b.lane.at(y);
			const int c = std::string_view(p.doc.key).compare(q.doc.key);
			return c != 0 ? c < 0 : p.slot > q.slot;
		};
		try {
			const std::size_t old = v.delta.size();
			for (std::size_t i = v.covered; i < size; ++i) {
				if (!b.lane.at(b.order[i]).doc.key.empty()) { v.delta.push_back(b.order[i]); }
			}
			if (v.delta.size() - old == 1 && old) {
				const std::uint32_t last = v.delta.back();
				v.delta.pop_back();
				v.delta.insert(std::upper_bound(v.delta.begin(), v.delta.end(), last, by_key), last);
			} else {
				std::sort(v.delta.begin() + old, v.delta.end(), by_key);
				std::inplace_merge(v.delta.begin(), v.delta.begin() + old, v.delta.end(), by_key);
			}
			if (v.delta.size() > buffer_view::delta_cap) {
				std::vector<std::uint32_t> merged;
				merged.reserve(v.main.size() + v.delta.size());
				const auto current = [&](std::uint32_t r) { return find_key_in(b, b.lane.at(r).doc.key) == r; };
				std::vector<std::uint32_t> x, y;
				std::copy_if(v.main.begin(), v.main.end(), std::back_inserter(x), current);
				std::copy_if(v.delta.begin(), v.delta.end(), std::back_inserter(y), current);
				std::merge(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(merged), by_key);
				v.main.swap(merged);
				std::vector<std::uint32_t>().swap(v.delta);
			}
			v.covered = size;
		} catch (...) {
			v.reset();
			throw;
		}
	}

	template <class F>
	std::size_t scan_keys(std::string_view from, const std::string_view* to, std::size_t n, F& f) const {
		struct guard {
			std::uint32_t& depth;
			explicit guard(std::uint32_t& d) : depth(d) { ++depth; }
			~guard() { --depth; }
		} in_scan(scanning_);
		if (!n || (to && *to <= from)) { return 0; }
		std::vector<buffer_runs> bufs;  // oldest first
		bufs.reserve(frozen_.size() + 1);
		for (const auto& b : frozen_) {
			sync_view(*b);
			bufs.push_back({b.get()});
		}
		if (wb_) {
			sync_view(*wb_);
			bufs.push_back({wb_.get()});
		}
		return scan_segments(owned_segments{segs_}, bufs, pending_index(), from, to, n, f);
	}

	// --- the segment reads (D8): one implementation for the index and its snapshots -----------

	// A buffer's records as two more sources of a scan: its sorted runs (format-14-kv D3).
	struct buffer_runs {
		const buffer* b;
	};
	// The pending deletes by (segment id, ordinal): copies stage 1 removed, applied at the freeze.
	using pending_set = std::unordered_set<std::uint64_t>;
	static std::uint64_t pending_key(std::uint64_t segment, ordinal_t ord) noexcept { return segment << 32 | ord; }
	static bool pending_has(const pending_set* p, std::uint64_t segment, ordinal_t ord) { return p && !p->empty() && p->count(pending_key(segment, ord)); }

	// A get's segment half: the segments newest first, each through the key range, the filter, and
	// the dictionary with its live bits; f borrows the record for the call. A copy a pending delete
	// names is absent (D6).
	template <class Segs, class F>
	static bool get_segments(const Segs& segs, std::string_view key, F& f, const pending_set* pending = nullptr) {
		const std::uint64_t hash = xxh64::hash(key);
		for (std::size_t i = segs.size(); i-- > 0;) {
			const segment_view s = segs[i];
			if (const auto ord = s.seg->template get<keys_plugin>().find(key, live_bits{s.deleted ? s.deleted->data() : nullptr}, hash)) {
				if (pending_has(pending, s.id, *ord)) { return false; }
				index_detail::lent_frame lent;
				f(s.seg->template get<stored_plugin>().stored(*ord, lent.frame()));
				return true;
			}
		}
		return false;
	}

	// A scan's k-way merge over the segments, oldest first, and the buffers' runs, oldest buffer
	// first. Among a key's sources the newest wins: a buffer's over a segment's, a newer buffer's
	// over an older's, the larger slot within a buffer. A winning tombstone emits nothing (D6).
	template <class Segs, class F>
	static std::size_t scan_segments(const Segs& segs, std::span<const buffer_runs> buffers, const pending_set* pending, std::string_view from, const std::string_view* to, std::size_t n, F& f) {
		if (!n || (to && *to <= from)) { return 0; }
		struct source {
			segment_view seg;
			keys::reader::range cur;
			typename stored_plugin::reader::stored_frame frame;
		};
		const auto skip_dead = [&](source& s) {
			while (s.cur.valid() && ((s.seg.dead && s.seg.deleted->test(s.cur.ordinal())) || pending_has(pending, s.seg.id, s.cur.ordinal()))) { s.cur.next(); }
		};
		std::vector<source> src;  // by segment, oldest first
		src.reserve(segs.size());
		for (std::size_t i = 0; i < segs.size(); ++i) {
			const segment_view sv = segs[i];
			const auto& k = sv.seg->template get<keys_plugin>();
			if (!k.key_count() || k.max_key() < from || (to && k.min_key() >= *to)) { continue; }
			src.push_back({sv, k.range(from), {}});
			skip_dead(src.back());
		}
		struct run {
			const buffer* b;
			const std::vector<std::uint32_t>* refs;
			std::size_t at;
			std::string_view key() const { return b->lane.at((*refs)[at]).doc.key; }
			bool valid() const { return at < refs->size(); }
		};
		std::vector<run> runs;  // two per buffer: main, then delta
		runs.reserve(2 * buffers.size());
		for (const buffer_runs& br : buffers) {
			for (const auto* refs : {&br.b->runs.main, &br.b->runs.delta}) {
				const auto it = std::lower_bound(refs->begin(), refs->end(), from, [&](std::uint32_t r, std::string_view key) { return std::string_view(br.b->lane.at(r).doc.key) < key; });
				runs.push_back({br.b, refs, std::size_t(it - refs->begin())});
			}
		}
		// Source ids: 0..S-1 the segments, then the runs.
		const std::size_t S = src.size();
		const auto valid = [&](std::size_t s) { return s < S ? src[s].cur.valid() : runs[s - S].valid(); };
		const auto key_of = [&](std::size_t s) { return s < S ? src[s].cur.key() : runs[s - S].key(); };
		std::vector<std::size_t> tied;
		tied.reserve(S + runs.size());
		std::size_t count = 0;
		while (count < n) {
			std::string_view best;
			tied.clear();
			for (std::size_t s = 0; s < S + runs.size(); ++s) {
				if (!valid(s)) { continue; }
				const std::string_view k = key_of(s);
				if (tied.empty() || k < best) {
					best = k;
					tied.clear();
					tied.push_back(s);
				} else if (k == best) {
					tied.push_back(s);
				}
			}
			if (tied.empty() || (to && best >= *to)) { break; }
			// The newest: the last tied source, or the other run of its buffer when its slot is larger.
			std::size_t pick = tied.back();
			if (pick >= S && (pick - S) % 2 == 1 && tied.size() > 1 && tied[tied.size() - 2] == pick - 1) {
				const run &m = runs[pick - 1 - S], &d = runs[pick - S];
				if (m.b->lane.at((*m.refs)[m.at]).slot > d.b->lane.at((*d.refs)[d.at]).slot) { pick = pick - 1; }
			}
			const std::string_view key = best;
			bool emit = true;
			std::string_view value;
			std::uint64_t version = 0;
			if (pick < S) {
				source& s = src[pick];
				const ordinal_t ord = s.cur.ordinal();
				value = s.seg.seg->template get<stored_plugin>().stored(ord, s.frame);
				version = s.seg.seg->template get<keys_plugin>().version(ord);
			} else {
				const run& r = runs[pick - S];
				const record& x = r.b->lane.at((*r.refs)[r.at]);
				emit = !x.tombstone();
				if (emit) {
					value = static_cast<const typename stored_plugin::part&>(x.doc).stored;
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
			// Every source holding the key moves past it: the runs first, while `key` (perhaps a
			// segment cursor's) is still valid.
			for (const std::size_t s : tied) {
				if (s >= S) {
					run& r = runs[s - S];
					while (r.valid() && r.key() == key) { ++r.at; }
				}
			}
			for (const std::size_t s : tied) {
				if (s < S) {
					src[s].cur.next();
					skip_dead(src[s]);
				}
			}
		}
		return count;
	}

	// --- memory -----------------------------------------------------------------------------

	static std::uint64_t doc_bytes(const document& d) { return sizeof(document) + 96 + d.part_bytes(); }
	// The buffers' maps, position indexes, and scan runs.
	std::uint64_t map_bytes() const {
		std::uint64_t n = 0;
		for_each_buffer([&](const buffer& b) { n += b.keys.bytes() + b.docids.bytes() + b.order.bytes() + b.runs.bytes(); });
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
	bool needs_flush() const {
		if (buffered_records() == 0) { return false; }
		if (buffered_bytes() >= opts_.buffer_share || map_bytes() >= opts_.buffer_share / 4) { return true; }
		if (!opts_.budget || !opts_.budget->pressure()) { return false; }
		const std::uint64_t div = opts_.pressure_flush_divisor;
		const std::uint64_t share = std::min(opts_.buffer_share, opts_.budget->capacity());
		return !div || buffered_bytes() + map_bytes() >= share / div;
	}

	// --- in-memory state ------------------------------------------------------------------

	struct open_segment {
		files::segment_ref ref;
		std::shared_ptr<const kronuz::journal::Mapping> mapping;  // the file, mapped (ARCHITECTURE, I/O)
		std::string owned;                                         // or its bytes, when the backend cannot map
		segment seg;
		deleted_bits deleted;   // bit per ordinal, copied on write (shared-reader D3)
		std::uint64_t dead = 0;  // bits set in deleted
		bool deletes_dirty = false;          // deletes since its live-docs generation
		bool damaged = false;                // a background merge found it corrupt; heal() clears it
		std::unique_ptr<kronuz::journal::File> unsynced;  // flushed, awaiting the commit's sync
		open_segment(files::segment_ref r, std::string b) : ref(r), owned(std::move(b)), seg(owned), deleted((seg.core().doc_count() + 63) / 64) {}
		open_segment(files::segment_ref r, std::shared_ptr<const kronuz::journal::Mapping> m)
			: ref(r), mapping(std::move(m)), seg(mapping->bytes()), deleted((seg.core().doc_count() + 63) / 64) {}
		open_segment(const open_segment&) = delete;
		open_segment& operator=(const open_segment&) = delete;
		// Dropped under the write lease, and unmapped there (D4, D6; SR-D5).
		~open_segment() { detail::owner_only(); }
		std::string_view bytes() const noexcept { return mapping ? mapping->bytes() : std::string_view(owned); }
	};
	// segs_ as segment_views, made as they are read, so the index's reads allocate nothing.
	struct owned_segments {
		const std::vector<std::shared_ptr<open_segment>>& segs;
		std::size_t size() const noexcept { return segs.size(); }
		segment_view operator[](std::size_t i) const noexcept {
			const open_segment& s = *segs[i];
			return {s.ref.id, &s.seg, s.dead ? &s.deleted : nullptr, s.dead};
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

	// A record (D3): the header the sequencer fills, then the writer's copy of the document (only
	// the key in a tombstone or an inverting layout, whose inverter holds the document). Nothing
	// is written after the record's map slot is stored (WB-9).
	struct record {
		static constexpr std::uint32_t tombstone_bit = std::uint32_t(1) << 31;
		std::uint64_t docid, version;
		std::uint32_t position;  // its transaction's place in the buffer, from 1 (D5 reads it)
		wbuf::ref_t prev;        // the record its key's slot, else its docid's, named before it
		std::uint32_t slot;      // its place in the position index
		std::uint32_t aux;       // the tombstone bit, then the inverter's ordinal
		document doc;
		record(std::uint64_t d, std::uint64_t v, std::uint32_t pos, wbuf::ref_t p, std::uint32_t s, std::uint32_t x, document dc)
			: docid(d), version(v), position(pos), prev(p), slot(s), aux(x), doc(std::move(dc)) {}
		bool tombstone() const noexcept { return aux & tombstone_bit; }
		ordinal_t ordinal() const noexcept { return aux & ~tombstone_bit; }
	};
	// A buffer: active while it takes writes, then frozen (D8) until its flush installs. Its
	// records sit in one lane (one writer, D14's mode of D3); the key map and the docid map name
	// each key's and docid's newest record; the position index lists accepted records in order.
	struct buffer {
		wbuf::lane<record> lane;
		wbuf::version_map keys;
		mutable wbuf::version_map docids;  // built once a docid-addressed operation or a keyless document needs it (D3)
		mutable bool docids_built = false;
		wbuf::stable_array<wbuf::ref_t> order;  // the position index (D9)
		std::uint32_t transactions = 0;          // accepted, the last one's position
		std::size_t docs = 0;                    // records that are not tombstones
		std::uint64_t bytes = 0;                 // the documents' estimate (doc_bytes)
		std::vector<std::pair<std::uint64_t, ordinal_t>> pending;  // segment copies removed, applied at the freeze (D6)
		pending_set pending_keys;
		std::unique_ptr<active_buffer> inv;  // an inverting layout's documents
		mutable buffer_view runs;            // the records by key, built when a scan asks
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

	// Space errors before a sync stay recoverable (write_unsynced throws space_error);
	// every other I/O error fences.
	struct space_error : std::system_error {
		using std::system_error::system_error;
	};
	template <class F>
	void guarded(F&& f) {
		try {
			f();
		} catch (const space_error& e) {
			throw std::system_error(e.code(), e.what());
		} catch (const std::system_error& e) {
			fenced_ = true;
			throw index_fenced(std::string("strata: durability failure: ") + e.what());
		}
	}

	// A segment just written to f: mapped when the backend maps and the mapping holds what
	// was written (size and footer), so its bytes leave memory; else kept.
	static std::unique_ptr<open_segment> written_segment(kronuz::journal::File& f, const files::segment_ref& r, std::string bytes) {
		if (auto m = f.map(); m && m->bytes().size() == r.size && (format::read_version(m->bytes()), format::decode_footer(m->bytes()).toc_crc == r.footer_crc)) {
			return std::make_unique<open_segment>(r, std::move(m));
		}
		return std::make_unique<open_segment>(r, std::move(bytes));
	}

	// A segment just streamed to `file`: mapped when the backend maps and the mapping holds
	// what was written, so its bytes leave memory; else read back (and charged to `held`).
	static std::unique_ptr<open_segment> streamed_segment(std::unique_ptr<kronuz::journal::File> file, files::segment_ref ref, flush_charge* held = nullptr) {
		std::unique_ptr<open_segment> seg;
		if (auto m = file->map(); m && m->bytes().size() == ref.size) {
			ref.footer_crc = format::decode_footer(m->bytes()).toc_crc;
			seg = std::make_unique<open_segment>(ref, std::move(m));
		} else {
			std::string bytes = index_detail::read_all(*file);  // a backend that cannot map
			ref.footer_crc = format::decode_footer(bytes).toc_crc;
			if (held) { held->grow(bytes.capacity()); }
			seg = std::make_unique<open_segment>(ref, std::move(bytes));
		}
		seg->unsynced = std::move(file);
		return seg;
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

	// A check-in's chores, under the lease: merges finished on the pool published, views no
	// reader holds freed and their files queued. They never throw: a failure fences the index,
	// and the next writer call reports it through live() (CR2).
	void run_chores() noexcept {
		chores_owed_.store(false, std::memory_order_relaxed);
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
		if (pub_->current && pub_->changes == pub_->published) { return; }
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
			v->views_.push_back({s->ref.id, &s->seg, s->dead ? &v->bits_.back() : nullptr, s->dead});
		}
		const view* last = pub_->current.get();
		v->meta_ = last && *last->meta_ == meta_ ? last->meta_ : std::make_shared<const std::map<std::string, std::string>>(meta_);
		v->key_prefix_ = m_.key_prefix;
		v->last_docid_ = m_.last_docid;
		v->schema_version_ = m_.schema_version;
		v->configured_ = m_.configured;
		v->position_ = m_.applied;
		v->generation_ = pub_->generation + 1;
		return v;
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
		if (!pub_ || pub_->retired.empty()) { return false; }
		const std::size_t before = pub_->retired.size();
		const std::size_t left = pub_->retired.reclaim(*pub_->rs->registry, exec_->now());
		assert(left <= pub_->rs->registry->capacity());
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
			const std::uint64_t docs = s->seg.core().doc_count();
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
			opts_.zstd_merged ? std::optional<format::stored_codec>(format::zstd_codec(1)) : std::nullopt};
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
				live += job.sources[k]->seg.core().doc_count() - dead;
			}
			if (live == 0) {  // nothing to write: publication drops the sources
				job.done.store(true, std::memory_order_release);
				return;
			}
			job.memory = flush_charge(ctx.budget);
			if (!ctx.exec->checkpoint(stop)) { throw task::cancelled{}; }
			std::vector<basic_merge_source<Layout>> sources;
			for (std::size_t k = 0; k < job.sources.size(); ++k) { sources.push_back({&job.sources[k]->seg, &job.deleted[k]}); }
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
				if (!(mapped = file->map())) {
					bytes = index_detail::read_all(*ctx.io->open_existing(name));
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
			job.output = mapped ? std::make_unique<open_segment>(ref, std::move(mapped)) : std::make_unique<open_segment>(ref, std::move(bytes));
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
				check_segment(src->seg, &src->deleted, r, m_.key_prefix);
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
		group_.reset();
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

	// reclaim()'s file half: the queue, about `bytes` of it.
	std::uint64_t reclaim_files(std::uint64_t bytes) {
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
			if (s->deletes_dirty) { n += 96 + (std::uint64_t(s->seg.core().doc_count()) + 7) / 8; }
		}
		if (meta_dirty_ || !m_.meta.generation) {
			n += 96;
			for (const auto& [k, v] : meta_) { n += k.size() + v.size() + 20; }
		}
		return n;
	}

	// Reclaims every queued file for admission that is short of space; false if none was.
	bool drain() {
		if (reclaim_.empty() && !unlinked_) { return false; }
		reclaim();
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
		const std::uint64_t want = on_disk_ + unlinked_ + truncated_ + reserved_ + allowance_ + held_;
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

	wbuf::ref_t find_key_in(const buffer& b, std::string_view key) const {
		return b.keys.find(index_detail::key_hash(key), [&](wbuf::ref_t r) { return std::string_view(b.lane.at(r).doc.key) == key; });
	}
	wbuf::ref_t find_docid_in(const buffer& b, std::uint64_t docid) const {
		build_docids(b);
		return b.docids.find(index_detail::docid_hash(docid), [&](wbuf::ref_t r) { return b.lane.at(r).docid == docid; });
	}
	// The docid map, from the position index, the first time something needs it (D3). Every
	// record takes its docid's slot, in order, so the newest names each docid.
	void build_docids(const buffer& b) const {
		if (b.docids_built) { return; }
		b.docids.reserve(b.order.size());
		for (std::size_t i = 0; i < b.order.size(); ++i) { store_docid(b, b.order[i]); }
		b.docids_built = true;
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
		return x.doc.key.empty() ? find_docid_in(b, x.docid) == r : find_key_in(b, x.doc.key) == r;
	}
	copy buffered_copy(const buffer& b, typename copy::where_t w, std::size_t at, wbuf::ref_t r) const {
		const record& x = b.lane.at(r);
		if (x.tombstone()) { return {}; }
		return {w, at, r, 0, x.docid, x.version};
	}

	// A docid's live copy: the buffers newest first, through their docid maps, then the segments
	// newest first with their live bits and the pending deletes.
	copy locate_docid(std::uint64_t docid) const {
		if (wb_) {
			if (const auto r = find_docid_in(*wb_, docid); r != wbuf::no_ref) { return buffered_copy(*wb_, copy::active, 0, r); }
		}
		for (std::size_t i = frozen_.size(); i-- > 0;) {
			if (const auto r = find_docid_in(*frozen_[i], docid); r != wbuf::no_ref) { return buffered_copy(*frozen_[i], copy::frozen, i, r); }
		}
		const pending_set* pending = pending_index();
		for (std::size_t i = segs_.size(); i-- > 0;) {
			const auto& s = *segs_[i];
			if (auto ord = s.seg.core().ordinal_of(docid); ord && !((s.deleted[*ord >> 6] >> (*ord & 63)) & 1)) {
				if (pending_has(pending, s.ref.id, *ord)) { return {}; }
				return {copy::segment, i, wbuf::no_ref, *ord, docid, s.seg.template get<keys_plugin>().version(*ord)};
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
			const auto& keys = s.seg.template get<keys_plugin>();
			if (const auto ord = keys.find(key, live_bits{s.deleted.data()}, hash)) {
				if (pending_has(pending, s.ref.id, *ord)) { return {}; }
				return {copy::segment, i, wbuf::no_ref, *ord, s.seg.core().docid(*ord), keys.version(*ord)};
			}
		}
		return {};
	}
	// A copy's key, when the buffer holds it; a segment's is not recoverable from its ordinal.
	std::string_view key_of_copy(const copy& c) const {
		if (c.where == copy::active) { return wb_->lane.at(c.ref).doc.key; }
		if (c.where == copy::frozen) { return frozen_[c.at]->lane.at(c.ref).doc.key; }
		return {};
	}

	// --- transactions -----------------------------------------------------------------------

	// Each mutation records how to undo it; a rejection unwinds them in reverse. Records stay in
	// the lane, unreferenced; the position index rewinds to its count before the transaction.
	struct undo_step {
		enum kind { key_slot, docid_slot, pending, segment_dirty, last_docid, meta } type;
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
			b->lane.truncate(mark_.lane);  // the rejected records and their document copies
			b->order.rewind(mark_.records);
			b->docs = mark_.docs;
			b->bytes = mark_.bytes;
		}
	}

	// Removes a live copy a newer record or tombstone supersedes. In a buffer nothing changes: the
	// newer record shadows it (D6). In a segment it is a pending delete, applied at the freeze.
	void remove_copy(const copy& c) {
		if (c.where != copy::segment) { return; }
		auto& s = *segs_[c.at];
		buffer& b = *wb_;
		assert(s.ref.id < (std::uint64_t(1) << 32));
		b.pending.push_back({s.ref.id, c.ord});
		b.pending_keys.insert(pending_key(s.ref.id, c.ord));
		undo_.push_back({undo_step::pending});
		undo_.push_back({undo_step::segment_dirty, {}, c.at, {}, 0, {}, s.deletes_dirty});
		s.deletes_dirty = true;  // its live-docs file changes at the next freeze
	}

	// Appends a record and points its key's slot, and its docid's when that map exists, at it:
	// the record is complete before either store (WB-9). A tombstone carries only its key.
	wbuf::ref_t push_record(buffer& b, std::uint64_t docid, std::uint64_t version, const document* doc, std::string_view key, bool tomb) {
		const std::uint32_t kh = key.empty() ? 0 : index_detail::key_hash(key);
		wbuf::version_map::place kp;
		if (!key.empty()) { kp = b.keys.locate(kh, [&](wbuf::ref_t q) { return std::string_view(b.lane.at(q).doc.key) == key; }); }
		wbuf::ref_t prev = wbuf::no_ref;
		if (!key.empty()) {
			prev = wbuf::version_map::ref_of(kp);
		} else if (b.docids_built) {
			prev = find_docid_in(b, docid);
		}
		const auto slot = std::uint32_t(b.order.size());
		std::uint32_t aux = tomb ? record::tombstone_bit : 0;
		if (inverting() && !tomb) { aux |= std::uint32_t((b.inv ? b.inv->buf.doc_count() : 0) + pending_docs_.size()); }
		document copy_of;
		if (tomb || inverting()) {
			copy_of.key.assign(key);
		} else {
			copy_of = *doc;
			for_each_entry<Layout>([&]<class E>(std::type_identity<E>) {
				if constexpr (requires(typename E::part& p) { E::buffered(p); }) { E::buffered(static_cast<typename E::part&>(copy_of)); }
			});
		}
		const wbuf::ref_t r = b.lane.append(docid, version, b.transactions + 1, prev, slot, aux, std::move(copy_of));
		if (!key.empty()) {
			b.keys.store(kp, kh, r);
			undo_.push_back({undo_step::key_slot, kp});
		}
		if (b.docids_built) { undo_.push_back({undo_step::docid_slot, store_docid(b, r)}); }
		b.order.push_back(r);
		if (!tomb) { ++b.docs; }
		b.bytes += tomb || inverting() ? sizeof(record) : doc_bytes(*doc);
		return r;
	}

	void bump_last_docid(std::uint64_t docid) {
		if (docid <= m_.last_docid) { return; }
		undo_.push_back({undo_step::last_docid, {}, 0, {}, m_.last_docid});
		m_.last_docid = docid;
	}

	// The core's stage-1 context: the index's key prefix (F8).
	validate_context context() const { return {m_.key_prefix}; }
	std::optional<std::string_view> key_of(const document& d, bool& valid) const { return keys_plugin::key_of(context(), d, valid); }

	// Validation that can reject runs here, before anything changes (design D10).
	bool valid(const document& d) const { return d.valid(context()); }

	// The active buffer, ready for the transaction: created at the first write, its maps sized so
	// no growth moves a slot the undo log names, its docid map built when the transaction
	// addresses a docid or writes a keyless document.
	buffer& writable_buffer(const transaction& t) {
		if (!wb_) { wb_ = std::make_unique<buffer>(); }
		buffer& b = *wb_;
		bool docids = false;
		for (const auto& o : t.ops) {
			const bool writes = o.type == operation::add || o.type == operation::replace_key || o.type == operation::replace_docid;
			docids = docids || o.type == operation::replace_docid || o.type == operation::delete_docid || (writes && o.doc.key.empty());
		}
		b.keys.reserve(2 * t.ops.size());
		if (docids) { build_docids(b); }
		if (b.docids_built) { b.docids.reserve(2 * t.ops.size()); }
		mark_ = {b.order.size(), b.docs, b.bytes, b.lane.tell()};
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
			push_record(b, docid, version, &o.doc, new_key, false);
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
		if (!inverting()) { return static_cast<const typename stored_plugin::part&>(x.doc).stored; }
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
				const ordinal_t ord = a.buf.add(p.docid, p.version, p.key, *p.doc);
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

	// Whether the transaction's documents fit the active buffer's 32-bit structures.
	bool fits(const transaction& t) const noexcept {
		if (wb_ && wb_->lane.full()) { return false; }  // a full chunk directory freezes (D3)
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
		if (!wb_ || wb_->order.size() == 0) { return; }
		assert(pending_docs_.empty());
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
		frozen_.push_back(std::move(wb_));
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
		if (!frozen_.empty() || wb_) {
			for (std::size_t i = first; i < segs_.size(); ++i) {
				auto& s = *segs_[i];
				const auto& core = s.seg.core();
				const auto& keys = s.seg.template get<keys_plugin>();
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
					for_each_buffer([&](const buffer& b) { gone = gone || (b.docids_built && find_docid_in(b, core.docid(o)) != wbuf::no_ref); });
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
			if (newest(b, b.order[i])) { order.push_back(b.lane.at(b.order[i]).ordinal()); }
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
			if (newest(b, b.order[i])) { docs.push_back(&b.lane.at(b.order[i])); }
		}
		sorting.grow(docs.capacity() * sizeof(docs[0]));
		if (!docs.empty()) {
			if (configured_structures() & format::bit::index_sort) {
				std::sort(docs.begin(), docs.end(), [](const record* a, const record* c) {
					const std::string_view x = a->doc.key, y = c->doc.key;
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
							w.add({x->docid, x->version, x->doc.key}, x->doc, true);
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

	void commit_files() {
		// A segment whose documents are all deleted leaves without a merge; retention frees
		// its files.
		if (std::erase_if(segs_, [](const auto& s) { return s->dead == s->seg.core().doc_count(); })) { changed(); }
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
				l.doc_count = s->seg.core().doc_count();
				for (ordinal_t o = 0; o < s->seg.core().doc_count(); ++o) {
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
				s->ref.live_count = s->seg.core().doc_count();
				s->ref.live_length = s->seg.total_length();
			}
		}
		if (md) {
			const std::string bytes = files::encode(*md);
			m_.meta = {md->generation, bytes.size(), files::trailing_crc(bytes)};
			meta_dirty_ = false;
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
			next.mask |= s->seg.core().structures();
			const std::uint32_t v = s->seg.core().version();
			next.min_version = next.min_version ? std::min(next.min_version, v) : v;
			next.max_version = std::max(next.max_version, v);
			next.segments.push_back(s->ref);
			next.doc_count += s->ref.doc_count;
			next.total_length += s->seg.total_length();
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
		for (ordinal_t o = 0; o < s.seg.core().doc_count(); ++o) {
			if (!((s.deleted[o >> 6] >> (o & 63)) & 1)) { n += s.seg.length(o); }
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
		std::uint64_t top = 0, max_segment = 0, max_live = 0, max_meta = 0;
		for (const auto& n : names) {
			auto p = index_detail::parse_name(n);
			if (!p) { continue; }
			track(n, read_checked([&] { return io_->open_existing(n)->size(); }, &media_retries_));  // an EIO twice is damage
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
		m_ = read_checked([&] { return files::decode_manifest_for(index_detail::read_all(*io_->open_existing(index_detail::manifest_name(chosen))), readable()); }, &media_retries_);
		if (m_.generation != chosen) { throw corrupt_index("manifest generation differs from its name"); }
		// Every file the manifest names must exist and agree with its entry.
		check_readable(m_);
		auto files_of = load_files(*io_, m_, false, &media_retries_);
		segs_ = std::move(files_of.segments);
		meta_ = std::move(files_of.meta);
		if (!opts_.key_prefix.empty() && opts_.key_prefix != m_.key_prefix) { throw std::invalid_argument("strata: the index's key prefix differs"); }
		opts_.key_prefix = m_.key_prefix;
		// Counters start above every name on disk, so no name is ever reused (D3).
		m_.next_segment = std::max(m_.next_segment, max_segment + 1);
		m_.next_live = std::max(m_.next_live, max_live + 1);
		m_.next_meta = std::max(m_.next_meta, max_meta + 1);
		next_manifest_ = std::max(m_.generation, top) + 1;
		// An older manifest becomes the head by committing its content as a new generation,
		// so power loss leaves the old head or the chosen one, never a manifest between (D3).
		if (chosen != manifests.back()) {
			fallback_ = fallback_report{chosen, manifests.back(), std::uint64_t(manifests.end() - std::upper_bound(manifests.begin(), manifests.end(), chosen))};
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
	};
	// Runs a read and its checks, and once more if either fails: an unreliable read (a
	// transient media fault) passes the second time; damage on the medium fails both, and
	// an EIO then reports as corrupt_index, while other errors stay what they are.
	template <class F>
	static auto read_checked(F&& f, std::uint64_t* retries) -> decltype(f()) {
		try {
			return f();
		} catch (const corrupt_index&) {
		} catch (const corrupt_segment&) {
		} catch (const std::system_error&) {
		}
		if (retries) { ++*retries; }
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
	static loaded_files load_files(kronuz::journal::IO& io, const files::manifest& m, bool whole, std::uint64_t* retries = nullptr) {
		loaded_files out;
		for (const auto& r : m.segments) {
			out.segments.emplace_back(read_checked([&] { return load_segment(io, m, r, whole); }, retries));
		}
		if (m.meta.generation) {
			out.meta = read_checked([&] {
				const std::string mb = read_named(io, index_detail::meta_name(m.meta.generation), m.meta.size);
				if (files::trailing_crc(mb) != m.meta.crc) { throw corrupt_index("metadata differs from the manifest"); }
				const files::metadata md = files::decode_metadata(mb);
				if (md.generation != m.meta.generation) { throw corrupt_index("metadata generation"); }
				return md.entries;
			}, retries);
		}
		return out;
	}
	// The manifest's mask and version range passed check_readable, so a segment it names that
	// this binary cannot read disagrees with its manifest: damage (F2, F20).
	static std::unique_ptr<open_segment> load_segment(kronuz::journal::IO& io, const files::manifest& m, const files::segment_ref& r, bool whole) {
		try {
			return load_segment_of(io, m, r, whole);
		} catch (const unsupported_segment& e) {
			throw corrupt_segment(e.what());
		}
	}
	static std::unique_ptr<open_segment> load_segment_of(kronuz::journal::IO& io, const files::manifest& m, const files::segment_ref& r, bool whole) {
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
		auto s = mapped ? std::make_unique<open_segment>(r, std::move(mapped)) : std::make_unique<open_segment>(r, std::move(bytes));
		if (s->seg.core().doc_count() != r.doc_count) { throw corrupt_index("segment document count"); }
		// The manifest vouches for its segments: one outside its mask or version range is damage.
		const std::uint32_t v = s->seg.core().version();
		if ((s->seg.core().structures() & ~m.mask) || v < m.min_version || v > m.max_version) {
			throw corrupt_segment("segment outside its manifest's structures or versions");
		}
		if (whole) { s->seg.verify_all(); }
		if (r.live.generation) {
			const std::string lb = read_named(io, index_detail::live_name(r.id, r.live.generation), r.live.size);
			if (files::trailing_crc(lb) != r.live.crc) { throw corrupt_index("live docs differ from the manifest"); }
			const files::live_docs l = files::decode_live_docs(lb);
			if (l.segment != r.id || l.generation != r.live.generation || l.doc_count != r.doc_count || l.live_count != r.live_count ||
				l.live_length != r.live_length) {
				throw corrupt_index("live docs disagree with the manifest");
			}
			std::uint64_t* bits = s->deleted.mutable_words();
			for (std::uint64_t o : l.deleted) { bits[o >> 6] |= std::uint64_t(1) << (o & 63); }
			s->dead = 0;
			for (std::uint64_t w : s->deleted) { s->dead += std::uint64_t(std::popcount(w)); }
		}
		return s;
	}

	std::unique_ptr<executor::group> group_;  // first: assigned first and ended first, in ~basic_index()
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
	std::unique_ptr<buffer> wb_;
	std::vector<std::unique_ptr<buffer>> frozen_;
	std::vector<pending_doc> pending_docs_;  // the transaction's documents, inverted once it is accepted
	std::uint64_t max_run_bytes_ = inverter::run_cap;  // a run's bytes at most; tests lower it (index_test_access)
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
	std::uint64_t media_retries_ = 0;
	std::optional<std::uint64_t> retention_blocked_;
	std::optional<fallback_report> fallback_;
	std::optional<strata::filesystem_warning> fs_warning_;
	std::optional<strata::directory_trust_warning> trust_warning_;
	std::uint64_t on_disk_ = 0, reserved_ = 0;
	std::uint64_t unlinked_ = 0;  // removed, awaiting a directory sync to be freed
	std::uint64_t obsoleted_ = 0;  // bytes the merges published in this call made obsolete
	std::uint64_t debt_ = 0;       // obsoleted bytes not yet reclaimed: publishes and commits pay it
	std::uint64_t truncated_ = 0;  // truncated in this reclaim step, awaiting the file's sync
	std::uint64_t held_ = 0;      // reserved by a merge or install while it writes
	std::uint64_t allowance_ = 0, manifest_bytes_ = 0;  // the commit allowance, and its manifest's part
	std::uint64_t applied_since_commit_ = 0;
	std::uint64_t next_manifest_ = 1;
	std::optional<std::uint64_t> pending_pin_, pending_unpin_;
	bool meta_dirty_ = false;
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
