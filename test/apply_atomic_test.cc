// apply() leaves no partial state when stage 1 throws (CT-R5, CT-R5a). A failed transaction (a
// segment read that throws, or an allocation that fails at any point of the call) must leave the
// index equal to a twin that never saw it: reads, metadata, the storage budget, the committed files,
// and the next transaction. Variants: a live reader (the rollback keeps the lane), earlier buffered
// records, and a storage budget (the reservation is returned).
#include "kv_test.hh"

#include "strata/core/format.hh"

#include <algorithm>
#include <new>
#include <string>

#if defined(__SANITIZE_THREAD__)
#define APPLY_ATOMIC_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define APPLY_ATOMIC_TSAN 1
#endif
#endif

// Fails the Nth allocation while armed; an operator new that counts costs nothing when disarmed.
// TSan's runtime defines the replaceable operators itself, so the sweep is skipped under it.
static std::size_t fail_countdown = 0;
static bool fired = false;
#ifndef APPLY_ATOMIC_TSAN
void* operator new(std::size_t n) {
	if (fail_countdown && --fail_countdown == 0) {
		fired = true;
		throw std::bad_alloc();
	}
	if (void* p = std::malloc(n ? n : 1)) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
// The nothrow forms pair with the replaced delete (std::stable_sort's buffer), so ASan sees no mismatch.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
	try {
		return operator new(n);
	} catch (...) {
		return nullptr;
	}
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

static const auto uuid = uuid_of("apply-atomic");

static std::uint64_t head_generation(const faultfs::filesystem& fs) {
	std::uint64_t g = 0;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::manifest) { g = std::max(g, p->a); }
	}
	return g;
}

// S1 holds Q1 and S2 holds Q2; with `damage`, S1's keys section is damaged at its size, so the
// open passes and the first read of that region throws corrupt_segment.
struct world {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	byte_budget storage{1ull << 40}, mem{1ull << 40};
	reader_registry reg{32};
	kv_index::options o;
	std::optional<kv_index> ix;
	std::optional<kv_index::reader_handle> rd;

	world(bool damage, bool reader, bool prior) {
		o.key_prefix = "Q";
		o.auto_merge = false;
		o.identity_seed = 7;
		o.storage = &storage;
		o.budget = &mem;
		if (reader) { o.readers = &reg; }
		{
			auto c = kv_index::create(io, o, uuid);
			c.apply(tx({add("Q1", "a")}), {});
			c.commit();
			c.apply(tx({add("Q2", "b")}), {});
			c.commit();
			c.close();
		}
		if (damage) {
			const auto m = files::decode_manifest(fs.view(index_detail::manifest_name(head_generation(fs))));
			const std::string s1 = index_detail::segment_name(m.segments[0].id);
			std::string bytes(fs.view(s1));
			for (const auto& e : format::decode_toc(bytes, format::decode_footer(bytes))) {
				if (e.kind == format::section::keys && e.length) { bytes[e.offset + e.length / 2] ^= 1; }
			}
			io.remove(s1);
			auto f = io.create_exclusive(s1);
			f->write_at(0, bytes);
			f->sync();
		}
		ix.emplace(kv_index::open(io, o));
		if (reader) { rd.emplace(ix->reader()); }
		if (prior) {
			ix->apply(tx({add("Q5", "five"), set_meta("p", "1")}), {});
			ix->apply(tx({replace("Q2", "b0")}), {});
		}
	}

	// What the index shows: reads (the damaged region still throws on read), then metadata.
	std::string state() {
		std::string s;
		ix->refresh();
		for (const char* k : {"Q1", "Q2", "Q3", "Q5", "Q9"}) {
			try {
				auto v = ix->get(k);
				s += std::string(k) + "=" + (v ? *v : "-") + ";";
			} catch (const std::exception&) {
				s += std::string(k) + "=ERR;";
			}
		}
		for (const auto& [k, v] : ix->metadata()) { s += "m:" + k + "=" + v + ";"; }
		return s;
	}

	// The state, a clean follow-up transaction, the state again, both budgets, and the committed files.
	std::string finish() {
		std::string s = state();
		const auto v = ix->apply(tx({add("Q3", "c"), set_meta("z", "1")}), {});
		s += "v=" + std::to_string(int(v.result)) + ";" + state();
		s += "storage=" + std::to_string(storage.used()) + ";mem=" + std::to_string(mem.used()) + ";";
		ix->commit();
		for (const auto& n : fs.list()) { s += n.substr(0, 3) + std::to_string(fs.view(n).size()) + ","; }
		try {
			rd.reset();
			ix->close();
		} catch (const std::exception&) {
		}
		return s;
	}
};

enum class outcome { atomic, leaked, not_thrown };

// Applies `ops` to a world with an allocation failing at the Nth `new` of the call (0: none) and
// compares it with a twin that never applied them.
static outcome attempt(bool damage, bool reader, bool prior, std::size_t n, const std::vector<operation>& ops, const char* label) {
	std::string ref;
	{
		world twin(damage, reader, prior);
		ref = twin.finish();
	}
	world w(damage, reader, prior);
	const auto before = w.state();
	const std::uint64_t st0 = w.storage.used();
	bool threw = false;
	fired = false;
	fail_countdown = n;
	try {
		w.ix->apply(tx(ops), {});
	} catch (const std::bad_alloc&) {
		threw = true;
	} catch (const corrupt_segment&) {
		threw = true;
	}
	fail_countdown = 0;
	if (!threw) { return outcome::not_thrown; }
	const std::uint64_t st1 = w.storage.used();
	const bool same_budget = st1 == st0;
	const bool same = same_budget && w.state() == before && w.finish() == ref;
	if (!same) { std::fprintf(stderr, "%s: damage=%d reader=%d prior=%d N=%zu leaked (storage %llu -> %llu)\n", label, damage, reader, prior, n, (unsigned long long)st0, (unsigned long long)st1); }
	return same ? outcome::atomic : outcome::leaked;
}

int main() {
	// A segment read throws (CT-R5): the damaged region, in several shapes of failing transaction.
	const std::vector<std::vector<operation>> failing{
		{replace("Q2", "b2"), replace("Q1", "a2")},
		{add("Q9", "z"), replace("Q2", "b2"), replace("Q1", "a2")},
		{set_meta("m", "1"), replace("Q2", "b2"), replace("Q1", "a2")},
		{erase("Q2"), replace("Q1", "a2")},
	};
	for (const auto& ops : failing) {
		for (const bool reader : {false, true}) {
			for (const bool prior : {false, true}) { CHECK(attempt(true, reader, prior, 0, ops, "corrupt") == outcome::atomic); }
		}
	}
	// An allocation fails at every point of the call (CT-R5a), with and without the damage.
#ifdef APPLY_ATOMIC_TSAN
	std::printf("apply_atomic: %s (allocation sweep skipped under TSan)\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
#endif
	const std::vector<operation> mixed{replace("Q2", "b2"), replace("Q1", "a2"), set_meta("m", "1"), add("Q9", "z")};
	std::size_t points = 0;
	for (const bool damage : {true, false}) {
		for (const bool reader : {false, true}) {
			for (const bool prior : {false, true}) {
				for (std::size_t n = 1; n < 400; ++n) {
					const outcome r = attempt(damage, reader, prior, n, mixed, "alloc");
					CHECK(r != outcome::leaked);
					if (r == outcome::not_thrown || !fired) { break; }
					++points;
				}
			}
		}
	}
	CHECK(points > 100);
	std::printf("apply_atomic: %s (%zu allocation points)\n", failures ? "FAILED" : "ok", points);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
