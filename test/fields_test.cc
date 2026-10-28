// The field table (fields-and-keys D2, F1): how a transaction names fields, the model it must
// agree with, the races the design lists, the table's file, and its decoder. The layout adds one
// plugin whose parts carry field references, so the test sees which id each reference resolved to.
#include "strata/core/faultfs.hh"
#include "strata/core/fields.hh"
#include "strata/core/index.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;

// A stored plugin whose part also lists (field reference, term, the name the host means). resolve()
// rewrites each reference to an id and records (id, intended name) in `seen`.
struct probe_plugin : stored_plugin {
	struct use {
		field_ref ref;
		std::string term, intent;
	};
	struct part : stored_plugin::part {
		std::vector<use> uses;
	};
	static inline std::vector<std::pair<std::uint32_t, std::string>> seen;
	static bool validate(const validate_context& vc, const part& p) {
		if (!stored_plugin::validate(vc, p)) { return false; }
		for (const auto& u : p.uses) {
			if (!vc.field(u.ref)) { return false; }
		}
		return true;
	}
	static void resolve(part& p, const field_resolution& r) {
		for (auto& u : p.uses) {
			const auto id = r.resolve(u.ref);
			seen.emplace_back(*id, u.intent);
			u.ref = field_ref::id(*id);
		}
	}
	static std::uint64_t bytes(const part& p) { return p.stored.size(); }
	static std::uint64_t disk_estimate(const part& p) { return p.stored.size() + p.stored.size() / 16; }
};
using probe_layout = layout<keys_plugin, probe_plugin>;
using index_t = basic_index<probe_layout>;
using operation = index_t::operation;
using transaction = index_t::transaction;
using verdict_t = verdict;

static const std::array<char, 16> uuid = {'f', 'i', 'e', 'l', 'd', '-', 't', 'a', 'b', 'l', 'e', '-', 't', 'e', 's', 't'};

static field_declaration decl(std::string name, field_postings p = field_postings::freqs, bool norms = false, field_role role = field_role::indexed) {
	return {std::move(name), role, p, norms};
}
static operation add_doc(std::vector<probe_plugin::use> uses, const std::string& stored = "x") {
	operation o;
	o.type = operation::add;
	o.doc.stored = stored;
	o.doc.uses = std::move(uses);
	return o;
}
static probe_plugin::use ref_declared(std::uint32_t i, const std::string& name) { return {field_ref::declared(i), "t", name}; }
static probe_plugin::use ref_id(std::uint32_t n, const std::string& name) { return {field_ref::id(n), "t", name}; }

static index_t::options opts() {
	index_t::options o;
	o.key_prefix = "Q";
	o.auto_merge = false;
	o.identity_seed = 3;
	return o;
}

// The host's view of a table: a copy and its mark, as `fields()` hands them out.
struct host_view {
	field_table table;
	field_mark mark;
	explicit host_view(const index_t& ix) : table(ix.fields()), mark(ix.fields_mark()) {}
};

static verdict_t run(index_t& ix, const transaction& t, files::position at = {}) {
	probe_plugin::seen.clear();
	return ix.apply(t, at);
}

// --- the model ----------------------------------------------------------------------------

// What the table must hold after the transactions the model accepted, and the rejection it expects.
struct model {
	std::vector<field_declaration> table;
	bool keyed = true;
	// The verdict and, for an accepted transaction, the table; the model never keeps a rejected
	// transaction's declarations.
	verdict_t::outcome apply(const std::vector<field_declaration>& decls) {
		std::vector<field_declaration> next = table;
		for (const auto& d : decls) {
			if (field_problem(d)) { return verdict_t::invalid; }
			auto it = std::find_if(next.begin(), next.end(), [&](const auto& x) { return x.name == d.name; });
			if (it != next.end()) {
				if (!(*it == d)) { return verdict_t::invalid; }
				continue;
			}
			if (d.role == field_role::key && (!keyed || std::any_of(next.begin(), next.end(), [](const auto& x) { return x.role == field_role::key; }))) { return verdict_t::invalid; }
			next.push_back(d);
		}
		table = std::move(next);
		return verdict_t::accepted;
	}
	std::uint32_t id_of(const std::string& name) const {
		for (std::uint32_t i = 0; i < table.size(); ++i) {
			if (table[i].name == name) { return i; }
		}
		return ~0u;
	}
	bool matches(const field_table& t) const {
		if (t.size() != table.size()) { return false; }
		for (std::size_t i = 0; i < table.size(); ++i) {
			if (!(t[i] == table[i])) { return false; }
		}
		return true;
	}
};

