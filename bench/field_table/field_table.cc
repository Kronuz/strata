// T8 (fields-and-keys): bytes written to fields-* files, and the table's resident bytes, after
// declaring N fields one per commit. Bytes are counted, not timed: `field_table N...`.
//   field_table 2000 100000
// Written bytes are the sum, over every fields file the run created, of the file's largest size
// (the files only grow); resident bytes are the heap the table holds, from a counting allocator.
#include "strata/core/faultfs.hh"
#include "strata/core/index.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <string>

#if defined(__APPLE__)
#include <malloc/malloc.h>
static std::size_t usable(void* p) { return malloc_size(p); }
#else
#include <malloc.h>
static std::size_t usable(void* p) { return malloc_usable_size(p); }
#endif

static std::atomic<long long> live{0};
void* operator new(std::size_t n) {
	void* p = std::malloc(n ? n : 1);
	if (!p) { throw std::bad_alloc(); }
	live += (long long)usable(p);
	return p;
}
void operator delete(void* p) noexcept {
	if (p) { live -= (long long)usable(p); }
	std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

using namespace strata;
using index_t = basic_index<document_layout>;

int main(int argc, char** argv) {
	for (int a = 1; a < argc; ++a) {
		const std::size_t n = std::strtoull(argv[a], nullptr, 10);
		faultfs::filesystem fs;
		faultfs::io io{fs};
		index_t::options o;
		o.keyed = true;
		o.auto_merge = false;
		auto ix = index_t::create(io, o, std::array<char, 16>{'t', '8'});
		std::map<std::string, std::uint64_t> largest;
		const long long before = live.load();
		for (std::size_t i = 0; i < n; ++i) {
			index_t::transaction t;
			t.fields = {{"field-" + std::to_string(i), field_role::indexed, field_postings::freqs, false}};
			ix.apply(t, {});
			ix.commit();
			for (const auto& name : fs.list()) {
				if (auto p = index_detail::parse_name(name); p && p->type == index_detail::parsed_name::fields) {
					auto& m = largest[name];
					m = std::max<std::uint64_t>(m, fs.view(name).size());
				}
			}
		}
		const long long resident = live.load() - before;
		std::uint64_t written = 0;
		for (const auto& [name, size] : largest) { written += size; }
		const std::uint64_t final_size = files::encode_fields(ix.fields()).size();
		std::printf("fields %zu: written %llu B in %zu file(s), final table file %llu B (%.2fx); table resident ~%lld B (%.1f B a field)\n", n,
			(unsigned long long)written, largest.size(), (unsigned long long)final_size, double(written) / double(final_size), resident - 0, double(resident) / double(n));
	}
	return 0;
}
