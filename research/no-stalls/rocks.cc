// The RocksDB side of docs/research/no-stalls.md: the same corpus, offered rate, batch,
// windows and percentiles as research_ingest, with puts of (key, text) in write batches
// and gets of written keys as the reads, so the write stalls RocksDB answers debt with
// (level0_slowdown / stop, pending compaction bytes) show up in the same columns. Built
// against a pinned RocksDB release (STRATA_ROCKSDB_SRC and STRATA_ROCKSDB_BUILD).
//   research_rocks DIR CORPUS [rate=5000 batch=100 seconds=120 docs=100000 threads=1
//     limit_mb=0 memtable_mb=8 queries=100 stalls=1 compaction=level|universal
//     compression=none|lz4|zstd cache_mb=256 seed=7]
#include "windows.hh"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {

using namespace nostalls;

struct doc {
	std::string id, text;
};

std::vector<doc> load(const std::string& path, std::size_t limit) {
	std::vector<doc> out;
	std::ifstream in(path);
	std::string line;
	while (out.size() < limit && std::getline(in, line)) {
		auto id = line.find("\"id\": \"");
		auto text = line.find("\"text\": \"");
		if (id == std::string::npos || text == std::string::npos) { continue; }
		id += 7;
		text += 9;
		out.push_back({line.substr(id, line.find('"', id) - id), line.substr(text, line.find('"', text) - text)});
	}
	return out;
}

