// install() leaves the target mapping only files it owns. The target's segments were mapped from
// the source's files; a live source that merges, releases the checkpoint and reclaims truncates
// them (the mapped run, STRATA_FAULTFS_MAP=1, aborts on a truncation below a live mapping).
//   1. Reads of the installed content, deletes included, survive the source's reclaim, and so do
//      a view taken after the install and a guard held across it.
//   2. The target reopens to the same content once the source is gone.
#include "kv_test.hh"

#include <map>
#include <random>

namespace {

using model_t = std::map<std::string, std::string>;

template <class R>
model_t scanned(const R& r) {
	model_t out;
	r.scan("", [&](std::string_view k, std::string_view v, std::uint64_t) { out.emplace(std::string(k), std::string(v)); });
	return out;
}

kv_index::options base() {
	kv_index::options o;
	o.keyed = true;
	o.reclaim_step = 4096;
	return o;
}

}  // namespace

int main() {
	faultfs::filesystem fss, fst;
	faultfs::io ios(fss), iot(fst);
	auto so = base();
	auto S = kv_index::create(ios, so, uuid_of("S"));
	std::string big(600, ' ');
	std::mt19937_64 rnd(3);
	for (auto& c : big) { c = char('a' + rnd() % 26); }
	model_t model;
	for (int s = 0; s < 3; ++s) {
		for (int i = 0; i < 40; ++i) {
			const std::string k = "k" + std::to_string(s * 40 + i);
			CHECK(S.apply(tx({replace(k, big)}), {}).result == verdict::accepted);
			model[k] = big;
		}
		S.commit();
	}
	for (const char* k : {"k3", "k45"}) {  // live-docs generations in the checkpoint
		CHECK(S.apply(tx({erase(k)}), {}).result == verdict::accepted);
		model.erase(k);
	}
	S.commit();
	const std::uint64_t cp = S.checkpoint();

	reader_registry reg(4);
	{
	auto to = base();
	to.readers = &reg;
	auto T = kv_index::create(iot, to, uuid_of("T"));
	CHECK(T.apply(tx({replace("kold", "x")}), {}).result == verdict::accepted);
	const model_t old_content{{"kold", "x"}};
	const auto h = T.reader();
	const auto before = h.acquire();  // a guard across the install holds the target's old content
	T.install(ios, cp);
	CHECK(scanned(*before) == old_content);
	CHECK(scanned(T) == model && T.get("k3") == std::nullopt && T.get("k5") == std::optional<std::string>(big));
	const auto after = h.acquire();
	CHECK(scanned(*after) == model);

	// the source moves on and gives up the checkpoint
	S.force_merge(1);
	S.commit();
	S.release(cp);
	S.commit();
	S.reclaim();

	CHECK(scanned(T) == model && T.get("k5") == std::optional<std::string>(big) && T.get("k45") == std::nullopt);
	CHECK(scanned(*after) == model);
	CHECK(scanned(*before) == old_content);

	T.commit();
	}
	auto reopened = kv_index::open(iot, base());
	CHECK(scanned(reopened) == model);
	return failures != 0;
}