// Transactions built against stale tables, pipelined into one index: new names that repeat (the same
// name in two transactions, the same name twice in one) with the same or another declaration, and
// references by declaration and by id mixed. Every term's resolved field is the one the host named.
static void property(std::uint64_t seed, int steps) {
	std::mt19937_64 rng(seed);
	faultfs::filesystem fs;
	faultfs::io io{fs};
	auto ix = index_t::create(io, opts(), uuid);
	model m;
	std::vector<host_view> stale;
	const std::vector<std::string> pool = {"title", "body", "tag", "id", "date", "user", "geo", "zz", "a", "b", "c", "d"};
	std::size_t accepted = 0, rejected = 0, id_refs = 0, declared_refs = 0;
	for (int step = 0; step < steps; ++step) {
		if (stale.empty() || rng() % 3 == 0) { stale.emplace_back(ix); }
		if (stale.size() > 6) { stale.erase(stale.begin() + rng() % stale.size()); }
		const host_view& view = stale[rng() % stale.size()];
		transaction t;
		for (std::size_t i = 0, n = rng() % 4; i < n; ++i) {
			// A known name with the same declaration, or another one now and then.
			const std::string& name = pool[rng() % pool.size()];
			auto d = decl(name, rng() % 8 ? field_postings::freqs : field_postings::positions);
			if (rng() % 30 == 0) { d.name.clear(); }
			t.fields.push_back(d);
		}
		const bool use_mark = rng() % 4 != 0;
		if (use_mark) { t.expected_fields = view.mark; }
		for (std::size_t o = 0, n = 1 + rng() % 3; o < n; ++o) {
			std::vector<probe_plugin::use> uses;
			for (std::size_t u = 0, k = 1 + rng() % 3; u < k; ++u) {
				if (!t.fields.empty() && (rng() % 2 || view.table.size() == 0)) {
					const std::uint32_t i = std::uint32_t(rng() % t.fields.size());
					uses.push_back(ref_declared(i, t.fields[i].name));
				} else if (view.table.size()) {
					const std::uint32_t n2 = std::uint32_t(rng() % view.table.size());
					uses.push_back(ref_id(n2, view.table[n2].name));
				}
			}
			if (!uses.empty()) { t.ops.push_back(add_doc(std::move(uses))); }
		}
		// The model's verdict: the references the transaction can resolve, then the declarations.
		bool refs_ok = true;
		for (const auto& o : t.ops) {
			for (const auto& u : o.doc.uses) {
				if (u.ref.type == field_ref::declaration ? u.ref.n >= t.fields.size() : !use_mark || u.ref.n >= view.mark.size) { refs_ok = false; }
			}
		}
		model before = m;
		const auto want = m.apply(t.fields);
		const auto got = run(ix, t);
		if (want != verdict_t::accepted) {
			CHECK(got.result == verdict_t::invalid);
			m = before;
			++rejected;
		} else if (!refs_ok) {
			CHECK(got.result == verdict_t::invalid);
			m = before;  // the references failed after the declarations: those roll back too
			++rejected;
		} else {
			CHECK(got.result == verdict_t::accepted);
			++accepted;
			for (const auto& [id, intent] : probe_plugin::seen) {
				CHECK(id < ix.fields().size() && ix.fields()[id].name == intent);  // the field the host named
				CHECK(id == m.id_of(intent));
			}
			for (const auto& o : t.ops) {
				for (const auto& u : o.doc.uses) { (u.ref.type == field_ref::declaration ? declared_refs : id_refs)++; }
			}
		}
		CHECK(m.matches(ix.fields()));
	}
	CHECK(accepted > steps / 4 && rejected > 0 && id_refs > 0 && declared_refs > 0);
	// Reopened (a commit first), the table and its chain are the ones in memory.
	ix.commit();
	const field_mark mark = ix.fields_mark();
	const std::uint64_t digest = ix.digest();
	ix.close();
	auto again = index_t::open(io, opts());
	CHECK(again.fields_mark() == mark && again.digest() == digest && m.matches(again.fields()));
	again.check();
}

