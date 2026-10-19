// Sustained ingest with queries beside it, per window (docs/research/no-stalls.md): one
// writer thread upserts corpus documents at an offered rate into N indexes that share one
// pool and one budget, refreshing every second as a near-real-time host does; one reader
// thread runs TOP_10 queries over owned snapshots at its own rate; a line per second gives
// both sides' p50/p99/p999 beside the engine's merge debt. The controllers are the research
// note's ideas: proportional admission (I1), a debt-adaptive merge width (I2), the host's
// merge scheduler across indexes by cheapest retirement or by query heat (I4), and a merge
// paused at its checkpoints while a flush runs (I3). Build with -DSTRATA_RESEARCH=ON.
//   research_ingest DIR CORPUS [key=value...]   (keys below, in `usage`)
#include "../../tools/engine_query.hh"
#include "completion/posix.h"
#include "strata/index.hh"
#include "strata/pool_executor.hh"
#include "strata/rate_limiter.hh"
#include "windows.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace nostalls;

const char* usage =
	"research_ingest DIR CORPUS [rate=5000 batch=100 seconds=120 docs=100000 indexes=1 threads=1\n"
	"  limit_mb=0 refresh_ms=1000 queries=100 control=none|proportional|feedforward|refresh|adaptive|global|hot\n"
	"  low=8 high=64 floor_rate=0.05 pause_on_flush=0 zipf=1.0 budget_mb=2048 share_mb=256 seed=7]\n";

struct doc {
	std::string id, text;
};

// The corpus's lines as id and text; its text is pre-tokenized words between quotes.
std::vector<doc> load(const std::string& path, std::size_t limit) {
	std::vector<doc> out;
	std::ifstream in(path);
	std::string line;
	while (out.size() < limit && std::getline(in, line)) {
		auto id = line.find("\"id\": \"");
		auto text = line.find("\"text\": \"");
		if (id == std::string::npos || text == std::string::npos) { continue; }
		id += 7;
		text += 9;
		out.push_back({line.substr(id, line.find('"', id) - id), line.substr(text, line.find('"', text) - text)});
	}
	return out;
}

// Queries from the loaded documents' vocabulary: terms with 200 <= df <= 20,000 in the
// sample, a third single terms, a third conjunctions, a third disjunctions.
std::vector<std::string> make_queries(const std::vector<doc>& docs, std::uint64_t seed) {
	std::unordered_map<std::string_view, std::uint32_t> df;
	std::vector<std::string_view> seen;
	for (const auto& d : docs) {
		seen.clear();
		std::string_view t = d.text;
		for (std::size_t a = 0; a < t.size();) {
			std::size_t b = t.find(' ', a);
			if (b == std::string_view::npos) { b = t.size(); }
			if (b > a) { seen.push_back(t.substr(a, b - a)); }
			a = b + 1;
		}
		std::sort(seen.begin(), seen.end());
		seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
		for (auto w : seen) { ++df[w]; }
	}
	std::vector<std::string> terms;
	for (const auto& [w, n] : df) {
		if (n >= 200 && n <= 20000) { terms.emplace_back(w); }
	}
	std::sort(terms.begin(), terms.end());
	std::mt19937_64 rng(seed);
	std::shuffle(terms.begin(), terms.end(), rng);
	std::vector<std::string> out;
	for (std::size_t i = 0; i + 1 < terms.size() && out.size() < 300; i += 2) {
		switch (out.size() % 3) {
		case 0: out.push_back(terms[i]); break;
		case 1: out.push_back("+" + terms[i] + " +" + terms[i + 1]); break;
		default: out.push_back(terms[i] + " " + terms[i + 1]); break;
		}
	}
	return out;
}

void fill(strata::document& d, std::string_view key, std::string_view id, std::string_view text) {
	d.terms.resize(1);
	d.terms[0].term.assign(key);
	d.terms[0].wdf = 0;
	d.terms[0].positions.clear();
	d.text.assign(text);
	d.tokens.clear();
	d.split_text = true;
	d.length = 0;
	d.stored.assign(id);
}

