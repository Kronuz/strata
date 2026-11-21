// What one open index costs on Xapiand's open path, eager against lazy (docs/perf/eager-open.md).
//   eager_open build ROOT COUNT DOCS SEGMENTS [corrupt]
//       COUNT clones of one search-layout index of DOCS Xapiand-shaped documents in SEGMENTS
//       commits; `corrupt` resealed with its manifest's last field dropped, so one segment names a
//       field id outside the table (what Xapiand's verify_fields refuses).
//   eager_open open ROOT COUNT ARM [query] [repeat N]
//       opens them on one thread, as Xapiand's catalog does, and prints the meters per index:
//       today  defer_io (so eager segments), then Xapiand's Core::verify_fields
//       eager  defer_io, no check of the host's
//       lazy   no defer_io, eager_segments off, no check of the host's
//       warm   today, then demote()
//       `query` then runs a key get and a term query per index (the first maps a lazy index).
//   eager_open identity ROOT COUNT ARM
//       per index: open, the digest, a key get and the term query; prints one hash of them all, or
//       the refusal and where it came (open or query).
#include "prospect/index.hh"
#include "prospect/searcher.hh"
#include "strata/core/budget.hh"
#include "strata/core/hazard.hh"

#include "completion/posix.h"
#include "../idle_index/meters.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/clonefile.h>
#endif
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "../idle_index/heap_count.hh"

namespace fs = std::filesystem;
using namespace idle_bench;
using field_part = prospect::inverted_plugin::field_part;

