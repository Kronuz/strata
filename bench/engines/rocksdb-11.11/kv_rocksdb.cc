// RocksDB's key-value driver (rocksdb-survey.md, section 4): our code (MIT) linking a pinned
// RocksDB built locally. One DB; T threads share it; the many-index row puts K column
// families in it, sharing the WAL, the block cache, and the write buffer manager.
//
// Memory: one HyperClockCache of the row's budget holds data, index, and filter blocks, and
// the write buffer manager charges memtables to it (a quarter of the budget), so one number
// bounds RocksDB as one byte_budget bounds strata's buffers.
//
// Variants (--variant): leveled or universal, block size, compression, the data block hash
// index; calibration picks the fastest per value size. Audit variants: `-tblidx` keeps index
// and filter blocks in the table reader, outside the cache and the budget; `-atomic` flushes
// every column family in one job.
//
// KV_ROCKSDB_STATS=1 (audit runs, not timed rows): RocksDB's statistics, flushes, compactions,
// and stall transitions per phase, one `{"rocksdb_phase":...}` line on stderr per phase.
#include "../../kv/kvdriver.hh"

#include <rocksdb/advanced_cache.h>
#include <rocksdb/cache.h>
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/write_buffer_manager.h>

#include <map>
#include <memory>
#include <mutex>

namespace {

void check(const rocksdb::Status& s, const char* what) {
	if (!s.ok()) {
		std::fprintf(stderr, "rocksdb %s: %s\n", what, s.ToString().c_str());
		std::exit(1);
	}
}

// Bytes the process read from storage, not from the page cache.
std::uint64_t device_read_bytes() {
#if defined(__APPLE__)
	rusage_info_v4 ri{};
	return proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) == 0 ? ri.ri_diskio_bytesread : 0;
#else
	std::uint64_t v = 0;
	if (FILE* f = std::fopen("/proc/self/io", "r")) {
		char line[128];
		unsigned long long x;
		while (std::fgets(line, sizeof line, f)) {
			if (std::sscanf(line, "read_bytes: %llu", &x) == 1) { v = x; }
		}
		std::fclose(f);
	}
	return v;
#endif
}

// Background events of the current phase, from RocksDB's listener threads.
class audit_listener : public rocksdb::EventListener {
public:
	void OnFlushCompleted(rocksdb::DB*, const rocksdb::FlushJobInfo& f) override {
		std::error_code ec;
		const auto bytes = std::filesystem::file_size(f.file_path, ec);
		std::lock_guard l(m_);
		++flushes_;
		flush_bytes_ += ec ? 0 : bytes;
		slowdown_at_flush_ += f.triggered_writes_slowdown;
		stop_at_flush_ += f.triggered_writes_stop;
	}
	void OnCompactionCompleted(rocksdb::DB*, const rocksdb::CompactionJobInfo& ci) override {
		char b[200];
		std::snprintf(b, sizeof b, "{\"from\":%d,\"to\":%d,\"in_files\":%zu,\"in\":%llu,\"out\":%llu,\"l0_after\":%d,\"reason\":\"%s\"}", ci.base_input_level, ci.output_level,
			ci.input_files.size(), (unsigned long long)ci.stats.total_input_bytes, (unsigned long long)ci.stats.total_output_bytes, ci.num_l0_files,
			rocksdb::GetCompactionReasonString(ci.compaction_reason));
		std::lock_guard l(m_);
		compactions_.emplace_back(b);
	}
	void OnStallConditionsChanged(const rocksdb::WriteStallInfo& w) override {
		static constexpr const char* names[] = {"delayed", "stopped", "normal"};
		std::lock_guard l(m_);
		stalls_.push_back(std::string(names[int(w.condition.prev)]) + ">" + names[int(w.condition.cur)]);
	}
	// The phase's events as JSON members, then cleared.
	std::string take() {
		std::lock_guard l(m_);
		std::string s = "\"flushes\":" + std::to_string(flushes_) + ",\"flush_bytes\":" + std::to_string(flush_bytes_) + ",\"flush_saw_slowdown\":" +
			std::to_string(slowdown_at_flush_) + ",\"flush_saw_stop\":" + std::to_string(stop_at_flush_) + ",\"compactions\":[";
		for (std::size_t i = 0; i < compactions_.size(); ++i) { s += (i ? "," : "") + compactions_[i]; }
		s += "],\"stall_changes\":[";
		for (std::size_t i = 0; i < stalls_.size(); ++i) { s += (i ? ",\"" : "\"") + stalls_[i] + "\""; }
		s += "]";
		flushes_ = flush_bytes_ = slowdown_at_flush_ = stop_at_flush_ = 0;
		compactions_.clear();
		stalls_.clear();
		return s;
	}

private:
	std::mutex m_;
	std::uint64_t flushes_ = 0, flush_bytes_ = 0, slowdown_at_flush_ = 0, stop_at_flush_ = 0;
	std::vector<std::string> compactions_, stalls_;
};

class rocksdb_kv {
public:
	static constexpr const char* name = "rocksdb";

