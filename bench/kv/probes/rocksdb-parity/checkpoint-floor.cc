// Probe (rocksdb-parity D2 item 2, N9): checkpoints and bytes written at a checkpoint trigger, on
// the many-indexes workload. N logged document indexes share one served turn log with kv_strata's
// geometry (4 MiB segments, 1 MiB commands). Each writes once, so all N are writing groups, then T
// threads put W values of V bytes, each thread over its own N / T indexes by Zipf(theta). The
// trigger is the bytes of commands an index logs before it checkpoints. A checkpoint is counted
// where an apply flushed the buffer (a commit). Prints one JSON line; phases go to stderr for
// wbytes.c (`phase NAME x`).
// With DIR/host present it reopens the indexes `--setup` left there.
//   checkpoint_floor DIR [--setup] [--indexes N] [--writes W] [--value V] [--threads T] [--theta Z] [--trigger B]
#include "strata/core/index.hh"
#include "strata/core/turn_seal.hh"
#include "strata/core/work_pool_executor.hh"

#include "completion/posix.h"
#include "journal/host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace jr = kronuz::journal;
using kv_index = strata::basic_index<strata::document_layout>;

constexpr std::uint64_t command_bytes = 1 << 20;

jr::TurnLogOptions geometry() {
	jr::TurnLogOptions g;
	g.segment_bytes = 4 << 20;
	return g;
}

std::shared_ptr<jr::IO> posix_io(const std::string& dir) {
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	return std::make_shared<jr::PosixIO>(dir, 0600, jr::PosixIOOptions{true, true});
}

void phase(const char* name) {
	char line[64];
	const int n = std::snprintf(line, sizeof line, "phase %s x\n", name);
	(void)!::write(2, line, std::size_t(n));
}

