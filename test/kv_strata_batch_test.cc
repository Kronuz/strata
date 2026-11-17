// kv_strata's sync batch (standalone-turn-log M2): a batch of writes is one transaction, so one
// record and one barrier on the host's log per batch, as RocksDB's WriteBatch is one synced write.
// The engine runs as in a row, its served loop on its own thread, over faultfs: the host's
// directory and the index's on separate filesystems, so the host's syncs are the log's barriers.
#include "kv_strata.hh"

#include "strata/core/faultfs.hh"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

std::string key_of(int i) {
	char b[17];
	std::snprintf(b, sizeof b, "key%013d", i);
	return b;
}

}  // namespace

int main() {
	faultfs::filesystem host_fs, index_fs;
	kvbench::config c;
	c.dir = "kv";
	c.dur = kvbench::durability::sync;
	c.batch = 16;
	c.memory = 64 << 20;
	c.background = 0;
	strata_kv e(c, [&](const std::string& dir) -> std::shared_ptr<kronuz::journal::IO> {
		return std::make_shared<faultfs::io>(dir.ends_with("/host") ? host_fs : index_fs);
	});
	e.on_phase("run", kvbench::durability::sync);
	const std::string value(100, 'v');
	// The first batch also carries the join's checkpoint record (D7).
	for (int i = 0; i < 16; ++i) { e.put(0, key_of(i), value); }
	e.durable(0);

	using faultfs::op;
	const std::uint64_t syncs = host_fs.operations(op::sync), writes = host_fs.operations(op::write);
	constexpr int batches = 32;
	int records = 0;
	for (int b = 0; b < batches; ++b) {
		const auto before = e.last(0);
		// Duplicate keys and an erase inside one batch apply in order.
		for (int i = 0; i < 14; ++i) { e.put(0, key_of(b * 14 + i), value + std::to_string(b)); }
		e.put(0, key_of(b * 14), "again");
		e.erase(0, key_of(b * 14 + 1));
		e.durable(0);
		const auto after = e.last(0);
		// One record per batch, holding the batch as its one transaction.
		CHECK(after.index == before.index + 1 && after.applied == 1);
		records += int(after.index - before.index);
	}
	const std::uint64_t barriers = host_fs.operations(op::sync) - syncs, written = host_fs.operations(op::write) - writes;
	CHECK(records == batches);
	CHECK(barriers == std::uint64_t(batches) && written == std::uint64_t(batches));
	std::string got;
	CHECK(e.get(0, key_of(0), got) && got == "again");
	CHECK(!e.get(0, key_of(1), got));
	CHECK(e.get(0, key_of(batches * 14 - 1), got) && got == value + std::to_string(batches - 1));

	// A batch past the command limit (1 MiB) splits into whole transactions, each acknowledged.
	const std::string big(100 << 10, 'b');
	const auto before = e.last(0);
	for (int i = 0; i < 16; ++i) { e.put(0, key_of(1000 + i), big); }
	e.durable(0);
	CHECK(e.last(0).index == before.index + 2);
	CHECK(e.get(0, key_of(1015), got) && got == big);

	std::printf("kv_strata batch: %d batches of 16 writes: %d records, %llu barriers, %llu writes\n", batches, records, (unsigned long long)barriers,
		(unsigned long long)written);
	if (failures) { std::fprintf(stderr, "%d failures\n", failures); }
	return failures ? 1 : 0;
}