	explicit rocksdb_kv(const kvbench::config& c) : c_(c), phase_dur_(c.dur), batches_(c.threads) {
		const std::string v = c.variant.empty() ? "lvl-4k-lz4" : c.variant;
		cache_ = rocksdb::HyperClockCacheOptions(c.memory, 0).MakeSharedCache();
		wbm_ = std::make_shared<rocksdb::WriteBufferManager>(c.memory / 4, cache_);
		rocksdb::BlockBasedTableOptions t;
		t.block_cache = cache_;
		t.cache_index_and_filter_blocks = v.find("-tblidx") == std::string::npos;
		t.pin_l0_filter_and_index_blocks_in_cache = true;
		t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
		t.block_size = v.find("-16k") != std::string::npos ? 16 << 10 : v.find("-8k") != std::string::npos ? 8 << 10 : 4 << 10;
		if (v.find("-hash") != std::string::npos) { t.data_block_index_type = rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash; }
		o_.atomic_flush = v.find("-atomic") != std::string::npos;
		o_.create_if_missing = true;
		o_.create_missing_column_families = true;
		o_.max_background_jobs = int(std::max(1u, c.background));
		o_.write_buffer_manager = wbm_;
		o_.write_buffer_size = std::max<std::uint64_t>(c.memory / 8, 1 << 20);
		o_.compaction_style = v.starts_with("uni") ? rocksdb::kCompactionStyleUniversal : rocksdb::kCompactionStyleLevel;
		o_.level_compaction_dynamic_level_bytes = true;
		const auto comp = v.find("-zstd") != std::string::npos ? rocksdb::kZSTD : v.find("-none") != std::string::npos ? rocksdb::kNoCompression : rocksdb::kLZ4Compression;
		o_.compression = comp;
		o_.bottommost_compression = v.find("-zbot") != std::string::npos ? rocksdb::kZSTD : comp;
		o_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
		if (c.indexes > 1) {
			// Each column family's memtables start small; the manager bounds them together.
			o_.write_buffer_size = std::max<std::uint64_t>(c.memory / 4 / c.indexes, 64 << 10);
		}
		wo_.disableWAL = c.dur == kvbench::durability::none;
		wo_.sync = false;
		if (const char* e = std::getenv("KV_ROCKSDB_STATS"); e && *e == '1') {
			o_.statistics = rocksdb::CreateDBStatistics();
			listener_ = std::make_shared<audit_listener>();
			o_.listeners.push_back(listener_);
		}
		open(true);
	}
	// The load runs at durability none whatever the row's setting: its puts go straight in, not
	// into the batch a sync row's run makes durable.
	void on_phase(const char* name, kvbench::durability d) {
		phase_dur_ = d;
		if (!listener_) { return; }
		dump(phase_);
		phase_ = name;
	}
	~rocksdb_kv() {
		if (listener_) { dump(phase_); }
		close();
	}

	void put(unsigned t, std::string_view key, std::string_view value) {
		if (phase_dur_ == kvbench::durability::sync) {
			check(batches_[t].Put(cf(key), rocksdb::Slice(key.data(), key.size()), rocksdb::Slice(value.data(), value.size())), "batch put");
			return;
		}
		check(db_->Put(wo_, cf(key), rocksdb::Slice(key.data(), key.size()), rocksdb::Slice(value.data(), value.size())), "put");
	}
	bool get(unsigned, std::string_view key, std::string& value) {
		const auto s = db_->Get(ro_, cf(key), rocksdb::Slice(key.data(), key.size()), &value);
		if (s.IsNotFound()) { return false; }
		check(s, "get");
		return true;
	}
	std::uint64_t scan(unsigned, std::string_view from, std::uint32_t n, std::string& sink) {
		std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro_, handles_[0]));
		std::uint64_t bytes = 0, got = 0;
		for (it->Seek(rocksdb::Slice(from.data(), from.size())); it->Valid() && (n == 0 || got < n); it->Next(), ++got) {
			const auto v = it->value();
			sink.append(v.data(), v.size());
			bytes += v.size();
		}
		check(it->status(), "scan");
		return bytes;
	}
	void durable(unsigned t) {
		rocksdb::WriteOptions w;
		w.sync = true;
		check(db_->Write(w, &batches_[t]), "sync write");
		batches_[t].Clear();
	}
	void settle() {
		rocksdb::FlushOptions f;
		f.wait = true;
		f.allow_write_stall = true;
		check(db_->Flush(f, handles_), "flush");
		rocksdb::WaitForCompactOptions w;
		check(db_->WaitForCompact(w), "wait for compact");
		if (!wo_.disableWAL) { check(db_->FlushWAL(true), "flush wal"); }
	}
	void reopen() {
		close();
		open(false);
	}
	std::string stats() {
		std::uint64_t sst = 0, live = 0, mem = 0, files = 0;
		for (auto* h : handles_) {
			std::uint64_t x = 0;
			if (db_->GetIntProperty(h, "rocksdb.total-sst-files-size", &x)) { sst += x; }
			if (db_->GetIntProperty(h, "rocksdb.estimate-live-data-size", &x)) { live += x; }
			if (db_->GetIntProperty(h, "rocksdb.cur-size-all-mem-tables", &x)) { mem += x; }
			std::string p;
			for (int l = 0; l < 7; ++l) {
				if (db_->GetProperty(h, "rocksdb.num-files-at-level" + std::to_string(l), &p)) { files += std::strtoull(p.c_str(), nullptr, 10); }
			}
		}
		char b[320];
		std::snprintf(b, sizeof b, "{\"sst_bytes\":%llu,\"live_bytes\":%llu,\"memtables\":%llu,\"sst_files\":%llu,\"cache_usage\":%zu,\"cache_pinned\":%zu,\"variant\":\"%s\"}",
			(unsigned long long)sst, (unsigned long long)live, (unsigned long long)mem, (unsigned long long)files, cache_->GetUsage(), cache_->GetPinnedUsage(),
			c_.variant.c_str());
		return b;
	}

