// T2 (durability-modes): CPU per document of the work document mode repeats on every voter
// (apply, flush, merge, commit) against what a segment-mode follower does instead (append
// the prepared batches to its log, then receive and verify the leader's segment files).
//
//   sizing leader CORPUS DIR [N]    index N documents as Phase 3's index path does; counts bytes
//   sizing ship CORPUS DIR [N]      the same, teeing every segment file into DIR/shipped
//   sizing log CORPUS DIR [N]       the prepared batches, framed with CRC-32C, written to DIR/log
//   sizing install DIR              every file in DIR/shipped read, CRC-32C'd, and written to DIR/follower
//
// Environment: BUFFER_MB (default 256), AUTO_MERGE (default 1).
#include "completion/posix.h"
#include "counting_io.hh"
#include "crc32c.hh"
#include "strata/index.hh"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
double secs(clock_type::time_point a, clock_type::time_point b) { return std::chrono::duration<double>(b - a).count(); }

struct cpu { double user = 0, sys = 0; };
cpu cpu_now() {
	rusage r{};
	::getrusage(RUSAGE_SELF, &r);
	return {double(r.ru_utime.tv_sec) + r.ru_utime.tv_usec / 1e6, double(r.ru_stime.tv_sec) + r.ru_stime.tv_usec / 1e6};
}

std::string_view field(std::string_view line, std::string_view name) {
	const std::string key = "\"" + std::string(name) + "\": \"";
	auto at = line.find(key);
	if (at == std::string_view::npos) { return {}; }
	at += key.size();
	return line.substr(at, line.find('"', at) - at);
}

struct doc { std::string id, text; };
std::vector<doc> read_corpus(const char* path, std::size_t n) {
	std::ifstream in(path);
	std::vector<doc> out;
	std::string line;
	while (out.size() < n && std::getline(in, line)) {
		if (line.empty()) { continue; }
		out.push_back({std::string(field(line, "id")), std::string(field(line, "text"))});
	}
	return out;
}

void fill(strata::operation& op, const doc& d) {
	op.type = strata::operation::replace_key;
	op.key.assign("Q");
	op.key.append(d.id);
	op.doc.terms.resize(1);
	op.doc.terms[0].term = op.key;
	op.doc.terms[0].wdf = 0;
	op.doc.terms[0].positions.clear();
	op.doc.text = d.text;
	op.doc.tokens.clear();
	op.doc.split_text = true;
	op.doc.length = 0;
	op.doc.stored = d.id;
}

void prepare_dir(const std::filesystem::path& dir) {
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
}

// A prepared batch as a log record: the key, the text, the stored record, and the flags, each
// length-prefixed, as the replicated command would carry them (a stand-in encoding).
void put_varint(std::string& out, std::uint64_t v) {
	while (v >= 0x80) { out.push_back(char(v | 0x80)); v >>= 7; }
	out.push_back(char(v));
}
void put_bytes(std::string& out, std::string_view s) { put_varint(out, s.size()); out.append(s); }
std::string encode_batch(const std::vector<doc>& docs, std::size_t from, std::size_t to) {
	std::string out;
	put_varint(out, to - from);
	for (std::size_t i = from; i < to; ++i) {
		out.push_back(char(1));  // replace_key
		put_bytes(out, "Q" + docs[i].id);
		put_bytes(out, docs[i].text);
		out.push_back(char(1));  // split_text
		put_bytes(out, docs[i].id);
	}
	return out;
}

