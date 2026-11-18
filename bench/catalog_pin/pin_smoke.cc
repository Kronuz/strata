// A smoke timing of the catalog's hot path (cold-tier D4; item 5b): one pin and unpin by key and one by handle
// on the single hottest key, single-threaded, the slot `open`. Built against two checkouts' headers (-I) to
// compare a branch with its parent; STRATA_ITEM5B selects the host's item 5b spelling.
//   pin_smoke [rounds] [pairs per round]   default: 11 rounds of 2,000,000
#include "strata/core/catalog.hh"
#include "strata/core/executor.hh"
#include "strata/core/faultfs.hh"
#include "strata/core/index.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace strata;
using kv_index = basic_index<document_layout>;

namespace {

struct budget_system {
	static resource_system make() {
		resource_system s;
		s.get_limits = [] { return resource_limits{1 << 20, 1 << 20}; };
		s.set_soft = [](std::uint64_t) { return false; };
		return s;
	}
};

std::uint64_t sink = 0;

}  // namespace

int main(int argc, char** argv) {
	const int rounds = argc > 1 ? std::atoi(argv[1]) : 11;
	const long pairs = argc > 2 ? std::atol(argv[2]) : 2'000'000;
	faultfs::filesystem fs;
	faultfs::io fio{fs};
	fs.map_files(true);
	kv_index::options o;
	o.keyed = true;
	{
		std::array<char, 16> uuid{};
		auto ix = kv_index::create(fio, o, uuid);
		ix.close();
	}
	resource_options ro;
	ro.system = budget_system::make();
	ro.descriptor_reserve = 64;
	resource_budget budget(ro);
	inline_executor exec;
	using cat = catalog<kv_index>;
	cat::host h;
	h.exec = &exec;
	h.budget = &budget;
	h.index_options = o;
	h.timer = [](std::uint64_t, std::function<void()> fn) { fn(); };  // never armed: nothing here fails
#ifdef STRATA_ITEM5B
	h.resolve = [&](const cat::opening& op) -> std::unique_ptr<kronuz::journal::IO> {
		return counted_io::make(op.counts, [&] { return std::unique_ptr<kronuz::journal::IO>(new faultfs::io(fs)); });
	};
#else
	h.resolve = [&](std::string_view, io_counts c) -> std::unique_ptr<kronuz::journal::IO> {
		return counted_io::make(c, [&] { return std::unique_ptr<kronuz::journal::IO>(new faultfs::io(fs)); });
	};
#endif
	cat c(std::move(h));
	std::vector<double> by_key, by_handle;
	const auto body = [&]() -> task::task<void> {
		cat::handle hd;
		{
			auto p = co_await c.pin_request("hot");
			hd = p.where();
		}
		using clk = std::chrono::steady_clock;
		for (int r = 0; r < rounds + 1; ++r) {  // round 0 warms
			auto t0 = clk::now();
			for (long i = 0; i < pairs; ++i) {
				auto p = co_await c.pin_request("hot");
				sink += p.where().slot;
			}
			auto t1 = clk::now();
			for (long i = 0; i < pairs; ++i) {
				auto p = co_await c.repin(hd, "hot");
				sink += p.where().slot;
			}
			auto t2 = clk::now();
			if (r) {
				by_key.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / double(pairs));
				by_handle.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count() / double(pairs));
			}
		}
	};
	task::run(exec.scheduler(), body());
	std::sort(by_key.begin(), by_key.end());
	std::sort(by_handle.begin(), by_handle.end());
	std::printf("by key    ns/pin+unpin: min %.2f median %.2f max %.2f\n", by_key.front(), by_key[by_key.size() / 2], by_key.back());
	std::printf("by handle ns/pin+unpin: min %.2f median %.2f max %.2f\n", by_handle.front(), by_handle[by_handle.size() / 2], by_handle.back());
	std::printf("(sink %llu)\n", (unsigned long long)sink);
	cat* cp = &c;
	task::run(exec.scheduler(), [cp]() -> task::task<void> { (void)co_await cp->shutdown(); }());
	return 0;
}
