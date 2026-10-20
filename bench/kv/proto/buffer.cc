// format-14-kv D3's micro-prototype (docs/design/format-14-kv.md): the write buffer's sorted
// view, sized against RocksDB's 153 bytes per buffered record (16-byte key, 100-byte value).
//
// Every variant keeps records inline in one arena whose chunks grow from 4 KiB to 1 MiB (so a
// small index pays a small buffer), each record [key length u32][value length u32][docid u64]
// [version u64][key][value], 8-aligned:
//   skiplist  RocksDB's memtable shape: a tower of next pointers per record (branching 4), the
//             only index; a stage-1 lookup is a skiplist search.
//   btree     the version map (an open-addressing hash of key to record, 8-byte slots) plus a
//             B-tree (Kronuz/cpp-btree) of 4-byte record offsets in key order.
//   sortrun   the version map plus a sort-on-demand view: records appended since the last view
//             are sorted into a run when a scan (or a flush) asks, runs of similar size merge
//             (a binary counter), and a scan merges the runs.
//   sortrun2  the same with two runs: a main run, and a delta of at most 4,096 records that
//             each view's new records merge into and that merges into the main run when full.
//
// Prints one JSON line: bytes per record (accounted and resident), insert, get, seek-of-10 and
// YCSB E (95% scans of 1 to 100 from a zipfian key, 5% inserts) per operation, and the full
// sort a flush pays (sortrun only).
#include "../kvgen.hh"
#include "btree/set.h"

#include <mach/mach.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;

std::uint64_t resident() {
	mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) { return 0; }
	return info.resident_size;
}

// Chunks of 4 KiB doubling to 1 MiB, then 1 MiB each; a record never spans two. Offsets are
// logical, in 8-byte units (u32: 32 GiB per buffer). A chunk's unused tail holds a sentinel.
class arena {
public:
	static constexpr std::uint64_t small = 4096, big = 1 << 20, small_chunks = 9;  // 4 KiB .. 1 MiB
	static constexpr std::uint64_t big_start = small * ((1 << small_chunks) - 1);
	static constexpr std::uint32_t sentinel = 0xFFFFFFFF;

	~arena() {
		for (char* c : chunks_) { std::free(c); }
	}
	// n bytes, 8-aligned; returns the record's offset in 8-byte units.
	std::uint32_t allocate(std::uint64_t n) {
		n = (n + 7) & ~std::uint64_t(7);
		if (chunks_.empty() || used_ + n > size_of(chunks_.size() - 1)) {
			if (!chunks_.empty() && used_ + 4 <= size_of(chunks_.size() - 1)) { std::memcpy(chunks_.back() + used_, &sentinel, 4); }
			const std::uint64_t size = size_of(chunks_.size());
			if (n > size) { std::abort(); }  // a prototype: records fit a chunk
			chunks_.push_back(static_cast<char*>(std::malloc(size)));
			bytes_ += size;
			used_ = 0;
		}
		const std::uint64_t at = start_of(chunks_.size() - 1) + used_;
		used_ += n;
		end_ = at + n;
		return std::uint32_t(at / 8);
	}
	char* at(std::uint32_t units) const noexcept {
		const std::uint64_t off = std::uint64_t(units) * 8;
		std::size_t k;
		if (off < big_start) {
			k = std::size_t(std::bit_width(off / small + 1) - 1);
		} else {
			k = small_chunks + std::size_t((off - big_start) / big);
		}
		return chunks_[k] + (off - start_of(k));
	}
	// The record after the one at `units` of `bytes`, skipping a chunk's tail; end() past the last.
	std::uint32_t next(std::uint32_t units, std::uint64_t bytes) const noexcept {
		std::uint64_t off = std::uint64_t(units) * 8 + ((bytes + 7) & ~std::uint64_t(7));
		if (off >= end_) { return end(); }
		const std::size_t k = chunk_of(off - 1);
		const std::uint64_t chunk_end = start_of(k) + size_of(k);
		std::uint32_t head = sentinel;
		if (off + 4 <= chunk_end) { std::memcpy(&head, chunks_[k] + (off - start_of(k)), 4); }
		if (off >= chunk_end || head == sentinel) { off = chunk_end; }
		return off >= end_ ? end() : std::uint32_t(off / 8);
	}
	std::uint32_t end() const noexcept { return std::uint32_t(end_ / 8); }
	std::uint64_t bytes() const noexcept { return bytes_; }

private:
	static std::uint64_t size_of(std::size_t k) noexcept { return k < small_chunks ? small << k : big; }
	static std::uint64_t start_of(std::size_t k) noexcept { return k < small_chunks ? small * ((std::uint64_t(1) << k) - 1) : big_start + (k - small_chunks) * big; }
	static std::size_t chunk_of(std::uint64_t off) noexcept {
		return off < big_start ? std::size_t(std::bit_width(off / small + 1) - 1) : small_chunks + std::size_t((off - big_start) / big);
	}
	std::vector<char*> chunks_;
	std::uint64_t used_ = 0, bytes_ = 0, end_ = 0;
};

