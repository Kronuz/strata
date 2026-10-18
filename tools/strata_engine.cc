// strata as a search-benchmark-game engine.
//   strata_engine index DIR < corpus.json   one segment from JSON lines ("text": whitespace tokens)
//   strata_engine serve DIR                  "COMMAND\tquery" lines -> one count per line
//   strata_engine terms DIR                  every term and its document frequency
//   strata_engine dictstats DIR              the term dictionary's bytes by part
//   strata_engine sections DIR               each section's kind and length, then the file size
//   strata_engine check DIR                  every byte verified and every structure decoded
//   strata_engine verify DIR < queries.txt   pruned top-k equals an exhaustive scan, k = 10, 100, 1000,
//                                            ordinals and scores, Lucene and Xapian BM25
//   strata_engine bench DIR COMMAND RUNS [bm25_xapian] < queries.txt
//                                            per query: fastest of RUNS in-process runs, in µs,
//                                            on a segment file or an index's segments
//   strata_engine build-small ROOT COUNT DOCS [THREADS] < corpus   COUNT indexes of DOCS documents (small-indexes)
//   strata_engine open-many ROOT COUNT       open them all; per index: open time, bytes, descriptors, maps
//   strata_engine cache-sim ROOT N CAPACITY ACCESSES   Zipf accesses through an LRU of open indexes
//   strata_engine churn DIR < ops            the soak's op stream through strata::index (bench/soak):
//                                            "P\tid\ttext" upserts, "D\tid" deletes, "C" commits and
//                                            prints "C\tµs\tsegments", "Q\tCOMMAND\tquery" prints
//                                            "Q\tµs\tresult"
// Query syntax: words are optional, +word is required, -word is excluded, "a b" is a phrase.
#include "completion/posix.h"
#include "engine_query.hh"
#include "residency.hh"
#include "strata/budget.hh"
#include "strata/index.hh"
#include "strata/check.hh"
#include "strata/search.hh"
#include "strata/searcher.hh"
#include "strata/writer.hh"

#include <algorithm>
#include <array>
#include <optional>
#include <random>
#include <list>
#include <deque>
#include <atomic>
#include <bit>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include <sys/resource.h>
#include <thread>
#include <charconv>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <string>

using namespace engine;

#ifdef STRATA_ENGINE_COUNT_ALLOCS
// Attribution builds only: every operator new and delete counted, live bytes by usable size.
#include <malloc.h>
namespace alloc_count {
std::atomic<long long> live{0}, calls{0};
inline void* take(std::size_t n) {
	void* p = std::malloc(n ? n : 1);
	if (!p) { throw std::bad_alloc(); }
	live += (long long)malloc_usable_size(p);
	++calls;
	return p;
}
inline void give(void* p) noexcept {
	if (!p) { return; }
	live -= (long long)malloc_usable_size(p);
	std::free(p);
}
}  // namespace alloc_count
void* operator new(std::size_t n) { return alloc_count::take(n); }
void* operator new[](std::size_t n) { return alloc_count::take(n); }
void operator delete(void* p) noexcept { alloc_count::give(p); }
void operator delete[](void* p) noexcept { alloc_count::give(p); }
void operator delete(void* p, std::size_t) noexcept { alloc_count::give(p); }
void operator delete[](void* p, std::size_t) noexcept { alloc_count::give(p); }
#endif

