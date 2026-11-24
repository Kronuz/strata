// strata's key-value engine for kvdriver.hh (rocksdb-survey.md, section 4): the core's
// `document_layout` (keys and stored records only, layers.md L12; key-ordered from format 14)
// through basic_index, linking strata::core alone.
// A put upserts by key. Durability `none` is an unlogged index, committed once at the end; `wal`
// and `sync` log every transaction through a turn-log host (standalone-turn-log D1, D8) in
// <dir>/host, with the indexes in <dir>/<i>. Under `wal` each put is its own transaction,
// acknowledged at apply (ack=none). Under `sync` a batch is one transaction, as RocksDB's
// WriteBatch is one synced write: applied at durable(), which waits for its record's barrier. A
// loop thread writes the turns, one barrier for every index's records; commit() is a checkpoint.
// Each index's group id is chosen at create and kept in <dir>/<i>/kv-group: a reopen at another
// loop count finds its group there, never from L (rocksdb-parity D10).
//
// A get is basic_index::get(key, f), the record lent to the copy into the caller's value; a
// scan is basic_index::scan(from, n, f), the records lent to the copy into the caller's sink.
// An erase deletes by key, batched like a put; a multiget is a loop of gets (the core
// has no batched get yet). Workload bulkload opens with automatic merges off; compact() then
// merges everything into one segment and commits.
#pragma once

#include "kvdriver.hh"

#include "strata/core/index.hh"
#include "strata/core/budget.hh"
#include "strata/core/work_pool_executor.hh"

#include "completion/posix.h"
#include "journal/host.h"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

using kv_layout = strata::document_layout;
constexpr const char* kv_layout_name = "document";
using kv_index = strata::basic_index<kv_layout>;

namespace {

// Bytes per section kind over every live segment's table of contents.
void section_bytes(const kv_index& ix, std::uint64_t (&bytes)[strata::format::section_kinds]) {
	for (const auto& v : ix.segments()) {
		for (const auto& e : v.seg()->core().sections()) {
			if (std::uint16_t(e.kind) < strata::format::section_kinds) { bytes[std::uint16_t(e.kind)] += e.length; }
		}
	}
}

// FNV-1a over every scanned key and value, printed at exit with KV_SCAN_DIGEST set: a scan's
// identity across changes to how it merges.
std::uint64_t scan_digest = 0xcbf29ce484222325ull;
const bool want_digest = std::getenv("KV_SCAN_DIGEST") != nullptr;
void digest(std::string_view a) {
	for (const unsigned char c : a) { scan_digest = (scan_digest ^ c) * 0x100000001b3ull; }
	scan_digest = (scan_digest ^ 0xff) * 0x100000001b3ull;
}
struct digest_printer {
	~digest_printer() {
		if (want_digest) { std::fprintf(stderr, "scan digest %016llx\n", (unsigned long long)scan_digest); }
	}
} digest_printer_;

}  // namespace

namespace {

constexpr kronuz::journal::PosixIOOptions host_io{true, true};

// How the engine starts: creating its indexes or reopening them, at `loops` loops (0: KV_LOOPS, or 1).
struct kv_opening {
	bool create = true;
	std::uint32_t loops = 0;
};

class strata_kv {
public:
	static constexpr const char* name = "strata";

	// The IO over a directory, <dir>/host or an index's; a test passes faultfs.
	using io_maker = std::function<std::shared_ptr<kronuz::journal::IO>(const std::string& dir)>;
	static std::shared_ptr<kronuz::journal::IO> posix_io(const std::string& dir) {
		std::filesystem::create_directories(dir);
		std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
		return std::make_shared<kronuz::journal::PosixIO>(dir, 0600, host_io);
	}

	using opening = kv_opening;

	explicit strata_kv(const kvbench::config& c, io_maker io = posix_io, opening at = {})
		: c_(c), io_of_(std::move(io)), loops_wanted_(at.loops), budget_(std::max<std::uint64_t>(c.memory / 4, 1 << 20)) {
		if (c.background) {
			WorkPool::Options po;
			po.threads = c.background;
			po.reserve = 0;
			po.name_format = "merge{:02}";
			pool_ = std::make_unique<WorkPool>(po);
			exec_ = std::make_unique<strata::work_pool_executor>(*pool_);
		}
		shards_.resize(c.indexes > 1 ? c.indexes : c.threads);
		groups_.resize(shards_.size());
		try {
			open_host();
			for (std::size_t i = 0; i < shards_.size(); ++i) { open(i, at.create); }
		} catch (...) {
			shards_.clear();
			close_host();
			throw;
		}
	}
	~strata_kv() {
		shards_.clear();
		close_host();
		exec_.reset();
	}

