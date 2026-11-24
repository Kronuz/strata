// Probe (rocksdb-parity design): the cost of re-homing groups when a host opens with another loop
// count (journal Host's re-partition), on a real directory with nosync.c making syncs counted
// no-ops, so it measures CPU, bytes, syncs and memory, not device time. N groups, each a checkpoint
// record of `cp` bytes (an idle open index) and, for every `every`-th group, a batch of `batch`
// bytes (a recent write).
//   rehome DIR N L_FROM L_TO [cp=48] [batch=0] [every=1]
#include "completion/posix.h"

#include "journal/host.h"

#include <dlfcn.h>
#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace jr = kronuz::journal;

namespace {

std::uint64_t dir_bytes(const std::string& dir) {
	std::uint64_t n = 0;
	for (const auto& e : std::filesystem::directory_iterator(dir)) { n += e.is_regular_file() ? e.file_size() : 0; }
	return n;
}
// nosync.c's count of the syncs it skipped, when it is inserted.
std::uint64_t syncs() {
	static auto* f = reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "nosync_count"));
	return f ? f() : 0;
}

std::shared_ptr<jr::IO> open_io(const std::string& dir) { return std::make_shared<kronuz::journal::PosixIO>(dir, 0600, kronuz::journal::PosixIOOptions{true, true}); }

const jr::Identity id{'r', 'e', 'h', 'o', 'm', 'e'};

jr::TurnLogOptions geometry() {
	jr::TurnLogOptions g;
	g.segment_bytes = 4 << 20;  // the benchmark's (kv_strata.hh)
	return g;
}

double cpu() {
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	return double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}
std::uint64_t maxrss() {
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
	return std::uint64_t(ru.ru_maxrss);
#else
	return std::uint64_t(ru.ru_maxrss) * 1024;
#endif
}

void commit_all(jr::Host& h) {
	for (std::uint32_t l = 0; l < h.loops(); ++l) {
		while (!h.driver(l).idle()) { h.driver(l).commit(); }
	}
}

void measure(const std::string& dir, std::uint32_t loops, const char* what) {
	const auto s0 = syncs();
	const auto rss0 = maxrss();
	const auto bytes0 = dir_bytes(dir);
	const double c = cpu();
	const auto t = std::chrono::steady_clock::now();
	{
		jr::Host h(open_io(dir), id, jr::HostOptions{loops, geometry()});
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
		std::printf("%-22s L=%-2u open %.3f s cpu %.3f s syncs %llu dir %.1f -> %.1f MiB maxrss +%.1f MiB generation %llu\n", what, loops, s, cpu() - c,
			(unsigned long long)(syncs() - s0), double(bytes0) / (1 << 20), double(dir_bytes(dir)) / (1 << 20), double(maxrss() - rss0) / (1 << 20),
			(unsigned long long)h.generation());
	}
	std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 5) {
		std::fprintf(stderr, "rehome DIR N L_FROM L_TO [cp] [batch] [every]\n");
		return 2;
	}
	const std::string dir = argv[1];
	const std::uint64_t n = std::strtoull(argv[2], nullptr, 10);
	const auto from = std::uint32_t(std::atoi(argv[3])), to = std::uint32_t(std::atoi(argv[4]));
	const std::size_t cp = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 48;
	const std::size_t batch = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 0;
	const std::uint64_t every = argc > 7 ? std::strtoull(argv[7], nullptr, 10) : 1;
	const double c = cpu();
	const auto s0 = syncs();
	{
		jr::Host h(open_io(dir), id, jr::HostOptions{from, geometry()});
		std::uint64_t staged = 0;
		for (std::uint64_t g = 1; g <= n; ++g) {
			h.create_group(g);
			jr::TurnRecord r{g, 1, jr::TurnRecordKind::Checkpoint, std::string(cp, 'c')};
			h.driver_of(g).stage(r);
			staged += cp + 64;
			if (batch && g % every == 0) {
				jr::TurnRecord b{g, 2, jr::TurnRecordKind::Batch, std::string(batch, 'b'), 1};
				h.driver_of(g).stage(b);
				staged += batch + 64;
			}
			if (staged > (geometry().segment_bytes / 4 / 2) * from) {
				commit_all(h);
				staged = 0;
			}
		}
		commit_all(h);
	}
	std::printf("populate N=%llu L=%u cp=%zu batch=%zu every=%llu: cpu %.2f s, syncs %llu, dir %.1f MiB, maxrss %.1f MiB\n", (unsigned long long)n, from, cp, batch,
		(unsigned long long)every, cpu() - c, (unsigned long long)(syncs() - s0), double(dir_bytes(dir)) / (1 << 20), double(maxrss()) / (1 << 20));
	measure(dir, from, "reopen, same L");
	measure(dir, to, "reopen, re-partition");
	measure(dir, to, "reopen after, same L");
	return 0;
}