private:
	// The phase's tickers and histogram counts (nonzero only), background events, stall
	// counters (cumulative), files per level, and device bytes read; statistics then reset.
	void dump(const std::string& phase) {
		auto& st = *o_.statistics;
		std::string s = "{\"rocksdb_phase\":\"" + phase + "\",\"tickers\":{";
		bool first = true;
		for (const auto& [t, name] : rocksdb::TickersNameMap) {
			if (const auto v = st.getTickerCount(t)) {
				s += (first ? "\"" : ",\"") + name + "\":" + std::to_string(v);
				first = false;
			}
		}
		s += "},\"histograms\":{";
		first = true;
		for (const auto& [h, name] : rocksdb::HistogramsNameMap) {
			rocksdb::HistogramData d;
			st.histogramData(h, &d);
			if (d.count) {
				char b[200];
				std::snprintf(b, sizeof b, "%s\"%s\":[%llu,%llu]", first ? "" : ",", name.c_str(), (unsigned long long)d.count, (unsigned long long)d.sum);
				s += b;
				first = false;
			}
		}
		s += "}," + listener_->take() + ",\"stall_stats\":{";
		std::map<std::string, std::string> m;
		first = true;
		if (db_ && db_->GetMapProperty(handles_[0], rocksdb::DB::Properties::kCFWriteStallStats, &m)) {
			for (const auto& [k, v] : m) {
				if (v != "0") {
					s += (first ? "\"" : ",\"") + k + "\":" + v;
					first = false;
				}
			}
		}
		m.clear();
		if (db_ && db_->GetMapProperty(rocksdb::DB::Properties::kDBWriteStallStats, &m)) {
			for (const auto& [k, v] : m) {
				if (v != "0") {
					s += (first ? "\"db." : ",\"db.") + k + "\":" + v;
					first = false;
				}
			}
		}
		s += "},\"files_per_level\":[";
		for (int l = 0; l < 7; ++l) {
			std::uint64_t n = 0;
			std::string p;
			for (auto* h : handles_) {
				if (db_ && db_->GetProperty(h, "rocksdb.num-files-at-level" + std::to_string(l), &p)) { n += std::strtoull(p.c_str(), nullptr, 10); }
			}
			s += (l ? "," : "") + std::to_string(n);
		}
		const auto dev = device_read_bytes();
		s += "],\"device_read_bytes\":" + std::to_string(dev - device_read_) + "}";
		device_read_ = dev;
		std::fprintf(stderr, "%s\n", s.c_str());
		(void)st.Reset();
	}

	rocksdb::ColumnFamilyHandle* cf(std::string_view key) const {
		return handles_.size() == 1 ? handles_[0] : handles_[kvbench::route_hash(key) % handles_.size()];
	}
	void open(bool create) {
		std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
		rocksdb::ColumnFamilyOptions co(o_);
		cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, co);
		for (unsigned i = 1; i < c_.indexes; ++i) { cfs.emplace_back("i" + std::to_string(i), co); }
		(void)create;
		check(rocksdb::DB::Open(o_, c_.dir, cfs, &handles_, &db_), "open");
	}
	void close() {
		if (!db_) { return; }
		for (auto* h : handles_) { check(db_->DestroyColumnFamilyHandle(h), "destroy handle"); }
		handles_.clear();
		check(db_->Close(), "close");
		db_.reset();
	}

	kvbench::config c_;
	kvbench::durability phase_dur_;
	std::shared_ptr<audit_listener> listener_;
	std::string phase_ = "open";
	std::uint64_t device_read_ = 0;
	std::shared_ptr<rocksdb::Cache> cache_;
	std::shared_ptr<rocksdb::WriteBufferManager> wbm_;
	rocksdb::Options o_;
	rocksdb::WriteOptions wo_;
	rocksdb::ReadOptions ro_;
	std::unique_ptr<rocksdb::DB> db_;
	std::vector<rocksdb::ColumnFamilyHandle*> handles_;
	std::vector<rocksdb::WriteBatch> batches_;
};

}  // namespace

int main(int argc, char** argv) { return kvbench::main_of<rocksdb_kv>(argc, argv); }
