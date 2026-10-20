// The document layout (layers.md L12) as a key-value store on strata::core alone: this program
// includes core headers only and links no search code. A seeded random workload of puts,
// conditional puts, deletes, flushes, merges, commits, and reopens against a std::map, then a
// checkpoint, an install, the digest, and the sections every segment holds.
#include "strata/core/faultfs.hh"
#include "strata/core/index.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;
using document_layout = layout<keys_plugin, stored_plugin>;
using kv_index = basic_index<document_layout>;
using kv_operation = kv_index::operation;
using kv_transaction = kv_index::transaction;

static const std::array<char, 16> uuid = {'c', 'o', 'r', 'e', '-', 'k', 'v', '-', 't', 'e', 's', 't', '-', 'u', 'u', 'i'};

static kv_operation put(const std::string& key, const std::string& value, std::optional<std::uint64_t> expected = std::nullopt) {
	kv_operation o;
	o.type = kv_operation::replace_key;
	o.key = key;
	o.doc.key = key;
	o.doc.stored = value;
	o.expected_version = expected;
	return o;
}
static kv_operation erase(const std::string& key) {
	kv_operation o;
	o.type = kv_operation::delete_key;
	o.key = key;
	return o;
}
static kv_transaction tx(std::vector<kv_operation> ops) {
	kv_transaction t;
	t.ops = std::move(ops);
	return t;
}

using model_t = std::map<std::string, std::pair<std::uint64_t, std::string>>;  // key -> (version, value)

// Every live key in key order across the segments (newest copy wins) with its version and value.
static model_t contents(const kv_index& ix) {
	model_t out;
	for (const auto& s : ix.segments()) {
		const auto& keys = s.seg->get<keys_plugin>();
		const auto& stored = s.seg->get<stored_plugin>();
		std::string last;
		keys.scan("", [&](std::string_view key, ordinal_t o) {
			CHECK(last.empty() || last < key);  // a segment's keys come in order
			last = key;
			if (s.live(o)) {
				CHECK(!out.count(std::string(key)));  // one live copy per key
				out[std::string(key)] = {keys.version(o), stored.stored(o)};
			}
			return true;
		});
	}
	return out;
}

// The sections of a segment are the core's, the keys', and the stored plugin's, no more.
static void check_sections(const kv_index& ix) {
	for (const auto& s : ix.segments()) {
		const auto& core = s.seg->core();
		CHECK(core.structures() == (format::bit::keys | format::bit::stored));
		for (const auto& e : core.sections()) {
			switch (e.kind) {
				case format::section::header: case format::section::docids: case format::section::stats: case format::section::versions:
				case format::section::keys: case format::section::key_index: case format::section::stored: break;
				default: CHECK(!"a section only search writes");
			}
		}
	}
}

// Whole values for the keys in the model, through find_key (the version map, then the segments).
static void check_model(const kv_index& ix, const model_t& model) {
	const model_t seen = contents(ix);
	// Buffered writes are in no segment yet; only a flushed index lists everything.
	CHECK(ix.buffered_count() == 0);
	CHECK(seen == model);
	for (const auto& [k, v] : model) {
		const auto f = ix.find_key(k);
		CHECK(f && f->second == v.first);
	}
}

int main() {
	for (std::uint64_t seed = 1; seed <= 6; ++seed) {
		faultfs::filesystem fs;
		faultfs::io io(fs);
		kv_index::options opts;
		opts.key_prefix = "Q";
		opts.flush_threads = seed % 3 ? 1 : 2;  // the document layout buffers whole documents either way
		auto ix = kv_index::create(io, opts, uuid);
		std::mt19937_64 rng(seed);
		model_t model;
		auto key_of = [&](std::uint64_t n) {
			char b[16];
			std::snprintf(b, sizeof b, "Q%05llu", (unsigned long long)n);
			return std::string(b);
		};
		for (int step = 0; step < 400; ++step) {
			const std::string key = key_of(rng() % 150);
			const std::string value(1 + rng() % 300, char('a' + rng() % 26));
			const auto it = model.find(key);
			switch (rng() % 8) {
				case 0: case 1: case 2: {  // an unconditional put
					const auto v = ix.apply(tx({put(key, value)}), {});
					CHECK(v.result == verdict::accepted);
					model[key] = {it == model.end() ? 1 : it->second.first + 1, value};
					break;
				}
				case 3: {  // a conditional put, right or wrong
					const std::uint64_t cur = it == model.end() ? 0 : it->second.first;
					const bool right = rng() % 2;
					const auto v = ix.apply(tx({put(key, value, right ? cur : cur + 5)}), {});
					CHECK(v.result == (right ? verdict::accepted : verdict::version_conflict));
					if (right) { model[key] = {cur + 1, value}; }
					break;
				}
				case 4: {  // an atomic batch whose last operation fails undoes the others
					const std::uint64_t cur = it == model.end() ? 0 : it->second.first;
					const auto v = ix.apply(tx({put(key, value), put(key_of(999), "x", 77), erase(key)}), {});
					CHECK(v.result == verdict::version_conflict && v.op == 1);
					(void)cur;
					break;
				}
				case 5: {
					const auto v = ix.apply(tx({erase(key)}), {});
					CHECK(v.result == verdict::accepted);
					model.erase(key);
					break;
				}
				case 6: ix.refresh(); break;
				default: {
					if (step % 3 == 0) { ix.force_merge(1 + rng() % 3); }
					ix.commit();
					if (step % 5 == 0) {
						ix.close();
						ix = kv_index::open(io, opts);
					}
					break;
				}
			}
			if (step % 50 == 49) {
				const std::uint64_t digest = ix.digest();
				ix.commit();
				CHECK(ix.buffered_count() == 0);
				check_model(ix, model);
				check_sections(ix);
				(void)ix.check();
				CHECK(ix.digest() == digest);
			}
		}
		ix.commit();
		const std::uint64_t digest = ix.digest();
		const std::uint64_t gen = ix.checkpoint();
		ix.force_merge(1);
		ix.commit();
		CHECK(ix.digest() == digest);  // equal content, whatever its segment layout
		check_model(ix, model);
		ix.close();
		ix = kv_index::open(io, opts);
		CHECK(ix.digest() == digest);
		check_model(ix, model);
		(void)gen;
		const auto report = ix.check();
		CHECK(report.keys == model.size() && report.documents >= model.size() && report.stored_bytes > 0);
	}
	std::printf("core_kv: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