namespace {

std::string_view text_field(std::string_view line) {
	auto at = line.find("\"text\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 9;
	return line.substr(at, line.find('"', at) - at);
}

// The "id" field, stored like the baseline engines' stored id.
std::string_view id_field(std::string_view line) {
	auto at = line.find("\"id\": \"");
	if (at == std::string_view::npos) { return {}; }
	at += 7;
	return line.substr(at, line.find('"', at) - at);
}

// Lines from stdin in reads of up to 1 MiB: macOS's libc++ reads std::cin a character at a
// time through getc, locking stdin for each, whatever sync_with_stdio says. read(2) returns
// what is there, so an interactive client gets its answer.
class LineReader {
public:
	bool next(std::string& line) {
		line.clear();
		for (;;) {
			if (at_ == end_) {
				ssize_t count;
				do { count = ::read(0, buffer_.data(), buffer_.size()); } while (count < 0 && errno == EINTR);
				end_ = count > 0 ? static_cast<std::size_t>(count) : 0; at_ = 0;
				if (!end_) { return !line.empty(); }
			}
			auto* start = buffer_.data() + at_;
			auto* newline = static_cast<char*>(std::memchr(start, '\n', end_ - at_));
			if (newline) { line.append(start, newline); at_ = static_cast<std::size_t>(newline - buffer_.data()) + 1; return true; }
			line.append(start, buffer_.data() + end_); at_ = end_;
		}
	}
private:
	std::array<char, 1 << 20> buffer_{}; std::size_t at_ = 0, end_ = 0;
};

// One bit (the high bit of its byte) for each space byte in the word, exactly.
inline std::uint64_t spaces(std::uint64_t w) noexcept {
	constexpr std::uint64_t ones = 0x0101010101010101ull, low7 = 0x7F7F7F7F7F7F7F7Full;
	const std::uint64_t x = w ^ (ones * ' ');
	return ~(((x & low7) + low7) | x | low7);
}

// Fills d in place (its strings and vectors keep their capacity): the key in terms, the
// text's words as tokens at positions 1, 2, ..., split a word of 8 bytes at a time.
void fill_document(strata::document& d, std::string_view key, std::string_view id, std::string_view text) {
	d.terms.resize(1);
	d.terms[0].term.assign(key);
	d.terms[0].wdf = 0;
	d.terms[0].positions.clear();
	d.text.assign(text);
	d.tokens.resize(text.size() / 2 + 1);  // the most words the text can hold
	strata::text_token* out = d.tokens.data();
	std::uint32_t pos = 0;
	std::size_t start = 0;  // where the current word starts
	auto word_end = [&](std::size_t j) {
		if (j > start) { *out++ = {std::uint32_t(start), std::uint32_t(j - start), ++pos}; }
		start = j + 1;
	};
	std::size_t at = 0;
	for (; at + 8 <= text.size(); at += 8) {
		std::uint64_t w;
		std::memcpy(&w, text.data() + at, 8);
		for (std::uint64_t m = spaces(w); m; m &= m - 1) { word_end(at + std::size_t(std::countr_zero(m)) / 8); }
	}
	for (; at < text.size(); ++at) {
		if (text[at] == ' ') { word_end(at); }
	}
	word_end(text.size());
	d.tokens.resize(std::size_t(out - d.tokens.data()));
	d.length = pos;
	d.stored.assign(id);
}

// Phase 3's index: each document upserted by its key ("Q" + id, Xapiand's key prefix),
// the buffer flushing past STRATA_BUFFER_MB (default 256) on STRATA_THREADS inversion
// threads (default 1, D18) and merging by the tiered policy (STRATA_AUTO_MERGE=0 turns
// merges off), committed once at the end. Segments, versions,
// keys, and merges cost what Phase 3's gate measures (T4).
int index(const std::string& dir) {
	std::ios::sync_with_stdio(false);
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	const char* mb = std::getenv("STRATA_BUFFER_MB");
	o.buffer_share = (mb ? std::strtoull(mb, nullptr, 10) : 256) << 20;
	// STRATA_BUDGET_MB: the node's indexing budget (D17), this index's whole share of it,
	// which the gate calibrates to the reference engine's peak RSS (BENCHMARKS.md).
	std::unique_ptr<strata::byte_budget> budget;
	if (const char* node = std::getenv("STRATA_BUDGET_MB")) {
		budget = std::make_unique<strata::byte_budget>(std::strtoull(node, nullptr, 10) << 20);
		o.budget = budget.get();
		o.buffer_share = budget->capacity();
	}
	const char* merge = std::getenv("STRATA_AUTO_MERGE");
	o.auto_merge = !merge || std::string_view(merge) != "0";
	if (const char* threads = std::getenv("STRATA_THREADS")) { o.flush_threads = std::max<std::size_t>(1, std::strtoull(threads, nullptr, 10)); }
	const std::array<char, 16> uuid{'s', 't', 'r', 'a', 't', 'a', '-', 'b', 'e', 'n', 'c', 'h'};
	auto ix = strata::index::create(io, o, uuid);
	// The batch's operations are reused, so their strings and vectors keep their capacity.
	strata::transaction t;
	std::size_t batched = 0;
	auto apply = [&] {
		if (!batched) { return; }
		t.ops.resize(batched);
		if (const auto v = ix.apply(t, {}); v.result != strata::verdict::accepted) {
			std::cerr << "rejected: verdict " << int(v.result) << " at operation " << v.op << " (key " << t.ops[v.op].key << ")\n";
			std::exit(1);
		}
		batched = 0;
	};
	std::string line;
	std::uint64_t n = 0;
	std::vector<std::string> keys;
	const bool keep_keys = std::getenv("STRATA_DELETE_EVERY") != nullptr;
	for (LineReader in; in.next(line);) {
		if (line.empty()) { continue; }
		if (batched == t.ops.size()) { t.ops.emplace_back(); }
		strata::operation& op = t.ops[batched++];
		op.type = strata::operation::replace_key;
		const auto id = id_field(line);
		op.key.assign("Q");
		op.key.append(id);
		if (keep_keys) { keys.push_back(op.key); }
		fill_document(op.doc, op.key, id, text_field(line));
		if (batched == 1000) { apply(); }
		if (++n % 100000 == 0) { std::cerr << n << '\n'; }
	}
	apply();
	// STRATA_DELETE_EVERY=N deletes every Nth document by key: T8's index with deletes.
	std::uint64_t deleted = 0;
	if (const char* every = std::getenv("STRATA_DELETE_EVERY"); every && std::strtoull(every, nullptr, 10)) {
		const auto step = std::strtoull(every, nullptr, 10);
		for (std::uint64_t i = 0; i < keys.size(); i += step, ++deleted) {
			if (batched == t.ops.size()) { t.ops.emplace_back(); }
			strata::operation& op = t.ops[batched++];
			op = strata::operation{};
			op.type = strata::operation::delete_key;
			op.key = keys[i];
			if (batched == 1000) { apply(); }
		}
		apply();
	}
	ix.commit();
	std::cerr << "index: " << n << " documents, " << deleted << " deleted, " << ix.segments().size() << " segments\n";
	return 0;
}

// COUNT keyed indexes at ROOT/0 .. COUNT-1 of DOCS documents each, from stdin's first lines
// (cycled, so each index's ids stay distinct), committed and closed, built on every core:
// the many-small-indexes rows (docs/perf/small-indexes.md, M0, M2).
//   strata_engine build-small ROOT COUNT DOCS [THREADS] < corpus
// THREADS builders at once (default: one a core); THREADS = COUNT writes every index at once.
// STRATA_BUDGET_MB: one node budget the indexes share, each a THREADS-th of it (M2).
int build_small(const std::string& root, std::size_t count, std::size_t docs, std::size_t threads) {
	std::vector<std::string> lines;
	const std::size_t want = std::max<std::size_t>(docs, std::min<std::size_t>(count * docs, 200000));
	{
		const auto in = std::make_unique<LineReader>();
		for (std::string line; lines.size() < want && in->next(line);) {
			if (!line.empty()) { lines.push_back(line); }
		}
	}
	if (lines.size() < docs) { std::cerr << "build-small: only " << lines.size() << " lines\n"; return 1; }
	std::filesystem::create_directories(root);
	if (!threads) { threads = std::max(1u, std::thread::hardware_concurrency()); }
	std::unique_ptr<strata::byte_budget> budget;
	if (const char* node = std::getenv("STRATA_BUDGET_MB")) { budget = std::make_unique<strata::byte_budget>(std::strtoull(node, nullptr, 10) << 20); }
	const char* merge = std::getenv("STRATA_AUTO_MERGE");
	std::vector<std::exception_ptr> errors(threads);
	std::vector<std::thread> pool;
	for (std::size_t k = 0; k < threads; ++k) {
		pool.emplace_back([&, k] {
			try {
				strata::transaction t;
				for (std::size_t i = k; i < count; i += threads) {
					const std::string dir = root + "/" + std::to_string(i);
					std::filesystem::create_directories(dir);
					std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
					kronuz::journal::PosixIO io(dir);
					strata::index::options o;
					o.key_prefix = "Q";
					o.auto_merge = !merge || std::string_view(merge) != "0";
					if (budget) {
						o.budget = budget.get();
						o.buffer_share = budget->capacity() / threads;
					}
					const std::array<char, 16> uuid{'s', 't', 'r', 'a', 't', 'a', '-', 's', 'm', 'a', 'l', 'l'};
					auto ix = strata::index::create(io, o, uuid);
					// Batches of 1,000 operations, as the index command applies them.
					for (std::size_t d0 = 0; d0 < docs; d0 += 1000) {
						t.ops.resize(std::min<std::size_t>(1000, docs - d0));
						for (std::size_t d = 0; d < t.ops.size(); ++d) {
							const std::string& line = lines[(i * docs + d0 + d) % lines.size()];
							strata::operation& op = t.ops[d];
							op.type = strata::operation::replace_key;
							const auto id = id_field(line);
							op.key.assign("Q");
							op.key.append(id);
							fill_document(op.doc, op.key, id, text_field(line));
						}
						if (ix.apply(t, {}).result != strata::verdict::accepted) { throw std::runtime_error("rejected"); }
					}
					ix.commit();
				}
			} catch (...) { errors[k] = std::current_exception(); }
		});
	}
	for (auto& t : pool) { t.join(); }
	for (const auto& e : errors) {
		if (e) { std::rethrow_exception(e); }
	}
	std::cerr << "build-small: " << count << " indexes of " << docs << " documents\n";
	return 0;
}

// What this process holds: resident kB (all, anonymous, file-backed), descriptors, mappings.
struct holdings {
	long rss = -1, anon = -1, file = -1, fds = -1, maps = -1;
};
holdings sample_holdings() {
	holdings h;
	if (std::ifstream status("/proc/self/status"); status) {
		for (std::string line; std::getline(status, line);) {
			auto field = [&](const char* name, long& v) {
				if (line.rfind(name, 0) == 0) { v = std::strtol(line.c_str() + std::strlen(name), nullptr, 10); }
			};
			field("VmRSS:", h.rss);
			field("RssAnon:", h.anon);
			field("RssFile:", h.file);
		}
	}
	std::error_code ec;
	for (const char* dir : {"/proc/self/fd", "/dev/fd"}) {
		long n = 0;
		for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) { ++n; }
		if (!ec) {
			h.fds = n;
			break;
		}
		ec.clear();
	}
	if (std::ifstream maps("/proc/self/maps"); maps) {
		h.maps = 0;
		for (std::string line; std::getline(maps, line);) { ++h.maps; }
	}
#ifdef __APPLE__
	// Mach: resident bytes split as anonymous (internal) and file-backed (external), and regions.
	task_vm_info_data_t vm{};
	mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &n) == KERN_SUCCESS) {
		h.rss = long(vm.resident_size / 1024);
		h.anon = long(vm.internal / 1024);
		h.file = long(vm.external / 1024);
	}
	h.maps = 0;
	for (mach_vm_address_t at = 0;;) {
		mach_vm_size_t size = 0;
		natural_t depth = 0;
		vm_region_submap_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &at, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS) { break; }
		++h.maps;
		at += size;
	}