struct record {
	std::uint32_t klen, vlen;
	std::uint64_t docid, version;
	std::string_view key() const noexcept { return {reinterpret_cast<const char*>(this + 1), klen}; }
	std::string_view value() const noexcept { return {reinterpret_cast<const char*>(this + 1) + klen, vlen}; }
	std::uint64_t size() const noexcept { return sizeof(record) + klen + vlen; }
};
static_assert(sizeof(record) == 24);

std::uint64_t hash_of(std::string_view k) noexcept {
	std::uint64_t h = 0xCBF29CE484222325ull;
	for (char c : k) { h = (h ^ std::uint8_t(c)) * 0x100000001B3ull; }
	return h ^ (h >> 29);
}

// The version map: 8-byte slots [offset + 1 u32][hash's high half u32], linear probing, at most
// 7/8 full, doubling.
class version_map {
public:
	explicit version_map(const arena& a) : a_(&a) {}
	const record* find(std::string_view key) const noexcept {
		if (slots_.empty()) { return nullptr; }
		const std::uint64_t h = hash_of(key);
		for (std::size_t i = h & mask_;; i = (i + 1) & mask_) {
			const std::uint64_t s = slots_[i];
			if (s == 0) { return nullptr; }
			if (std::uint32_t(s >> 32) == std::uint32_t(h >> 32)) {
				const auto* r = reinterpret_cast<const record*>(a_->at(std::uint32_t(s) - 1));
				if (r->key() == key) { return r; }
			}
		}
	}
	void insert(std::string_view key, std::uint32_t units) {
		if (8 * (size_ + 1) > 7 * slots_.size()) { grow(); }
		place(hash_of(key), units);
		++size_;
	}
	std::uint64_t bytes() const noexcept { return slots_.capacity() * 8; }

private:
	void place(std::uint64_t h, std::uint32_t units) noexcept {
		std::size_t i = h & mask_;
		while (slots_[i] != 0) { i = (i + 1) & mask_; }
		slots_[i] = (h >> 32) << 32 | (std::uint64_t(units) + 1);
	}
	void grow() {
		std::vector<std::uint64_t> old(std::max<std::size_t>(1024, slots_.size() * 2), 0);
		old.swap(slots_);
		mask_ = slots_.size() - 1;
		for (const std::uint64_t s : old) {
			if (s != 0) {
				const auto* r = reinterpret_cast<const record*>(a_->at(std::uint32_t(s) - 1));
				place(hash_of(r->key()), std::uint32_t(s) - 1);
			}
		}
	}
	const arena* a_;
	std::vector<std::uint64_t> slots_;
	std::size_t mask_ = 0, size_ = 0;
};

std::uint32_t append(arena& a, std::string_view key, std::string_view value, std::uint64_t docid) {
	const std::uint32_t at = a.allocate(sizeof(record) + key.size() + value.size());
	auto* r = reinterpret_cast<record*>(a.at(at));
	r->klen = std::uint32_t(key.size());
	r->vlen = std::uint32_t(value.size());
	r->docid = docid;
	r->version = 1;
	std::memcpy(r + 1, key.data(), key.size());
	std::memcpy(reinterpret_cast<char*>(r + 1) + key.size(), value.data(), value.size());
	return at;
}

// --- skiplist ---------------------------------------------------------------------------------