int index_run(const char* corpus, const std::filesystem::path& dir, std::size_t n, bool ship) {
	const auto t0 = clock_type::now();
	const auto docs = read_corpus(corpus, n);
	std::uint64_t text_bytes = 0;
	for (const auto& d : docs) { text_bytes += d.text.size() + d.id.size(); }
	const auto data = dir / "data";
	prepare_dir(data);
	if (ship) { prepare_dir(dir / "shipped"); }
	kronuz::journal::PosixIO posix(data.string());
	probe::counts c;
	probe::counting_io io(posix, c);
	if (ship) { io.tee_dir = (dir / "shipped").string(); }
	strata::index::options o;
	o.key_prefix = "Q";
	const char* mb = std::getenv("BUFFER_MB");
	o.buffer_share = (mb ? std::strtoull(mb, nullptr, 10) : 256) << 20;
	const char* am = std::getenv("AUTO_MERGE");
	o.auto_merge = !am || std::string_view(am) != "0";
	o.flush_threads = 1;
	const std::array<char, 16> uuid{'d', 'u', 'r', 'a', 'b', 'i', 'l', 'i', 't', 'y', '-', 's', 'i', 'z', 'e', '1'};
	const auto t1 = clock_type::now();
	auto ix = strata::index::create(io, o, uuid);
	const auto t2 = clock_type::now();
	const cpu c0 = cpu_now();
	strata::transaction t;
	t.ops.resize(1000);
	std::size_t batched = 0;
	auto apply = [&] {
		if (!batched) { return; }
		t.ops.resize(batched);
		if (ix.apply(t, {}).result != strata::verdict::accepted) { std::fprintf(stderr, "rejected\n"); std::exit(1); }
		batched = 0;
		t.ops.resize(1000);
	};
	for (const auto& d : docs) {
		fill(t.ops[batched++], d);
		if (batched == 1000) { apply(); }
	}
	apply();
	ix.commit();
	const cpu c1 = cpu_now();
	const auto t3 = clock_type::now();
	const auto segments = ix.segments().size();
	ix.close();
	const auto t4 = clock_type::now();
	std::uint64_t written = 0;
	for (const auto& [k, b] : c.bytes_by_kind) { written += b; }
	std::uint64_t final_bytes = 0;
	for (const auto& e : std::filesystem::directory_iterator(data)) { final_bytes += e.file_size(); }
	std::printf("%s: %zu documents (%llu text and id bytes), BUFFER_MB %llu, auto_merge %d\n", ship ? "ship" : "leader", docs.size(),
		(unsigned long long)text_bytes, (unsigned long long)(o.buffer_share >> 20), int(o.auto_merge));
	std::printf("  times: read corpus %.3f s, create %.3f s, timed apply+flush+merge+commit %.3f s, close %.3f s, total %.3f s\n",
		secs(t0, t1), secs(t1, t2), secs(t2, t3), secs(t3, t4), secs(t0, t4));
	std::printf("  cpu (timed part): user %.3f s, sys %.3f s, total %.3f s; %.2f us per document\n", c1.user - c0.user, c1.sys - c0.sys,
		(c1.user - c0.user) + (c1.sys - c0.sys), ((c1.user - c0.user) + (c1.sys - c0.sys)) * 1e6 / double(docs.size()));
	std::printf("  segments at the end %zu; files created: ", segments);
	for (const auto& [k, f] : c.files_by_kind) { std::printf("%s %llu  ", k.c_str(), (unsigned long long)f); }
	std::printf("\n  bytes written: total %llu; ", (unsigned long long)written);
	for (const auto& [k, b] : c.bytes_by_kind) { std::printf("%s %llu  ", k.c_str(), (unsigned long long)b); }
	std::printf("\n  final directory bytes %llu; durability calls %llu\n", (unsigned long long)final_bytes, (unsigned long long)c.durability_calls());
	return 0;
}

