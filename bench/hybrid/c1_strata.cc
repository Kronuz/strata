// C1's strata arm (docs/design/hybrid-search.md C1; driver bench/hybrid/c1.py): one index per
// tenant, each vector a document whose stored value is its 768 float32s, answered by an exact scan.
//   c1_strata load  ROOT N VECFILE [i8]         N indexes of 1,000 documents, from VECFILE's rows
//   c1_strata serve ROOT N QFILE SAMPLE OUTDIR [i8]
//                                               a catalog over ROOT as a host runs it (defer_io, the
//                                               sweep on); prints `ready`, then per `pass NAME` line
//                                               runs query i on tenant SAMPLE[i], writes
//                                               `seconds id...` lines to OUTDIR/NAME.txt, prints `done`
// No vector code exists in strata yet; this measures what the design's exact search starts from.
// `i8` (hybrid-search.md A2) stores each vector as a float32 scale and 768 int8 codes (772 bytes
// against 3,072) and scores the codes against the float32 query: the code column read cold.
#include "../idle_index/meters.hh"

#include "strata/core/budget.hh"
#include "strata/core/catalog.hh"
#include "strata/core/hazard.hh"
#include "strata/core/index.hh"
#include "strata/core/work_pool_executor.hh"

#include "completion/posix.h"

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace strata;
using namespace idle_bench;
using kv_index = basic_index<document_layout>;
using cat_t = catalog<kv_index>;