// A rejected declaring transaction leaves no trace and the next transaction reuses its ids.
static void rollback_then_reuse() {
	faultfs::filesystem fs, fs2;
	faultfs::io io{fs}, io2{fs2};
	auto ix = index_t::create(io, opts(), uuid);
	auto twin = index_t::create(io2, opts(), uuid);
	ix.apply([] {
		transaction t;
		t.fields = {decl("keep")};
		t.ops = {add_doc({ref_declared(0, "keep")})};
		return t;
	}(), {});
	twin.apply([] {
		transaction t;
		t.fields = {decl("keep")};
		t.ops = {add_doc({ref_declared(0, "keep")})};
		return t;
	}(), {});
	transaction bad;
	bad.fields = {decl("lost1"), decl("lost2")};
	bad.ops = {add_doc({ref_declared(0, "lost1")}), add_doc({ref_declared(7, "nowhere")})};  // the second does not resolve
	const auto before = ix.fields_mark();
	CHECK(run(ix, bad).result == verdict_t::invalid);
	CHECK(ix.fields_mark() == before && ix.fields().size() == 1 && !ix.fields().find("lost1") && ix.digest() == twin.digest());
	transaction extra;  // only a declaration: the digests must tell the tables apart
	extra.fields = {decl("extra")};
	CHECK(run(ix, extra).result == verdict_t::accepted && ix.fields().find("extra") == 1u);  // the id the rolled-back one would have taken
	CHECK(twin.digest() != ix.digest());
	CHECK(run(twin, extra).result == verdict_t::accepted && twin.digest() == ix.digest());
	transaction next;
	next.fields = {decl("fresh")};
	next.ops = {add_doc({ref_declared(0, "fresh")})};
	const auto v = run(ix, next);
	CHECK(v.result == verdict_t::accepted && probe_plugin::seen.size() == 1 && probe_plugin::seen[0].first == 2);
	CHECK(twin.digest() != ix.digest());  // the digest covers the table
	CHECK(run(twin, next).result == verdict_t::accepted && twin.digest() == ix.digest() && twin.fields_mark() == ix.fields_mark());
}

