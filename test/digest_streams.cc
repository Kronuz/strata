// T6's identity across versions (manifest-log): seeded operation streams over a keyed and a keyless
// index, half under a small memory budget, with commits, checkpoints, releases and merges. It prints
// one line per stream: the verdicts, a hash of the digest after every commit, the final digest, the
// digest after a reopen and at each checkpoint still pinned, newest first. test/digest_vs_ref.py
// builds it against this tree's headers and another commit's and compares the lines.
#include "strata/core/faultfs.hh"
#include "strata/core/index.hh"

#include <cstdio>
#include <deque>
#include <random>
#include <string>

using namespace strata;
using kv_index = basic_index<document_layout>;
using operation = kv_index::operation;
using transaction = kv_index::transaction;

static int failures = 0;

static std::string line(std::uint64_t seed, bool keyed) {
	std::mt19937_64 r(seed);
	faultfs::filesystem fs;
	faultfs::io io(fs);
	byte_budget mem{32 * 1024};
	kv_index::options o;
	o.keyed = keyed;
	o.identity_seed = seed + 1;
	if (seed % 2) {
		o.budget = &mem;
		o.buffer_share = 4 * 1024;
	}
	std::array<char, 16> uuid{};
	for (std::size_t i = 0; i < 16; ++i) { uuid[i] = char('a' + (seed + i) % 26); }
	auto key = [&] { return keyed ? "Q" + std::to_string(r() % 30) : std::string(); };
	auto body = [&] { return "doc " + std::to_string(r() % 1000) + std::string(r() % 60, 'x'); };
	std::string verdicts, out;
	std::uint64_t commits = 0, final_digest = 0;
	std::deque<std::uint64_t> pins;
	{
		auto ix = kv_index::create(io, o, uuid);
		for (int i = 0; i < 300; ++i) {
			const auto pick = r() % 100;
			if (pick < 4) {
				ix.force_merge(1 + r() % 3);
			} else if (pick < 6) {
				ix.maybe_merge();
			} else if (pick < 16) {
				ix.commit();
				commits = commits * 1099511628211ull ^ ix.digest();
			} else if (pick < 18) {
				pins.push_back(ix.checkpoint());
			} else if (pick < 20 && !pins.empty()) {
				ix.release(pins.front());
				pins.pop_front();
			} else {
				transaction t;
				for (std::size_t k = 1 + r() % 3; k-- > 0;) {
					operation op;
					switch (r() % 8) {
					case 0: op.type = operation::add; op.doc.key = key(); op.doc.stored = body(); break;
					case 1:
					case 2:
						if (keyed) {
							op.type = operation::replace_key;
							op.key = key();
							op.doc.key = op.key;
						} else {
							op.type = operation::replace_docid;
							op.docid = 1 + r() % 60;
						}
						op.doc.stored = body();
						break;
					case 3:
						if (keyed) {
							op.type = operation::delete_key;
							op.key = key();
						} else {
							op.type = operation::delete_docid;
							op.docid = 1 + r() % 60;
						}
						break;
					case 4: op.type = operation::delete_docid; op.docid = 1 + r() % 60; break;
					case 5: op.type = operation::set_meta; op.meta_key = "m" + std::to_string(r() % 5); op.meta_value = std::to_string(r() % 1000); break;
					case 6: op.type = operation::delete_meta; op.meta_key = "m" + std::to_string(r() % 5); break;
					default: op.type = operation::add; op.doc.stored = body(); break;
					}
					t.ops.push_back(std::move(op));
				}
				if (r() % 15 == 0) { t.set_schema = r() % 4; }
				verdicts += char('0' + int(ix.apply(t, {}).result));
			}
		}
		ix.commit();
		final_digest = ix.digest();
		out = std::to_string(seed) + (keyed ? " keyed " : " keyless ") + verdicts + " commits=" + std::to_string(commits) + " final=" + std::to_string(final_digest);
		ix.close();
	}
	const std::uint64_t reopened = kv_index::open(io, o).digest();
	out += " reopened=" + std::to_string(reopened);
	if (reopened != final_digest) { ++failures; }
	// Newest first: an open at `to` may drop the generations above it (D6.8).
	for (auto g = pins.rbegin(); g != pins.rend(); ++g) {
		try {
			out += " pin=" + std::to_string(kv_index::open(io, o, *g).digest());
		} catch (const std::exception& e) {
			out += std::string(" pin=") + e.what();
		}
	}
	return out;
}

int main(int argc, char** argv) {
	const std::uint64_t streams = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 40;
	for (std::uint64_t seed = 0; seed < streams; ++seed) {
		for (const bool keyed : {true, false}) {
			std::printf("%s\n", line(seed, keyed).c_str());
		}
	}
	if (failures) { std::fprintf(stderr, "digest_streams: %d reopens differ\n", failures); }
	return failures ? 1 : 0;
}
