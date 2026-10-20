// strata's key-value driver (rocksdb-survey.md, section 4): format 13's `document_layout`
// (keys and stored records only, layers.md L12) through basic_index. A put is a one-operation
// transaction upserting by key; durability `sync` commits after each batch (strata has no
// standalone turn log yet, K2), `none` commits once at the end.
//
// strata has no public get-by-key that returns the stored record, and no ordered view of
// buffered keys (K3). The driver reaches both through the index's test hook: a get is
// lookup_key's own walk (the version map, then segments newest first) returning the buffered
// record or the segment's stored record; a scan merges each segment's key scan with a sorted
// set of the keys written since the buffer last flushed. Each is reported as such.
#include "kvdriver.hh"

#include "strata/budget.hh"
#include "strata/index.hh"
#include "strata/pool_executor.hh"

#include "completion/posix.h"

#include <map>
#include <memory>
#include <set>

#ifdef KV_SEARCH_LAYOUT
using kv_layout = strata::search_layout;
constexpr const char* kv_layout_name = "search";
#else
using kv_layout = strata::layout<strata::keys_plugin, strata::stored_plugin>;
constexpr const char* kv_layout_name = "document";
#endif
using kv_index = strata::basic_index<kv_layout>;

namespace strata {
struct index_test_access {
	using stored_reader = stored_plugin::reader;
	// The live copy's stored record: the version map's buffered copy, else the newest
	// segment that holds the key live. `cache` keeps one decoded frame per segment id.
	template <class Cache>
	static bool get(const kv_index& ix, std::string_view key, std::string& out, Cache* cache) {
		if (const auto v = ix.key_slot(key)) {
			out.assign(buffered_stored(ix, *v));
			return true;
		}
		const std::uint64_t hash = xxh64::hash(key);
		for (std::size_t i = ix.segs_.size(); i-- > 0;) {
			const auto& s = *ix.segs_[i];
			const auto& keys = s.seg.template get<keys_plugin>();
			if (const auto ord = keys.find(key, live_bits{s.deleted.data()}, hash)) {
				const auto& st = s.seg.template get<stored_plugin>();
				if (cache) {
					out.assign(st.stored(*ord, (*cache)[s.ref.id]));
				} else {
					stored_reader::stored_frame f;
					out.assign(st.stored(*ord, f));
				}
				return true;
			}
		}
		return false;
	}
	static std::string_view buffered_stored(const kv_index& ix, std::size_t i) {
		if (!ix.inverting()) { return ix.buffer_[i].doc.stored; }
		return i < ix.active_count() ? ix.active_->buf.stored(ordinal_t(i)) : std::string_view(ix.pending_docs_[i - ix.active_count()].doc->stored);
	}
	static bool buffered(const kv_index& ix, std::string_view key) { return ix.key_slot(key).has_value(); }
	// Bytes per section kind over every live segment's file.
	static void section_bytes(const kv_index& ix, std::uint64_t (&bytes)[format::section_kinds]) {
		for (const auto& s : ix.segs_) {
			const std::string_view file = s->bytes();
			const auto f = format::decode_footer(file);
			for (const auto& e : format::decode_toc(file, f)) {
				if (std::uint16_t(e.kind) < format::section_kinds) { bytes[std::uint16_t(e.kind)] += e.length; }
			}
		}
	}

	// Up to n live keys >= from (n 0: all), in order, with each value appended to sink: every
	// segment's scan and the buffered keys merged. Returns the value bytes read.
	static std::uint64_t scan(const kv_index& ix, const std::set<std::string>& buffered_keys, std::string_view from, std::uint32_t n, std::string& sink) {
		struct hit {
			std::string key;  // a segment's scan lends its key only for the call
			std::size_t seg;  // ~0: buffered
			ordinal_t ord;
		};
		std::vector<hit> hits;
		const std::size_t want = n ? n : ~std::size_t(0);
		for (std::size_t i = 0; i < ix.segs_.size(); ++i) {
			const auto& s = *ix.segs_[i];
			const live_bits live{s.deleted.data()};
			std::size_t got = 0;
			s.seg.template get<keys_plugin>().scan(from, [&](std::string_view k, ordinal_t o) {
				if (!live.live(o)) { return true; }
				hits.push_back({std::string(k), i, o});
				return ++got < want;
			});
		}
		std::size_t got = 0;
		for (auto it = buffered_keys.lower_bound(std::string(from)); it != buffered_keys.end() && got < want; ++it) {
			if (ix.key_slot(*it)) {
				hits.push_back({*it, ~std::size_t(0), 0});
				++got;
			}
		}
		std::sort(hits.begin(), hits.end(), [](const hit& a, const hit& b) { return a.key < b.key; });
		if (hits.size() > want) { hits.resize(want); }
		std::map<std::size_t, stored_reader::stored_frame> frames;
		std::uint64_t bytes = 0;
		for (const auto& h : hits) {
			std::string_view v;
			if (h.seg == ~std::size_t(0)) {
				v = buffered_stored(ix, *ix.key_slot(h.key));
			} else {
				v = ix.segs_[h.seg]->seg.template get<stored_plugin>().stored(h.ord, frames[h.seg]);
			}
			sink.append(v);
			bytes += v.size();
		}
		return bytes;
	}
};
}  // namespace strata

namespace {

using access = strata::index_test_access;
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
		return access::get<std::map<std::uint64_t, strata::stored_plugin::reader::stored_frame>>(*route(t, key).ix, key, value, nullptr);
	}
	std::uint64_t scan(unsigned t, std::string_view from, std::uint32_t n, std::string& sink) {
		shard& s = shards_[c_.indexes > 1 ? 0 : t];
		return access::scan(*s.ix, s.keys, from, n, sink);
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
			s.ix->wait_merges();
			s.ix->commit();
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
		for (auto& s : shards_) { access::section_bytes(*s.ix, sections); }
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