// A host holding ids from before a lost tail is refused, never misattributed.
static void lost_tail() {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	std::optional<index_t> ix(index_t::create(io, opts(), uuid));
	transaction first;
	first.fields = {decl("x"), decl("y")};
	CHECK(run(*ix, first).result == verdict_t::accepted);
	ix->commit();
	transaction tail;  // applied, never committed: lost with the crash
	tail.fields = {decl("z")};
	CHECK(run(*ix, tail).result == verdict_t::accepted);
	const host_view held(*ix);  // the host read size 3, with z at id 2
	CHECK(held.mark.size == 3 && held.table[2].name == "z");
	ix.reset();
	fs.crash(11);
	ix.emplace(index_t::open(io, opts()));
	CHECK(ix->fields().size() == 2);
	// A shorter table: the host's mark is past it.
	transaction stale;
	stale.expected_fields = held.mark;
	stale.ops = {add_doc({ref_id(2, "z")})};
	auto v = run(*ix, stale);
	CHECK(v.result == verdict_t::fields_conflict && v.current_fields == ix->fields_mark() && probe_plugin::seen.empty());
	// The ids reused under another name: same size, another chain.
	transaction other;
	other.fields = {decl("w")};
	CHECK(run(*ix, other).result == verdict_t::accepted && ix->fields()[2].name == "w" && ix->fields().size() == 3);
	v = run(*ix, stale);
	CHECK(v.result == verdict_t::fields_conflict && probe_plugin::seen.empty() && v.current_fields == ix->fields_mark());
	CHECK(ix->fields_mark().size == held.mark.size && ix->fields_mark().chain != held.mark.chain);
	// The prefix both agree on stays valid: ordinary concurrency never rejects.
	transaction fine;
	fine.expected_fields = field_mark{2, *ix->fields().chain_at(2)};
	fine.ops = {add_doc({ref_id(1, "y")})};
	v = run(*ix, fine);
	CHECK(v.result == verdict_t::accepted && probe_plugin::seen.size() == 1 && probe_plugin::seen[0].first == 1);
	// The mark of a table that grew since: still valid, and an id above the mark is not.
	transaction beyond;
	beyond.expected_fields = field_mark{2, *ix->fields().chain_at(2)};
	beyond.ops = {add_doc({ref_id(2, "w")})};
	CHECK(run(*ix, beyond).result == verdict_t::invalid);
}

// Stage 1's rules, one at a time.
static void rules() {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	auto ix = index_t::create(io, opts(), uuid);
	auto verdict_of = [&](transaction t) { return run(ix, t).result; };
	auto with_fields = [](std::vector<field_declaration> f, std::vector<operation> ops = {}) {
		transaction t;
		t.fields = std::move(f);
		t.ops = std::move(ops);
		return t;
	};
	CHECK(verdict_of(with_fields({decl("")})) == verdict_t::invalid);                                            // name length
	field_declaration bad_role = decl("r");
	bad_role.role = field_role(2);
	CHECK(verdict_of(with_fields({bad_role})) == verdict_t::invalid);                                           // a known role
	field_declaration bad_postings = decl("p");
	bad_postings.postings = field_postings(3);
	CHECK(verdict_of(with_fields({bad_postings})) == verdict_t::invalid);                                       // a known postings kind
	CHECK(verdict_of(with_fields({decl("k", field_postings::freqs, false, field_role::key)})) == verdict_t::invalid);  // a key field holds no postings
	CHECK(verdict_of(with_fields({decl("k", field_postings::docs, false, field_role::key)})) == verdict_t::accepted);
	CHECK(verdict_of(with_fields({decl("k2", field_postings::docs, false, field_role::key)})) == verdict_t::invalid);  // at most one key field
	CHECK(verdict_of(with_fields({decl("k", field_postings::docs, false, field_role::key), decl("n1")})) == verdict_t::accepted);  // identical answers its id
	CHECK(ix.fields().size() == 2 && ix.fields().key_field() == 0u);
	CHECK(verdict_of(with_fields({decl("n1", field_postings::positions)})) == verdict_t::invalid);                // a different declaration of a known name
	CHECK(verdict_of(with_fields({decl("n1"), decl("n1")})) == verdict_t::accepted);                              // the same twice in one
	CHECK(run(ix, with_fields({decl("fresh"), decl("n1", field_postings::docs)})).field == std::optional<std::size_t>(1));  // the entry named
	CHECK(!ix.fields().find("fresh"));
	// References: declaration range, id range, an id without a mark.
	CHECK(verdict_of(with_fields({decl("q")}, {add_doc({ref_declared(1, "q")})})) == verdict_t::invalid);
	CHECK(verdict_of(with_fields({decl("q")}, {add_doc({ref_declared(0, "q")})})) == verdict_t::accepted);
	CHECK(verdict_of(with_fields({}, {add_doc({ref_id(0, "x")})})) == verdict_t::invalid);                       // an id needs expected_fields
	transaction marked = with_fields({}, {add_doc({ref_id(ix.fields().size() - 1, "q")})});
	marked.expected_fields = ix.fields_mark();
	CHECK(verdict_of(marked) == verdict_t::accepted);
	marked.ops = {add_doc({ref_id(std::uint32_t(ix.fields().size()), "x")})};                                   // one past the mark
	CHECK(verdict_of(marked) == verdict_t::invalid);
	// A conflicting mark declares nothing.
	transaction conflict = with_fields({decl("never")}, {});
	conflict.expected_fields = field_mark{ix.fields().size(), ix.fields_mark().chain + 1};
	CHECK(verdict_of(conflict) == verdict_t::fields_conflict && !ix.fields().find("never"));
	// A keyless index refuses a key field.
	faultfs::filesystem fs2;
	faultfs::io io2{fs2};
	auto o = opts();
	o.key_prefix = "";
	auto keyless = index_t::create(io2, o, uuid);
	CHECK(run(keyless, with_fields({decl("k", field_postings::docs, false, field_role::key)})).result == verdict_t::invalid);
	CHECK(run(keyless, with_fields({decl("n")})).result == verdict_t::accepted);
}

