// The cold tier's step 2, item 6 (docs/design/cold-tier.md D6; CT-R10, CT-R31, CT-R41; criterion S8): the
// catalog's S3-FIFO over the model's trace with stub indexes. Every request takes two pins (a use by key, a
// re-pin by handle); the stubs run under defer_io, so the sweep demotes them to `owed` over soft_hot and most
// hits wake one; transitions run on the manual executor, drained every 4 requests, so uses meet `opening`,
// `waking`, `demoting` and `closing` as waiters; a pass runs after every miss and after every drain.
//   catalog_s8_test [CELL...]   CELL: t099 t099s t12 t12s (the gated cells), real099 real12 (the real limits,
//   ungated), model (also run the reference model and check it against the criterion's numbers); no CELL: the four
//   gated cells. S8_SCALE=k divides the key space, capacity, hot cap and trace by k (smoke).
//   S8_EVERY_PIN_USE=1 makes the re-pin a use too (the harness mutant of S8.5).
#include "counted_hooks.hh"
#include "kv_test.hh"
#include "s3fifo_model.hh"
#include "strata/core/catalog.hh"
#include "strata/core/executor.hh"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {

using strata::catalog;
using strata::pin_kind;
using strata::slot_state;
using S = slot_state;

thread_local io_counts last_counts;

// An index with nothing in it: hot while it holds a map, warm (owed) once demoted, hot again when promoted.
struct stub_index {
	struct options {};
	using reader_handle = int;
	explicit stub_index(io_counts c) : counts_(c) { hold(); }
	static stub_index open(kronuz::journal::IO&, const options&) { throw std::logic_error("the host opens stubs"); }
	stub_index(stub_index&&) noexcept = default;
	reader_handle reader() { return 0; }
	const write_lease& lease() const noexcept { return lease_; }
	bool demote() {
		map_ = resource_reservation();
		owed_ = true;
		return true;
	}
	bool promote_owed() const noexcept { return owed_; }
	void promote() {
		if (owed_) {
			hold();
			owed_ = false;
		}
	}
	bool close_ready() { return true; }
	void close() { map_ = resource_reservation(); }
	bool closing() const noexcept { return false; }

private:
	void hold() { map_ = counts_.budget->reserve(resource::maps, 1, counts_.catalog, counts_.slot); }
	write_lease lease_;
	resource_reservation map_;
	io_counts counts_;
	bool owed_ = false;
};

using cat_t = catalog<stub_index>;

struct config {
	double theta = 0.99;
	bool scan = false;
	std::size_t keys = 1'000'000, cap = 110'000, hot = 10'000;
	std::uint64_t warm = 3'000'000, measured = 8'000'000, first_n = 1'000'000;
	bool lifted = true;
	bool every_pin_use = false;
	const char* name = "";
};

struct run_result {
	double all = 0, first = 0;
	std::uint64_t requests = 0, passes = 0, closes = 0, peak_resident = 0, peak_over = 0;
	cat_t::stats_t stats;
	double seconds = 0;
};

task::task<void> request(cat_t& cat, std::string key, bool second_use) {
	auto p = co_await cat.pin_request(key, true);
	auto q = co_await cat.repin(p.where(), key, pin_kind::request, second_use);
}

run_result run_catalog(const s8::trace& tr, const config& cfg) {
	const auto t0 = std::chrono::steady_clock::now();
	faultfs::filesystem fs;
	resource_options ro;
	ro.system = test_system(1 << 24, 1 << 24, 1 << 24);
	ro.descriptor_reserve = 64;
	resource_budget budget(ro);
	inline_executor exec;
	cat_t::host h;
	h.exec = &exec;
	h.budget = &budget;
	h.soft_open = cfg.cap;
	h.soft_hot = cfg.hot;
	h.resolve = [&](std::string_view, io_counts c) -> std::unique_ptr<kronuz::journal::IO> {
		last_counts = c;
		return std::make_unique<faultfs::io>(fs);
	};
	h.open_index = [](kronuz::journal::IO&, const stub_index::options&) { return stub_index(last_counts); };
	cat_t cat(std::move(h));
	const auto lim = cfg.lifted ? cat_t::policy_limits::lifted() : cat_t::policy_limits{};

	run_result out;
	std::uint64_t hits = 0, first = 0;
	std::vector<task::task<void>> live;
	const task::scheduler_ref sched = task::scheduler_ref::of(exec.scheduler());
	auto pass = [&] {
		cat.policy_pass(lim);
		++out.passes;
		const auto qs = cat.queue_sizes();
		const std::uint64_t resident = qs.small + qs.main;
		out.peak_resident = std::max(out.peak_resident, resident);
		out.peak_over = std::max(out.peak_over, resident > cfg.cap ? resident - cfg.cap : 0);
	};
	for (std::size_t i = 0; i < tr.keys.size(); ++i) {
		const std::uint64_t before = cat.misses();
		{
			const task::running_on on(sched);
			live.push_back(request(cat, "k" + std::to_string(tr.keys[i]), cfg.every_pin_use));
			live.back().handle().resume();
		}
		const bool miss = cat.misses() != before;
		if (i >= tr.measure_from) {
			hits += !miss;
			if (i < tr.measure_from + cfg.first_n) { first += !miss; }
		}
		if (miss) { pass(); }
		if ((i & 3) == 3) {
			exec.run();
			live.clear();
			pass();
		}
	}
	exec.run();
	live.clear();
	out.requests = tr.keys.size() - tr.measure_from;
	out.all = double(hits) / double(out.requests);
	out.first = double(first) / double(std::min<std::uint64_t>(out.requests, cfg.first_n));
	out.stats = cat.stats();
	out.closes = out.stats.evictions;
	// Close what is open so the destructors find no live index.
	for (int round = 0; round < 8; ++round) {
		bool any = false;
		for (std::uint32_t i = 0; i < cat.slots_made(); ++i) {
			const auto st = cat_t::state_of(cat.word_of(i));
			if (st == S::open) { any |= cat.begin_demote(i); }
			if (st == S::owed) { any |= cat.begin_close(i); }
		}
		exec.run();
		if (!any) { break; }
	}
	for (std::uint32_t i = 0; i < cat.slots_made(); ++i) {
		if (cat_t::state_of(cat.word_of(i)) == S::open) { cat.begin_close(i); }
	}
	exec.run();
	out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	return out;
}

