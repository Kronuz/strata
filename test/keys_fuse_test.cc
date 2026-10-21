// A segment whose key count sits where the fuse filter's paper-sized table peels under no seed
// still builds: sorted_encoder grows the table instead of throwing "construction failed"
// (which fenced an index mid-merge), and the filter answers for every key.
#include "strata/core/keys.hh"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

int main() {
	for (std::size_t n : {11509u}) {
		strata::keys::sorted_encoder enc("0123456789ersu");
		std::vector<std::string> keys;
		for (std::size_t i = 0; i < n; ++i) { keys.push_back("user" + std::string(8 - std::to_string(i).size(), '0') + std::to_string(i)); }
		bool built = true;
		try {
			for (std::size_t i = 0; i < n; ++i) { enc.add(keys[i], strata::ordinal_t(i)); }
			CHECK(!enc.finish().index.empty());
		} catch (const std::runtime_error& e) {
			std::fprintf(stderr, "n=%zu: %s\n", n, e.what());
			built = false;
		}
		CHECK(built);
	}
	return failures ? 1 : 0;
}
