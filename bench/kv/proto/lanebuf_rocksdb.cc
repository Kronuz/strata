// RocksDB 11.11 through the bake-off's workload code (lanebuf_work.hh), so the prototype and RocksDB
// run the same loops: T1 (fill, overwrite), T2/T3 (rww), T11 (requests over 1,000 column families).
// One DB, WAL off or on (--wal 0|1), memtables sized so nothing flushes during a row, the block cache
// large enough to hold the data (the prototype's segments are in memory), allow_concurrent_memtable_write
// at its default. T11's column families share one write queue and WAL: that asymmetry is the row's, stated
// in the report.
//   build: see bench/engines/rocksdb-11.11/build.sh for the library; ROCKSDB_SRC / ROCKSDB_BUILD as there.
#include "lanebuf_work.hh"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include <filesystem>
#include <memory>

using lbwork::args;

namespace {

void check(const rocksdb::Status& s, const char* what) {
	if (!s.ok()) {
		std::fprintf(stderr, "rocksdb %s: %s\n", what, s.ToString().c_str());
		std::exit(1);
	}
}

class rdb_engine {
public:
	struct ticket {
		unsigned idx;
	};
	rdb_engine(const std::string& dir, unsigned cfs, bool wal, std::uint64_t cache_mb, std::uint64_t memtable_mb) : wal_(wal) {
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		rocksdb::Options o;
		o.create_if_missing = true;
		o.create_missing_column_families = true;
		o.max_background_jobs = 2;
		o.write_buffer_size = std::size_t(memtable_mb) << 20;
		o.max_write_buffer_number = 4;
		o.min_write_buffer_number_to_merge = 1;
		o.compression = rocksdb::kNoCompression;
		o.statistics = nullptr;
		rocksdb::BlockBasedTableOptions t;
		cache_ = rocksdb::HyperClockCacheOptions(std::size_t(cache_mb) << 20, 0).MakeSharedCache();
		t.block_cache = cache_;
		t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
		t.cache_index_and_filter_blocks = true;
		t.pin_l0_filter_and_index_blocks_in_cache = true;
		o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
		std::vector<rocksdb::ColumnFamilyDescriptor> d;
		d.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(o));
		for (unsigned i = 1; i < cfs; ++i) { d.emplace_back("cf" + std::to_string(i), rocksdb::ColumnFamilyOptions(o)); }
		std::vector<rocksdb::ColumnFamilyHandle*> hs;
		check(rocksdb::DB::Open(rocksdb::DBOptions(o), dir + "/db", d, &hs, &db_), "open");
		cf_ = std::move(hs);
		wo_.disableWAL = !wal;
	}
	~rdb_engine() {
		for (auto* h : cf_) { db_->DestroyColumnFamilyHandle(h); }
	}
	ticket checkout(unsigned i) { return {i}; }
	bool put(ticket& t, std::string_view key, std::string_view value) { return db_->Put(wo_, cf_[t.idx], key, value).ok(); }
	bool get(ticket& t, std::string_view key, std::string& out) { return db_->Get(ro_, cf_[t.idx], key, &out).ok(); }
	std::size_t scan(ticket& t, std::string_view from, unsigned n, std::uint64_t& bytes) {
		std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro_, cf_[t.idx]));
		std::size_t got = 0;
		for (it->Seek(from); it->Valid() && got < n; it->Next()) {
			bytes += it->key().size() + it->value().size();
			++got;
		}
		return got;
	}
	void checkin(ticket&) {}
	void quiesce() {}
	// A small index's records live in a table file, as the prototype's live in a segment.
	void load_index(unsigned i, std::uint64_t n, std::uint64_t vsz, kvbench::values& gen) {
		rocksdb::WriteBatch b;
		for (std::uint64_t k = 0; k < n; ++k) {
			b.Put(cf_[i], kvbench::key_of(k), gen.next(vsz));
			if (b.Count() >= 500) {
				check(db_->Write(load_wo(), &b), "load");
				b.Clear();
			}
		}
		if (b.Count()) { check(db_->Write(load_wo(), &b), "load"); }
		check(db_->Flush(rocksdb::FlushOptions{}, cf_[i]), "flush");
	}
	static rocksdb::WriteOptions load_wo() {
		rocksdb::WriteOptions w;
		w.disableWAL = true;
		return w;
	}
	std::string stats_json() {
		std::string mt, sz;
		db_->GetProperty("rocksdb.cur-size-all-mem-tables", &mt);
		db_->GetProperty("rocksdb.estimate-live-data-size", &sz);
		return lbwork::fmt("{\"wal\":%d,\"memtables_bytes\":%s,\"live_data_bytes\":%s}", wal_ ? 1 : 0, mt.empty() ? "0" : mt.c_str(), sz.empty() ? "0" : sz.c_str());
	}

private:
	bool wal_;
	std::unique_ptr<rocksdb::DB> db_;
	std::vector<rocksdb::ColumnFamilyHandle*> cf_;
	std::shared_ptr<rocksdb::Cache> cache_;
	rocksdb::WriteOptions wo_;
	rocksdb::ReadOptions ro_;
};

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s fill|overwrite|rww|requests [options]; --dir D --wal 0|1 --cache-mb N --memtable-mb N --indexes K\n", argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	const args a(argc - 1, argv + 1);
	const unsigned K = mode == "requests" ? unsigned(a.num("--indexes", 1000)) : 1;
	rdb_engine e(a.str("--dir", ".scratch/rdb"), K, a.num("--wal", 0) != 0, a.num("--cache-mb", 4096), a.num("--memtable-mb", 1024));
	auto head = [&](const std::string& body) { std::printf("{\"impl\":\"rocksdb\",\"result\":%s}\n", body.c_str()); };
	if (mode == "fill" || mode == "overwrite") {
		head(lbwork::fill(e, a));
	} else if (mode == "rww") {
		head(lbwork::rww(e, a));
	} else if (mode == "requests") {
		lbwork::requests<rdb_engine> rq(e, a);
		const auto u0 = kvbench::sample();
		rq.preload(unsigned(a.num("--preload-threads", 4)));
		const auto u1 = kvbench::sample();
		std::printf("{\"impl\":\"rocksdb\",\"preload\":{\"records\":%llu,\"rss_delta\":%llu,\"engine\":%s}}\n", (unsigned long long)rq.total_records(), (unsigned long long)(u1.rss - u0.rss),
			e.stats_json().c_str());
		const double D = a.real("--seconds", 5);
		for (const auto& d : lbwork::split(a.str("--dists", "uniform,zipf"))) {
			for (const auto& m : lbwork::split(a.str("--mixes", "1w3r,3w1r"))) {
				for (const auto& r : lbwork::split(a.str("--rs", "1,4,8,16"))) { head(rq.run(unsigned(std::strtoul(r.c_str(), nullptr, 10)), d, m, D)); }
			}
		}
	} else {
		std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
		return 2;
	}
	return 0;
}