// The pool executor with its checkpoints counted and timed (the gap between two is the
// slice a merge imposes on the pool), and pausable at them (I3). One gap histogram over
// every merge thread; the pause latency is the time from the request to the first merge
// thread parked at a checkpoint.
class metered_executor final : public strata::executor {
public:
	explicit metered_executor(strata::executor& inner) : inner_(&inner) {}
	std::unique_ptr<group> open() override { return std::make_unique<mgroup>(*this, inner_->open()); }
	std::uint64_t now() override { return inner_->now(); }
	bool sleep_until(std::uint64_t t, const task::cancel_token& stop) override {
		last_ = clock::time_point{};  // a sleep is not a slice
		return inner_->sleep_until(t, stop);
	}
	bool checkpoint(const task::cancel_token& stop) override {
		const auto now = clock::now();
		if (last_ != clock::time_point{}) { gap(micros(last_, now)); }
		if (paused_.load(std::memory_order_acquire)) {
			std::unique_lock l(mu_);
			if (paused_.load(std::memory_order_relaxed) && !parked_reported_) {
				parked_reported_ = true;
				pause_latency_.push_back(micros(pause_at_, clock::now()));
			}
			cv_.wait(l, [&] { return !paused_.load(std::memory_order_relaxed) || stop.requested(); });
		}
		last_ = clock::now();
		checkpoints_.fetch_add(1, std::memory_order_relaxed);
		return inner_->checkpoint(stop);
	}
	void pause(bool on) {
		{
			std::lock_guard l(mu_);
			if (on) {
				pause_at_ = clock::now();
				parked_reported_ = false;
			}
			paused_.store(on, std::memory_order_release);
		}
		cv_.notify_all();
	}
	std::uint64_t posted() const noexcept { return posted_.load(std::memory_order_relaxed); }
	std::uint64_t checkpoints() const noexcept { return checkpoints_.load(std::memory_order_relaxed); }
	void report() {
		std::printf("# checkpoints %llu, merges posted %llu, gaps by ms bucket (<1, 1-2, 2-4, ... >=1024):", (unsigned long long)checkpoints(), (unsigned long long)posted());
		for (const auto& b : gaps_) { std::printf(" %llu", (unsigned long long)b.load()); }
		std::printf("\n# gap max %.1f ms\n", gap_max_.load() / 1000.0);
		std::lock_guard l(mu_);
		if (!pause_latency_.empty()) {
			auto v = pause_latency_;
			const auto p = percentiles(v);
			std::printf("# pause latency: n %zu p50 %.1f ms p99 %.1f ms max %.1f ms\n", p.n, p.p50 / 1000, p.p99 / 1000, p.max / 1000);
		}
	}

private:
	class mgroup final : public group {
	public:
		mgroup(metered_executor& e, std::unique_ptr<group> g) : e_(&e), g_(std::move(g)) {}
		void post(work w) override {
			e_->posted_.fetch_add(1, std::memory_order_relaxed);
			g_->post([w = std::move(w)](const task::cancel_token& stop) {
				last_ = clock::time_point{};
				w(stop);
			});
		}
		void cancel() noexcept override { g_->cancel(); }
		void wait() override { g_->wait(); }

	private:
		metered_executor* e_;
		std::unique_ptr<group> g_;
	};
	void gap(double us) {
		std::size_t b = 0;
		for (double ms = us / 1000; ms >= 1 && b + 1 < gaps_.size(); ms /= 2) { ++b; }
		gaps_[b].fetch_add(1, std::memory_order_relaxed);
		std::uint64_t m = gap_max_.load(std::memory_order_relaxed);
		while (m < std::uint64_t(us) && !gap_max_.compare_exchange_weak(m, std::uint64_t(us))) {}
	}
	static thread_local clock::time_point last_;
	strata::executor* inner_;
	std::atomic<std::uint64_t> posted_{0}, checkpoints_{0}, gap_max_{0};
	std::array<std::atomic<std::uint64_t>, 12> gaps_{};
	std::mutex mu_;
	std::condition_variable cv_;
	std::atomic<bool> paused_{false};  // read without the lock, written under it
	bool parked_reported_ = false;
	clock::time_point pause_at_;
	std::vector<double> pause_latency_;
};
thread_local clock::time_point metered_executor::last_{};

// One index's debt as the policy sees it: segments against the tiers' allowance.
struct debt {
	std::size_t segments = 0;
	double allowed = 0;
	std::uint64_t bytes = 0;
	double excess() const { return std::max(0.0, double(segments) - allowed); }
};