#endif
	return h;
}

// Opens ROOT/0 .. COUNT-1 in this process and keeps them all open: each open's time, and
// per index the resident bytes, descriptors, and mappings it adds (Linux's /proc; elsewhere
// what the platform reports). The descriptor limit rises to its hard limit for this process.
//   strata_engine open-many ROOT COUNT
int open_many(const std::string& root, std::size_t count) {
	if (struct rlimit rl; ::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		::setrlimit(RLIMIT_NOFILE, &rl);
	}
	const holdings before = sample_holdings();
	std::vector<std::unique_ptr<kronuz::journal::PosixIO>> ios;
	std::vector<strata::index> open;
	ios.reserve(count);
	open.reserve(count);
	std::vector<double> us;
	strata::index::options o;
	o.key_prefix = "Q";
	using clock = std::chrono::steady_clock;
	const auto start = clock::now();
#ifdef STRATA_ENGINE_COUNT_ALLOCS
	long long io_bytes = 0, open_bytes = 0, segment_bytes = 0;
#endif
	for (std::size_t i = 0; i < count; ++i) {
		const auto t0 = clock::now();
#ifdef STRATA_ENGINE_COUNT_ALLOCS
		const long long a0 = alloc_count::live;
#endif
		ios.push_back(std::make_unique<kronuz::journal::PosixIO>(root + "/" + std::to_string(i)));
#ifdef STRATA_ENGINE_COUNT_ALLOCS
		const long long a1 = alloc_count::live;
#endif
		open.push_back(strata::index::open(*ios.back(), o));
		us.push_back(std::chrono::duration<double, std::micro>(clock::now() - t0).count());
#ifdef STRATA_ENGINE_COUNT_ALLOCS
		const long long a2 = alloc_count::live;
		io_bytes += a1 - a0;
		open_bytes += a2 - a1;
		// The segment's parsed state alone: a second strata::segment over the same bytes.
		if (const auto views = open.back().segments(); !views.empty()) {
			residency::file f = residency::file::open(root + "/" + std::to_string(i) + "/seg-" + std::to_string(views[0].id) + ".sx");
			const auto bytes = f.view({0, f.size()});
			const long long b0 = alloc_count::live;
			{
				strata::segment again(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
				segment_bytes += alloc_count::live - b0;
			}
		}
#endif
	}
#ifdef STRATA_ENGINE_COUNT_ALLOCS
	std::printf("heap per index: PosixIO %.0f B, index::open %.0f B (of it, one segment's parsed state %.0f B)\n", double(io_bytes) / double(count),
		double(open_bytes) / double(count), double(segment_bytes) / double(count));
	{
		// Pieces every open index holds, alone: its own executor and group, an empty deque, the options.
		auto bytes_of = [](auto&& make) {
			const long long b0 = alloc_count::live;
			auto held = make();
			return alloc_count::live - b0;
		};
		const long long exec = bytes_of([] {
			auto e = std::make_unique<strata::inline_executor>();
			auto g = e->open();
			return std::pair(std::move(e), std::move(g));
		});
		const long long deque = bytes_of([] { return std::make_unique<std::deque<std::string>>(); });
		const long long options = bytes_of([&] { return std::make_unique<strata::index::options>(o); });
		std::printf("pieces: inline executor and group %lld B, empty deque %lld B, options %lld B\n", exec, deque, options);
	}
#endif
	const double total = std::chrono::duration<double>(clock::now() - start).count();
	const holdings after = sample_holdings();
	std::sort(us.begin(), us.end());
	auto at = [&](double q) { return us.empty() ? 0.0 : us[std::min(us.size() - 1, std::size_t(q * double(us.size())))]; };
	double mean = 0;
	for (double v : us) { mean += v; }
	mean /= double(std::max<std::size_t>(1, us.size()));
	auto per = [&](long a, long b, double scale) { return a < 0 || b < 0 ? -1.0 : double(b - a) * scale / double(count); };
	std::size_t segments = 0;
	for (const auto& ix : open) { segments += ix.manifest().segments.size(); }  // segments() would read them
	std::printf("open-many: %zu indexes, %zu segments, %.3f s\n", count, segments, total);
	std::printf("open us: mean %.1f p50 %.1f p99 %.1f max %.1f\n", mean, at(0.5), at(0.99), us.empty() ? 0.0 : us.back());
	std::printf("per index: rss %.0f B, anon %.0f B, file %.0f B, fds %.2f, maps %.2f\n", per(before.rss, after.rss, 1024), per(before.anon, after.anon, 1024),
		per(before.file, after.file, 1024), per(before.fds, after.fds, 1), per(before.maps, after.maps, 1));
	std::printf("process: rss %ld kB, anon %ld kB, file %ld kB, fds %ld, maps %ld; sizeof(index) %zu B\n", after.rss, after.anon, after.file, after.fds, after.maps,
		sizeof(strata::index));
	// STRATA_OPEN_TOUCH=1: then each index's first use, its segments read (a search's first step).
	if (const char* t = std::getenv("STRATA_OPEN_TOUCH"); t && std::string_view(t) == "1") {
		std::vector<double> touch;
		std::uint64_t docs = 0;
		for (const auto& ix : open) {
			const auto t0 = clock::now();
			for (const auto& v : ix.segments()) { docs += v.seg->doc_count(); }
			touch.push_back(std::chrono::duration<double, std::micro>(clock::now() - t0).count());
		}
		const holdings touched = sample_holdings();
		std::sort(touch.begin(), touch.end());
		double tm = 0;
		for (double v : touch) { tm += v; }
		tm /= double(std::max<std::size_t>(1, touch.size()));
		std::printf("first use us: mean %.1f p50 %.1f p99 %.1f (%llu documents)\n", tm, touch.empty() ? 0.0 : touch[touch.size() / 2],
			touch.empty() ? 0.0 : touch[std::min(touch.size() - 1, std::size_t(0.99 * double(touch.size())))], (unsigned long long)docs);
		std::printf("per index after first use: rss %.0f B, anon %.0f B, file %.0f B, fds %.2f, maps %.2f\n", per(before.rss, touched.rss, 1024),
			per(before.anon, touched.anon, 1024), per(before.file, touched.file, 1024), per(before.fds, touched.fds, 1), per(before.maps, touched.maps, 1));
	}
	return 0;
}

// Idle indexes closed behind handles, simulated (docs/perf/small-indexes.md, F3): ACCESSES
// accesses to ROOT/0 .. N-1 drawn Zipf (s = 1, a fixed seed), each opening its index on a miss
// (closing the least recently used past CAPACITY) and looking one key up; per access timing,
// the miss rate, and the holdings at the end.
//   strata_engine cache-sim ROOT N CAPACITY ACCESSES
int cache_sim(const std::string& root, std::size_t n, std::size_t capacity, std::size_t accesses) {
	if (struct rlimit rl; ::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		::setrlimit(RLIMIT_NOFILE, &rl);
	}
	// Zipf over 1..n by inverse transform on the cumulative weights.
	std::vector<double> cdf(n);
	double sum = 0;
	for (std::size_t i = 0; i < n; ++i) { cdf[i] = sum += 1.0 / double(i + 1); }
	std::mt19937_64 rng(2026);
	std::uniform_real_distribution<double> u(0, sum);
	struct entry {
		std::unique_ptr<kronuz::journal::PosixIO> io;
		std::optional<strata::index> ix;
		std::list<std::size_t>::iterator at;  // in lru, while open
		std::string key;                      // one of its keys, read at its first open
	};
	std::vector<entry> entries(n);
	std::list<std::size_t> lru;  // most recent first
	strata::index::options o;
	o.key_prefix = "Q";
	const holdings before = sample_holdings();
	using clock = std::chrono::steady_clock;
	std::vector<double> us;
	us.reserve(accesses);
	std::size_t misses = 0, found = 0;
	for (std::size_t a = 0; a < accesses; ++a) {
		const std::size_t i = std::size_t(std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin());
		entry& e = entries[std::min(i, n - 1)];
		const auto t0 = clock::now();
		if (!e.ix) {
			++misses;
			if (lru.size() >= capacity) {
				entry& old = entries[lru.back()];
				old.ix.reset();
				old.io.reset();
				lru.pop_back();
			}
			e.io = std::make_unique<kronuz::journal::PosixIO>(root + "/" + std::to_string(&e - entries.data()));
			e.ix.emplace(strata::index::open(*e.io, o));
			lru.push_front(std::size_t(&e - entries.data()));
			e.at = lru.begin();
		} else {
			lru.splice(lru.begin(), lru, e.at);
		}
		if (e.key.empty()) {  // a key of the index, found once by scanning its first segment
			for (const auto& v : e.ix->segments()) {
				v.seg->scan_terms([&](std::string_view term, const strata::format::term_info&) {
					if (term.starts_with("Q")) { e.key = std::string(term); }
					return e.key.empty();
				});
				if (!e.key.empty()) { break; }
			}
		}
		found += e.ix->find_key(e.key).has_value();
		us.push_back(std::chrono::duration<double, std::micro>(clock::now() - t0).count());
	}
	const holdings after = sample_holdings();
	std::sort(us.begin(), us.end());
	double mean = 0;
	for (double v : us) { mean += v; }
	mean /= double(std::max<std::size_t>(1, us.size()));
	auto at = [&](double q) { return us.empty() ? 0.0 : us[std::min(us.size() - 1, std::size_t(q * double(us.size())))]; };
	std::printf("cache-sim: %zu indexes, capacity %zu, %zu accesses, %zu misses (%.1f%%), %zu keys found\n", n, capacity, accesses, misses,
		100.0 * double(misses) / double(std::max<std::size_t>(1, accesses)), found);
	std::printf("access us: mean %.1f p50 %.1f p99 %.1f\n", mean, at(0.5), at(0.99));
	std::printf("held: rss %ld kB, anon %ld kB, file %ld kB, fds %ld, maps %ld (deltas %ld kB, %ld kB, %ld kB, %ld, %ld)\n", after.rss, after.anon, after.file,
		after.fds, after.maps, after.rss - before.rss, after.anon - before.anon, after.file - before.file, after.fds - before.fds, after.maps - before.maps);
	return 0;
}

// Merges every segment into one and commits: the "after merging" index of T8. The merge
// reorders (the default); STRATA_REORDER=0 keeps docid order, and STRATA_REORDER_THREADS
// bisects on that many threads.
int merge(const std::string& dir) {
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	if (const char* r = std::getenv("STRATA_REORDER"); r && std::string_view(r) == "0") { o.reordering = strata::index::options::reorder_mode::off; }
	if (const char* t = std::getenv("STRATA_REORDER_THREADS")) { o.reorder_settings.bisection.threads = std::uint32_t(std::max(1L, std::strtol(t, nullptr, 10))); }
	auto ix = strata::index::open(io, o);
	const auto start = std::chrono::steady_clock::now();
	ix.force_merge(1);
	ix.commit();
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const bool ordered = ix.segments().size() != 1 || ix.segments()[0].seg->docid_ordered();
	std::cerr << "merged: " << ix.segments().size() << " segments, " << (ordered ? "docid order" : "reordered") << ", " << secs << " s\n";
	return 0;
}

strata::segment open(const std::string& dir, residency::file& file) {
	file = residency::file::open(dir + "/segment.sx");
	auto bytes = file.view({0, file.size()});
	return strata::segment(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

int serve(const std::string& dir) {
	std::string line;
	std::cout.setf(std::ios::unitbuf);
	if (!std::filesystem::exists(dir + "/segment.sx")) {  // an index of segments (Phase 3)
		kronuz::journal::PosixIO io(dir);
		strata::index::options o;
		o.key_prefix = "Q";
		auto ix = strata::index::open(io, o);
		std::vector<strata::segment_snapshot> segs;
		for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted, v.dead}); }
		for (LineReader in; in.next(line);) {
			const std::size_t tab = line.find('\t');
			std::cout << run(std::span<const strata::segment_snapshot>(segs), std::string_view(line.data(), tab), std::string_view(line).substr(tab + 1)) << '\n';
		}
		return 0;
	}
	residency::file file;
	strata::segment seg = open(dir, file);
	for (LineReader in; in.next(line);) {
		const std::size_t tab = line.find('\t');
		std::cout << run(seg, std::string_view(line.data(), tab), std::string_view(line).substr(tab + 1)) << '\n';
	}
	return 0;
}

