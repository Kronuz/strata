// strata's key-value driver (rocksdb-survey.md, section 4): the core's `document_layout` (keys
// and stored records only, layers.md L12; key-ordered from format 14) through basic_index,
// linking strata::core alone.
// A put is a one-operation transaction upserting by key; durability `sync` commits after each
// batch (strata has no standalone turn log yet, K2), `none` commits once at the end.
//
// A get is basic_index::get(key, f), the record lent to the copy into the caller's value. A
// scan has no core API yet and no ordered view of buffered keys (K3): the driver merges each
// segment's key scan (segments(), public) with a sorted set of the keys it wrote since the
// buffer last flushed, whose values come from get.
#include "kvdriver.hh"

#include "strata/core/index.hh"
#include "strata/core/budget.hh"
#include "strata/core/pool_executor.hh"

#include "completion/posix.h"

#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>

using kv_layout = strata::document_layout;
constexpr const char* kv_layout_name = "document";
using kv_index = strata::basic_index<kv_layout>;

namespace {

using stored_frames = std::map<std::uint64_t, strata::stored_plugin::reader::stored_frame>;  // by segment id

// Bytes per section kind over every live segment's table of contents.
void section_bytes(const kv_index& ix, std::uint64_t (&bytes)[strata::format::section_kinds]) {
	for (const auto& v : ix.segments()) {
		for (const auto& e : v.seg->core().sections()) {
			if (std::uint16_t(e.kind) < strata::format::section_kinds) { bytes[std::uint16_t(e.kind)] += e.length; }
		}
	}
}

// Up to n live keys >= from (n 0: all), in order, with each value appended to sink: every
// segment's scan and the buffered keys merged. Returns the value bytes read.
std::uint64_t scan_digest = 0xcbf29ce484222325ull;
void digest(std::string_view a) {
	for (const unsigned char c : a) { scan_digest = (scan_digest ^ c) * 0x100000001b3ull; }
	scan_digest = (scan_digest ^ 0xff) * 0x100000001b3ull;
}
struct digest_printer {
	~digest_printer() {
		if (std::getenv("KV_SCAN_DIGEST")) { std::fprintf(stderr, "scan digest %016llx\n", (unsigned long long)scan_digest); }
	}
} digest_printer_;
const bool want_digest = std::getenv("KV_SCAN_DIGEST") != nullptr;
const bool scan_old = std::getenv("KV_SCAN_OLD") != nullptr;

// The same scan as a lazy k-way merge: each source is a cursor advanced only as far as the
// output needs, and a key is copied nowhere.
std::uint64_t scan_lazy(const kv_index& ix, const std::set<std::string>& buffered_keys, std::string_view from, std::uint32_t n, std::string& sink) {
	const std::size_t want = n ? n : ~std::size_t(0);
	const auto segs = ix.segments();
	std::deque<strata::keys::reader::range> cur;
	std::vector<std::size_t> segi;
	for (std::size_t i = 0; i < segs.size(); ++i) {
		cur.emplace_back(segs[i].seg->template get<strata::keys_plugin>().range(from));
		segi.push_back(i);
	}
	auto bit = buffered_keys.lower_bound(std::string(from));
	const auto live_skip = [&](std::size_t c) {
		while (cur[c].valid() && !segs[segi[c]].live(cur[c].ordinal())) { cur[c].next(); }
	};
	const auto buffered_skip = [&] {
		while (bit != buffered_keys.end() && !ix.find_key(*bit)) { ++bit; }
	};
	for (std::size_t c = 0; c < cur.size(); ++c) { live_skip(c); }
	buffered_skip();
	std::map<std::size_t, strata::stored_plugin::reader::stored_frame> frames;
	std::uint64_t bytes = 0;
	for (std::size_t got = 0; got < want; ++got) {
		// The smallest head; ties go to the buffer, then the highest segment, and every tied source advances.
		std::string_view best;
		bool have = false;
		for (std::size_t c = 0; c < cur.size(); ++c) {
			if (cur[c].valid() && (!have || cur[c].key() <= best)) { best = cur[c].key(); have = true; }
		}
		const bool buf = bit != buffered_keys.end();
		if (buf && (!have || *bit <= best)) { best = *bit; have = true; }
		if (!have) { break; }
		std::string_view v;
		std::optional<std::string> bv;
		if (buf && *bit == best) {
			bv = ix.get(*bit);
			if (bv) { v = *bv; }
		} else {
			std::size_t pick = cur.size();
			for (std::size_t c = 0; c < cur.size(); ++c) {
				if (cur[c].valid() && cur[c].key() == best) { pick = c; }
			}
			v = segs[segi[pick]].seg->template get<strata::stored_plugin>().stored(cur[pick].ordinal(), frames[segi[pick]]);
		}
		sink.append(v);
		bytes += v.size();
		if (want_digest) { digest(best); digest(v); }
		// Advance past best (copy first: a cursor's key is invalidated by next).
		const std::string key(best);
		for (std::size_t c = 0; c < cur.size(); ++c) {
			while (cur[c].valid() && cur[c].key() == key) {
				cur[c].next();
				live_skip(c);
			}
		}
		if (bit != buffered_keys.end() && *bit == key) {
			++bit;
			buffered_skip();
		}
	}
	return bytes;
}

std::uint64_t scan_old_path(const kv_index& ix, const std::set<std::string>& buffered_keys, std::string_view from, std::uint32_t n, std::string& sink) {
	struct hit {
		std::string key;  // a segment's scan lends its key only for the call
		std::size_t seg;  // ~0: buffered
		strata::ordinal_t ord;
	};
	std::vector<hit> hits;
	const std::size_t want = n ? n : ~std::size_t(0);
	const auto segs = ix.segments();
	for (std::size_t i = 0; i < segs.size(); ++i) {
		std::size_t got = 0;
		segs[i].seg->template get<strata::keys_plugin>().scan(from, [&](std::string_view k, strata::ordinal_t o) {
			if (!segs[i].live(o)) { return true; }
			hits.push_back({std::string(k), i, o});
			return ++got < want;
		});
	}
	std::size_t got = 0;
	for (auto it = buffered_keys.lower_bound(std::string(from)); it != buffered_keys.end() && got < want; ++it) {
		if (ix.find_key(*it)) {
			hits.push_back({*it, ~std::size_t(0), 0});
			++got;
		}
	}
	std::sort(hits.begin(), hits.end(), [](const hit& a, const hit& b) { return a.key < b.key; });
	hits.erase(std::unique(hits.begin(), hits.end(), [](const hit& a, const hit& b) { return a.key == b.key; }), hits.end());
	if (hits.size() > want) { hits.resize(want); }
	std::map<std::size_t, strata::stored_plugin::reader::stored_frame> frames;
	std::uint64_t bytes = 0;
	for (const auto& h : hits) {
		if (h.seg == ~std::size_t(0)) {
			const auto v = ix.get(h.key);
			if (v) {
				sink.append(*v);
				bytes += v->size();
				if (want_digest) { digest(h.key); digest(*v); }
			}
		} else {
			const std::string_view v = segs[h.seg].seg->template get<strata::stored_plugin>().stored(h.ord, frames[h.seg]);
			sink.append(v);
			bytes += v.size();
			if (want_digest) { digest(h.key); digest(v); }
		}
	}
	return bytes;
}

std::uint64_t scan(const kv_index& ix, const std::set<std::string>& buffered_keys, std::string_view from, std::uint32_t n, std::string& sink) {
	return scan_old ? scan_old_path(ix, buffered_keys, from, n, sink) : scan_lazy(ix, buffered_keys, from, n, sink);
}

}  // namespace

