// format-14-kv D2's micro-prototype (docs/design/format-14-kv.md): the decoded-block cache's
// structure under one to sixteen reader threads.
//
//   clock  set-associative CLOCK: 16 ways per set, one 64-bit meta word per slot holding the
//          state, a reference count, and a CLOCK countdown, so a hit is one compare-and-swap and
//          a release one atomic subtraction; eviction sweeps only the key's own set. No locks.
//   lru    RocksDB's LRUCache shape: 64 shards, a mutex each, a hash map and an LRU list.
//
// Workload: a universe of 4 KiB blocks keyed by (segment, block); each thread looks up
// scrambled-zipfian keys (YCSB's), and on a miss "decodes" (copies 4 KiB into a new buffer) and
// inserts. Prints one JSON line: total operations per second, ns per operation per thread, and
// the hit rate.
#include "../kvgen.hh"
#include "clock_cache.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;
constexpr std::size_t block_size = 4096;

struct block {
	char bytes[block_size];
};

using kvproto::mix;
template <class V> using clock_cache_of = kvproto::clock_cache<V>;

// --- lru: sharded, a mutex per shard ---------------------------------------------------------

class lru_cache {
public:
	struct node {
		std::uint64_t key;
		block* value;
		std::atomic<std::uint32_t> refs{0};
		std::list<node*>::iterator at;
	};
	struct handle {
		node* n = nullptr;
		const block* value() const noexcept { return n->value; }
	};
	static constexpr std::size_t shard_count = 64;

	explicit lru_cache(std::size_t capacity_blocks) : per_shard_(std::max<std::size_t>(1, capacity_blocks / shard_count)) {}
	~lru_cache() {
		for (auto& sh : shards_) {
			for (node* n : sh.order) {
				delete n->value;
				delete n;
			}
		}
	}