int terms(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::ios::sync_with_stdio(false);
	seg.scan_terms([](std::string_view key, const strata::format::term_info& info) {
		std::cout << key << '\t' << info.df << '\n';
		return true;
	});
	return 0;
}

// Every section's kind and length in bytes, in file order, then the file's size.
int sections(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	static constexpr const char* names[] = {"header", "terms", "term_index", "postings", "positions", "lengths",
		"stored", "docids", "stats", "norms", "versions", "pk_filter"};
	for (const auto& e : seg.sections()) {
		const auto k = std::size_t(e.kind);
		std::cout << (k < std::size(names) ? names[k] : "unknown") << '\t' << e.length << '\n';
	}
	std::cout << "file\t" << file.size() << '\n';
	return 0;
}

// The term dictionary's bytes by part: raw keys and each encoded term-info field, against
// the terms and term_index sections; the difference is the dictionary's own structure
// (negative when prefix sharing saves more than its block headers cost).
int dictstats(const std::string& dir) {
	residency::file file;
	strata::segment seg = open(dir, file);
	auto leb = [](std::uint64_t v) { std::uint64_t n = 1; while (v >= 128) { v >>= 7; ++n; } return n; };
	std::uint64_t terms = 0, single = 0, absolute = 0, keys = 0, values = 0, flags = 0, df = 0, cf = 0, ordinal = 0, impacts = 0;
	std::uint64_t postings_length = 0, postings_offset = 0, positions_length = 0, positions_offset = 0;
	strata::format::term_run run;
	seg.dictionary().scan("", [&](std::string_view key, std::string_view value) {
		const auto t = strata::format::decode_term_info(value, run);
		const bool abs = t.flags & strata::format::absolute;
		// The cursors an absolute entry stored: where its own bytes start.
		strata::format::term_run run_before;
		run_before.postings = (t.flags & strata::format::single_posting) ? run.postings : t.postings;
		run_before.positions = (t.flags & strata::format::no_positions) ? run.positions : t.positions;
		++terms;
		absolute += abs;
		keys += key.size();
		values += value.size();
		df += leb(std::uint64_t(t.df) << 5);
		if (!(t.flags & strata::format::cf_is_df)) { cf += leb(t.flags & strata::format::single_posting ? t.single_wdf : t.cf); }
		if (abs) {
			postings_offset += leb(run_before.postings);
			positions_offset += leb(run_before.positions);
		}
		if (t.flags & strata::format::single_posting) {
			++single;
			ordinal += leb(t.postings);
		} else {
			postings_length += leb(t.postings_length);
		}
		if (!(t.flags & strata::format::no_positions)) { positions_length += leb(t.positions_length); }
		if (t.df >= strata::format::block) {
			std::string imp;
			strata::format::encode_impacts(imp, t.impacts);
			impacts += imp.size();
		}
		return true;
	});
	std::uint64_t section_terms = 0, section_index = 0;
	for (const auto& e : seg.sections()) {
		if (e.kind == strata::format::section::terms) { section_terms = e.length; }
		if (e.kind == strata::format::section::term_index) { section_index = e.length; }
	}
	auto row = [&](const char* name, std::uint64_t bytes) { std::printf("%-36s %14llu %8.2f B/term\n", name, (unsigned long long)bytes, double(bytes) / double(terms)); };
	std::printf("terms %llu, single-posting %llu, absolute offsets %llu\n", (unsigned long long)terms, (unsigned long long)single, (unsigned long long)absolute);
	row("keys (raw)", keys);
	row("value: df and flags", df);
	row("value: cf, or a single's wdf, unless 1", cf);
	row("value: single ordinal", ordinal);
	row("value: postings length", postings_length);
	row("value: postings cursor (absolute)", postings_offset);
	row("value: positions length", positions_length);
	row("value: positions cursor (absolute)", positions_offset);
	row("value: competitive pairs (df >= 128)", impacts);
	row("values total", values);
	row("terms section", section_terms);
	row("term_index section", section_index);
	std::printf("%-36s %14lld %8.2f B/term\n", "structure (terms - keys - values)", (long long)section_terms - (long long)(keys + values), (double(section_terms) - double(keys + values)) / double(terms));
	return values == flags + df + cf + ordinal + postings_length + postings_offset + positions_length + positions_offset + impacts ? 0 : 1;
}