static bool throws(const std::function<void()>& f) {
	try {
		f();
	} catch (...) {
		return true;
	}
	return false;
}

static std::vector<std::string> fields_files(const faultfs::filesystem& fs) {
	std::vector<std::string> out;
	for (const auto& n : fs.list()) {
		if (auto p = index_detail::parse_name(n); p && p->type == index_detail::parsed_name::fields) { out.push_back(n); }
	}
	return out;
}

// The file: one file appended to across commits, old manifests keep their prefix, replay is skipped,
// two replicas applying one log agree, a checkpoint installs.
static void file_and_replicas() {
	faultfs::filesystem fs, fs2, fs3;
	faultfs::io io{fs}, io2{fs2}, io3{fs3};
	auto a = index_t::create(io, opts(), uuid);
	auto b = index_t::create(io2, opts(), uuid);
	std::vector<transaction> log;
	for (int i = 0; i < 20; ++i) {
		transaction t;
		t.fields = {decl("f" + std::to_string(i)), decl("f" + std::to_string(i / 2))};
		t.ops = {add_doc({ref_declared(0, t.fields[0].name)})};
		log.push_back(t);
	}
	std::vector<std::uint64_t> checkpoints;
	std::vector<std::size_t> sizes;
	for (std::size_t i = 0; i < log.size(); ++i) {
		const files::position at{i + 1, 1};
		CHECK(a.apply(log[i], at).result == verdict_t::accepted && b.apply(log[i], at).result == verdict_t::accepted);
		CHECK(a.apply(log[i], at).result == verdict_t::skipped);  // a replay
		if (i % 5 == 4) {
			checkpoints.push_back(a.checkpoint());
			sizes.push_back(a.fields().size());
		}
		if (i % 3 == 0) { a.commit(); }
	}
	CHECK(a.fields() == b.fields() && a.fields_mark() == b.fields_mark() && a.digest() == b.digest());
	a.commit();
	CHECK(fields_files(fs).size() == 1);  // appended to, one file
	a.check();
	a.close();
	// An install adopts the table, and the installing index writes it under a file of its own.
	auto head = index_t::open(io, opts());
	CHECK(head.fields() == b.fields());
	head.close();
	auto c = index_t::create(io3, opts(), uuid);
	transaction own;
	own.fields = {decl("only-here")};
	CHECK(c.apply(own, {}).result == verdict_t::accepted);
	c.commit();
	c.install(io, checkpoints.back());
	CHECK(c.fields().size() == sizes.back() && !c.fields().find("only-here"));
	c.commit();
	c.close();
	auto reopened = index_t::open(io3, opts());
	CHECK(reopened.fields().size() == sizes.back() && !reopened.fields().find("only-here"));
	reopened.check();
	CHECK(reopened.fields_mark().chain == *b.fields().chain_at(sizes.back()));
	// A checkpoint's manifest reads the prefix it was committed with.
	// (newest first: opening one commits it as the head, and pins newer than it leave with the old head)
	for (std::size_t j = checkpoints.size(); j-- > 0;) {
		const std::size_t i = j;
		auto old = index_t::open(io, opts(), checkpoints[i]);
		CHECK(old.fields().size() == sizes[i] && old.fields().mark().chain == *b.fields().chain_at(sizes[i]));
		old.close();
	}
}