	bool lookup(std::uint64_t key, handle& h) {
		shard& sh = shard_of(key);
		std::lock_guard<std::mutex> g(sh.m);
		auto it = sh.map.find(key);
		if (it == sh.map.end()) { return false; }
		node* n = it->second;
		sh.order.splice(sh.order.begin(), sh.order, n->at);
		n->refs.fetch_add(1, std::memory_order_relaxed);
		h.n = n;
		return true;
	}
	void release(handle& h) noexcept { h.n->refs.fetch_sub(1, std::memory_order_release); }
	handle insert(std::uint64_t key, block* value) {
		shard& sh = shard_of(key);
		auto* n = new node{key, value};
		n->refs.store(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> g(sh.m);
		if (auto it = sh.map.find(key); it != sh.map.end()) {
			delete n->value;  // another thread inserted it first
			delete n;
			node* o = it->second;
			o->refs.fetch_add(1, std::memory_order_relaxed);
			return {o};
		}
		sh.order.push_front(n);
		n->at = sh.order.begin();
		sh.map.emplace(key, n);
		while (sh.map.size() > per_shard_) {
			auto victim = std::prev(sh.order.end());
			while (victim != sh.order.begin() && (*victim)->refs.load(std::memory_order_acquire) != 0) { --victim; }
			if ((*victim)->refs.load(std::memory_order_acquire) != 0) { break; }
			node* v = *victim;
			sh.map.erase(v->key);
			sh.order.erase(victim);
			delete v->value;
			delete v;
		}
		return {n};
	}

private:
	struct shard {
		std::mutex m;
		std::unordered_map<std::uint64_t, node*> map;
		std::list<node*> order;
	};
	shard& shard_of(std::uint64_t key) noexcept { return shards_[mix(key) % shard_count]; }
	std::size_t per_shard_;
	shard shards_[shard_count];
};

template <class Cache>
void worker(Cache& cache, unsigned t, std::uint64_t universe, double ms, const block& source, std::atomic<bool>& go, std::uint64_t& ops, std::uint64_t& hits) {
	kvbench::rng r(1000 + t);
	kvbench::scrambled_zipfian z;
	std::uint64_t sum = 0, n = 0, h = 0;
	while (!go.load(std::memory_order_acquire)) {}
	const auto start = clk::now();
	for (;;) {
		for (int i = 0; i < 256; ++i) {
			const std::uint64_t id = z.next(r, universe);
			const std::uint64_t key = (id / 1000) << 32 | (id % 1000);  // (segment, block)
			typename Cache::handle hd;
			if (cache.lookup(key, hd)) {
				++h;
			} else {
				auto* b = new block;
				std::memcpy(b->bytes, source.bytes, block_size);
				b->bytes[0] = char(id);
				hd = cache.insert(key, b);
			}
			sum += std::uint8_t(hd.value()->bytes[64]);
			cache.release(hd);
		}
		n += 256;
		if (std::chrono::duration<double, std::milli>(clk::now() - start).count() >= ms) { break; }
	}
	ops = n;
	hits = h;
	if (sum == 42) { std::fputc(' ', stderr); }
}

template <class Cache>
void run(const char* name, unsigned threads, std::uint64_t universe, std::size_t capacity, double ms, bool prefill) {
	Cache cache(capacity);
	block source;
	std::memset(source.bytes, 7, block_size);
	if (prefill) {  // every block once, so a cache that holds them all serves only hits
		for (std::uint64_t id = 0; id < universe; ++id) {
			const std::uint64_t key = (id / 1000) << 32 | (id % 1000);
			typename Cache::handle hd;
			if (!cache.lookup(key, hd)) {
				auto* b = new block;
				std::memcpy(b->bytes, source.bytes, block_size);
				hd = cache.insert(key, b);
			}
			cache.release(hd);
		}
	} else {  // warm-up: one thread runs the workload
		std::atomic<bool> go{true};
		std::uint64_t o = 0, h = 0;
		worker(cache, 99, universe, ms / 2, source, go, o, h);
	}
	std::vector<std::thread> ts;
	std::vector<std::uint64_t> ops(threads), hits(threads);
	std::atomic<bool> go{false};
	for (unsigned t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] { worker(cache, t, universe, ms, source, go, ops[t], hits[t]); });
	}
	const auto start = clk::now();
	go.store(true, std::memory_order_release);
	for (auto& th : ts) { th.join(); }
	const double secs = std::chrono::duration<double>(clk::now() - start).count();
	std::uint64_t total = 0, hit = 0;
	for (unsigned t = 0; t < threads; ++t) {
		total += ops[t];
		hit += hits[t];
	}
	std::printf("{\"impl\":\"%s\",\"prefill\":%d,\"threads\":%u,\"universe\":%llu,\"capacity\":%zu,\"mops\":%.3f,\"ns_per_op\":%.1f,\"hit_rate\":%.4f,\"secs\":%.2f}\n", name, int(prefill), threads,
		(unsigned long long)universe, capacity, double(total) / secs / 1e6, secs * 1e9 * threads / double(total), double(hit) / double(total), secs);
}

}  // namespace

int main(int argc, char** argv) {
	std::string impl = "clock";
	unsigned threads = 1;
	std::uint64_t universe = 200'000;
	std::size_t capacity = 65'536;  // blocks: 256 MiB of 4 KiB
	double ms = 1500;
	bool prefill = false;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view a = argv[i];
		const char* v = argv[i + 1];
		if (a == "--impl") { impl = v; }
		else if (a == "--prefill") { prefill = std::strtoul(v, nullptr, 10) != 0; }
		else if (a == "--threads") { threads = unsigned(std::strtoul(v, nullptr, 10)); }
		else if (a == "--universe") { universe = std::strtoull(v, nullptr, 10); }
		else if (a == "--capacity") { capacity = std::strtoull(v, nullptr, 10); }
		else if (a == "--ms") { ms = std::strtod(v, nullptr); }
		else {
			std::fprintf(stderr, "unknown %s\n", argv[i]);
			return 2;
		}
	}
	if (impl == "clock") {
		run<clock_cache_of<block>>("clock", threads, universe, capacity, ms, prefill);
	} else if (impl == "lru") {
		run<lru_cache>("lru", threads, universe, capacity, ms, prefill);
	} else {
		return 2;
	}
	return 0;
}
