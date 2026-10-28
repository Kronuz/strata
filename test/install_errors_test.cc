// install()'s reopen of the copies it wrote (docs/reviews/install-architect.md, C2 to C4).
//   1. A host error (EMFILE from open, ENOMEM from map) reopening a copy propagates as itself,
//      leaves the target as it was and unfenced, and a retry succeeds; it is not a media retry.
//      An EIO on both reads is the copy's damage: corrupt_index, also unfenced.
//   2. A copy damaged silently in the write installs, and is never a wrong answer: mapped, the
//      first read throws corrupt_segment and heal() names the segment; unmapped, reads are served
//      from memory and the next open finds the damage.
#include "kv_test.hh"

#include <cerrno>
#include <random>
#include <set>
#include <system_error>

namespace {

namespace J = kronuz::journal;

enum class where { open, map };

struct map_fails : J::File {
	std::unique_ptr<J::File> in;
	int code;
	map_fails(std::unique_ptr<J::File> f, int c) : in(std::move(f)), code(c) {}
	std::uint64_t size() override { return in->size(); }
	std::size_t read_at(std::uint64_t o, std::span<char> b) override { return in->read_at(o, b); }
	std::size_t write_at(std::uint64_t o, std::string_view b) override { return in->write_at(o, b); }
	void truncate(std::uint64_t n) override { in->truncate(n); }
	void sync() override { in->sync(); }
	std::shared_ptr<const J::Mapping> map() override { throw std::system_error(code, std::generic_category(), "probe: map"); }
};

// Forwards to faultfs::io; while armed, fails `budget` times opening (or mapping) a segment file
// created during the install.
struct host_io : J::IO {
	J::IO& in;
	where at = where::open;
	int code = EMFILE;
	bool armed = false;
	int budget = 0;
	int thrown = 0;
	std::set<std::string> created;
	explicit host_io(J::IO& i) : in(i) {}
	J::FilesystemClass directory_filesystem() const override { return in.directory_filesystem(); }
	J::DirectoryTrust directory_trust() const override { return in.directory_trust(); }
	std::unique_ptr<J::OwnerLock> acquire_owner(bool c) override { return in.acquire_owner(c); }
	std::unique_ptr<J::OwnerLock> acquire_directory_owner() override { return in.acquire_directory_owner(); }
	std::unique_ptr<J::File> open_existing(std::string_view n) override {
		const bool hit = armed && budget > 0 && created.count(std::string(n));
		if (hit && at == where::open) {
			if (--budget == 0) { armed = false; }
			++thrown;
			throw std::system_error(code, std::generic_category(), "probe: open");
		}
		auto f = in.open_existing(n);
		if (hit) {
			if (--budget == 0) { armed = false; }
			++thrown;
			return std::make_unique<map_fails>(std::move(f), code);
		}
		return f;
	}
	std::unique_ptr<J::File> create_exclusive(std::string_view n) override {
		const std::string s(n);
		if (armed && s.rfind("seg-", 0) == 0 && s.size() > 3 && s.compare(s.size() - 3, 3, ".sx") == 0) { created.insert(s); }
		return in.create_exclusive(n);
	}
	void replace(std::string_view a, std::string_view b) override { in.replace(a, b); }
	void remove(std::string_view n) override { in.remove(n); }
	std::unique_ptr<J::DirectoryCursor> scan_directory() override { return in.scan_directory(); }
	std::optional<J::EntryFootprint> entry_footprint(std::string_view n) override { return in.entry_footprint(n); }
	void sync_directory() override { in.sync_directory(); }
	void sync_files(std::span<J::File* const> f) override { in.sync_files(f); }
};

using model_t = std::map<std::string, std::string>;

kv_index::options base() {
	kv_index::options o;
	o.keyed = true;
	o.reclaim_step = 4096;
	o.eager_segments = true;  // these cases are an eager install's: it maps the copies it owns (cold-tier D9)
	return o;
}

enum class outcome { ok, fenced, corrupt_index, host, other };

template <class F>
outcome attempt(F&& f, int code = 0) {
	try {
		f();
		return outcome::ok;
	} catch (const index_fenced&) {
		return outcome::fenced;
	} catch (const corrupt_index&) {
		return outcome::corrupt_index;
	} catch (const std::system_error& e) {
		return e.code().value() == code ? outcome::host : outcome::other;
	} catch (...) {
		return outcome::other;
	}
}

model_t fill(kv_index& S, int segments, const std::string& value) {
	model_t model;
	for (int s = 0; s < segments; ++s) {
		for (int i = 0; i < 40; ++i) {
			const std::string k = "k" + std::to_string(s * 40 + i);
			CHECK(S.apply(tx({replace(k, value)}), {}).result == verdict::accepted);
			model[k] = value;
		}
		S.commit();
	}
	return model;
}

std::string random_value() {
	std::string big(600, ' ');
	std::mt19937_64 rnd(3);
	for (auto& c : big) { c = char('a' + rnd() % 26); }
	return big;
}

// A reopen failing `times` (2: both of read_checked's tries) with errno `code` at `w`.
void reopen_fails(where w, int code, int times) {
	faultfs::filesystem fss, fst;
	faultfs::io ios(fss), raw(fst);
	host_io iot(raw);
	auto S = kv_index::create(ios, base(), uuid_of("S"));
	const std::string big = random_value();
	const model_t model = fill(S, 3, big);
	const std::uint64_t cp = S.checkpoint();
	{
	auto T = kv_index::create(iot, base(), uuid_of("T"));
	CHECK(T.apply(tx({replace("kold", "x")}), {}).result == verdict::accepted);
	T.commit();
	iot.at = w;
	iot.code = code;
	iot.armed = true;
	iot.budget = times;
	const auto r0 = T.media_retries();
	if (times == 1) {  // read_checked's second try succeeds
		CHECK(attempt([&] { T.install(ios, cp); }) == outcome::ok);
		CHECK(iot.thrown == 1);
		CHECK(T.media_retries() == r0);  // a host error is not the medium's
		CHECK(T.get("k5") == std::optional<std::string>(big) && !T.get("kold"));
		return;
	}
	const bool media = code == EIO;
	CHECK(attempt([&] { T.install(ios, cp); }, code) == (media ? outcome::corrupt_index : outcome::host));
	CHECK(iot.thrown == 2);
	CHECK(T.media_retries() == r0 + (media ? 1 : 0));
	CHECK(T.get("kold") == std::optional<std::string>("x") && !T.get("k5"));  // as it was
	CHECK(attempt([&] { (void)T.apply(tx({replace("knew", "y")}), {}); }) == outcome::ok);  // not fenced
	iot.created.clear();
	CHECK(attempt([&] { T.install(ios, cp); }) == outcome::ok);  // armed is spent
	CHECK(T.get("k5") == std::optional<std::string>(big) && !T.get("kold") && !T.get("knew"));
	T.commit();
	}
	auto R = kv_index::open(iot, base());
	CHECK(R.get("k59") == std::optional<std::string>(big) && !R.get("kold"));
}

// A copy damaged in the write, `at` bytes into its first large write.
void damaged_copy(bool mapped, std::size_t at) {
	faultfs::filesystem fss, fst;
	faultfs::io ios(fss), iot(fst);
	auto S = kv_index::create(ios, base(), uuid_of("S"));
	const std::string big = random_value();
	const model_t model = fill(S, 3, big);
	const std::uint64_t cp = S.checkpoint();
	{
		auto T = kv_index::create(iot, base(), uuid_of("T"));
		fst.damage_writes(5000, at, 1);
		CHECK(attempt([&] { T.install(ios, cp); }) == outcome::ok);  // never verified, as every write
		CHECK(fst.damaged_writes() == 1);
		CHECK(attempt([&] { (void)T.apply(tx({replace("kzz", "x")}), {}); }) == outcome::ok);  // not fenced
		int ok = 0, thrown = 0;
		for (const auto& [k, v] : model) {
			try {
				const auto g = T.get(k);
				CHECK(g == std::optional<std::string>(v));  // never a wrong answer
				++ok;
			} catch (const corrupt_segment&) {
				++thrown;
			}
		}
		if (mapped) {
			CHECK(thrown > 0 && ok > 0);  // the damaged region only
			CHECK(T.heal().size() == 1);
		} else {
			CHECK(thrown == 0);  // served from the verified bytes in memory
			CHECK(T.heal().empty());
		}
		T.commit();
	}
	int ok = 0, thrown = 0;
	try {
		auto R = kv_index::open(iot, base());
		for (const auto& [k, v] : model) {
			try {
				CHECK(R.get(k) == std::optional<std::string>(v));
				++ok;
			} catch (const corrupt_segment&) {
				++thrown;
			}
		}
	} catch (const corrupt_index&) {
		thrown = 1;
	}
	CHECK(thrown > 0);  // the next open reports the damage
}

}  // namespace

int main() {
	const char* m = std::getenv("STRATA_FAULTFS_MAP");
	const bool mapped = m && m[0] == '1';
	if (mapped) {  // the reopen is mapped-only
		for (const where w : {where::open, where::map}) {
			const int host = w == where::open ? EMFILE : ENOMEM;
			reopen_fails(w, host, 1);
			reopen_fails(w, host, 2);
		}
		reopen_fails(where::open, EIO, 2);
	}
	damaged_copy(mapped, 3000);
	return failures != 0;
}
