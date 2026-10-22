// Today's single-writer buffer (strata main's document-layout index) through the bake-off's T1
// loop: W writer threads take one lock per transaction around index::apply, which is how W callers
// share a buffer that has one writer. Informational: apply carries the engine's whole stage 1
// (admission, undo log, charge), which the prototypes' lean loops do not. Writes only.
#include "lanebuf_work.hh"

#include "strata/core/budget.hh"
#include "strata/core/index.hh"

#include "completion/posix.h"

#include <filesystem>
#include <mutex>

using lbwork::args;
using kv_index = strata::basic_index<strata::document_layout>;

namespace {

struct main_engine {
	struct ticket {
		unsigned idx;
	};
	std::mutex mu;
	std::unique_ptr<kv_index> ix;
	explicit main_engine(kronuz::journal::PosixIO& io, strata::byte_budget& budget) {
		kv_index::options o;
		o.key_prefix = "Q";
		o.budget = &budget;
		o.buffer_share = std::uint64_t(8) << 30;
		const std::array<char, 16> uuid{'l', 'b', 'm', 'a', 'i', 'n'};
		ix = std::make_unique<kv_index>(kv_index::create(io, o, uuid));
	}
	ticket checkout(unsigned i) { return {i}; }
	bool put(ticket&, std::string_view key, std::string_view value) {
		kv_index::transaction tx;
		tx.ops.resize(1);
		auto& op = tx.ops[0];
		op.type = kv_index::operation::replace_key;
		op.key = key;
		op.doc.key = op.key;
		op.doc.stored.assign(value);
		std::lock_guard l(mu);
		return ix->apply(tx, {}).result == strata::verdict::accepted;
	}
	void checkin(ticket&) {}
	void quiesce() {}
	std::string stats_json() { return lbwork::fmt("{\"buffered\":%llu}", (unsigned long long)ix->buffered_count()); }
};

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s fill|overwrite [options]\n", argv[0]);
		return 2;
	}
	const args a(argc - 1, argv + 1);
	const std::string dir = a.str("--dir", ".scratch/lbmain");
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	std::filesystem::permissions(dir, std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
	strata::byte_budget budget(std::uint64_t(16) << 30);
	int rc = 0;
	{
		kronuz::journal::PosixIO io(dir, 0600, kronuz::journal::PosixIOOptions{true, true});
		main_engine e(io, budget);
		const std::string mode = argv[1];
		if (mode == "fill" || mode == "overwrite") {
			std::printf("{\"impl\":\"main\",\"result\":%s}\n", lbwork::fill(e, a).c_str());
		} else {
			std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
			rc = 2;
		}
	}
	std::filesystem::remove_all(dir);
	return rc;
}