class skiplist_buffer {
public:
	static constexpr int max_height = 12;
	// A node: [height u64][next pointers × height][record].
	struct node {
		std::uint64_t height;
		node* next[1];
	};
	skiplist_buffer() : head_(static_cast<node*>(std::calloc(1, sizeof(node) + sizeof(node*) * max_height))) { head_->height = max_height; }
	~skiplist_buffer() { std::free(head_); }

	void insert(std::string_view key, std::string_view value, std::uint64_t docid) {
		node* prev[max_height];
		find_ge(key, prev);
		int h = 1;
		while (h < max_height && (rng_.next() & 3) == 0) { ++h; }
		const std::uint64_t bytes = 8 + 8 * std::uint64_t(h) + sizeof(record) + key.size() + value.size();
		auto* n = reinterpret_cast<node*>(a_.at(a_.allocate(bytes)));
		n->height = std::uint64_t(h);
		auto* r = rec(n);
		r->klen = std::uint32_t(key.size());
		r->vlen = std::uint32_t(value.size());
		r->docid = docid;
		r->version = 1;
		std::memcpy(r + 1, key.data(), key.size());
		std::memcpy(reinterpret_cast<char*>(r + 1) + key.size(), value.data(), value.size());
		for (int i = 0; i < h; ++i) {
			n->next[i] = prev[i]->next[i];
			prev[i]->next[i] = n;
		}
	}
	const record* get(std::string_view key) const {
		node* prev[max_height];
		node* n = find_ge(key, prev);
		return n && rec(n)->key() == key ? rec(n) : nullptr;
	}
	template <class F>
	void scan(std::string_view from, std::size_t count, F&& f) const {
		node* prev[max_height];
		for (node* n = find_ge(from, prev); n && count; n = n->next[0], --count) { f(*rec(n)); }
	}
	std::uint64_t bytes() const noexcept { return a_.bytes(); }

private:
	static record* rec(node* n) noexcept { return reinterpret_cast<record*>(&n->next[n->height]); }
	node* find_ge(std::string_view key, node** prev) const {
		node* x = head_;
		for (int level = max_height - 1; level >= 0; --level) {
			for (node* n = x->next[level]; n && rec(n)->key() < key; n = x->next[level]) { x = n; }
			prev[level] = x;
		}
		return x->next[0];
	}
	arena a_;
	node* head_;
	kvbench::rng rng_{42};
};

// --- btree: the version map plus a B-tree of offsets ------------------------------------------

class btree_buffer {
public:
	static constexpr std::uint32_t probe_id = 0xFFFFFFFF;
	struct less {
		const btree_buffer* b;
		std::string_view key(std::uint32_t x) const noexcept { return x == probe_id ? b->probe_ : reinterpret_cast<const record*>(b->a_.at(x))->key(); }
		bool operator()(std::uint32_t x, std::uint32_t y) const noexcept { return key(x) < key(y); }
	};
	btree_buffer() : map_(a_), order_(less{this}) {}

	void insert(std::string_view key, std::string_view value, std::uint64_t docid) {
		if (map_.find(key)) { return; }  // stage 1's lookup
		const std::uint32_t at = append(a_, key, value, docid);
		map_.insert(key, at);
		order_.insert(at);
	}
	const record* get(std::string_view key) const { return map_.find(key); }
	template <class F>
	void scan(std::string_view from, std::size_t count, F&& f) const {
		probe_ = from;
		for (auto it = order_.lower_bound(probe_id); it != order_.end() && count; ++it, --count) { f(*reinterpret_cast<const record*>(a_.at(*it))); }
	}
	std::uint64_t bytes() const noexcept { return a_.bytes() + map_.bytes(); }  // the tree's nodes: in RSS

private:
	arena a_;
	version_map map_;
	btree::set<std::uint32_t, less> order_;
	mutable std::string_view probe_;
};

// --- sortrun: the version map plus sorted runs built on demand --------------------------------

class sortrun_buffer {
public:
	// two_level: one main run and one delta run of at most delta_cap records, which merges into
	// the main run when full; otherwise runs of similar size merge (a binary counter).
	static constexpr std::size_t delta_cap = 4096;
	explicit sortrun_buffer(bool two_level = false) : map_(a_), two_level_(two_level) {}

