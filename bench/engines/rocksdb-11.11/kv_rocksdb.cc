// RocksDB's key-value driver (rocksdb-survey.md, section 4): our code (MIT) linking a pinned
// RocksDB built locally. One DB; T threads share it; the many-index row puts K column
// families in it, sharing the WAL, the block cache, and the write buffer manager.
//
// Memory: one HyperClockCache of the row's budget holds data, index, and filter blocks, and
// the write buffer manager charges memtables to it (a quarter of the budget), so one number
// bounds RocksDB as one byte_budget bounds strata's buffers.
//
// Variants (--variant): leveled or universal, block size, compression, the data block hash
// index; calibration picks the fastest per value size.
#include "../../kv/kvdriver.hh"

#include <rocksdb/advanced_cache.h>
#include <rocksdb/cache.h>
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/write_buffer_manager.h>

#include <memory>

namespace {

void check(const rocksdb::Status& s, const char* what) {
	if (!s.ok()) {
		std::fprintf(stderr, "rocksdb %s: %s\n", what, s.ToString().c_str());
		std::exit(1);
	}
}

class rocksdb_kv {
public:
	static constexpr const char* name = "rocksdb";

	explicit rocksdb_kv(const kvbench::config& c) : c_(c), batches_(c.threads) {
		const std::string v = c.variant.empty() ? "lvl-4k-lz4" : c.variant;
		cache_ = rocksdb::HyperClockCacheOptions(c.memory, 0).MakeSharedCache();
		wbm_ = std::make_shared<rocksdb::WriteBufferManager>(c.memory / 4, cache_);
		rocksdb::BlockBasedTableOptions t;
		t.block_cache = cache_;
		t.cache_index_and_filter_blocks = true;
		t.pin_l0_filter_and_index_blocks_in_cache = true;
		t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
		t.block_size = v.find("-16k") != std::string::npos ? 16 << 10 : v.find("-8k") != std::string::npos ? 8 << 10 : 4 << 10;
		if (v.find("-hash") != std::string::npos) { t.data_block_index_type = rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash; }
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
		open(true);
	}
	~rocksdb_kv() { close(); }

	void put(unsigned t, std::string_view key, std::string_view value) {
		if (c_.dur == kvbench::durability::sync) {
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
