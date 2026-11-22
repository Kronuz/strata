// The tolerant open (rocksdb-parity A2; journal docs/mechanisms/lost-bound.md): one damaged log
// block holds batches of three logged indexes on one host. C's head is past them, U's and W's
// are not; U wrote after them, W did not. C opens with every record, U and W fail log_lost and stay
// lost, and D, whose checkpoint record the block holds below its head, opens too. With the block's manifest damaged
// too, no bound is known and every index on the log fails log_lost. Usage: test_tolerant_open.
#include "kv_test.hh"
#include "strata/core/turn_seal.hh"

#include "journal/host.h"

#include <algorithm>

namespace {

namespace jr = kronuz::journal;
using strata::turn_sealer;

constexpr std::uint64_t command_bytes = 32 << 10;
constexpr std::uint32_t block = 4096;
constexpr std::uint64_t c_group = 11, u_group = 12, w_group = 13, d_group = 14;

template <class E, class F>
bool throws_as(F&& f) {
	try {
		f();
	} catch (const E&) {
		return true;
	} catch (...) {
		return false;
	}
	return false;
}
jr::Identity host_identity() {
	jr::Identity id{};
	const char name[] = "tolerant-open";
	std::copy(name, name + sizeof name - 1, id.begin());
	return id;
}
jr::TurnLogOptions geometry() {
	jr::TurnLogOptions g;
	g.segment_bytes = 256 << 10;
	g.block_bytes = block;
	return g;
}

// The journal host's directory over its own faultfs directory. Its owner lock is a token.
class host_view final : public jr::IO {
public:
	explicit host_view(faultfs::io& io) : io_(io) {}
	jr::FilesystemClass directory_filesystem() const override { return io_.directory_filesystem(); }
	jr::DirectoryTrust directory_trust() const override { return io_.directory_trust(); }
	std::unique_ptr<jr::OwnerLock> acquire_owner(bool) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::OwnerLock> lock_owner(const jr::OwnerLockOptions&) override { return std::make_unique<jr::OwnerLock>(); }
	std::unique_ptr<jr::File> open_existing(std::string_view n) override { return io_.open_existing(n); }
	std::unique_ptr<jr::File> open_existing_read_only(std::string_view n) override { return io_.open_existing_read_only(n); }
	std::unique_ptr<jr::File> open_reclaim_candidate_read_only(std::string_view n) override { return io_.open_reclaim_candidate_read_only(n); }
	void open_existing_read_only_into(std::string_view n, jr::File& f) override { io_.open_existing_read_only_into(n, f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, jr::File& f) override { return io_.open_reclaim_candidate_read_only_into(n, f); }
	std::unique_ptr<jr::File> create_exclusive(std::string_view n) override { return io_.create_exclusive(n); }
	void replace(std::string_view from, std::string_view to) override { io_.replace(from, to); }
	void remove(std::string_view n) override { io_.remove(n); }
	std::optional<jr::EntryFootprint> entry_footprint(std::string_view n) override { return io_.entry_footprint(n); }
	void order_files_and_directory(std::span<jr::File* const> files) override { io_.order_files_and_directory(files); }
	void sync_directory() override { io_.sync_directory(); }
	std::unique_ptr<jr::DirectoryCursor> scan_directory() override { return io_.scan_directory(); }

private:
	faultfs::io& io_;
};

struct host {
	jr::Host table;
	turn_sealer sealer;
	explicit host(faultfs::io& io) : table(std::make_shared<host_view>(io), host_identity(), jr::HostOptions{1, geometry()}), sealer(table, 0, host_identity(), geometry(), command_bytes) {}
	void turns() {
		while (sealer.turn()) {}
	}
};

kv_index::options logged(turn_sealer& s, std::uint64_t group) {
	kv_index::options o;
	o.keyed = true;
	o.log = &s;
	o.log_group = group;
	o.command_bytes = command_bytes;
	return o;
}

// The host's turn-log segments, by name.
std::map<std::string, std::string> segments(faultfs::io& io) {
	std::map<std::string, std::string> out;
	for (auto c = io.scan_directory(); auto name = c->next();) {
		if (name->find("-turns-") == std::string::npos) { continue; }
		auto f = io.open_existing_read_only(*name);
		std::string bytes(f->size(), '\0');
		f->read_at(0, std::span(bytes.data(), bytes.size()));
		out[*name] = std::move(bytes);
	}
	return out;
}
// The segment and offset holding `bytes`.
std::pair<std::string, std::uint64_t> find(faultfs::io& io, const std::string& bytes) {
	for (const auto& [name, data] : segments(io)) {
		if (auto at = data.find(bytes); at != std::string::npos) { return {name, at}; }
	}
	return {};
}
void flip(faultfs::io& io, const std::string& name, std::uint64_t offset) {
	auto f = io.open_existing(name);
	char c = 0;
	f->read_at(offset, std::span(&c, 1));
	c = static_cast<char>(c ^ 0x5a);
	f->write_at(offset, std::string_view(&c, 1));
	f->sync();
}

std::string run(char letter) { return std::string(600, letter); }

struct world {
	faultfs::filesystem hfs, cfs, ufs, wfs, dfs;
	faultfs::io hio{hfs}, cio{cfs}, uio{ufs}, wio{wfs}, dio{dfs};
	std::string segment;
	std::uint64_t at = 0;  // a byte of U's batch in the damaged block
};

// Each index joins and commits k0. The damaged turn holds one batch each of C, U and W, in one
// block X. U's next batch, 8 KiB, starts in X and writes X's manifest in the next block's header;
// then C and U write a batch each, and C and D commit: C's checkpoint record is staged and never
// sealed, so the log holds nothing after the damage that covers C's head.
void build(world& w) {
	host h(w.hio);
	kv_index c = kv_index::create(w.cio, logged(h.sealer, c_group), uuid_of("tolerant-c"));
	kv_index u = kv_index::create(w.uio, logged(h.sealer, u_group), uuid_of("tolerant-u"));
	kv_index wi = kv_index::create(w.wio, logged(h.sealer, w_group), uuid_of("tolerant-w"));
	kv_index d = kv_index::create(w.dio, logged(h.sealer, d_group), uuid_of("tolerant-d"));
	for (kv_index* ix : {&c, &u, &wi, &d}) {
		CHECK(ix->apply(tx({replace("k0", "zero")}), {}).result == verdict::accepted);
		ix->commit();
		h.turns();
	}
	CHECK(c.apply(tx({replace("c1", run('C'))}), {}).result == verdict::accepted);
	CHECK(u.apply(tx({replace("u1", run('U'))}), {}).result == verdict::accepted);
	CHECK(wi.apply(tx({replace("w1", run('W'))}), {}).result == verdict::accepted);
	h.turns();
	CHECK(u.apply(tx({replace("u-filler", std::string(8 << 10, 'V'))}), {}).result == verdict::accepted);
	h.turns();
	CHECK(c.apply(tx({replace("c2", "two")}), {}).result == verdict::accepted);
	CHECK(u.apply(tx({replace("u2", "two")}), {}).result == verdict::accepted);
	CHECK(d.apply(tx({replace("d1", "one")}), {}).result == verdict::accepted);
	h.turns();
	c.commit();
	d.commit();
	const auto [segment, at] = find(w.hio, run('U'));
	w.segment = segment;
	w.at = at + 300;
	for (char letter : {'C', 'U', 'W'}) {
		const auto [name, offset] = find(w.hio, run(letter));
		CHECK(name == segment && offset / block == at / block && (offset + 599) / block == at / block);
	}
}

void manifest_named() {
	world w;
	build(w);
	flip(w.hio, w.segment, w.at);
	host h(w.hio);
	// Each group's sequences: 1 its join's checkpoint, 2 k0, 3 the commit's checkpoint. X holds C's
	// c1 (4), U's u1 (4) and its filler's start (5), W's w1 (4), and D's checkpoint (3).
	for (const auto& [g, b] : std::map<std::uint64_t, std::uint64_t>{{c_group, 4}, {u_group, 5}, {w_group, 4}, {d_group, 3}}) {
		CHECK(h.table.status(g) == jr::GroupStatus::bounded && h.table.bound(g) == b);
	}
	{
		kv_index c = kv_index::open(w.cio, logged(h.sealer, c_group));
		CHECK(c.get("k0") == "zero" && c.get("c1") == run('C') && c.get("c2") == "two");
		CHECK(c.apply(tx({replace("c3", "three")}), {}).result == verdict::accepted);
		c.commit();
		h.turns();
	}
	CHECK(throws_as<log_lost>([&] { (void)kv_index::open(w.uio, logged(h.sealer, u_group)); }));
	CHECK(throws_as<log_lost>([&] { (void)kv_index::open(w.wio, logged(h.sealer, w_group)); }));
	CHECK(h.table.status(u_group) == jr::GroupStatus::lost && h.table.status(w_group) == jr::GroupStatus::lost);
	{
		kv_index d = kv_index::open(w.dio, logged(h.sealer, d_group));
		CHECK(d.get("k0") == "zero" && d.get("d1") == "one");
	}
	// A reopen: U and W stay lost; C's checkpoint after the bound cleared it.
	host again(w.hio);
	CHECK(again.table.status(u_group) == jr::GroupStatus::lost && again.table.status(w_group) == jr::GroupStatus::lost);
	CHECK(again.table.status(c_group) == jr::GroupStatus::stored);
	kv_index c = kv_index::open(w.cio, logged(again.sealer, c_group));
	CHECK(c.get("c1") == run('C') && c.get("c3") == "three");
}

// The damaged block's manifest, in the next block's header, damaged too: no bound is known.
void manifest_unknown() {
	world w;
	build(w);
	flip(w.hio, w.segment, w.at);
	flip(w.hio, w.segment, (w.at / block + 1) * block + 10);
	host h(w.hio);
	CHECK(throws_as<log_lost>([&] { (void)kv_index::open(w.cio, logged(h.sealer, c_group)); }));
	CHECK(throws_as<log_lost>([&] { (void)kv_index::open(w.uio, logged(h.sealer, u_group)); }));
	CHECK(throws_as<log_lost>([&] { (void)kv_index::open(w.wio, logged(h.sealer, w_group)); }));
}

}  // namespace

int main() {
	manifest_named();
	manifest_unknown();
	std::printf("tolerant open: %d failures\n", failures);
	return failures ? 1 : 0;
}