namespace {

constexpr kronuz::journal::PosixIOOptions host_io{true, true};

class strata_kv {
public:
	static constexpr const char* name = "strata";

	explicit strata_kv(const kvbench::config& c) : c_(c), budget_(std::max<std::uint64_t>(c.memory / 4, 1 << 20)) {
		if (c.background) {
			pool_ = std::make_unique<ThreadPool<>>("merge{}", c.background);
			exec_ = std::make_unique<strata::pool_executor<>>(*pool_);
		}
		shards_.resize(c.indexes > 1 ? c.indexes : c.threads);
		for (std::size_t i = 0; i < shards_.size(); ++i) { open(i, true); }
	}
	~strata_kv() {
		shards_.clear();
		exec_.reset();
	}

	void put(unsigned t, std::string_view key, std::string_view value) {
		shard& s = route(t, key);
		auto& op = s.tx.ops[0];
		op.key.assign(key);
		op.doc.key.assign(key);
		op.doc.stored.assign(value);
		const std::size_t before = s.ix->buffered_count();
		if (const auto v = s.ix->apply(s.tx, {}); v.result != strata::verdict::accepted) {
			std::fprintf(stderr, "rejected: verdict %d\n", int(v.result));
			std::exit(1);
		}
		if (s.ix->buffered_count() <= before) { s.keys.clear(); }  // the buffer flushed
		if (c_.w == kvbench::workload::ycsb_e || c_.w == kvbench::workload::seekrandom || c_.w == kvbench::workload::readseq) { s.keys.emplace(key); }
	}
	bool get(unsigned t, std::string_view key, std::string& value) {
		return route(t, key).ix->get(key, [&](std::string_view v) { value.assign(v); });
	}
	std::uint64_t scan(unsigned t, std::string_view from, std::uint32_t n, std::string& sink) {
		shard& s = shards_[c_.indexes > 1 ? 0 : t];
		return ::scan(*s.ix, s.keys, from, n, sink);
	}
	void durable(unsigned t) { shards_[c_.indexes > 1 ? 0 : t].ix->commit(); }
	void settle() {
		for (auto& s : shards_) {
			s.ix->refresh();
			s.keys.clear();
			for (int round = 0; round < 64; ++round) {
				s.ix->wait_merges();
				s.ix->maybe_merge();
				if (!s.ix->merges_pending()) { break; }
			}
			s.ix->commit();
			// A commit with nothing new still writes a manifest (three barriers): commit again
			// only for merges this commit started.
			if (s.ix->merges_pending()) {
				s.ix->wait_merges();
				s.ix->commit();
			}
		}
	}
	void reopen() {
		for (std::size_t i = 0; i < shards_.size(); ++i) {
			shards_[i].ix.reset();
			shards_[i].io.reset();
		}
		for (std::size_t i = 0; i < shards_.size(); ++i) { open(i, false); }
	}
	std::string stats() {
		std::uint64_t segments = 0, docs = 0;
		for (auto& s : shards_) {
			for (const auto& v : s.ix->segments()) {
				++segments;
				docs += v.seg->core().doc_count() - v.dead;
			}
		}
		std::uint64_t sections[strata::format::section_kinds] = {};
		for (auto& s : shards_) { ::section_bytes(*s.ix, sections); }
		std::string by = "{";
		for (std::uint16_t k = 0; k < strata::format::section_kinds; ++k) {
			if (sections[k]) { by += (by.size() > 1 ? ",\"" : "\"") + std::to_string(k) + "\":" + std::to_string(sections[k]); }
		}
		by += "}";
		char b[256];
		std::snprintf(b, sizeof b, "{\"segments\":%llu,\"live_docs\":%llu,\"budget_peak\":%llu,\"shards\":%zu,\"layout\":\"%s\",\"sections\":", (unsigned long long)segments,
			(unsigned long long)docs, (unsigned long long)budget_.peak(), shards_.size(), kv_layout_name);
		return b + by + "}";
	}

private:
	struct shard {
		std::unique_ptr<kronuz::journal::PosixIO> io;
		std::unique_ptr<kv_index> ix;
		kv_index::transaction tx;
		std::set<std::string> keys;  // written since the buffer last flushed, for scans
	};
	shard& route(unsigned t, std::string_view key) {
		if (c_.indexes <= 1) { return shards_[t]; }
		return shards_[kvbench::route_hash(key) % shards_.size()];
	}
	void open(std::size_t i, bool create) {
		shard& s = shards_[i];
		const std::string dir = shards_.size() == 1 ? c_.dir : c_.dir + "/" + std::to_string(i);
		std::filesystem::create_directories(dir);
		std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
		s.io = std::make_unique<kronuz::journal::PosixIO>(dir, 0600, host_io);
		kv_index::options o;
		o.key_prefix = "Q";
		o.budget = &budget_;
		o.buffer_share = std::max<std::uint64_t>(budget_.capacity() / shards_.size(), 64 << 10);
		o.auto_merge = true;
		o.background = exec_.get();
		o.identity_seed = c_.seed + i;
		const std::array<char, 16> uuid{'k', 'v', 'b', 'e', 'n', 'c', 'h'};
		s.ix = std::make_unique<kv_index>(create ? kv_index::create(*s.io, o, uuid) : kv_index::open(*s.io, o));
		s.tx.ops.resize(1);
		s.tx.ops[0].type = kv_index::operation::replace_key;
	}

	kvbench::config c_;
	strata::byte_budget budget_;
	std::unique_ptr<ThreadPool<>> pool_;
	std::unique_ptr<strata::executor> exec_;
	std::vector<shard> shards_;
};

}  // namespace

int main(int argc, char** argv) { return kvbench::main_of<strata_kv>(argc, argv); }