// Dictionary experiments without reindexing: dictdump writes a segment's terms and
// infos to a file with this binary's reader; dictsim rebuilds the dictionary from that
// file with the termdict and term-info encoding the (possibly different) binary was built
// with, and prints its size.
int dictdump(const std::string& dir, const std::string& path) {
	residency::file file;
	strata::segment seg = open(dir, file);
	std::FILE* out = std::fopen(path.c_str(), "wb");
	if (!out) { return 1; }
	auto u64 = [&](std::uint64_t v) { std::fwrite(&v, 8, 1, out); };
	seg.scan_terms([&](std::string_view key, const strata::format::term_info& t) {
		u64(key.size());
		std::fwrite(key.data(), 1, key.size(), out);
		for (std::uint64_t v : {std::uint64_t(t.flags), std::uint64_t(t.df), t.cf, t.postings, t.positions, std::uint64_t(t.single_wdf), t.postings_length, t.positions_length,
				 std::uint64_t(t.impacts.size())}) {
			u64(v);
		}
		for (const auto& i : t.impacts) { u64(std::uint64_t(i.wdf) << 8 | i.length); }
		return true;
	});
	std::fclose(out);
	return 0;
}

int dictsim(const std::string& path) {
	std::FILE* in = std::fopen(path.c_str(), "rb");
	if (!in) { return 1; }
	auto u64 = [&] {
		std::uint64_t v = 0;
		return std::fread(&v, 8, 1, in) == 1 ? std::optional<std::uint64_t>(v) : std::nullopt;
	};
	// A first pass over the keys for the dictionary's alphabet.
	std::array<bool, 256> used{};
	std::string key;
	while (auto n = u64()) {
		key.resize(*n);
		if (std::fread(key.data(), 1, *n, in) != *n) { break; }
		for (char c : key) { used[std::uint8_t(c)] = true; }
		std::uint64_t skip[9];
		if (std::fread(skip, 8, 9, in) != 9) { break; }
		std::fseek(in, long(8 * skip[8]), SEEK_CUR);
	}
	std::rewind(in);
	std::string alphabet;
	for (unsigned c = 0; c < 256; ++c) {
		if (used[c]) { alphabet.push_back(char(c)); }
	}
	termdict::builder b(alphabet);
	strata::format::term_run run;
	std::uint64_t terms = 0;
	while (auto n = u64()) {
		key.resize(*n);
		if (std::fread(key.data(), 1, *n, in) != *n) { break; }
		strata::format::term_info t;
		t.flags = std::uint8_t(*u64());
		t.df = std::uint32_t(*u64());
		t.cf = *u64();
		t.postings = *u64();
		t.positions = *u64();
		t.single_wdf = std::uint32_t(*u64());
		t.postings_length = *u64();
		t.positions_length = *u64();
		for (std::uint64_t k = *u64(); k; --k) {
			const std::uint64_t v = *u64();
			t.impacts.push_back({std::uint32_t(v >> 8), std::uint8_t(v)});
		}
		b.add(key, strata::format::encode_term_info(t, run, b.next_is_restart()));
		++terms;
	}
	std::fclose(in);
	const auto tb = b.finish();
	std::printf("terms %llu: blocks %zu + index %zu = %zu bytes, %.2f B/term (restart interval %zu, blocks of %zu-%zu)\n", (unsigned long long)terms, tb.blocks.size(), tb.index.size(),
		tb.blocks.size() + tb.index.size(), double(tb.blocks.size() + tb.index.size()) / double(terms), termdict::restart_interval, termdict::min_block_entries, termdict::max_block_entries);
	return 0;
}