// A torn tail past the manifest's prefix is never overwritten: the next commit starts a new file.
// check() compares the file with the table.
static void torn_tail_and_check() {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	{
		auto ix = index_t::create(io, opts(), uuid);
		transaction t;
		t.fields = {decl("alpha"), decl("beta")};
		CHECK(run(ix, t).result == verdict_t::accepted);
		ix.commit();
		ix.check();
		ix.close();
	}
	const std::string name = index_detail::fields_name(1);
	const std::string prefix(fs.view(name));
	{
		auto f = io.open_existing(name);
		f->write_at(f->size(), "torn");
		f->sync();
	}
	auto ix = index_t::open(io, opts());
	transaction t;
	t.fields = {decl("gamma")};
	CHECK(run(ix, t).result == verdict_t::accepted);
	ix.commit();
	const auto files_now = fields_files(fs);
	CHECK(std::count(files_now.begin(), files_now.end(), index_detail::fields_name(2)) == 1);  // a new file, not an append over the tail
	CHECK(!std::count(files_now.begin(), files_now.end(), name) || std::string(fs.view(name)).substr(0, prefix.size()) == prefix);
	CHECK(ix.fields().size() == 3);
	ix.close();
	// Damage inside the committed prefix, after the open: check() sees it.
	auto again = index_t::open(io, opts());
	again.check();
	const std::string current = index_detail::fields_name(2);
	{
		auto f = io.open_existing(current);
		const std::uint64_t at = f->size() - 8;
		f->write_at(at, "#");
		f->sync();
	}
	CHECK(throws([&] { again.check(); }));
}

// Power off at every mutating operation of a declaring commit (an append, and a new file after a
// torn tail): the reopened table is the old one or the new one, nothing between, and it keeps working.
static void crash_points() {
	for (const bool rewrite : {false, true}) {
		for (std::uint64_t cut = 0; cut < 40; ++cut) {
			for (std::uint64_t seed = 0; seed < 3; ++seed) {
				faultfs::filesystem fs;
				faultfs::io io{fs};
				field_mark old_mark, new_mark;
				std::size_t old_size = 0;
				{
					auto ix = index_t::create(io, opts(), uuid);
					transaction t;
					t.fields = {decl("a"), decl("b")};
					run(ix, t, {1, 1});
					ix.commit();
					old_mark = ix.fields_mark();
					old_size = ix.fields().size();
					if (rewrite) {  // leave a torn tail so the next commit writes a new file
						auto f = io.open_existing(index_detail::fields_name(1));
						f->write_at(f->size(), "garbage");
						f->sync();
						ix.close();
						auto reopened = index_t::open(io, opts());
						transaction u;
						u.fields = {decl("c"), decl("d")};
						run(reopened, u, {2, 1});
						new_mark = reopened.fields_mark();
						fs.power_off_after(cut);
						try {
							reopened.commit();
						} catch (const std::exception&) {
						}
					} else {
						transaction u;
						u.fields = {decl("c"), decl("d")};
						run(ix, u, {2, 1});
						new_mark = ix.fields_mark();
						fs.power_off_after(cut);
						try {
							ix.commit();
						} catch (const std::exception&) {
						}
					}
				}
				fs.crash(seed * 977 + cut);
				auto ix = index_t::open(io, opts());
				CHECK(ix.fields_mark() == old_mark || ix.fields_mark() == new_mark);
				CHECK(ix.fields().size() == (ix.fields_mark() == old_mark ? old_size : old_size + 2));
				ix.check();
				// It continues: declare more, commit, reopen.
				transaction more;
				more.fields = {decl("e")};
				CHECK(run(ix, more, {9, 1}).result == verdict_t::accepted);
				ix.commit();
				const auto mark = ix.fields_mark();
				ix.close();
				auto last = index_t::open(io, opts());
				CHECK(last.fields_mark() == mark);
				last.check();
			}
		}
	}
}