double cpu_seconds() {
	rusage u{};
	getrusage(RUSAGE_SELF, &u);
	return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

std::uint64_t prop(rocksdb::DB& db, const char* name) {
	std::uint64_t v = 0;
	db.GetIntProperty(name, &v);
	return v;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fputs("research_rocks DIR CORPUS [key=value...]\n", stderr);
		return 2;
	}
	const std::string dir = argv[1], corpus = argv[2];
	const args a(argc, argv, 3);
	const double rate = a.num("rate", 5000), queries_per_s = a.num("queries", 100);
	const std::size_t batch = a.u64("batch", 100), seconds = a.u64("seconds", 120), ndocs = a.u64("docs", 100000), threads = std::max<std::uint64_t>(1, a.u64("threads", 1));
	const std::uint64_t limit = a.u64("limit_mb", 0) << 20, memtable = a.u64("memtable_mb", 8) << 20, seed = a.u64("seed", 7);
	const bool stalls = a.u64("stalls", 1) != 0;
	const std::string compaction = a.get("compaction", "level"), compression = a.get("compression", "none");

	auto docs = load(corpus, ndocs);
	if (docs.empty()) {
		std::fprintf(stderr, "no documents in %s\n", corpus.c_str());
		return 1;
	}
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	rocksdb::Options o;
	o.create_if_missing = true;
	o.max_background_jobs = int(threads) + 1;  // one flush thread beside the compaction threads, as strata's flush is the writer's
	o.max_subcompactions = 1;
	o.write_buffer_size = memtable;
	o.statistics = rocksdb::CreateDBStatistics();
	o.compression = compression == "lz4" ? rocksdb::kLZ4Compression : compression == "zstd" ? rocksdb::kZSTD : rocksdb::kNoCompression;
	if (compaction == "universal") { o.compaction_style = rocksdb::kCompactionStyleUniversal; }
	if (limit) { o.rate_limiter.reset(rocksdb::NewGenericRateLimiter(std::int64_t(limit), 100 * 1000, 10, rocksdb::RateLimiter::Mode::kWritesOnly)); }
	if (!stalls) {
		// Debt unbounded, as strata today: no slowdown, no stop.
		o.level0_slowdown_writes_trigger = 1 << 30;
		o.level0_stop_writes_trigger = 1 << 30;
		o.soft_pending_compaction_bytes_limit = 0;
		o.hard_pending_compaction_bytes_limit = 0;
		o.max_write_buffer_number = 64;
	}
	rocksdb::BlockBasedTableOptions t;
	t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
	t.block_cache = rocksdb::NewLRUCache(a.u64("cache_mb", 256) << 20);
	t.cache_index_and_filter_blocks = true;
	o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
	std::unique_ptr<rocksdb::DB> db;
	if (auto s = rocksdb::DB::Open(o, dir, &db); !s.ok()) {
		std::fprintf(stderr, "open: %s\n", s.ToString().c_str());
		return 1;
	}

	std::atomic<std::uint64_t> written{0};
	std::atomic<bool> stop{false};
	const double window = a.num("window", 1.0);
	const auto t0 = clock::now();
	windows w_sojourn(t0, window), w_service(t0, window), w_query(t0, window);

	// The reader: a get of a written key every 1/Q s, uniform over what is written.
	std::thread reader([&] {
		std::mt19937_64 rng(seed + 1);
		std::string value;
		for (std::uint64_t j = 0; !stop.load(std::memory_order_relaxed); ++j) {
			const auto due = t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(j) / queries_per_s));
			wait_until(due);
			if (stop.load(std::memory_order_relaxed)) { break; }
			const std::uint64_t n = written.load(std::memory_order_relaxed);
			const auto q0 = clock::now();
			if (n) {
				const std::uint64_t which = rng() % n;
				const std::uint64_t round = which / docs.size();
				std::string key = "Q" + docs[which % docs.size()].id;
				if (round) { key += "~" + std::to_string(round); }
				db->Get(rocksdb::ReadOptions(), key, &value);
			}
			w_query.add(clock::now(), micros(q0, clock::now()));  // the get's own time
		}
	});

	std::printf("# research_rocks v%d.%d.%d rate=%g batch=%zu seconds=%zu docs=%zu threads=%zu limit_mb=%llu memtable_mb=%llu queries=%g stalls=%d compaction=%s compression=%s window=%g\n",
		ROCKSDB_MAJOR, ROCKSDB_MINOR, ROCKSDB_PATCH, rate, batch, seconds, docs.size(), threads, (unsigned long long)(limit >> 20), (unsigned long long)(memtable >> 20), queries_per_s, int(stalls), compaction.c_str(), compression.c_str(), window);
	std::printf("win\toffered\tdone\tw_n\tw_p50\tw_p99\tw_p999\tw_max\ts_n\ts_p50\ts_p99\ts_p999\ts_max\tq_n\tq_p50\tq_p99\tq_p999\tq_max\tl0\tfiles\tpending_mb\tstall_ms\tcompact_mb\tflush_mb\tdelayed_rate\tstopped\tcpu_s\n");
	std::fflush(stdout);

	rocksdb::WriteOptions wo;
	wo.disableWAL = true;  // no log, as research_ingest commits once at the end
	std::uint64_t k = 0, done = 0, offered = 0, round = 0, cursor = 0;
	std::size_t reported = 0;
	double last_cpu = cpu_seconds();
	std::uint64_t last_stall = 0;
	auto last_tick = t0;
	auto report = [&](std::size_t upto) {
		for (; reported < upto; ++reported) {
			const std::uint64_t stall = o.statistics->getTickerCount(rocksdb::STALL_MICROS);
			std::uint64_t files = 0;
			for (int l = 0; l < 7; ++l) { files += prop(*db, ("rocksdb.num-files-at-level" + std::to_string(l)).c_str()); }
			const double cpu = cpu_seconds();
			std::printf("%zu\t%llu\t%llu", reported, (unsigned long long)offered, (unsigned long long)done);
			print_pct(w_sojourn.take(reported));
			print_pct(w_service.take(reported));
			print_pct(w_query.take(reported));
			std::printf("\t%llu\t%llu\t%.1f\t%.1f\t%.1f\t%.1f\t%llu\t%llu\t%.2f\n", (unsigned long long)prop(*db, "rocksdb.num-files-at-level0"), (unsigned long long)files,
				double(prop(*db, "rocksdb.estimate-pending-compaction-bytes")) / double(1 << 20), double(stall - last_stall) / 1000.0,
				double(o.statistics->getTickerCount(rocksdb::COMPACT_WRITE_BYTES)) / double(1 << 20), double(o.statistics->getTickerCount(rocksdb::FLUSH_WRITE_BYTES)) / double(1 << 20),
				(unsigned long long)(prop(*db, "rocksdb.actual-delayed-write-rate") >> 20), (unsigned long long)prop(*db, "rocksdb.is-write-stopped"), cpu - last_cpu);
			last_stall = stall;
			last_cpu = cpu;
			std::fflush(stdout);
		}
	};
	const auto end = t0 + std::chrono::seconds(seconds);
	rocksdb::WriteBatch wb;
	while (clock::now() < end) {
		const auto due = rate > 0 ? t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(k) * double(batch) / rate)) : clock::now();
		wait_until(due);
		const auto arrive = std::max(due, clock::now());
		wb.Clear();
		for (std::size_t j = 0; j < batch; ++j) {
			const doc& d = docs[cursor];
			std::string key = "Q" + d.id;
			if (round) { key += "~" + std::to_string(round); }
			wb.Put(key, d.text);
			if (++cursor == docs.size()) {
				cursor = 0;
				++round;
			}
		}
		if (auto s = db->Write(wo, &wb); !s.ok()) {
			std::fprintf(stderr, "write: %s\n", s.ToString().c_str());
			return 1;
		}
		const auto finished = clock::now();
		w_sojourn.add(finished, micros(due, finished));
		w_service.add(finished, micros(arrive, finished));
		done += batch;
		written.store(done, std::memory_order_relaxed);
		++k;
		offered = rate > 0 ? std::max(done, std::uint64_t(secs(t0, finished) * rate)) : done;
		if (secs(last_tick, clock::now()) >= 1.0) {
			last_tick = clock::now();
			report(std::size_t(secs(t0, clock::now()) / window));
		}
	}
	stop.store(true);
	reader.join();
	report(std::size_t(secs(t0, clock::now()) / window) + 1);
	const auto w0 = clock::now();
	db->Flush(rocksdb::FlushOptions());
	std::uint64_t files = 0;
	for (int l = 0; l < 7; ++l) { files += prop(*db, ("rocksdb.num-files-at-level" + std::to_string(l)).c_str()); }
	std::printf("# end: %llu documents in %llu batches; %llu files, %.1f MB on disk, compaction wrote %.1f MB, flushes %.1f MB, stall %.1f s total; flush at end %.1f s\n",
		(unsigned long long)done, (unsigned long long)k, (unsigned long long)files, double(prop(*db, "rocksdb.total-sst-files-size")) / double(1 << 20),
		double(o.statistics->getTickerCount(rocksdb::COMPACT_WRITE_BYTES)) / double(1 << 20), double(o.statistics->getTickerCount(rocksdb::FLUSH_WRITE_BYTES)) / double(1 << 20),
		double(o.statistics->getTickerCount(rocksdb::STALL_MICROS)) / 1e6, secs(w0, clock::now()));
	db.reset();
	return 0;
}