	void insert(std::string_view key, std::string_view value, std::uint64_t docid) {
		if (map_.find(key)) { return; }
		const std::uint32_t at = append(a_, key, value, docid);
		map_.insert(key, at);
		if (unsorted_from_ == arena_end_) { unsorted_from_ = at; }
		arena_end_ = a_.end();
	}
	const record* get(std::string_view key) const { return map_.find(key); }
	// Sorts what arrived since the last view into a run; merges runs of similar size.
	void snapshot() {
		if (unsorted_from_ == arena_end_) { return; }
		std::vector<std::uint32_t> run;
		for (std::uint32_t u = unsorted_from_; u != a_.end(); u = a_.next(u, rec(u)->size())) { run.push_back(u); }
		std::sort(run.begin(), run.end(), [&](std::uint32_t x, std::uint32_t y) { return rec(x)->key() < rec(y)->key(); });
		runs_.push_back(std::move(run));
		auto by_key = [&](std::uint32_t x, std::uint32_t y) { return rec(x)->key() < rec(y)->key(); };
		auto merge_last = [&] {
			auto& a = runs_[runs_.size() - 2];
			auto& b = runs_.back();
			std::vector<std::uint32_t> merged(a.size() + b.size());
			std::merge(a.begin(), a.end(), b.begin(), b.end(), merged.begin(), by_key);
			runs_.pop_back();
			runs_.back() = std::move(merged);
		};
		if (two_level_) {
			if (runs_.size() == 3) { merge_last(); }  // the new run into the delta
			if (runs_.size() == 2 && runs_.back().size() > delta_cap) { merge_last(); }  // the delta into the main run
			unsorted_from_ = arena_end_ = a_.end();
			return;
		}
		while (runs_.size() > 1 && runs_[runs_.size() - 2].size() <= 2 * runs_.back().size()) {
			auto& a = runs_[runs_.size() - 2];
			auto& b = runs_.back();
			std::vector<std::uint32_t> merged(a.size() + b.size());
			std::merge(a.begin(), a.end(), b.begin(), b.end(), merged.begin(), [&](std::uint32_t x, std::uint32_t y) { return rec(x)->key() < rec(y)->key(); });
			runs_.pop_back();
			runs_.back() = std::move(merged);
		}
		unsorted_from_ = arena_end_ = a_.end();
	}
	template <class F>
	void scan(std::string_view from, std::size_t count, F&& f) {
		snapshot();
		std::size_t at[64];
		const std::size_t n = runs_.size();
		for (std::size_t i = 0; i < n; ++i) {
			const auto& r = runs_[i];
			at[i] = std::size_t(std::lower_bound(r.begin(), r.end(), from, [&](std::uint32_t x, std::string_view k) { return rec(x)->key() < k; }) - r.begin());
		}
		while (count--) {
			std::size_t best = n;
			std::string_view best_key;
			for (std::size_t i = 0; i < n; ++i) {
				if (at[i] < runs_[i].size()) {
					const std::string_view k = rec(runs_[i][at[i]])->key();
					if (best == n || k < best_key) { best = i, best_key = k; }
				}
			}
			if (best == n) { return; }
			f(*rec(runs_[best][at[best]++]));
		}
	}
	std::uint64_t bytes() const noexcept {
		std::uint64_t n = a_.bytes() + map_.bytes();
		for (const auto& r : runs_) { n += r.capacity() * 4; }
		return n;
	}

private:
	const record* rec(std::uint32_t u) const noexcept { return reinterpret_cast<const record*>(a_.at(u)); }
	arena a_;
	version_map map_;
	std::vector<std::vector<std::uint32_t>> runs_;
	std::uint32_t unsorted_from_ = 0, arena_end_ = 0;
	bool two_level_;
};

struct sortrun2_buffer : sortrun_buffer {
	sortrun2_buffer() : sortrun_buffer(true) {}
};

volatile std::uint64_t sink_;