int log_run(const char* corpus, const std::filesystem::path& dir, std::size_t n) {
	const auto t0 = clock_type::now();
	const auto docs = read_corpus(corpus, n);
	std::vector<std::string> batches;
	const cpu e0 = cpu_now();
	for (std::size_t i = 0; i < docs.size(); i += 1000) { batches.push_back(encode_batch(docs, i, std::min(docs.size(), i + 1000))); }
	const cpu e1 = cpu_now();
	prepare_dir(dir / "log");
	const int fd = ::open((dir / "log" / "turns").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) { std::perror("open"); return 1; }
	const auto t1 = clock_type::now();
	const cpu c0 = cpu_now();
	std::uint64_t bytes = 0;
	std::string record;
	for (const auto& b : batches) {
		// One turn per batch: a 16-byte header (length, sequence, CRC-32C of the payload), the payload.
		record.clear();
		const std::uint32_t crc = hashes::crc32c(b.data(), b.size());
		const std::uint64_t len = b.size();
		record.append(reinterpret_cast<const char*>(&len), 8);
		record.append(reinterpret_cast<const char*>(&crc), 4);
		record.append(4, '\0');
		record.append(b);
		for (std::size_t at = 0; at < record.size();) {
			const auto w = ::write(fd, record.data() + at, record.size() - at);
			if (w < 0) { std::perror("write"); return 1; }
			at += std::size_t(w);
		}
		bytes += record.size();
	}
	const cpu c1 = cpu_now();
	const auto t2 = clock_type::now();
	::close(fd);
	std::printf("log: %zu documents in %zu batches, %llu bytes appended (%.1f per document)\n", docs.size(), batches.size(),
		(unsigned long long)bytes, double(bytes) / double(docs.size()));
	std::printf("  times: read corpus %.3f s, timed append %.3f s, total %.3f s\n", secs(t0, t1), secs(t1, t2), secs(t0, t2));
	std::printf("  cpu: encode (leader, once) %.3f s; append (each voter) user %.3f s, sys %.3f s, total %.3f s; %.2f us per document\n",
		(e1.user - e0.user) + (e1.sys - e0.sys), c1.user - c0.user, c1.sys - c0.sys, (c1.user - c0.user) + (c1.sys - c0.sys),
		((c1.user - c0.user) + (c1.sys - c0.sys)) * 1e6 / double(docs.size()));
	return 0;
}

int install_run(const std::filesystem::path& dir, std::size_t n) {
	const auto t0 = clock_type::now();
	prepare_dir(dir / "follower");
	std::vector<std::filesystem::path> files;
	for (const auto& e : std::filesystem::directory_iterator(dir / "shipped")) { files.push_back(e.path()); }
	std::sort(files.begin(), files.end());
	std::vector<char> buf;
	std::uint64_t bytes = 0;
	std::uint32_t mix = 0;
	const auto t1 = clock_type::now();
	const cpu c0 = cpu_now();
	for (const auto& p : files) {
		const int in = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
		struct stat st{};
		if (in < 0 || ::fstat(in, &st)) { std::perror("open shipped"); return 1; }
		buf.resize(std::size_t(st.st_size));
		for (std::size_t at = 0; at < buf.size();) {
			const auto r = ::read(in, buf.data() + at, buf.size() - at);
			if (r <= 0) { std::perror("read"); return 1; }
			at += std::size_t(r);
		}
		::close(in);
		mix ^= hashes::crc32c(buf.data(), buf.size());  // the whole-file check
		const int out = ::open((dir / "follower" / p.filename()).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (out < 0) { std::perror("open follower"); return 1; }
		for (std::size_t at = 0; at < buf.size();) {
			const auto w = ::write(out, buf.data() + at, buf.size() - at);
			if (w < 0) { std::perror("write"); return 1; }
			at += std::size_t(w);
		}
		::close(out);
		bytes += buf.size();
	}
	const cpu c1 = cpu_now();
	const auto t2 = clock_type::now();
	std::printf("install: %zu files, %llu bytes received and verified (crc mix %08x)\n", files.size(), (unsigned long long)bytes, mix);
	std::printf("  times: list %.3f s, timed receive+verify+write %.3f s, total %.3f s\n", secs(t0, t1), secs(t1, t2), secs(t0, t2));
	std::printf("  cpu: user %.3f s, sys %.3f s, total %.3f s; %.2f us per document of %zu\n", c1.user - c0.user, c1.sys - c0.sys,
		(c1.user - c0.user) + (c1.sys - c0.sys), ((c1.user - c0.user) + (c1.sys - c0.sys)) * 1e6 / double(n), n);
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) { std::fprintf(stderr, "usage: sizing leader|ship|log CORPUS DIR [N] | sizing install DIR [N]\n"); return 2; }
	const std::string mode = argv[1];
	if (mode == "install") { return install_run(argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 100000); }
	if (argc < 4) { return 2; }
	const std::size_t n = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 100000;
	if (mode == "leader" || mode == "ship") { return index_run(argv[2], argv[3], n, mode == "ship"); }
	if (mode == "log") { return log_run(argv[2], argv[3], n); }
	return 2;
}