	// A sync phase batches writes until durable(); any other phase applies each alone.
	void on_phase(const char*, kvbench::durability d) {
		for (auto& s : shards_) { apply(s); }
		batching_ = d == kvbench::durability::sync;
	}
	void put(unsigned t, std::string_view key, std::string_view value) {
		shard& s = route(t, key);
		auto& op = next_op(s);
		op.type = kv_index::operation::replace_key;
		op.key.assign(key);
		op.doc.key.assign(key);
		op.doc.stored.assign(value);
		if (!batching_) { apply(s); }
	}
	bool get(unsigned t, std::string_view key, std::string& value) {
		return route(t, key).ix->get(key, [&](std::string_view v) { value.assign(v); });
	}
	void erase(unsigned t, std::string_view key) {
		shard& s = route(t, key);
		auto& op = next_op(s);
		op.type = kv_index::operation::delete_key;
		op.key.assign(key);
		if (!batching_) { apply(s); }
	}
	template <class F>
	void multiget(unsigned t, std::span<const std::string_view> keys, F&& hit) {
		for (std::size_t i = 0; i < keys.size(); ++i) {
			shard& s = route(t, keys[i]);
			if (s.ix->get(keys[i], [&](std::string_view v) { s.got.assign(v); })) { hit(i, std::string_view(s.got)); }
		}
	}
	void compact() {
		for (auto& s : shards_) {
			s.ix->refresh();
			s.ix->force_merge(1);
			s.ix->commit();
		}
	}
	std::uint64_t scan(unsigned t, std::string_view from, std::uint32_t n, std::string& sink) {
		std::uint64_t bytes = 0;
		const auto each = [&](std::string_view key, std::string_view value, std::uint64_t) {
			sink.append(value);
			bytes += value.size();
			if (want_digest) {
				digest(key);
				digest(value);
			}
			return true;
		};
		const kv_index& ix = *shards_[c_.indexes > 1 ? 0 : t].ix;
		if (n) {
			ix.scan(from, n, each);
		} else {
			ix.scan(from, each);  // n 0: to the end
		}
		return bytes;
	}
	// Applies the batch, then a logged index acknowledges it at its record's barrier; an unlogged
	// one commits. The many-index row's batch spans the shards its keys route to.
	void durable(unsigned t) {
		const auto one = [&](shard& s) {
			apply(s);
			if (!sealers_.empty()) {
				s.ix->await_durable(s.last);
			} else {
				s.ix->commit();
			}
		};
		if (c_.indexes <= 1) { return one(shards_[t]); }
		for (auto& s : shards_) { apply(s); }
		for (auto& s : shards_) { one(s); }
	}
	// The last applied position of a thread's index: the test's count of records per batch.
	strata::files::position last(unsigned t) const { return shards_[t].last; }
	// The deletes posted to the executor are part of settling (older headers delete inline).
	template <class Ix>
	static void wait_deletes(Ix& ix) {
		if constexpr (requires { ix.wait_deletes(); }) { ix.wait_deletes(); }
	}
	void settle() {
		for (auto& s : shards_) {
			s.ix->refresh();
			for (int round = 0; round < 64; ++round) {
				s.ix->wait_merges();
				s.ix->maybe_merge();
				if (!s.ix->merges_pending()) { break; }
			}
			s.ix->commit();
			// A commit with nothing new still writes a manifest (three barriers): commit again
			// only for merges this commit started.
			if (s.ix->merges_pending()) {
				s.ix->wait_merges();
				s.ix->commit();
			}
			// Settled means the obsolete-file queue is drained too; KV_SETTLE_RECLAIM=0 leaves it.
			if (const char* r = std::getenv("KV_SETTLE_RECLAIM"); !r || std::strcmp(r, "0") != 0) { s.ix->reclaim(); }
			wait_deletes(*s.ix);
		}
	}
	// The host reopens too, so each index recovers its group from the log.
	void reopen() {
		for (std::size_t i = 0; i < shards_.size(); ++i) {
			shards_[i].ix.reset();
			shards_[i].io.reset();
		}
		close_host();
		open_host();
		for (std::size_t i = 0; i < shards_.size(); ++i) { open(i, false); }
	}
	// Tests: index i, its group, and the host's loop count (0 unlogged).
	const kv_index& index(std::size_t i) const { return *shards_[i].ix; }
	std::uint64_t group(std::size_t i) const { return groups_[i]; }
	std::uint32_t loops() const { return host_ ? host_->loops() : 0; }
	std::string stats() {
		std::uint64_t segments = 0, docs = 0;
		for (auto& s : shards_) {
			for (const auto& v : s.ix->segments()) {
				++segments;
				docs += v.seg()->core().doc_count() - v.dead;
			}
		}
		// KV_SEGREPORT: per-segment docs, dead and section bytes, and the reclaim queue, on stderr.
		if (std::getenv("KV_SEGREPORT")) {
			for (auto& s : shards_) {
				std::fprintf(stderr, "reclaim_pending %zu merges_pending %zu\n", s.ix->reclaim_pending(), s.ix->merges_pending());
				for (const auto& v : s.ix->segments()) {
					std::uint64_t len = 0;
					for (const auto& e : v.seg()->core().sections()) { len += e.length; }
					std::fprintf(stderr, "seg %llu docs %llu dead %llu sections %llu\n", (unsigned long long)v.id, (unsigned long long)v.seg()->core().doc_count(), (unsigned long long)v.dead, (unsigned long long)len);
				}
			}
		}
		std::uint64_t sections[strata::format::section_kinds] = {};
		for (auto& s : shards_) { ::section_bytes(*s.ix, sections); }
		std::string by = "{";
		for (std::uint16_t k = 0; k < strata::format::section_kinds; ++k) {
			if (sections[k]) { by += (by.size() > 1 ? ",\"" : "\"") + std::to_string(k) + "\":" + std::to_string(sections[k]); }
		}
		by += "}";
		char b[256];
		std::snprintf(b, sizeof b, "{\"segments\":%llu,\"live_docs\":%llu,\"budget_peak\":%llu,\"shards\":%zu,\"layout\":\"%s\",\"sections\":", (unsigned long long)segments,
			(unsigned long long)docs, (unsigned long long)budget_.peak(), shards_.size(), kv_layout_name);
		return b + by + "}";
	}

private:
	struct shard {
		std::shared_ptr<kronuz::journal::IO> io;
		std::unique_ptr<kv_index> ix;
		kv_index::transaction tx;  // the batch: its first n ops
		std::size_t n = 0;
		std::string got;  // multiget's copy target
		strata::files::position last;  // the last write's position, which durable() awaits
	};
	// The op the next write fills. A batch is one transaction whatever its size: past the record
	// cap it streams as chunk records, all or nothing (rocksdb-parity A3).
	kv_index::operation& next_op(shard& s) {
		if (s.n == s.tx.ops.size()) { s.tx.ops.emplace_back(); }
		return s.tx.ops[s.n++];
	}
	void apply(shard& s) {
		if (!s.n) { return; }
		if (s.n < s.tx.ops.size()) { s.tx.ops.resize(s.n); }
		s.n = 0;
		if (const auto v = s.ix->apply(s.tx, {}); v.result != strata::verdict::accepted) {
			std::fprintf(stderr, "rejected: verdict %d at op %zu\n", int(v.result), std::size_t(v.op));
			std::exit(1);
		}
		s.last = s.ix->applied();
	}
	bool logged() const noexcept { return c_.dur != kvbench::durability::none; }
	// The rows' values are at most a few KiB: a 1 MiB record cap lets the host's segments be 4 MiB
	// (two framed commands, D3) rather than the 129 MiB that 64 MiB commands need.
	static constexpr std::uint64_t command_bytes = 1 << 20;
	static kronuz::journal::TurnLogOptions turns() {
		kronuz::journal::TurnLogOptions g;
		g.segment_bytes = 4 << 20;
		return g;
	}
	void open_host() {
		if (!logged()) { return; }
		const kronuz::journal::Identity id{'k', 'v', 'b', 'e', 'n', 'c', 'h', '-', 'h', 'o', 's', 't'};
		// KV_LOOPS: the host's loops, one turn log each (M3's N-log row).
		const char* l = std::getenv("KV_LOOPS");
		const std::uint32_t loops = loops_wanted_ ? loops_wanted_ : l ? std::max(1u, unsigned(std::strtoul(l, nullptr, 10))) : 1u;
		host_ = std::make_unique<kronuz::journal::Host>(io_of_(c_.dir + "/host"), id, kronuz::journal::HostOptions{loops, turns()});
		for (std::uint32_t i = 0; i < loops; ++i) {
			sealers_.push_back(std::make_unique<strata::turn_sealer>(*host_, i, id, turns(), command_bytes));
			// A failed turn ends the loop: the sealer keeps the failure, and its writers' waits throw it.
			loops_.emplace_back([s = sealers_.back().get()] {
				try {
					s->serve();
				} catch (const std::exception&) {
				}
			});
		}
	}
	// A created index's group: the next id after index i - 1's on loop i mod L, so the indexes
	// spread over the loops at create.
	std::uint64_t pick_group(std::size_t i) const {
		std::uint64_t g = i ? groups_[i - 1] + 1 : 1;
		while (host_->loop(g) != i % host_->loops()) { ++g; }
		return g;
	}
	static constexpr std::string_view group_file = "kv-group";
	static void store_group(kronuz::journal::IO& io, std::uint64_t g) {
		const std::string text = std::to_string(g);
		auto f = io.create_exclusive(group_file);
		for (std::size_t at = 0; at < text.size();) { at += f->write_at(at, std::string_view(text).substr(at)); }
		f->sync();
		io.sync_directory();
	}
	static std::uint64_t stored_group(kronuz::journal::IO& io) {
		auto f = io.open_existing_read_only(group_file);
		std::string text(f->size(), '\0');
		for (std::size_t at = 0; at < text.size();) {
			const std::size_t n = f->read_at(at, std::span<char>(text.data() + at, text.size() - at));
			if (!n) { break; }
			at += n;
		}
		std::uint64_t g = 0;
		const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), g);
		if (ec != std::errc{} || end != text.data() + text.size() || !g) { throw std::runtime_error("kv_strata: a damaged kv-group file"); }
		return g;
	}
	void close_host() {
		for (auto& s : sealers_) { s->stop(); }
		for (auto& t : loops_) { t.join(); }
		loops_.clear();
		sealers_.clear();
		host_.reset();
	}
	shard& route(unsigned t, std::string_view key) {
		if (c_.indexes <= 1) { return shards_[t]; }
		return shards_[kvbench::route_hash(key) % shards_.size()];
	}
	void open(std::size_t i, bool create) {
		shard& s = shards_[i];
		const std::string dir = shards_.size() == 1 && !logged() ? c_.dir : c_.dir + "/" + std::to_string(i);
		s.io = io_of_(dir);
		kv_index::options o;
		o.keyed = true;
		o.budget = &budget_;
		o.buffer_share = std::max<std::uint64_t>(budget_.capacity() / shards_.size(), 64 << 10);
		o.auto_merge = c_.w != kvbench::workload::bulkload;
		o.background = exec_.get();
		o.identity_seed = c_.seed + i;
		if (!sealers_.empty()) {
			groups_[i] = create ? pick_group(i) : stored_group(*s.io);
			if (create) { store_group(*s.io, groups_[i]); }
			o.log_group = groups_[i];  // the index joins its group at create (D7)
			o.log = sealers_[host_->loop(o.log_group)].get();
			o.command_bytes = command_bytes;
		}
		const std::array<char, 16> uuid{'k', 'v', 'b', 'e', 'n', 'c', 'h'};
		s.ix = std::make_unique<kv_index>(create ? kv_index::create(*s.io, o, uuid) : kv_index::open(*s.io, o));
	}


	kvbench::config c_;
	io_maker io_of_;
	bool batching_ = false;
	std::unique_ptr<kronuz::journal::Host> host_;
	std::vector<std::unique_ptr<strata::turn_sealer>> sealers_;  // one per loop
	std::vector<std::thread> loops_;
	std::vector<std::uint64_t> groups_;  // index i's group, stored in its directory
	std::uint32_t loops_wanted_ = 0;
	strata::byte_budget budget_;
	std::unique_ptr<WorkPool> pool_;
	std::unique_ptr<strata::executor> exec_;
	std::vector<shard> shards_;
};

}  // namespace