namespace {

constexpr std::size_t dim = 768, per = 1000, top = 10;
constexpr std::size_t i8_size = sizeof(float) + dim;  // scale, then one code per component
bool i8 = false;

std::vector<float> read_floats(const char* path, std::size_t first, std::size_t count) {
	std::vector<float> v(count);
	std::ifstream f(path, std::ios::binary);
	f.seekg(std::streamoff(first * sizeof(float)));
	f.read(reinterpret_cast<char*>(v.data()), std::streamsize(count * sizeof(float)));
	if (!f) { throw std::runtime_error(std::string("short read: ") + path); }
	return v;
}

kv_index::options index_options() {
	kv_index::options o;
	o.keyed = true;
	return o;
}

// Scale = max |x| / 127, then each component rounded to the nearest code.
std::string quantize(const float* v) {
	float m = 0;
	for (std::size_t i = 0; i < dim; ++i) { m = std::max(m, std::fabs(v[i])); }
	const float scale = m / 127.0f;
	std::string out(i8_size, '\0');
	std::memcpy(out.data(), &scale, sizeof scale);
	for (std::size_t i = 0; i < dim; ++i) {
		out[sizeof scale + i] = char(std::int8_t(std::lrintf(scale > 0 ? v[i] / scale : 0.0f)));
	}
	return out;
}

int load(const fs::path& root, std::uint64_t n, const char* vecs) {
	const auto t0 = clk::now();
	for (std::uint64_t t = 0; t < n; ++t) {
		const std::vector<float> rows = read_floats(vecs, t * per * dim, per * dim);
		const fs::path dir = dir_of(root, t);
		fs::create_directories(dir);
		kronuz::journal::PosixIO io(dir, 0600, kronuz::journal::PosixIOOptions{true, true});
		kv_index ix = kv_index::create(io, index_options(), std::array<char, 16>{'c', '1'});
		kv_index::transaction tx;
		tx.ops.resize(1);
		tx.ops[0].type = kv_index::operation::replace_key;
		for (std::size_t d = 0; d < per; ++d) {
			auto& op = tx.ops[0];
			op.key = key_of(d);
			op.doc.key = op.key;
			const float* v = rows.data() + d * dim;
			if (i8) {
				const std::string code = quantize(v);
				op.doc.stored.assign(code);
			} else {
				op.doc.stored.assign(std::string_view(reinterpret_cast<const char*>(v), dim * sizeof(float)));
			}
			if (ix.apply(tx, {}).result != verdict::accepted) { throw std::runtime_error("apply refused"); }
		}
		ix.commit();
	}
	std::fprintf(stderr, "loaded %llu indexes in %.1f s\n", (unsigned long long)n, us(t0, clk::now()) / 1e6);
	return 0;
}

// The host's timer: `fn` runs on the timer's thread after `delay_ns` (catalog_tiers' shape).
class timer_thread {
public:
	timer_thread() : t_([this] { loop(); }) {}
	~timer_thread() {
		{
			std::lock_guard l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		t_.join();
	}
	void add(std::uint64_t delay_ns, std::function<void()> fn) {
		{
			std::lock_guard l(m_);
			q_.emplace(clk::now() + std::chrono::nanoseconds(delay_ns), std::move(fn));
		}
		cv_.notify_all();
	}

private:
	void loop() {
		std::unique_lock l(m_);
		for (;;) {
			if (q_.empty()) {
				if (stop_) { return; }
				cv_.wait(l);
			} else if (q_.begin()->first <= clk::now() || stop_) {
				auto fn = std::move(q_.begin()->second);
				q_.erase(q_.begin());
				l.unlock();
				fn();
				l.lock();
			} else {
				cv_.wait_until(l, q_.begin()->first);
			}
		}
	}
	std::mutex m_;
	std::condition_variable cv_;
	std::multimap<clk::time_point, std::function<void()>> q_;
	bool stop_ = false;
	std::thread t_;
};

// The request thread's scheduler: the catalog posts a resumed pin here.
class thread_sched {
public:
	void post(std::coroutine_handle<> h) {
		{
			std::lock_guard l(mu_);
			q_.push_back(h);
		}
		cv_.notify_one();
	}
	std::uint64_t now() const noexcept { return 0; }
	void run(task::task<void> body) {
		bool done = false;
		const task::running_on on(task::scheduler_ref::of(*this));
		task::spawn(task::scheduler_ref::of(*this), std::move(body), [&](std::exception_ptr e) {
			if (e) { std::terminate(); }
			{
				std::lock_guard l(mu_);
				done = true;
			}
			cv_.notify_one();
		});
		for (;;) {
			std::coroutine_handle<> h;
			{
				std::unique_lock l(mu_);
				cv_.wait(l, [&] { return !q_.empty() || done; });
				if (q_.empty()) { return; }
				h = q_.front();
				q_.pop_front();
			}
			h.resume();
		}
	}

private:
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<std::coroutine_handle<>> q_;
};

// Eight accumulators so the reduction vectorizes without reassociation flags.
float dot(const char* v, const float* q) {
	float acc[8] = {};
	for (std::size_t i = 0; i < dim; i += 8) {
		float x[8];
		std::memcpy(x, v + i * sizeof(float), sizeof x);
		for (int j = 0; j < 8; ++j) { acc[j] += x[j] * q[i + std::size_t(j)]; }
	}
	return ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
}

// The codes against the float32 query, scaled back: eight accumulators as `dot`.
float dot_i8(const char* v, const float* q) {
	float scale;
	std::memcpy(&scale, v, sizeof scale);
	const auto* c = reinterpret_cast<const std::int8_t*>(v + sizeof scale);
	float acc[8] = {};
	for (std::size_t i = 0; i < dim; i += 8) {
		for (int j = 0; j < 8; ++j) { acc[j] += float(c[i + std::size_t(j)]) * q[i + std::size_t(j)]; }
	}
	return scale * (((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7])));
}

using hit = std::pair<float, std::uint32_t>;

// One query: a pin by key, a reader, an exact scan of every document, the top 10 by inner product.
task::task<std::vector<hit>> query(cat_t& c, std::string key, const float* q) {
	std::vector<hit> best;
	best.reserve(top + 1);
	auto p = co_await c.pin_request(key);
	const auto g = p.reader().acquire();
	g->scan("", [&](std::string_view k, std::string_view v, std::uint64_t) {
		if (v.size() != (i8 ? i8_size : dim * sizeof(float))) { throw std::runtime_error("value of another size"); }
		const float s = i8 ? dot_i8(v.data(), q) : dot(v.data(), q);
		if (best.size() == top && s <= best.back().first) { return; }
		const hit h{s, std::uint32_t(std::strtoul(std::string(k.substr(1)).c_str(), nullptr, 10))};
		best.insert(std::upper_bound(best.begin(), best.end(), h, [](const hit& a, const hit& b) { return a.first > b.first; }), h);
		if (best.size() > top) { best.pop_back(); }
	});
	co_return best;
}

int serve(const fs::path& root, std::uint64_t n, const char* qfile, const char* sample_file, const fs::path& out) {
	std::vector<std::uint64_t> sample;
	{
		std::ifstream f(sample_file);
		for (std::uint64_t t; f >> t;) { sample.push_back(t); }
	}
	const std::vector<float> qs = read_floats(qfile, 0, sample.size() * dim);
	WorkPool::Options w;
	w.threads = 9;  // eight chores, one worker in reserve (catalog_tiers' host)
	w.name_format = "c1{:02}";
	WorkPool pool(w);
	work_pool_executor exec(pool, 8);
	resource_budget budget;
	reader_registry registry{256};
	byte_budget bytes{std::uint64_t(16) << 30};
	timer_thread timer;
	cat_t::host h;
	h.exec = &exec;
	h.budget = &budget;
	h.index_options = index_options();
	h.index_options.defer_io = true;  // as Xapiand's host sets it (src/engine/strata/shard.cc)
	h.index_options.budget = &bytes;
	h.index_options.buffer_share = std::uint64_t(1) << 30;
	h.index_options.readers = &registry;
	h.resolve = [&](const cat_t::opening& op) -> std::unique_ptr<kronuz::journal::IO> {
		const fs::path dir = dir_of(root, std::strtoull(std::string(op.key.substr(1)).c_str(), nullptr, 10));
		return counted_io::make(op.counts, [&] { return std::make_unique<kronuz::journal::PosixIO>(dir, 0600, kronuz::journal::PosixIOOptions{true, true}); });
	};
	h.timer = [&](std::uint64_t delay, std::function<void()> fn) { timer.add(delay, std::move(fn)); };
	auto cat = std::make_unique<cat_t>(std::move(h));
	cat->start();
	std::printf("ready\n");
	std::fflush(stdout);
	for (std::string line; std::getline(std::cin, line);) {
		if (line == "quit") { break; }
		if (line.rfind("pass ", 0) != 0) { continue; }
		std::FILE* f = std::fopen((out / (line.substr(5) + ".txt")).c_str(), "w");
		thread_sched s;
		s.run([&]() -> task::task<void> {
			for (std::size_t i = 0; i < sample.size(); ++i) {
				const auto a = clk::now();
				const std::vector<hit> best = co_await query(*cat, "i" + std::to_string(sample[i]), qs.data() + i * dim);
				const double sec = us(a, clk::now()) / 1e6;
				std::fprintf(f, "%.9f", sec);
				for (const hit& x : best) { std::fprintf(f, " %u", x.second); }
				std::fprintf(f, "\n");
			}
		}());
		std::fclose(f);
		std::printf("done\n");
		std::fflush(stdout);
	}
	cat->stop_sweep();
	{
		thread_sched s;
		cat_t* c = cat.get();
		s.run([c]() -> task::task<void> { (void)co_await c->shutdown(); }());
	}
	cat.reset();
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string cmd = argc > 1 ? argv[1] : "";
	if (argc > 1 && std::string(argv[argc - 1]) == "i8") {
		i8 = true;
		--argc;
	}
	if (cmd == "load" && argc == 5) { return load(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4]); }
	if (cmd == "serve" && argc == 7) { return serve(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4], argv[5], argv[6]); }
	std::fprintf(stderr, "usage: c1_strata load ROOT N VECFILE [i8] | serve ROOT N QFILE SAMPLE OUTDIR [i8]\n");
	return 2;
}