// Readers see a table that never changes under them: views taken while the writer declares.
static void readers() {
	faultfs::filesystem fs;
	faultfs::io io{fs};
	reader_registry reg{32};
	auto o = opts();
	o.readers = &reg;
	auto ix = index_t::create(io, o, uuid);
	auto h = ix.reader();
	std::atomic<bool> stop{false};
	std::atomic<std::size_t> seen_max{0};
	std::atomic<std::size_t> passes{0};
	std::atomic<int> bad{0};
	// Every view is validated; the last pass runs after `stop`, so it sees the final table.
	std::thread reader([&] {
		for (bool last = false; !last;) {
			last = stop.load();
			const auto g = h.acquire();
			const field_table& t = g->fields();
			for (std::size_t i = 0; i < t.size(); ++i) {
				if (t[i].name != "f" + std::to_string(i) || t.find(t[i].name) != i) { bad++; }
			}
			seen_max.store(std::max(seen_max.load(), t.size()));
			passes++;
		}
	});
	// The reader provably overlaps the writer: it completes a pass before the first and during the run.
	const auto await_pass = [&] {
		for (const auto at = passes.load(); passes.load() == at;) { std::this_thread::yield(); }
	};
	await_pass();
	for (int i = 0; i < 400; ++i) {
		transaction t;
		t.fields = {decl("f" + std::to_string(i))};
		ix.apply(t, {});
		if (i % 7 == 0) { ix.refresh(); }
		if (i % 50 == 0) { ix.commit(); }
		if (i == 200) { await_pass(); }
	}
	ix.refresh();
	stop = true;
	reader.join();
	CHECK(h.acquire()->fields() == ix.fields() && h.acquire()->digest() == ix.digest());
	CHECK(bad == 0 && seen_max == 400);
}

// --- the decoder ----------------------------------------------------------------------------

static std::string reseal(std::string body) {
	files::detail::seal(body);
	return body;
}
static std::string file_of(const std::string& blocks) { return files::fields_header() + blocks; }
static bool refuses(const std::string& file) {
	field_table t;
	try {
		(void)files::decode_fields(file, t);
	} catch (const corrupt_index&) {
		return true;
	} catch (const unsupported_index&) {
		return true;
	}
	return false;
}

