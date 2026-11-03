// Fixtures for the catalog's tests (docs/design/cold-tier.md D4; S4): directories on faultfs, each a
// committed index; a kernel model and a budget behind counted_io; a catalog over them.
#pragma once

#include "counted_hooks.hh"
#include "cold_hooks.hh"
#include "strata/core/catalog.hh"
#include "strata/core/executor.hh"

#include <deque>
#include <map>

namespace {

struct payload {
	int serial = 0;
};

using cat_t = strata::catalog<kv_index, payload>;

// `n` directories "k0".."k<n-1>" each holding a committed index of `segments` segments.
template <class Exec>
struct world {
	struct dir {
		faultfs::filesystem fs;
		faultfs::io io{fs};
		dir() { fs.map_files(true); }
	};
	std::deque<dir> dirs;
	kernel kern;
	resource_budget budget;
	Exec exec;
	std::map<std::string, int, std::less<>> alias;  // key -> directory, when it is not "k<N>"
	std::atomic<int> resolves{0}, made{0};
	std::atomic<int> fail_resolve{0};  // errno the next resolves fail with (while > 0)
	std::atomic<int> fail_left{0};     // resolves left that fail
	kv_index::options iopts;
	std::uint64_t soft_open = 0, soft_hot = 0;  // the policy's caps (D6); 0: none
	std::unique_ptr<cat_t> cat;

	static resource_options ropts() {
		resource_options o;
		o.system = test_system(1 << 20, 1 << 20, 1 << 20);
		o.descriptor_reserve = 64;
		return o;
	}
	world(std::size_t n, std::size_t segments, bool defer_io, bool eager = true, std::function<void(const char*, bool)> hook = {}) : budget(ropts()) {
		iopts.keyed = true;
		iopts.eager_segments = eager;
		iopts.auto_merge = false;
		iopts.defer_io = defer_io;
		for (std::size_t i = 0; i < n; ++i) {
			dirs.emplace_back();
			auto o = iopts;
			o.readers = nullptr;
			auto ix = kv_index::create(dirs.back().io, o, uuid_of(("cat" + std::to_string(i)).c_str()));
			fill(ix, segments);
			ix.close();
		}
		make_catalog(std::move(hook));
	}
	void make_catalog(std::function<void(const char*, bool)> hook) {
		typename cat_t::host h;
		h.exec = &exec;
		h.budget = &budget;
		h.index_options = iopts;
		h.soft_open = soft_open;
		h.soft_hot = soft_hot;
		h.before_cas = std::move(hook);
		h.resolve = [this](std::string_view key, io_counts c) -> std::unique_ptr<kronuz::journal::IO> {
			++resolves;
			if (fail_left.load() > 0 && fail_left.fetch_sub(1) > 0) { throw std::system_error(fail_resolve.load(), std::generic_category(), "resolve"); }
			int d;
			if (auto it = alias.find(key); it != alias.end()) {
				d = it->second;
			} else {
				d = std::atoi(std::string(key.substr(1)).c_str());
			}
			return counted_io::make(c, [&] { return probe_io::make(kern, dirs[d].io); });
		};
		h.make_payload = [this](std::string_view) { return payload{++made}; };
		cat = std::make_unique<cat_t>(std::move(h));
	}
	// A new catalog with the policy's caps and hook; nothing may be open.
	void remake(std::uint64_t so, std::uint64_t sh, std::function<void(const char*, bool)> hook = {}) {
		drain();
		cat.reset();
		soft_open = so, soft_hot = sh;
		make_catalog(std::move(hook));
	}
	~world() {
		// Close what is open so the destructors find no live index.
		drain();
	}
	// Closes every idle slot; what is pinned stays (a test bug).
	void drain() {
		for (int round = 0; round < 8; ++round) {
			bool any = false;
			cat->wait_chores();
			for (std::uint32_t i = 0; i < cat->slots_made(); ++i) {
				const auto st = cat_t::state_of(cat->word_of(i));
				if (st == slot_state::open) { any |= cat->begin_demote(i); }
				if (st == slot_state::owed) { any |= cat->begin_close(i); }
			}
			cat->wait_chores();
			for (std::uint32_t i = 0; i < cat->slots_made(); ++i) {
				if (cat_t::state_of(cat->word_of(i)) == slot_state::open) { any |= cat->begin_close(i); }
			}
			cat->wait_chores();
			if (!any) { break; }
		}
	}
};

}  // namespace