double run_model(const s8::trace& tr, std::size_t cap, std::uint64_t first_n, double& first_out) {
	s8::model m(cap);
	std::uint64_t hits = 0, first = 0;
	for (std::size_t i = 0; i < tr.keys.size(); ++i) {
		const bool h = m.access(tr.keys[i]);
		if (i >= tr.measure_from) {
			hits += h;
			if (i < tr.measure_from + first_n) { first += h; }
		}
	}
	const std::uint64_t n = tr.keys.size() - tr.measure_from;
	first_out = double(first) / double(std::min<std::uint64_t>(n, first_n));
	return double(hits) / double(n);
}

void report(const config& c, const run_result& o) {
	const auto& s = o.stats;
	std::printf("%-8s theta=%.2f scan=%d %s: all %.4f first %.4f | %llu requests, %llu passes, %llu closes, peak resident %llu (over cap by %llu) | "
	            "promoted %llu decayed %llu demotes %llu busy %llu ghost hits %llu | met opening %llu waking %llu demoting %llu closing %llu | wakes %llu | %.1f s\n",
		c.name, c.theta, c.scan, c.lifted ? "lifted" : "real", o.all, o.first, (unsigned long long)o.requests, (unsigned long long)o.passes,
		(unsigned long long)o.closes, (unsigned long long)o.peak_resident, (unsigned long long)o.peak_over, (unsigned long long)s.promoted,
		(unsigned long long)s.decayed, (unsigned long long)s.evict_demotes, (unsigned long long)s.busy, (unsigned long long)s.ghost_hits,
		(unsigned long long)s.waited[unsigned(S::opening)], (unsigned long long)s.waited[unsigned(S::waking)], (unsigned long long)s.waited[unsigned(S::demoting)],
		(unsigned long long)s.waited[unsigned(S::closing)], (unsigned long long)s.wakes_posted, o.seconds);
	std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
	std::size_t scale = 1;
	if (const char* e = std::getenv("S8_SCALE")) { scale = std::max<std::size_t>(1, std::strtoull(e, nullptr, 10)); }
	const bool mutant_pin = std::getenv("S8_EVERY_PIN_USE") != nullptr;
	std::vector<std::string> cells(argv + 1, argv + argc);
	bool with_model = false;
	std::erase_if(cells, [&](const std::string& c) { return c == "model" && (with_model = true); });
	if (cells.empty()) { cells = {"t099", "t099s", "t12", "t12s"}; }
	for (const std::string& cell : cells) {
		config c;
		c.name = cell.c_str();
		c.keys /= scale, c.cap /= scale, c.hot /= scale, c.warm /= scale, c.measured /= scale, c.first_n /= scale;
		c.every_pin_use = mutant_pin;
		if (cell.rfind("t099", 0) == 0) {
			c.theta = 0.99, c.scan = cell.back() == 's';
		} else if (cell.rfind("t12", 0) == 0) {
			c.theta = 1.2, c.scan = cell.back() == 's';
		} else if (cell == "real099") {
			c.theta = 0.99, c.lifted = false;
		} else if (cell == "real12") {
			c.theta = 1.2, c.lifted = false;
		} else {
			std::fprintf(stderr, "unknown cell %s\n", cell.c_str());
			return 2;
		}
		const s8::trace tr(c.keys, c.theta, c.warm, c.measured, c.scan);
		const run_result o = run_catalog(tr, c);
		report(c, o);
		const s8::reference* ref = nullptr;
		for (const auto& r : s8::references) {
			if (r.theta == c.theta && r.scan == c.scan) { ref = &r; }
		}
		if (with_model) {
			double first;
			const double all = run_model(tr, c.cap, c.first_n, first);
			std::printf("  model: all %.4f first %.4f (criterion %.4f %.4f)\n", all, first, ref->all, ref->first);
			if (scale == 1) {
				CHECK(std::fabs(all - ref->all) < 0.0005 && std::fabs(first - ref->first) < 0.0005);
			}
			CHECK(std::fabs(o.all - all) <= 0.005 && std::fabs(o.first - first) <= 0.005);
		} else if (scale == 1 && c.lifted) {
			std::printf("  vs reference: all %+.4f first %+.4f\n", o.all - ref->all, o.first - ref->first);
			CHECK(std::fabs(o.all - ref->all) <= 0.005);
			CHECK(std::fabs(o.first - ref->first) <= 0.005);
		}
		if (c.lifted) {
			CHECK(o.stats.waited[unsigned(S::opening)] > 0 && o.stats.waited[unsigned(S::waking)] > 0 && o.stats.waited[unsigned(S::demoting)] > 0 && o.stats.waited[unsigned(S::closing)] > 0);
			CHECK(o.peak_resident <= c.cap + 16);
		}
	}
	if (failures) {
		std::fprintf(stderr, "catalog_s8: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("catalog_s8: ok");
	return 0;
}