std::string query_of(const std::string& line) {
	auto at = line.find("\"query\": \"");
	if (at == std::string::npos) { return {}; }
	std::string q;
	for (std::size_t i = at + 10; i < line.size() && line[i] != '"'; ++i) { q.push_back(line[i] == '\\' ? line[++i] : line[i]); }
	return q;
}

// For every query, under both scoring schemes: the pruned top-k against a scan that
// scores every match, and the counts. Scores agree within 1e-9 relative, rank by rank;
// ordinals agree exactly, except that each run of hits whose scores agree within 1e-9
// must be drawn from the scan's documents with that score (ties compare as sets, since
// summation order can perturb near-equal scores).
// verify over an index's segments (T8, C14's form): for each query and both scoring
// schemes, an exhaustive scan of every live document with the snapshot's statistics,
// against the pruned multi-segment search at k = 10, 100, and 1000 and its count. Ties at
// a score may resolve to any of the tied documents.
int verify_index(const std::string& dir) {
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	auto ix = strata::index::open(io, o);
	std::vector<strata::segment_snapshot> segs;
	for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted, v.dead}); }
	std::span<const strata::segment_snapshot> view(segs);
	std::size_t queries = 0, mismatches = 0;
	auto close = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(a, b)); };
	LineReader in; for (std::string line; in.next(line);) {
		const std::string q = query_of(line);
		if (q.empty()) { continue; }
		++queries;
		const parsed p = parse(q);
		for (const auto scheme : {strata::scheme::bm25, strata::scheme::bm25_xapian}) {
			strata::term_table table(view);
			auto builder = [&](const strata::segment& seg) { return build(seg, p, scheme, &table); };
			std::vector<strata::doc_hit> scan;
			for (const auto& sv : segs) {
				auto all = builder(*sv.seg);
				if (!all) { continue; }
				for (auto d = all->next(); d != strata::no_more_docs; d = all->next()) {
					if (sv.deleted && ((*sv.deleted)[d >> 6] >> (d & 63)) & 1) { continue; }
					scan.push_back({sv.seg->docid(d), all->score()});
				}
			}
			std::sort(scan.begin(), scan.end(), [](const auto& a, const auto& b) { return a.score != b.score ? a.score > b.score : a.docid < b.docid; });
			bool ok = strata::search_count(view, builder) == scan.size();
			for (std::size_t k : {10u, 100u, 1000u}) {
				std::uint64_t counted = 0;
				const auto hits = strata::search(view, builder, k, &counted);
				const std::size_t n = std::min<std::size_t>(k, scan.size());
				ok = ok && counted == scan.size() && hits.size() == n;
				for (std::size_t a = 0; ok && a < n;) {
					std::size_t b = a + 1;
					while (b < n && close(scan[b].score, scan[a].score)) { ++b; }
					std::vector<std::uint64_t> tied, got;
					for (std::size_t i = a; i < scan.size() && close(scan[i].score, scan[a].score); ++i) { tied.push_back(scan[i].docid); }
					for (std::size_t i = a; i > 0 && close(scan[i - 1].score, scan[a].score); --i) { tied.push_back(scan[i - 1].docid); }
					for (std::size_t i = a; i < b; ++i) { ok = ok && close(hits[i].score, scan[i].score); got.push_back(hits[i].docid); }
					std::sort(tied.begin(), tied.end()); std::sort(got.begin(), got.end());
					ok = ok && std::adjacent_find(got.begin(), got.end()) == got.end() && std::includes(tied.begin(), tied.end(), got.begin(), got.end());
					a = b;
				}
				if (!ok) {
					++mismatches;
					std::cout << "MISMATCH " << (scheme == strata::scheme::bm25 ? "bm25" : "bm25_xapian") << " k=" << k << '\t' << q << '\n';
					break;
				}
			}
		}
	}
	std::cout << queries << " queries, " << mismatches << " mismatches over " << segs.size() << " segments\n";
	return mismatches ? 1 : 0;
}