template <class Buffer>
void run(const char* name, std::uint64_t records, std::uint64_t value_size) {
	kvbench::values gen(1);
	char key[kvbench::key_size];
	kvbench::rng r(5);
	std::vector<std::uint64_t> order(records);
	for (std::uint64_t i = 0; i < records; ++i) { order[i] = i; }
	for (std::uint64_t i = records; i > 1; --i) { std::swap(order[i - 1], order[r.below(i)]); }
	std::vector<std::uint32_t> probes(1 << 18);
	for (auto& p : probes) { p = std::uint32_t(r.below(records)); }
	const std::uint64_t rss0 = resident();

	auto b = std::make_unique<Buffer>();
	const auto t0 = clk::now();
	for (std::uint64_t i = 0; i < records; ++i) {
		kvbench::make_key(order[i], key);
		b->insert({key, kvbench::key_size}, gen.next(value_size), i + 1);
	}
	const double insert_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / double(records);
	const std::uint64_t rss1 = resident();
	const std::uint64_t accounted = b->bytes();

	double sort_ns = 0;
	if constexpr (requires(Buffer& x) { x.snapshot(); }) {
		const auto s0 = clk::now();
		b->snapshot();
		sort_ns = std::chrono::duration<double, std::nano>(clk::now() - s0).count() / double(records);
	}
	auto g0 = clk::now();
	const std::size_t gets = 400'000;
	for (std::size_t i = 0; i < gets; ++i) {
		kvbench::make_key(probes[i % probes.size()], key);
		sink_ += b->get({key, kvbench::key_size})->docid;
	}
	const double get_ns = std::chrono::duration<double, std::nano>(clk::now() - g0).count() / double(gets);
	g0 = clk::now();
	const std::size_t seeks = 200'000;
	for (std::size_t i = 0; i < seeks; ++i) {
		kvbench::make_key(probes[i % probes.size()], key);
		b->scan({key, kvbench::key_size}, 10, [&](const record& rec) { sink_ += rec.vlen; });
	}
	const double seek_ns = std::chrono::duration<double, std::nano>(clk::now() - g0).count() / double(seeks);
	// YCSB E over the buffer alone: 95% scans of 1..100 from a zipfian key, 5% inserts.
	kvbench::scrambled_zipfian z;
	std::uint64_t next_id = records;
	g0 = clk::now();
	const std::size_t ops = 100'000;
	for (std::size_t i = 0; i < ops; ++i) {
		if (r.unit() < 0.05) {
			kvbench::make_key(next_id, key);
			b->insert({key, kvbench::key_size}, gen.next(value_size), ++next_id);
		} else {
			kvbench::make_key(z.next(r, next_id), key);
			b->scan({key, kvbench::key_size}, 1 + r.below(100), [&](const record& rec) { sink_ += rec.vlen; });
		}
	}
	const double ycsb_e_ns = std::chrono::duration<double, std::nano>(clk::now() - g0).count() / double(ops);
	const double total_s = std::chrono::duration<double>(clk::now() - t0).count();
	std::printf(
		"{\"impl\":\"%s\",\"records\":%llu,\"value\":%llu,\"bytes_per_record\":%.1f,\"rss_per_record\":%.1f,\"insert_ns\":%.1f,\"get_ns\":%.1f,\"seek10_ns\":%.1f,"
		"\"ycsb_e_ns\":%.1f,\"sort_ns\":%.1f,\"total_s\":%.2f}\n",
		name, (unsigned long long)records, (unsigned long long)value_size, double(accounted) / double(records), double(rss1 - rss0) / double(records), insert_ns, get_ns,
		seek_ns, ycsb_e_ns, sort_ns, total_s);
}

}  // namespace

int main(int argc, char** argv) {
	std::string impl = "skiplist";
	std::uint64_t records = 1'000'000, value = 100;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view a = argv[i];
		const char* v = argv[i + 1];
		if (a == "--impl") { impl = v; }
		else if (a == "--records") { records = std::strtoull(v, nullptr, 10); }
		else if (a == "--value") { value = std::strtoull(v, nullptr, 10); }
		else {
			std::fprintf(stderr, "unknown %s\n", argv[i]);
			return 2;
		}
	}
	if (impl == "skiplist") { run<skiplist_buffer>("skiplist", records, value); }
	else if (impl == "btree") { run<btree_buffer>("btree", records, value); }
	else if (impl == "sortrun") { run<sortrun_buffer>("sortrun", records, value); }
	else if (impl == "sortrun2") { run<sortrun2_buffer>("sortrun2", records, value); }
	else { return 2; }
	return 0;
}