std::uint64_t splitmix(std::uint64_t& s) {
	std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

std::uint64_t arg(int argc, char** argv, const char* name, std::uint64_t dflt) {
	for (int i = 2; i + 1 < argc; ++i) {
		if (!std::strcmp(argv[i], name)) { return std::strtoull(argv[i + 1], nullptr, 10); }
	}
	return dflt;
}

double seconds_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: checkpoint_floor DIR [--setup] [--indexes N] [--writes W] [--value V] [--threads T] [--theta Z(x100)] [--trigger B]\n");
		return 2;
	}
	const std::string dir = argv[1];
	const std::size_t n = arg(argc, argv, "--indexes", 10000);
	const std::uint64_t writes = arg(argc, argv, "--writes", 2000000);
	const std::size_t value = arg(argc, argv, "--value", 100);
	const unsigned threads = unsigned(std::max<std::uint64_t>(1, arg(argc, argv, "--threads", 4)));
	const double theta = double(arg(argc, argv, "--theta", 99)) / 100;
	const std::uint64_t trigger = arg(argc, argv, "--trigger", 0);  // 0: the index's default
	bool setup = false;
	for (int i = 2; i < argc; ++i) { setup |= !std::strcmp(argv[i], "--setup"); }
	const bool reopen = !setup && std::filesystem::exists(dir + "/host");

	const jr::Identity id{'c', 'k', 'p', 't', '-', 'f', 'l', 'o', 'o', 'r'};
	jr::Host host(posix_io(dir + "/host"), id, jr::HostOptions{1, geometry()});
	strata::turn_sealer sealer(host, 0, id, geometry(), command_bytes);
	std::thread loop([&] {
		try {
			sealer.serve();
		} catch (const std::exception& e) {
			std::fprintf(stderr, "turn log failed: %s\n", e.what());
		}
	});
	WorkPool::Options po;
	po.threads = 2;
	po.reserve = 0;
	po.name_format = "merge{:02}";
	auto pool = std::make_unique<WorkPool>(po);
	auto exec = std::make_unique<strata::work_pool_executor>(*pool);

	phase(reopen ? "open" : "create");
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<std::shared_ptr<jr::IO>> ios(n);
	std::vector<std::unique_ptr<kv_index>> ixs(n);
	// On 16 threads. A create's syncs take about 70 ms on the host, mostly its group's, one at a
	// time: `--setup` creates (under nosync.c) and leaves the groups open, and a run reopens them.
	auto create = [&](std::size_t i) {
		char name[32];
		std::snprintf(name, sizeof name, "/i%06zu", i);
		ios[i] = posix_io(dir + name);
		kv_index::options o;
		o.keyed = true;
		o.buffer_share = std::uint64_t(256) << 20;  // the trigger, not the buffer, ends a record run
		o.background = exec.get();
		o.identity_seed = i + 1;
		o.log = &sealer;
		o.log_group = i + 1;
		o.command_bytes = command_bytes;
		if (trigger) { o.checkpoint_floor_bytes = trigger; }  // at 10k writers the window's share is below any floor
		std::array<char, 16> uuid{'c', 'k', 'p', 't'};
		std::memcpy(uuid.data() + 8, &i, sizeof i);
		ixs[i] = std::make_unique<kv_index>(reopen ? kv_index::open(*ios[i], o) : kv_index::create(*ios[i], o, uuid));
	};
	{
		std::atomic<std::size_t> next{0};
		std::vector<std::thread> cs;
		for (int t = 0; t < 16; ++t) {
			cs.emplace_back([&] {
				for (std::size_t i; (i = next++) < n;) { create(i); }
			});
		}
		for (auto& c : cs) { c.join(); }
	}
	const double create_s = seconds_since(t0);
	if (setup) {
		sealer.stop();
		loop.join();
		std::printf("{\"setup\":%zu,\"create_s\":%.2f}\n", n, create_s);
		std::fflush(stdout);
		std::exit(0);  // no leave: the groups stay open for the run's reopen
	}

	// Per thread: its indexes' Zipf CDF, and its count of puts and checkpoints.
	const std::size_t per = (n + threads - 1) / threads;
	std::vector<double> cdf(per);
	double sum = 0;
	for (std::size_t r = 0; r < per; ++r) { cdf[r] = sum += 1.0 / std::pow(double(r + 1), theta); }
	for (auto& c : cdf) { c /= sum; }
	std::atomic<std::uint64_t> checkpoints{0}, put{0};
	std::vector<std::uint64_t> generations(n);
	for (std::size_t i = 0; i < n; ++i) { generations[i] = ixs[i]->manifest().generation; }

	phase("run");
	const auto t1 = std::chrono::steady_clock::now();
	std::vector<std::thread> ws;
	for (unsigned t = 0; t < threads; ++t) {
		ws.emplace_back([&, t] {
			std::uint64_t rng = 0x5eed + t, mine = 0, cps = 0;
			const std::uint64_t quota = writes / threads;
			std::string body(value, 'v');
			kv_index::transaction tx;
			tx.ops.resize(1);
			auto& op = tx.ops[0];
			op.type = kv_index::operation::replace_key;
			auto one = [&](std::size_t i) {
				char key[24];
				std::snprintf(key, sizeof key, "%016" PRIx64, splitmix(rng));
				op.key.assign(key);
				op.doc.key.assign(key);
				std::memcpy(body.data(), key, std::min<std::size_t>(16, body.size()));
				op.doc.stored = body;
				const std::size_t before = ixs[i]->buffered_count();
				if (const auto v = ixs[i]->apply(tx, {}); v.result != strata::verdict::accepted) {
					std::fprintf(stderr, "rejected: verdict %d at index %zu\n", int(v.result), i);
					std::exit(1);
				}
				if (ixs[i]->buffered_count() <= before) { ++cps; }
				++mine;
			};
			for (std::size_t i = t; i < n; i += threads) { one(i); }  // every index writes
			while (mine < quota) {
				const double u = double(splitmix(rng) >> 11) / double(1ull << 53);
				const std::size_t r = std::size_t(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
				const std::size_t i = std::min(r, per - 1) * threads + t;
				if (i < n) { one(i); }
			}
			put += mine;
			checkpoints += cps;
		});
	}
	for (auto& w : ws) { w.join(); }
	const double run_s = seconds_since(t1);
	phase("end");

	std::uint64_t commits = 0;
	for (std::size_t i = 0; i < n; ++i) { commits += ixs[i]->manifest().generation - generations[i]; }
	sealer.stop();
	loop.join();
	const jr::TurnLogStats st = host.driver(0).log().stats();
	const std::size_t segments = host.driver(0).log().data_segments().size();
	std::printf(
		"{\"indexes\":%zu,\"writes\":%" PRIu64 ",\"value\":%zu,\"threads\":%u,\"theta\":%.2f,\"trigger\":%" PRIu64 ",\"open_s\":%.2f,\"run_s\":%.3f,"
		"\"puts_per_s\":%.0f,\"checkpoints\":%" PRIu64 ",\"checkpoints_per_s\":%.1f,\"commits\":%" PRIu64 ",\"log_bytes_written\":%" PRIu64
		",\"log_bytes_appended\":%" PRIu64 ",\"forwarded_bytes\":%" PRIu64 ",\"forwarded_records\":%" PRIu64 ",\"turns\":%" PRIu64
		",\"segments_prepared\":%" PRIu64 ",\"segments_recycled\":%" PRIu64 ",\"segments_removed\":%" PRIu64 ",\"data_segments\":%zu}\n",
		n, put.load(), value, threads, theta, trigger, create_s, run_s, double(put.load()) / run_s, checkpoints.load(), double(checkpoints.load()) / run_s, commits,
		st.bytes_written, st.bytes_appended, st.forwarded_bytes, st.forwarded_records, st.turns, st.segments_prepared, st.segments_recycled, st.segments_removed, segments);
	std::fflush(stdout);
	// The indexes' leaves (a commit and a turn each) are not part of the probe.
	std::exit(0);
}