namespace {

enum class arm { today, eager, lazy, warm };
arm parse_arm(const std::string& s) {
	if (s == "today") { return arm::today; }
	if (s == "eager") { return arm::eager; }
	if (s == "lazy") { return arm::lazy; }
	if (s == "warm") { return arm::warm; }
	std::fprintf(stderr, "arm: today, eager, lazy or warm\n");
	std::exit(2);
}

// Xapiand's host options (src/engine/strata/shard.cc index_options), auto_merge off so the fixture
// keeps its segments.
prospect::index::options options_of(arm a, strata::byte_budget* b, strata::reader_registry* r) {
	prospect::index::options o;
	o.keyed = true;
	o.auto_merge = false;
	o.defer_io = a != arm::lazy;
	o.eager_segments = a != arm::lazy;
	o.budget = b;
	o.readers = r;
	return o;
}

// Xapiand's Core::verify_fields (src/engine/strata/shard.cc).
void verify_fields(const prospect::index& ix) {
	const auto& table = ix.fields();
	for (const auto& view : ix.segments()) { view.seg()->get<prospect::inverted_plugin>().dictionary().check_fields(table.size(), table.key_field()); }
}

// --- build ------------------------------------------------------------------------------------

constexpr std::uint32_t vocabulary = 5000;
// Fields as a Xapiand schema makes them: text with positions, keywords, booleans, the key last but one.
std::vector<strata::field_declaration> declarations(bool corrupt) {
	using strata::field_postings;
	using strata::field_role;
	std::vector<strata::field_declaration> d = {{"title", field_role::indexed, field_postings::positions, false},
		{"body", field_role::indexed, field_postings::positions, false}, {"tags", field_role::indexed, field_postings::docs, false},
		{"lang", field_role::indexed, field_postings::docs, false}, {"status", field_role::indexed, field_postings::docs, false},
		{"author", field_role::indexed, field_postings::freqs, false}, {"date", field_role::indexed, field_postings::docs, false}};
	for (int i = 0; i < 6; ++i) { d.push_back({"flag" + std::to_string(i), field_role::indexed, field_postings::docs, false}); }
	d.push_back({"_id", field_role::key, field_postings::docs, false});
	if (corrupt) { d.push_back({"zz", field_role::indexed, field_postings::docs, false}); }
	return d;
}

std::string word(std::uint32_t r) { return "w" + std::to_string(r); }

// A text part: `n` words drawn log-uniformly from the vocabulary, positions from 1.
field_part text(std::uint32_t f, std::uint32_t n, std::mt19937_64& g) {
	std::map<std::string, std::vector<std::uint32_t>> at;
	std::uniform_real_distribution<double> u(0, 1);
	for (std::uint32_t p = 1; p <= n; ++p) { at[word(std::uint32_t(std::pow(double(vocabulary), u(g))) - 1)].push_back(p); }
	field_part fp;
	fp.field = strata::field_ref::declared(f);
	for (auto& [t, ps] : at) { fp.terms.push_back({t, std::uint32_t(ps.size()), std::move(ps)}); }
	fp.length = n;
	return fp;
}
field_part keyword(std::uint32_t f, std::vector<std::string> ts) {
	std::sort(ts.begin(), ts.end());
	ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
	field_part fp;
	fp.field = strata::field_ref::declared(f);
	for (auto& t : ts) { fp.terms.push_back({std::move(t), 1, {}}); }
	fp.length = std::uint32_t(fp.terms.size());
	return fp;
}

prospect::operation document(std::uint64_t d, bool corrupt, std::mt19937_64& g) {
	prospect::operation o;
	o.type = prospect::operation::replace_key;
	o.key = key_of(d);
	o.doc.key = o.key;
	auto& f = o.doc.fields;
	f.push_back(text(0, 6, g));
	f.push_back(text(1, 40, g));
	f.push_back(keyword(2, {"t" + std::to_string(g() % 50), "t" + std::to_string(g() % 50), "t" + std::to_string(g() % 50)}));
	f.push_back(keyword(3, {"l" + std::to_string(g() % 5)}));
	f.push_back(keyword(4, {"s" + std::to_string(g() % 3)}));
	f.push_back(keyword(5, {"a" + std::to_string(g() % 200)}));
	f.push_back(keyword(6, {"d" + std::to_string(g() % 365)}));
	for (std::uint32_t i = 0; i < 6; ++i) { f.push_back(keyword(7 + i, {std::to_string(g() % (2 + i))})); }
	if (corrupt && d == 0) { f.push_back(keyword(14, {"z"})); }
	std::uint32_t length = 0;
	for (const auto& p : f) { length += p.length; }
	o.doc.length = length;
	std::string stored(200, '\0');
	for (auto& c : stored) { c = char('a' + g() % 26); }
	o.doc.stored = std::move(stored);
	o.doc.values.push_back({0, std::to_string(g() % 100000)});
	o.doc.values.push_back({1, std::to_string(d)});
	return o;
}

void clone_file(const fs::path& from, const fs::path& to) {
#ifdef __APPLE__
	if (::clonefile(from.c_str(), to.c_str(), 0) == 0) { return; }
#elif defined(__linux__)
	{
		const int s = ::open(from.c_str(), O_RDONLY);
		const int d = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (s >= 0 && d >= 0 && ::ioctl(d, FICLONE, s) == 0) { ::close(s); ::close(d); return; }
		if (s >= 0) { ::close(s); }
		if (d >= 0) { ::close(d); }
		std::error_code ec;
		fs::remove(to, ec);
	}
#endif
	fs::copy_file(from, to);
}

// The head state resealed with its last field dropped (test/index_test.cc's reseal).
void drop_last_field(const fs::path& dir) {
	kronuz::journal::PosixIO io(dir, 0600, kronuz::journal::PosixIOOptions{true, true});
	auto m = strata::index_detail::read_state(io);
	m.fields.truncate(m.fields.size() - 1);
	strata::files::recount(m);
	const std::string name = strata::index_detail::manifest_name(m.generation);
	try {
		io.remove(name);
	} catch (const std::exception&) {
	}
	auto f = io.create_exclusive(name);
	strata::index_detail::write_all(*f, strata::files::frame_snapshot(m));
	f->sync();
}

int build(const fs::path& root, std::uint64_t count, std::uint64_t docs, std::uint64_t segments, bool corrupt) {
	fs::create_directories(root);
	const fs::path tmpl = root / "template";
	fs::remove_all(tmpl);
	fs::create_directories(tmpl);
	{
		kronuz::journal::PosixIO io(tmpl, 0600, kronuz::journal::PosixIOOptions{true, true});
		auto o = options_of(arm::lazy, nullptr, nullptr);
		auto ix = prospect::index::create(io, o, std::array<char, 16>{'e', 'a', 'g', 'e', 'r'});
		std::mt19937_64 g(42);
		const std::uint64_t per = (docs + segments - 1) / segments;
		prospect::transaction tx;
		tx.fields = declarations(corrupt);
		for (std::uint64_t d = 0; d < docs; ++d) {
			tx.ops.push_back(document(d, corrupt, g));
			if (tx.ops.size() == 100 || (d + 1) % per == 0 || d + 1 == docs) {
				if (ix.apply(tx, {}).result != strata::verdict::accepted) { std::fprintf(stderr, "refused at %llu\n", (unsigned long long)d); std::abort(); }
				tx.ops.clear();
			}
			if ((d + 1) % per == 0 || d + 1 == docs) { ix.commit(); }
		}
		std::printf("template: %zu segments, %zu fields\n", ix.segments().size(), ix.fields().size());
	}
	if (corrupt) { drop_last_field(tmpl); }
	std::vector<fs::path> files;
	std::uintmax_t bytes = 0;
	for (const auto& e : fs::directory_iterator(tmpl)) {
		if (e.is_regular_file()) { files.push_back(e.path().filename()); bytes += e.file_size(); }
	}
	std::printf("template: %zu files, %ju bytes\n", files.size(), bytes);
	const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
	std::atomic<std::uint64_t> next{0};
	std::vector<std::thread> ts;
	const auto t0 = clk::now();
	for (unsigned t = 0; t < nt; ++t) {
		ts.emplace_back([&] {
			for (;;) {
				const std::uint64_t i = next.fetch_add(1);
				if (i >= count) { return; }
				const fs::path d = dir_of(root, i);
				if (i % 1000 == 0) { fs::create_directories(d.parent_path()); }
				else { while (!fs::exists(d.parent_path())) { std::this_thread::yield(); } }
				fs::create_directory(d);
				for (const auto& f : files) { clone_file(tmpl / f, d / f); }
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	std::printf("built %llu indexes in %.1f s\n", (unsigned long long)count, us(t0, clk::now()) / 1e6);
	return 0;
}

// --- open -------------------------------------------------------------------------------------

struct slot {
	std::unique_ptr<kronuz::journal::PosixIO> io;
	std::unique_ptr<prospect::index> ix;
	prospect::index::reader_handle rd;
};

void raise_nofile() {
	struct rlimit rl{};
	getrlimit(RLIMIT_NOFILE, &rl);
	rlim_t want = rl.rlim_max == RLIM_INFINITY ? rlim_t(10000000) : rl.rlim_max;
#ifdef __APPLE__
	want = std::min<rlim_t>(want, 1048575);
#endif
	rl.rlim_cur = want;
	(void)::setrlimit(RLIMIT_NOFILE, &rl);
}

// Opens one index as the arm says.
void open_one(slot& s, const fs::path& dir, arm a, strata::byte_budget* b, strata::reader_registry* r) {
	s.io = std::make_unique<kronuz::journal::PosixIO>(dir, 0600, kronuz::journal::PosixIOOptions{true, true});
	s.ix = std::make_unique<prospect::index>(prospect::index::open(*s.io, options_of(a, b, r)));
	if (a == arm::today || a == arm::warm) { verify_fields(*s.ix); }
	s.rd = s.ix->reader();
	if (a == arm::warm) { (void)s.ix->demote(); }
}

// A key get and a term query on the shared reader; returns a hash of what they read.
std::uint64_t query_one(const slot& s, std::uint64_t docs_hint) {
	auto g = s.rd.acquire();
	std::uint64_t h = 1469598103934665603ull;
	auto mix = [&](std::uint64_t x) { h = (h ^ x) * 1099511628211ull; };
	const auto got = g->get(key_of(docs_hint / 2));
	mix(got ? std::hash<std::string>{}(*got) : 7);
	const auto body = g->fields().find("body");
	for (const auto& sv : g->segments()) {
		const auto& inv = sv.seg()->get<prospect::inverted_plugin>();
		const auto info = body ? inv.find_term(*body, "w3") : std::nullopt;
		if (!info) { continue; }
		prospect::postings_iterator it(inv, *info);
		for (auto o = it.next(); o != prospect::no_more_docs; o = it.next()) {
			if (sv.live(o)) { mix(sv.id << 32 | o); }
		}
	}
	return h;
}

int open_many(const fs::path& root, std::uint64_t count, arm a, bool query, std::uint64_t docs_hint) {
	raise_nofile();
	strata::byte_budget budget(std::uint64_t(64) << 30);
	strata::reader_registry registry(64);
	std::vector<slot> slots(count);
	std::vector<double> t_open, t_q1, t_q2, t_close;
	const meters m0 = take();
#ifdef IDLE_COUNT
	const std::size_t b0 = live_bytes, c0 = live_count;
#endif
	const auto topen = clk::now();
	for (std::uint64_t i = 0; i < count; ++i) {
		const auto t = clk::now();
		open_one(slots[i], dir_of(root, i), a, &budget, &registry);
		t_open.push_back(us(t, clk::now()));
	}
	const double open_total = us(topen, clk::now()) / 1e6;
	const meters m1 = take();
	const double n = double(count);
	std::printf("opened %llu in %.3f s\n", (unsigned long long)count, open_total);
	report("after open", m0, m1, n);
#ifdef IDLE_COUNT
	std::printf("heap after open: %.1f B/index in %.2f allocations\n", double(live_bytes - b0) / n, double(live_count - c0) / n);
#endif
	print_pct("open", summarize(t_open));
	std::printf("open cpu %.4f s (%.1f us/index)\n", m1.cpu - m0.cpu, (m1.cpu - m0.cpu) / n * 1e6);
	if (query) {
		std::uint64_t h = 0;
		for (int round = 0; round < 2; ++round) {
			auto& out = round ? t_q2 : t_q1;
			for (std::uint64_t i = 0; i < count; ++i) {
				const auto t = clk::now();
				h += query_one(slots[i], docs_hint);
				out.push_back(us(t, clk::now()));
			}
			const meters mq = take();
			report(round ? "after query 2" : "after query 1", m0, mq, n);
#ifdef IDLE_COUNT
			std::printf("heap after query %d: %.1f B/index in %.2f allocations\n", round + 1, double(live_bytes - b0) / n, double(live_count - c0) / n);
#endif
		}
		print_pct("first query", summarize(t_q1));
		print_pct("second query", summarize(t_q2));
		std::printf("query hash %016llx\n", (unsigned long long)h);
	}
	const auto tclose = clk::now();
	for (auto& s : slots) {
		const auto t = clk::now();
		s.rd = {};
		s.ix.reset();
		s.io.reset();
		t_close.push_back(us(t, clk::now()));
	}
	print_pct("close", summarize(t_close));
	std::printf("closed in %.3f s\n", us(tclose, clk::now()) / 1e6);
	return 0;
}

// The results and refusals of each index under an arm: open, digest, key get, term query.
int identity(const fs::path& root, std::uint64_t count, arm a, std::uint64_t docs_hint) {
	raise_nofile();
	strata::byte_budget budget(std::uint64_t(64) << 30);
	strata::reader_registry registry(64);
	std::uint64_t h = 0, refused_open = 0, refused_query = 0;
	std::map<std::string, std::uint64_t> whats;
	for (std::uint64_t i = 0; i < count; ++i) {
		slot s;
		try {
			open_one(s, dir_of(root, i), a, &budget, &registry);
		} catch (const std::exception& e) {
			++refused_open;
			++whats[std::string("open: ") + e.what()];
			continue;
		}
		try {
			if (a == arm::warm) { s.ix->promote(); }
			h += query_one(s, docs_hint);
			h += s.rd.acquire()->digest();
		} catch (const std::exception& e) {
			++refused_query;
			++whats[std::string("query: ") + e.what()];
		}
	}
	std::printf("identity %llu indexes: hash %016llx, refused at open %llu, at query %llu\n", (unsigned long long)count, (unsigned long long)h, (unsigned long long)refused_open,
		(unsigned long long)refused_query);
	for (const auto& [w, c] : whats) { std::printf("  %llu x %s\n", (unsigned long long)c, w.c_str()); }
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 5) {
		std::fprintf(stderr, "usage: eager_open build ROOT COUNT DOCS SEGMENTS [corrupt] | open ROOT COUNT ARM [query] [docs N] | identity ROOT COUNT ARM [docs N]\n");
		return 2;
	}
	const std::string cmd = argv[1];
	const fs::path root = argv[2];
	const std::uint64_t count = std::strtoull(argv[3], nullptr, 10);
	if (cmd == "build") {
		return build(root, count, std::strtoull(argv[4], nullptr, 10), argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 1, argc > 6 && std::string(argv[6]) == "corrupt");
	}
	const arm a = parse_arm(argv[4]);
	bool query = false;
	std::uint64_t docs = 1000;
	for (int i = 5; i < argc; ++i) {
		const std::string s = argv[i];
		if (s == "query") { query = true; }
		else if (s == "docs" && i + 1 < argc) { docs = std::strtoull(argv[++i], nullptr, 10); }
	}
	if (cmd == "open") { return open_many(root, count, a, query, docs); }
	if (cmd == "identity") { return identity(root, count, a, docs); }
	return 2;
}
