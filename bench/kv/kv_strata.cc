// strata's key-value driver (rocksdb-survey.md, section 4): the core's `document_layout` (keys
// and stored records only, layers.md L12; key-ordered from format 14) through basic_index,
// linking strata::core alone.
// A put is a one-operation transaction upserting by key. Durability `none` is an unlogged index,
// committed once at the end; `wal` and `sync` log every transaction through a turn-log host
// (standalone-turn-log D1, D8) in <dir>/host, with the indexes in <dir>/<i>: `wal` acknowledges
// at apply (ack=none), and `sync` waits for its batch's last record to be durable. A loop thread
// writes the turns, one barrier for every index's records; commit() is a checkpoint.
//
// A get is basic_index::get(key, f), the record lent to the copy into the caller's value; a
// scan is basic_index::scan(from, n, f), the records lent to the copy into the caller's sink.
// An erase is a one-operation transaction deleting by key; a multiget is a loop of gets (the core
// has no batched get yet). Workload bulkload opens with automatic merges off; compact() then
// merges everything into one segment and commits.
#include "kvdriver.hh"

#include "strata/core/index.hh"
#include "strata/core/budget.hh"
#include "strata/core/work_pool_executor.hh"

#include "completion/posix.h"
#include "journal/host.h"

#include <cstdlib>
#include <cstring>
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

class strata_kv {
public:
	static constexpr const char* name = "strata";

	explicit strata_kv(const kvbench::config& c) : c_(c), budget_(std::max<std::uint64_t>(c.memory / 4, 1 << 20)) {
		if (c.background) {
			WorkPool::Options po;
			po.threads = c.background;
			po.reserve = 0;
			po.name_format = "merge{:02}";
			pool_ = std::make_unique<WorkPool>(po);
			exec_ = std::make_unique<strata::work_pool_executor>(*pool_);
		}
		shards_.resize(c.indexes > 1 ? c.indexes : c.threads);
		open_host();
		for (std::size_t i = 0; i < shards_.size(); ++i) { open(i, true); }
	}
	~strata_kv() {
		shards_.clear();
		close_host();
		exec_.reset();
	}

	void put(unsigned t, std::string_view key, std::string_view value) {
		shard& s = route(t, key);
		auto& op = s.tx.ops[0];
		op.key.assign(key);
		op.doc.key.assign(key);
		op.doc.stored.assign(value);
		if (const auto v = s.ix->apply(s.tx, {}); v.result != strata::verdict::accepted) {
			std::fprintf(stderr, "rejected: verdict %d\n", int(v.result));
			std::exit(1);
		}
		s.last = s.ix->applied();
	}
	bool get(unsigned t, std::string_view key, std::string& value) {
		return route(t, key).ix->get(key, [&](std::string_view v) { value.assign(v); });
	}
	void erase(unsigned t, std::string_view key) {
		shard& s = route(t, key);
		auto& op = s.del.ops[0];
		op.key.assign(key);
		if (const auto v = s.ix->apply(s.del, {}); v.result != strata::verdict::accepted) {
			std::fprintf(stderr, "rejected: delete verdict %d\n", int(v.result));
			std::exit(1);
		}
		s.last = s.ix->applied();
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
	// A logged index acknowledges the batch at its last record's barrier; an unlogged one commits.
	void durable(unsigned t) {
		shard& s = shards_[c_.indexes > 1 ? 0 : t];
		if (sealer_) {
			s.ix->await_durable(s.last);
		} else {
			s.ix->commit();
		}
	}
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
		std::unique_ptr<kronuz::journal::PosixIO> io;
		std::unique_ptr<kv_index> ix;
		kv_index::transaction tx, del;
		std::string got;  // multiget's copy target
		strata::files::position last;  // the last write's position, which durable() awaits
	};
	bool logged() const noexcept { return c_.dur != kvbench::durability::none; }
	// The rows' values are at most a few KiB: a 1 MiB command limit lets the host's segments be
	// 4 MiB (two framed commands, D3) rather than the 129 MiB that 64 MiB commands need.
	static constexpr std::uint64_t command_bytes = 1 << 20;
	static kronuz::journal::TurnLogOptions turns() {
		kronuz::journal::TurnLogOptions g;
		g.segment_bytes = 4 << 20;
		return g;
	}
	void open_host() {
		if (!logged()) { return; }
		const std::string dir = c_.dir + "/host";
		std::filesystem::create_directories(dir);
		std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
		const kronuz::journal::Identity id{'k', 'v', 'b', 'e', 'n', 'c', 'h', '-', 'h', 'o', 's', 't'};
		host_ = std::make_unique<kronuz::journal::Host>(std::make_shared<kronuz::journal::PosixIO>(dir, 0600, host_io), id, kronuz::journal::HostOptions{1, turns()});
		sealer_ = std::make_unique<strata::turn_sealer>(*host_, 0, id, turns(), command_bytes);
		loop_ = std::thread([this] { sealer_->serve(); });
	}
	void close_host() {
		if (!sealer_) { return; }
		sealer_->stop();
		loop_.join();
		sealer_.reset();
		host_.reset();
	}
	shard& route(unsigned t, std::string_view key) {
		if (c_.indexes <= 1) { return shards_[t]; }
		return shards_[kvbench::route_hash(key) % shards_.size()];
	}
	void open(std::size_t i, bool create) {
		shard& s = shards_[i];
		const std::string dir = shards_.size() == 1 && !logged() ? c_.dir : c_.dir + "/" + std::to_string(i);
		std::filesystem::create_directories(dir);
		std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
		s.io = std::make_unique<kronuz::journal::PosixIO>(dir, 0600, host_io);
		kv_index::options o;
		o.keyed = true;
		o.budget = &budget_;
		o.buffer_share = std::max<std::uint64_t>(budget_.capacity() / shards_.size(), 64 << 10);
		o.auto_merge = c_.w != kvbench::workload::bulkload;
		o.background = exec_.get();
		o.identity_seed = c_.seed + i;
		if (sealer_) {
			o.log = sealer_.get();
			o.log_group = group_of(i);  // the index joins its group at create (D7)
			o.command_bytes = command_bytes;
		}
		const std::array<char, 16> uuid{'k', 'v', 'b', 'e', 'n', 'c', 'h'};
		s.ix = std::make_unique<kv_index>(create ? kv_index::create(*s.io, o, uuid) : kv_index::open(*s.io, o));
		s.tx.ops.resize(1);
		s.tx.ops[0].type = kv_index::operation::replace_key;
		s.del.ops.resize(1);
		s.del.ops[0].type = kv_index::operation::delete_key;
	}

	static std::uint64_t group_of(std::size_t i) noexcept { return i + 1; }

	kvbench::config c_;
	std::unique_ptr<kronuz::journal::Host> host_;
	std::unique_ptr<strata::turn_sealer> sealer_;
	std::thread loop_;
	strata::byte_budget budget_;
	std::unique_ptr<WorkPool> pool_;
	std::unique_ptr<strata::executor> exec_;
	std::vector<shard> shards_;
};

}  // namespace

int main(int argc, char** argv) { return kvbench::main_of<strata_kv>(argc, argv); }