static void decoder() {
	field_table t;
	t.append(decl("alpha"));
	t.append(decl("beta", field_postings::positions, true));
	t.append(decl("key", field_postings::docs, false, field_role::key));
	const std::string good = files::encode_fields(t);
	field_table back;
	const std::uint32_t last = files::decode_fields(good, back);
	CHECK(back == t && back.mark() == t.mark() && last == files::trailing_crc(good));
	// A block per declaration decodes to the same table and chain.
	std::string blocks;
	for (std::size_t i = 0; i < t.size(); ++i) { blocks += files::encode_fields_block(t, i, i + 1); }
	field_table again;
	files::decode_fields(file_of(blocks), again);
	CHECK(again == t && again.mark() == t.mark());
	// Every prefix that ends inside a block, and every flipped bit, is refused.
	for (std::size_t n = 14; n < good.size(); ++n) { CHECK(refuses(good.substr(0, n))); }
	for (std::size_t n = 0; n < 13; ++n) { CHECK(refuses(good.substr(0, n))); }
	for (std::size_t i = 0; i < good.size(); ++i) {
		std::string x = good;
		x[i] ^= 1;
		CHECK(refuses(x));
	}
	CHECK(!refuses(files::fields_header()));  // no blocks: an empty table
	CHECK(refuses(good + "x"));
	// Rules the checksum cannot catch: well-formed blocks that break them.
	auto block = [](const std::vector<std::string>& decls) {
		std::string body;
		files::detail::put(body, decls.size());
		for (const auto& d : decls) { body += d; }
		return reseal(body);
	};
	auto d = [](const std::string& name, int role = 0, int postings = 1, int flags = 0) {
		std::string out;
		files::detail::put_bytes(out, name);
		out.push_back(char(role));
		out.push_back(char(postings));
		out.push_back(char(flags));
		return out;
	};
	CHECK(!refuses(file_of(block({d("a"), d("b")}))));
	CHECK(refuses(file_of(block({}))));                       // a block of none
	CHECK(refuses(file_of(block({d("")}))));                  // an empty name
	CHECK(refuses(file_of(block({d("a"), d("a")}))));         // a name twice
	CHECK(refuses(file_of(block({d("a")}) + block({d("a")}))));  // across blocks
	CHECK(refuses(file_of(block({d("a", 2)}))));              // a role
	CHECK(refuses(file_of(block({d("a", 0, 3)}))));           // a postings kind
	CHECK(refuses(file_of(block({d("a", 0, 1, 2)}))));        // flags
	CHECK(refuses(file_of(block({d("k", 1, 1)}))));           // a key field with postings
	CHECK(refuses(file_of(block({d("k", 1, 0, 1)}))));        // a key field with norms
	CHECK(refuses(file_of(block({d("k", 1, 0), d("l", 1, 0)}))));  // two key fields
	std::string huge;
	files::detail::put(huge, std::uint64_t(1) << 40);          // a count the bytes cannot hold
	CHECK(refuses(file_of(reseal(huge))));
	std::string magic = good;
	magic[0] = 'X';
	CHECK(refuses(magic));
	// Another version is unsupported, not damage.
	std::string other(good.substr(0, 8));
	files::detail::put(other, files::version + 1);
	other += good.substr(8 + 5);
	field_table scratch;
	bool unsupported = false;
	try {
		files::decode_fields(other, scratch);
	} catch (const unsupported_index&) {
		unsupported = true;
	}
	CHECK(unsupported);
}

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "--seeds") {  // writes the fuzz corpus' seeds
		std::filesystem::create_directories(argv[2]);
		field_table t;
		t.append(decl("alpha"));
		t.append(decl("beta", field_postings::positions, true));
		t.append(decl("key", field_postings::docs, false, field_role::key));
		std::string blocks;
		for (std::size_t i = 0; i < t.size(); ++i) { blocks += files::encode_fields_block(t, i, i + 1); }
		const std::pair<const char*, std::string> seeds[] = {{"table", files::encode_fields(t)}, {"blocks", file_of(blocks)}, {"empty", files::fields_header()}};
		for (const auto& [name, bytes] : seeds) { std::ofstream(std::string(argv[2]) + "/" + name, std::ios::binary) << bytes; }
		return 0;
	}
	const bool trace = std::getenv("FIELDS_TRACE") != nullptr;
	auto stage = [&](const char* name, const std::function<void()>& f) {
		f();
		if (trace) { std::fprintf(stderr, "fields: %s done (%d failures)\n", name, failures); }
	};
	stage("property", [] { for (std::uint64_t seed = 1; seed <= 12; ++seed) { property(seed, 300); } });
	stage("rollback", rollback_then_reuse);
	stage("lost tail", lost_tail);
	stage("rules", rules);
	stage("file and replicas", file_and_replicas);
	stage("torn tail", torn_tail_and_check);
	stage("crash points", crash_points);
	stage("readers", readers);
	stage("decoder", decoder);
	std::printf("fields: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