// Every query's top k at k = 10, 100, and 1,000 under both schemes, as docids and exact
// scores (%a), with the live count: two indexes of the same documents print the same lines
// whatever their segments' order (reordering.md D5, the scale check).
int dump(const std::string& dir) {
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	auto ix = strata::index::open(io, o);
	std::vector<strata::segment_snapshot> segs;
	for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted, v.dead}); }
	LineReader in;
	for (std::string line; in.next(line);) {
		const std::string q = query_of(line);
		if (q.empty()) { continue; }
		const parsed p = parse(q);
		for (const auto scheme : {strata::scheme::bm25, strata::scheme::bm25_xapian}) {
			for (std::size_t k : {10u, 100u, 1000u}) {
				std::uint64_t n = 0;
				const auto hits = search_snapshot(segs, p, k, scheme, &n);
				std::printf("%s\t%d\t%zu\t%llu", q.c_str(), int(scheme), k, (unsigned long long)n);
				for (const auto& h : hits) { std::printf(" %llu:%a", (unsigned long long)h.docid, h.score); }
				std::printf("\n");
			}
		}
	}
	return 0;
}

int verify(const std::string& dir) {
	if (!std::filesystem::exists(dir + "/segment.sx")) { return verify_index(dir); }
	residency::file file;
	strata::segment seg = open(dir, file);
	std::size_t queries = 0, mismatches = 0;
	auto close = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(a, b)); };
	LineReader in; for (std::string line; in.next(line);) {
		const std::string q = query_of(line);
		if (q.empty()) { continue; }
		++queries;
		const parsed p = parse(q);
		for (const auto scheme : {strata::scheme::bm25, strata::scheme::bm25_xapian}) {
			auto all = build(seg, p, scheme);
			if (!all) { break; }
			std::vector<strata::hit> scan;
			for (auto d = all->next(); d != strata::no_more_docs; d = all->next()) { scan.push_back({d, all->score()}); }
			std::sort(scan.begin(), scan.end(), [](const auto& a, const auto& b) { return a.score != b.score ? a.score > b.score : a.ord < b.ord; });
			auto counted = build(seg, p, scheme);
			bool ok = strata::count(*counted) == scan.size();
			for (std::size_t k : {10u, 100u, 1000u}) {
				auto pruned = build(seg, p, scheme);
				const auto hits = strata::top_k(seg, *pruned, k);
				const std::size_t n = std::min<std::size_t>(k, scan.size());
				ok = ok && hits.size() == n;
				for (std::size_t a = 0; ok && a < n;) {
					std::size_t b = a + 1;
					while (b < n && close(scan[b].score, scan[a].score)) { ++b; }
					// Every scan document with this score, beyond the cut at k too.
					std::vector<strata::ordinal_t> tied;
					for (std::size_t i = a; i < scan.size() && close(scan[i].score, scan[a].score); ++i) { tied.push_back(scan[i].ord); }
					for (std::size_t i = a; i > 0 && close(scan[i - 1].score, scan[a].score); --i) { tied.push_back(scan[i - 1].ord); }
					std::sort(tied.begin(), tied.end());
					std::vector<strata::ordinal_t> got;
					for (std::size_t i = a; i < b; ++i) {
						ok = ok && close(hits[i].score, scan[i].score);
						got.push_back(hits[i].ord);
					}
					std::sort(got.begin(), got.end());
					ok = ok && std::adjacent_find(got.begin(), got.end()) == got.end() && std::includes(tied.begin(), tied.end(), got.begin(), got.end());
					a = b;
				}
				if (!ok) {
					++mismatches;
					std::cout << "MISMATCH " << (scheme == strata::scheme::bm25 ? "bm25" : "bm25_xapian") << " k=" << k << '\t' << q << '\n';
					break;
				}
			}
		}
	}
	std::cout << queries << " queries, " << mismatches << " mismatches\n";
	return mismatches ? 1 : 0;
}

// Times each query of queries.txt (the harness's JSON lines: {"query": "...", "tags": [...]})
// with answer(query): the fastest of runs, in µs.
template <class Answer>
int bench_queries(int runs, Answer&& answer) {
	LineReader in; for (std::string line; in.next(line);) {
		auto at = line.find("\"query\": \"");
		if (at == std::string::npos) { continue; }
		std::string unescaped;
		for (std::size_t i = at + 10; i < line.size() && line[i] != '"'; ++i) { unescaped.push_back(line[i] == '\\' ? line[++i] : line[i]); }
		double best = 1e300;
		std::uint64_t result = 0;
		for (int r = 0; r < runs; ++r) {
			auto t0 = std::chrono::steady_clock::now();
			result = answer(std::string_view(unescaped));
			best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
		std::cout << std::fixed << std::setprecision(1) << best << '\t' << result << '\t' << unescaped << '\n';
	}
	return 0;
}

// A segment file (segment.sx) is searched alone; an index directory through the
// snapshot of its segments, as serve answers, so a multi-segment index and its merged
// form time alike.
int bench(const std::string& dir, std::string_view command, int runs, strata::scheme scheme) {
	if (!std::filesystem::exists(dir + "/segment.sx")) {
		kronuz::journal::PosixIO io(dir);
		strata::index::options o;
		o.key_prefix = "Q";
		auto ix = strata::index::open(io, o);
		std::vector<strata::segment_snapshot> segs;
		for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted, v.dead}); }
		std::cerr << "bench: " << segs.size() << " segments\n";
		const std::span<const strata::segment_snapshot> view(segs);
		return bench_queries(runs, [&](std::string_view query) { return run(view, command, query, scheme); });
	}
	residency::file file;
	strata::segment seg = open(dir, file);
	return bench_queries(runs, [&](std::string_view query) { return run(seg, command, query, scheme); });
}

}  // namespace