debt measure(const strata::index& ix) {
	debt d;
	double bytes = 0, smallest = 0;
	std::size_t eligible = 0;
	const auto& p = ix.policy();
	for (const auto& v : ix.segments()) {
		++d.segments;
		d.bytes += v.bytes;
		const double docs = double(v.seg->core().doc_count());
		const double live = docs ? double(v.bytes) * (docs - double(v.dead)) / docs : 0;
		if (live >= double(p.max_merged_bytes) / 2) { continue; }
		bytes += live;
		smallest = eligible++ ? std::min(smallest, live) : live;
	}
	d.allowed = eligible ? p.allowed_segments(bytes, smallest) : p.segments_per_tier;
	return d;
}

std::vector<strata::merge_policy::candidate> candidates(const strata::index& ix) {
	std::vector<strata::merge_policy::candidate> c;
	for (const auto& v : ix.segments()) { c.push_back({v.id, v.bytes, v.seg->core().doc_count(), v.dead}); }
	return c;
}

double cpu_seconds() {
	rusage u{};
	getrusage(RUSAGE_SELF, &u);
	return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fputs(usage, stderr);
		return 2;
	}
	const std::string dir = argv[1], corpus = argv[2];
	const args a(argc, argv, 3);
	const double rate = a.num("rate", 5000), queries_per_s = a.num("queries", 100), zipf = a.num("zipf", 1.0);
	const std::size_t batch = a.u64("batch", 100), seconds = a.u64("seconds", 120), ndocs = a.u64("docs", 100000), n = std::max<std::uint64_t>(1, a.u64("indexes", 1)), threads = std::max<std::uint64_t>(1, a.u64("threads", 1));
	const std::uint64_t limit = a.u64("limit_mb", 0) << 20, refresh_ms = a.u64("refresh_ms", 1000), seed = a.u64("seed", 7);
	const std::string control = a.get("control", "none");
	const double low = a.num("low", 8), high = a.num("high", 64), floor_rate = a.num("floor_rate", 0.05);
	const bool pause_on_flush = a.u64("pause_on_flush", 0) != 0;
	if (control != "none" && control != "proportional" && control != "feedforward" && control != "refresh" && control != "adaptive" && control != "global" && control != "hot") {
		std::fputs(usage, stderr);
		return 2;
	}

	auto docs = load(corpus, ndocs);
	if (docs.empty()) {
		std::fprintf(stderr, "no documents in %s\n", corpus.c_str());
		return 1;
	}
	const auto queries = make_queries(docs, seed);
	std::vector<engine::parsed> parsed;
	for (const auto& q : queries) { parsed.push_back(engine::parse(q)); }

	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	ThreadPool<> pool("merge{}", threads);
	strata::pool_executor<> pool_exec(pool);
	metered_executor exec(pool_exec);
	strata::rate_limiter limiter(limit);
	strata::byte_budget budget(a.u64("budget_mb", 2048) << 20);
	std::vector<std::unique_ptr<kronuz::journal::PosixIO>> ios;
	std::vector<std::unique_ptr<strata::index>> indexes;
	for (std::size_t i = 0; i < n; ++i) {
		const std::string sub = dir + "/ix" + std::to_string(i);
		std::filesystem::create_directories(sub);
		std::filesystem::permissions(sub, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
		ios.push_back(std::make_unique<kronuz::journal::PosixIO>(sub));
		strata::index::options o;
		o.key_prefix = "Q";
		o.budget = &budget;
		o.buffer_share = a.u64("share_mb", 256) << 20;
		o.auto_merge = control != "global" && control != "hot";
		o.background = &exec;
		o.rate = &limiter;
		o.merge_memory_wait = 0;
		std::array<char, 16> uuid{'n', 'o', 's', 't', 'a', 'l', 'l', 's'};
		uuid[15] = char(i);
		indexes.push_back(std::make_unique<strata::index>(strata::index::create(*ios.back(), o, uuid)));
	}

	// Snapshots the reader searches, published by the writer; heat per index from the reader.
	std::mutex snap_mu;
	std::vector<std::shared_ptr<const strata::index::snapshot>> snaps(n);
	std::vector<std::atomic<std::uint64_t>> heat(n);
	for (auto& h : heat) { h.store(0); }
	std::atomic<bool> stop{false};

	const double window = a.num("window", 1.0);
	const auto t0 = clock::now();
	windows w_sojourn(t0, window), w_service(t0, window), w_query(t0, window);

	// The reader: a query every 1/Q s, on the index Zipf picks, over its latest snapshot.
	std::vector<double> cdf(n);
	{
		double sum = 0;
		for (std::size_t i = 0; i < n; ++i) { sum += 1.0 / std::pow(double(i + 1), zipf); }
		double acc = 0;
		for (std::size_t i = 0; i < n; ++i) {
			acc += 1.0 / std::pow(double(i + 1), zipf) / sum;
			cdf[i] = acc;
		}
	}
	std::thread reader([&] {
		std::mt19937_64 rng(seed + 1);
		std::uniform_real_distribution<double> u(0, 1);
		for (std::uint64_t j = 0; !stop.load(std::memory_order_relaxed); ++j) {
			const auto due = t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(j) / queries_per_s));
			wait_until(due);
			if (stop.load(std::memory_order_relaxed)) { break; }
			const std::size_t which = std::size_t(std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin());
			std::shared_ptr<const strata::index::snapshot> s;
			{
				std::lock_guard l(snap_mu);
				s = snaps[std::min(which, n - 1)];
			}
			const auto q0 = clock::now();
			if (s) {
				std::vector<strata::segment_snapshot> segs;
				for (const auto& v : s->segs) { segs.push_back({v.seg, v.deleted, v.dead}); }
				engine::search_snapshot(segs, parsed[j % parsed.size()], 10);
			}
			heat[std::min(which, n - 1)].fetch_add(1, std::memory_order_relaxed);
			w_query.add(clock::now(), micros(q0, clock::now()));  // the query's own time: the reader never waits for the writer
		}
	});

	// The writer: transactions of `batch` documents, round robin over the indexes, each due
	// at its offered time; the controllers run at every refresh tick.
	std::printf("# research_ingest rate=%g batch=%zu seconds=%zu docs=%zu indexes=%zu threads=%zu limit_mb=%llu refresh_ms=%llu queries=%g control=%s low=%g high=%g pause_on_flush=%d zipf=%g window=%g\n",
		rate, batch, seconds, docs.size(), n, threads, (unsigned long long)(limit >> 20), (unsigned long long)refresh_ms, queries_per_s, control.c_str(), low, high, int(pause_on_flush), zipf, window);
	std::printf("win\toffered\tdone\tw_n\tw_p50\tw_p99\tw_p999\tw_max\ts_n\ts_p50\ts_p99\ts_p999\ts_max\tq_n\tq_p50\tq_p99\tq_p999\tq_max\tsegs\tsegs_max\tallowed\texcess\tpending\tmerge_mb\tflush_ms\trate_cap\twidth\tcpu_s\trefresh\n");
	std::fflush(stdout);

	strata::transaction t;
	std::uint64_t k = 0, done = 0, offered = 0, round = 0, cursor = 0;
	double rate_cap = rate;
	std::size_t width = 8;
	std::size_t reported = 0;
	double flush_ms = 0, last_cpu = cpu_seconds();
	auto last_tick = t0, last_admit = t0;
	// Each index refreshes every refresh_ms, staggered: one index per refresh_ms / n.
	std::vector<clock::time_point> last_refresh(n, t0);
	std::size_t next_refresh = 0;
	const std::size_t slack = a.u64("queue", 1);  // merges queued beyond the pool's threads (global, hot)
	std::vector<double> ewma(n, 0);
	// Feed-forward admission (I1's design form) and the adaptive refresh.
	double refresh_eff = double(refresh_ms);  // the refresh interval in force (ms)
	double bytes_per_doc = 0, merged_rate = 0, last_merged = 0;
	auto last_merge_tick = t0;
	auto publish = [&](std::size_t i) {
		auto s = indexes[i]->take_snapshot();
		std::lock_guard l(snap_mu);
		snaps[i] = std::move(s);
	};
	auto report = [&](std::size_t upto) {
		for (; reported < upto; ++reported) {
			std::size_t segs = 0, segs_max = 0, pending = 0;
			double allowed = 0, excess = 0;
			for (const auto& ix : indexes) {
				const auto d = measure(*ix);
				segs += d.segments;
				segs_max = std::max(segs_max, d.segments);
				allowed += d.allowed;
				excess += d.excess();
				pending += ix->merges_pending();
			}
			const double cpu = cpu_seconds();
			std::printf("%zu\t%llu\t%llu", reported, (unsigned long long)offered, (unsigned long long)done);
			print_pct(w_sojourn.take(reported));
			print_pct(w_service.take(reported));
			print_pct(w_query.take(reported));
			std::printf("\t%zu\t%zu\t%.0f\t%.0f\t%zu\t%.1f\t%.0f\t%.0f\t%zu\t%.2f\t%.0f\n", segs, segs_max, allowed, excess, pending, double(limiter.admitted()) / double(1 << 20), flush_ms, rate_cap, width, cpu - last_cpu, refresh_eff);
			last_cpu = cpu;
			flush_ms = 0;
			std::fflush(stdout);
		}
	};
	const auto end = t0 + std::chrono::seconds(seconds);
	while (clock::now() < end) {
		const auto due = rate > 0 ? t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(k) * double(batch) / rate)) : clock::now();
		wait_until(due);
		const auto arrive = std::max(due, clock::now());
		// Admission (I1): transactions spaced at the capped rate; never stopped.
		if ((control == "proportional" || control == "feedforward") && rate_cap < rate) {
			const auto gap = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(batch) / rate_cap));
			wait_until(last_admit + gap);
		}
		last_admit = clock::now();
		const std::size_t i = std::size_t(k % n);
		t.ops.resize(batch);
		for (std::size_t j = 0; j < batch; ++j) {
			auto& op = t.ops[j];
			op.type = strata::operation::replace_key;
			const doc& d = docs[cursor];
			op.key.assign("Q");
			op.key.append(d.id);
			if (round) {
				op.key.push_back('~');
				op.key.append(std::to_string(round));
			}
			fill(op.doc, op.key, d.id, d.text);
			if (++cursor == docs.size()) {
				cursor = 0;
				++round;
			}
		}
		if (indexes[i]->apply(t, {}).result != strata::verdict::accepted) {
			std::fprintf(stderr, "rejected at transaction %llu\n", (unsigned long long)k);
			return 1;
		}
		const auto finished = clock::now();
		w_sojourn.add(finished, micros(due, finished));
		w_service.add(finished, micros(arrive, finished));
		done += batch;
		++k;
		offered = rate > 0 ? std::max(done, std::uint64_t(secs(t0, finished) * rate)) : done;
		publish(i);

		if (secs(last_refresh[next_refresh], clock::now()) * 1000 >= refresh_eff) {
			const std::size_t j = next_refresh;
			next_refresh = (next_refresh + 1) % n;
			last_refresh[j] = clock::now();
			if (pause_on_flush) { exec.pause(true); }
			const auto f0 = clock::now();
			indexes[j]->refresh();
			publish(j);
			flush_ms += micros(f0, clock::now()) / 1000;
			if (pause_on_flush) { exec.pause(false); }
		}
		if (secs(last_tick, clock::now()) * 1000 >= double(refresh_ms)) {
			last_tick = clock::now();

			// The controllers, once per refresh interval.
			double excess = 0;
			std::size_t segs = 0;
			for (const auto& ix : indexes) {
				const auto d = measure(*ix);
				excess += d.excess();
				segs += d.segments;
			}
			if (control == "proportional") {
				const double x = std::clamp((excess - low) / (high - low), 0.0, 1.0);
				rate_cap = rate * std::max(floor_rate, 1.0 - x);
			} else if (control == "feedforward") {
				// The cap from measured capacity: the merge bytes per second the host delivered
				// (an EWMA over ticks) sustain merged / rewrites of flushed bytes, which at the
				// measured bytes per document is a document rate; above `low` excess the cap
				// also drains, down to half that rate at `high`. Bytes per document are read
				// off the first tick with segments (before any merge output exists).
				const double merged = double(limiter.admitted());
				const double dt = secs(last_merge_tick, clock::now());
				last_merge_tick = clock::now();
				merged_rate = 0.7 * merged_rate + 0.3 * ((merged - last_merged) / std::max(dt, 1e-3));
				last_merged = merged;
				std::uint64_t on_disk = 0;
				for (const auto& ix : indexes) {
					for (const auto& v : ix->segments()) { on_disk += v.bytes; }
				}
				if (bytes_per_doc == 0 && merged == 0 && done > 0 && on_disk > 0) { bytes_per_doc = double(on_disk) / double(done); }
				if (bytes_per_doc > 0 && merged > 0) {
					const double rewrites = std::max(1.0, merged / (bytes_per_doc * double(done)));
					const double sustainable = merged_rate / rewrites / bytes_per_doc;
					const double x = std::clamp((excess - low) / (high - low), 0.0, 1.0);
					rate_cap = excess < low ? rate : std::clamp(sustainable * (1.0 - 0.5 * x), floor_rate * rate, rate);
				}
			} else if (control == "refresh") {
				// The segment rate, not the document rate, is what merges must retire: a refresh
				// a second makes a segment a second whatever is admitted. Past `low` excess the
				// interval stretches, one second per 8 segments of excess, to 16 s at most, so
				// visibility latency pays instead of writers.
				refresh_eff = double(refresh_ms) * std::clamp(1.0 + (excess - low) / 8.0, 1.0, 16.0);
			} else if (control == "adaptive") {
				const double x = std::clamp((double(segs) - low) / (high - low), 0.0, 1.0);
				width = std::size_t(8 + std::lround(x * 22));  // 8 (Lucene's) to 30 (its forced width)
				for (auto& ix : indexes) {
					ix->policy().segments_per_tier = double(width);
					ix->policy().max_merge_at_once = width;
				}
			} else if (control == "global" || control == "hot") {
				// The host's merge scheduler: every index's policy merges, ranked, the pool's
				// width in flight at most. `global` ranks by bytes written per segment
				// retired (cheapest first); `hot` by that over the index's query heat, with an
				// index past four times its allowance merged first whatever its heat.
				struct cand {
					double priority;
					std::size_t index;
					std::vector<std::uint64_t> ids;
				};
				std::vector<cand> all;
				std::size_t in_flight = 0;
				for (std::size_t j = 0; j < n; ++j) {
					in_flight += indexes[j]->merges_pending();
					ewma[j] = 0.8 * ewma[j] + 0.2 * double(heat[j].exchange(0));
					const auto d = measure(*indexes[j]);
					for (auto& m : indexes[j]->policy().find_merges(candidates(*indexes[j]))) {
						double bytes = 0;
						for (const auto& v : indexes[j]->segments()) {
							if (std::find(m.begin(), m.end(), v.id) != m.end()) { bytes += double(v.bytes); }
						}
						const double retired = double(std::max<std::size_t>(1, m.size() - 1));
						double priority = bytes / retired;  // lower runs first
						if (control == "hot") {
							const bool overflow = double(d.segments) > 4 * d.allowed;
							priority = overflow ? -1.0 / (1 + bytes) : priority / (1e-3 + ewma[j]);
						}
						all.push_back({priority, j, std::move(m)});
					}
				}
				std::sort(all.begin(), all.end(), [](const cand& x, const cand& y) { return x.priority < y.priority; });
				std::vector<bool> busy(n, false);
				for (std::size_t j = 0; j < n; ++j) { busy[j] = indexes[j]->merges_pending() > 0; }
				for (const auto& c : all) {
					if (in_flight >= threads + slack) { break; }
					if (busy[c.index]) { continue; }  // one merge per index at a time (per_group 1)
					indexes[c.index]->schedule_merge(c.ids);
					busy[c.index] = true;
					++in_flight;
				}
			}
			report(std::size_t(secs(t0, clock::now()) / window));
		}
	}
	stop.store(true);
	reader.join();
	if (pause_on_flush) { exec.pause(false); }
	report(std::size_t(secs(t0, clock::now()) / window) + 1);
	const auto w0 = clock::now();
	for (auto& ix : indexes) { ix->wait_merges(); }
	for (auto& ix : indexes) { ix->commit(); }
	std::size_t segs = 0;
	std::uint64_t bytes = 0;
	for (const auto& ix : indexes) {
		for (const auto& v : ix->segments()) {
			++segs;
			bytes += v.bytes;
		}
	}
	std::printf("# end: %llu documents in %llu transactions; %zu segments, %.1f MB on disk, %.1f MB merged; drain and commit %.1f s\n",
		(unsigned long long)done, (unsigned long long)k, segs, double(bytes) / double(1 << 20), double(limiter.admitted()) / double(1 << 20), secs(w0, clock::now()));
	exec.report();
	indexes.clear();
	return 0;
}