// Bytes of each section in the page cache (mincore over a fresh mapping): run after
// serving, without dropping caches, to attribute serving memory by section.
int resident(const std::string& dir) {
	const std::string path = dir + "/segment.sx";
	const int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) { return 1; }
	struct stat st;
	::fstat(fd, &st);
	const std::size_t size = std::size_t(st.st_size), page = std::size_t(::sysconf(_SC_PAGESIZE));
	void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
	::close(fd);
	if (map == MAP_FAILED) { return 1; }
	std::vector<unsigned char> in((size + page - 1) / page);
	auto core = [&]<class A, class T>(int (*f)(A, std::size_t, T*)) { return f(map, size, reinterpret_cast<T*>(in.data())); };  // char* on macOS
	if (core(&::mincore) != 0) { return 1; }
	const std::string_view bytes(static_cast<const char*>(map), size);
	const auto toc = strata::format::decode_toc(bytes, strata::format::decode_footer(bytes));
	const char* names[] = {"header", "terms", "term_index", "postings", "positions", "lengths", "stored", "docids", "stats", "norms", "versions", "pk_filter"};
	std::uint64_t total = 0;
	for (const auto& e : toc) {
		std::uint64_t held = 0;  // the section's bytes on resident pages
		for (std::uint64_t p = e.offset / page; p * page < e.offset + e.length; ++p) {
			if (in[p] & 1) { held += std::min<std::uint64_t>((p + 1) * page, e.offset + e.length) - std::max<std::uint64_t>(p * page, e.offset); }
		}
		total += held;
		const auto k = std::size_t(e.kind);
		std::printf("%-12s %12llu bytes %10.1f MB resident (%.1f%%)\n", k < std::size(names) ? names[k] : "?", (unsigned long long)e.length, double(held) / 1e6,
			e.length ? 100.0 * double(held) / double(e.length) : 0.0);
	}
	std::printf("total %.1f MB resident of %.1f MB\n", double(total) / 1e6, double(size) / 1e6);
	::munmap(map, size);
	return 0;
}

// A document of whitespace tokens: each distinct token with its wdf and positions, the
// key term (wdf 0), and the id stored.

int churn(const std::string& dir) {
	std::ios::sync_with_stdio(false);
	std::filesystem::create_directories(dir);
	// PosixIO refuses a directory others can write; a umask of 002 leaves group write on.
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	kronuz::journal::PosixIO io(dir);
	strata::index::options o;
	o.key_prefix = "Q";
	o.auto_merge = true;
	const std::array<char, 16> uuid{'s', 't', 'r', 'a', 't', 'a', '-', 's', 'o', 'a', 'k'};
	auto ix = strata::index::create(io, o, uuid);
	using clock = std::chrono::steady_clock;
	auto us = [](clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t).count(); };
	strata::transaction t;
	auto apply = [&] {
		if (t.ops.empty()) { return; }
		if (ix.apply(t, {}).result != strata::verdict::accepted) { std::cerr << "rejected\n"; std::exit(1); }
		t.ops.clear();
	};
	std::string line;
	for (LineReader in; in.next(line);) {
		const std::string_view l = line;
		if (l.empty()) { continue; }
		const std::size_t a = l.find('\t'), b = a == std::string_view::npos ? a : l.find('\t', a + 1);
		switch (l[0]) {
		case 'P': {
			strata::operation op;
			op.type = strata::operation::replace_key;
			op.key = "Q" + std::string(l.substr(a + 1, b - a - 1));
			fill_document(op.doc, op.key, l.substr(a + 1, b - a - 1), l.substr(b + 1));
			t.ops.push_back(std::move(op));
			if (t.ops.size() == 1000) { apply(); }
			break;
		}
		case 'D': {
			strata::operation op;
			op.type = strata::operation::delete_key;
			op.key = "Q" + std::string(l.substr(a + 1));
			t.ops.push_back(std::move(op));
			if (t.ops.size() == 1000) { apply(); }
			break;
		}
		case 'C': {
			apply();
			const auto start = clock::now();
			ix.commit();
			std::cout << "C\t" << us(start) << '\t' << ix.segments().size() << std::endl;
			break;
		}
		case 'Q': {
			const std::string_view command = l.substr(a + 1, b - a - 1), query = l.substr(b + 1);
			const auto start = clock::now();
			std::vector<strata::segment_snapshot> segs;
			for (const auto& v : ix.segments()) { segs.push_back({v.seg, v.deleted, v.dead}); }
			// As serve answers it: weights from the snapshot's statistics, not each segment's.
			const std::uint64_t result = run(std::span<const strata::segment_snapshot>(segs), command, query);
			std::cout << "Q\t" << us(start) << '\t' << result << std::endl;
			break;
		}
		}
	}
	apply();
	ix.commit();
	std::cout.flush();
	return 0;
}

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "index" && argc == 3) { return index(argv[2]); }
	if (mode == "build-small" && (argc == 5 || argc == 6)) {
		return build_small(argv[2], std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 10), argc == 6 ? std::strtoull(argv[5], nullptr, 10) : 0);
	}
	if (mode == "open-many" && argc == 4) { return open_many(argv[2], std::strtoull(argv[3], nullptr, 10)); }
	if (mode == "cache-sim" && argc == 6) {
		return cache_sim(argv[2], std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 10), std::strtoull(argv[5], nullptr, 10));
	}
	if (mode == "merge" && argc == 3) { return merge(argv[2]); }
	if (mode == "serve" && argc == 3) { return serve(argv[2]); }
	if (mode == "terms" && argc == 3) { return terms(argv[2]); }
	if (mode == "sections" && argc == 3) { return sections(argv[2]); }
	if (mode == "dictstats" && argc == 3) { return dictstats(argv[2]); }
	if (mode == "dictdump" && argc == 4) { return dictdump(argv[2], argv[3]); }
	if (mode == "check" && argc == 3) {
		residency::file file;
		strata::segment seg = open(argv[2], file);
		strata::check_report r;
		strata::check_segment(seg, nullptr, r);
		std::printf("ok: %llu documents, %llu terms, %llu postings, %llu positions, %llu stored bytes\n", (unsigned long long)r.documents, (unsigned long long)r.terms,
			(unsigned long long)r.postings, (unsigned long long)r.positions, (unsigned long long)r.stored_bytes);
		return 0;
	}
	if (mode == "resident" && argc == 3) { return resident(argv[2]); }
	if (mode == "dictsim" && argc == 3) { return dictsim(argv[2]); }
	if (mode == "verify" && argc == 3) { return verify(argv[2]); }
	if (mode == "dump" && argc == 3) { return dump(argv[2]); }
	if (mode == "churn" && argc == 3) { return churn(argv[2]); }
	if (mode == "bench" && (argc == 5 || argc == 6)) {
		const bool xapian = argc == 6 && std::string_view(argv[5]) == "bm25_xapian";
		return bench(argv[2], argv[3], std::atoi(argv[4]), xapian ? strata::scheme::bm25_xapian : strata::scheme::bm25);
	}
	std::cerr << "usage: strata_engine index|serve|terms|dictstats|verify|dump|check|resident|churn DIR | bench DIR COMMAND RUNS < queries\n";
	return 2;
}
