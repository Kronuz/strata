// The install transfer (quest group-heal A1; docs/mechanisms/group-install.md): a leader pins a
// strata checkpoint at or past its bundle and serves 64 KiB chunks; a learner stages them, consensus
// persists the bundle, activation installs, and the learner catches up from the log. Checked by
// values: every digest (documents through their locators, the file set, the application state)
// equals the leader's and a replay's; bytes on the wire are counted exactly; a crash at every
// mutating operation of the stage, the activation and the recovery's own activation leaves the
// replica the old one or the complete new one. Cases: `values`, `retry`, `held`, `damaged`,
// `crash`, `deferred`, `rotted`, `threads` (TSan), or all with no argument.
#include "oracle.hh"

#include "strata/group/install.hh"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

namespace {

namespace sg = strata::group;
namespace fl = kronuz::faultlab;
namespace j = kronuz::journal;
using group_test::digest;

int failures = 0;
void check(bool ok, const std::string& what) {
	if (!ok) {
		++failures;
		std::cerr << "FAIL: " << what << '\n';
	}
}

// The log every replica applies: puts that rotate volumes, two uploads of 30 KiB chunks (one
// finished into a volume, one open across the bundle), and later entries past it.
std::map<std::uint64_t, blob::app::seen_entry> make_log() {
	std::map<std::uint64_t, blob::app::seen_entry> log;
	std::mt19937_64 rnd(7);
	auto text = [&](std::size_t n) {
		std::string s(n, ' ');
		for (auto& c : s) { c = char('a' + rnd() % 26); }
		return s;
	};
	std::uint64_t i = 0;
	auto add = [&](std::string c) { log[++i] = {1, kronuz::consensus::EntryKind::Command, std::move(c)}; };
	for (int k = 0; k < 10; ++k) { add("P|" + group_test::keys[std::size_t(k)] + "|" + text(9 + std::size_t(k) * 3)); }
	for (std::uint64_t at = 0; at < 7 * 30720; at += 30720) { add("U|1|" + std::to_string(at) + "|" + text(30720)); }
	add("F|1|k");
	for (int k = 0; k < 6; ++k) { add("P|" + group_test::keys[std::size_t(k)] + "|" + text(20)); }
	add("U|2|0|" + text(30720));
	add("U|2|30720|" + text(30720));
	add("D|c");
	// The bundle is at 26; the leader goes on.
	add("U|2|61440|" + text(20000));
	add("F|2|l");
	for (int k = 4; k < 12; ++k) { add("P|" + group_test::keys[std::size_t(k)] + "|" + text(31)); }
	add("U|3|0|" + text(1000));
	return log;
}
const auto the_log = make_log();
constexpr std::uint64_t old_at = 10, bundle_at = 26;
const std::uint64_t last = the_log.rbegin()->first;

strata::basic_index_options<strata::document_layout> index_options() {
	strata::basic_index_options<strata::document_layout> o;
	o.keyed = true;
	return o;
}

// A replica on its own faultfs directory: strata, the file set and the application state, with
// group's apply, capture and recovery steps written out (group.hh's, without the Driver).
struct replica {
	fl::filesystem fs;
	fl::io io{fs};
	std::optional<blob::index_type> ix;
	std::optional<sg::file_set> files;
	blob::app app;
	blob::state s;
	std::uint64_t applied = 0;
	strata::files::position head{};

	void create() {
		ix.emplace(blob::index_type::create(io, index_options(), {'l', 'd', 'r'}));
		files.emplace(io);
	}
	void apply(std::uint64_t through) {
		for (auto i = applied + 1; i <= through; ++i) {
			const auto& e = the_log.at(i);
			const sg::entry view{i, e.term, e.kind, e.command};
			app.effects(s, view, *files);
			if (auto t = app.transaction(s, view)) { ix->apply(*t, strata::files::position{i, 1}); }
			app.advance(s, view);
			applied = i;
		}
	}
	// group's checkpoint: the capture (with its mark), the syncs, strata's commit.
	sg::bundle checkpoint() {
		if (ix->applied() < strata::files::position{applied, 1}) { ix->apply(blob::transaction{}, strata::files::position{applied, 1}); }
		sg::bundle b{applied, 1, files->capture(), app.encode(s)};
		files->sync(b.files);
		ix->commit();
		return b;
	}
	// group's recovery, install's resume first: strata at P_H (deferred when absent), the set as
	// the bundle names it, the state. Throws where group fences.
	void recover(const std::optional<sg::bundle>& restored) {
		ix.reset();
		files.reset();
		try {
			ix.emplace(blob::index_type::open(io, index_options()));
		} catch (const strata::index_not_created&) {
			ix.emplace(blob::index_type::create_deferred(io, index_options(), {'l', 'r', 'n'}));
		}
		const auto b = restored.value_or(sg::bundle{});
		sg::resume(io, b, *ix);
		head = ix->applied();
		if (b.position > head.index) { throw std::runtime_error("P_b > P_H"); }
		files.emplace(io);
		files->recover(b.files);
		s = restored ? app.decode(b.state) : blob::state{};
		applied = b.position;
	}
	// The process stops: its objects go while the power is off, so they reach no device.
	void stop() {
		fs.power_off_after(0);
		ix.reset();
		files.reset();
	}
	digest of() {
		digest out;
		group_test::documents(*ix, [&](const std::string& n, std::uint64_t o, std::size_t l) { return files->read(n, o, l); }, out);
		for (const auto& [name, f] : files->capture()) { out.files[name] = {f.crc, files->read(name, 0, std::size_t(f.length))}; }
		out.state = app.encode(s);
		return out;
	}
};

// The leader: applied through the bundle, checkpointed, then on past it, and pinned there.
struct leader_state {
	replica r;
	sg::bundle b;
	sg::install_pin pin;
	std::uint64_t total = 0;  // every byte the pin names
	leader_state() {
		r.create();
		r.apply(bundle_at);
		b = r.checkpoint();
		r.apply(last);
		pin = sg::make_pin(*r.ix, r.io, b);
		for (const auto* list : {&pin.strata, &pin.files}) {
			for (const auto& f : *list) { total += f.length; }
		}
	}
};
leader_state& leader() {
	static leader_state l;
	return l;
}

// Answers fetches from the leader's pin, counting bytes; `fail` decides a fetch's error.
struct source {
	std::uint64_t fetches = 0, bytes = 0;
	std::function<std::error_code(std::uint64_t)> fail;
	sg::fetcher fetch() {
		return [this](std::string name, sg::fetch_done done) {
			const auto n = ++fetches;
			if (fail) {
				if (const auto e = fail(n)) {
					done(e, std::nullopt);
					return;
				}
			}
			auto got = sg::serve(leader().r.io, leader().pin, name);
			if (got) { bytes += got->size(); }
			done({}, std::move(got));
		};
	}
};

template <class T>
T run(task::task<T> work) {
	task::inline_scheduler s;
	std::optional<T> out;
	std::exception_ptr error;
	task::spawn(task::scheduler_ref::of(s), std::move(work), [&](std::exception_ptr e, std::optional<T> v) {
		error = e;
		out = std::move(v);
	});
	while (s.run()) {}
	if (error) { std::rethrow_exception(error); }
	return std::move(*out);
}

// The stage's bytes on the learner: what its staging names hold.
std::uint64_t staged_bytes(replica& r) {
	std::uint64_t n = 0;
	for (const auto& name : r.fs.list()) {
		if (name.starts_with("is.") || name.starts_with("ia.")) { n += r.io.open_existing(name)->size(); }
	}
	return n;
}

// Install end to end: the pin, the stage, consensus's persist (the restored bundle), recovery's
// activation, then catch-up from the log; the digest must equal the leader's and a replay's.
void install_and_check(replica& learner, source& src, const std::string& what, std::size_t window = 4) {
	const auto pin = run(sg::fetch_pin(src.fetch(), leader().b));
	check(pin == leader().pin, what + ": the fetched pin is the leader's");
	(void)run(sg::stage(learner.io, pin, src.fetch(), window));
	learner.recover(leader().b);
	check(learner.head.index >= bundle_at, what + ": strata at or past the bundle after activation");
	const auto names = learner.fs.list();
	check(std::none_of(names.begin(), names.end(), [](const std::string& n) { return n.starts_with("is.") || n.starts_with("ia.") || n == "ip"; }), what + ": the stage swept");
	learner.apply(last);
	check(learner.of() == leader().r.of(), what + ": digest equals the leader's");
	check(learner.of() == group_test::replay(the_log, last), what + ": digest equals a replay");
}

void values() {
	auto& l = leader();
	// The pin: strata's manifest log and segments, and the set's files at the bundle's lengths.
	check(l.pin.generation > 0 && !l.pin.strata.empty() && l.pin.strata.front().name.starts_with("manifest-"), "the pin names strata's manifest log first");
	check(l.pin.files.size() == l.b.files.size(), "the pin names every file of the bundle");
	std::uint64_t set_bytes = 0;
	for (const auto& f : l.pin.files) {
		check(l.b.files.at(f.name).length == f.length, "a set file's pinned length is the bundle's: " + f.name);
		set_bytes += f.length;
	}
	check(l.b.files.count("up-2") && l.b.files.at("up-2").length == 61440 && !l.b.files.at("up-2").crc, "the open upload is pinned at its bundle length, 61,440 B");
	check(l.r.files->length("up-2") == 0 && l.r.files->retired_count() > 0, "the leader retired it past the bundle");
	check(sg::decode_pin(sg::encode(l.pin)) == l.pin, "the pin round-trips");
	{
		auto bytes = sg::encode(l.pin);
		bytes[20] ^= 1;
		bool refused = false;
		try {
			(void)sg::decode_pin(bytes);
		} catch (const sg::corrupt_pin&) {
			refused = true;
		}
		check(refused, "a damaged pin is refused");
	}
	// A whole pin whose strata list names a file outside strata's names is refused.
	for (const char* name : {"../x", "app.vol-1", "ip"}) {
		auto bad = l.pin;
		bad.strata.back().name = name;
		bool refused = false;
		try {
			(void)sg::decode_pin(sg::encode(bad));
		} catch (const sg::corrupt_pin&) {
			refused = true;
		}
		check(refused, std::string("a pin naming strata file ") + name + " is refused");
	}
	// The serve: chunks of at most 64 KiB, nothing past a file, nothing it does not pin.
	const auto& up = *std::find_if(l.pin.files.begin(), l.pin.files.end(), [](const auto& f) { return f.name == "up-2"; });
	check(sg::serve(l.r.io, l.pin, sg::chunk_name(bundle_at, 'a', 0, "up-2"))->size() == 61440, "a 61,440 B file is one chunk");
	const auto& vol = *std::max_element(l.pin.files.begin(), l.pin.files.end(), [](const auto& a, const auto& b) { return a.length < b.length; });
	check(vol.length > 3 * 65536, "the largest set file spans more than three chunks");
	check(sg::serve(l.r.io, l.pin, sg::chunk_name(bundle_at, 'a', 65536, vol.name))->size() == 65536, "a middle chunk is 64 KiB");
	check(!sg::serve(l.r.io, l.pin, sg::chunk_name(bundle_at, 'a', up.length, "up-2")), "nothing at a file's pinned length");
	check(!sg::serve(l.r.io, l.pin, sg::chunk_name(bundle_at, 'a', 0, "vol-99")), "nothing for a name the pin does not hold");
	check(!sg::serve(l.r.io, l.pin, sg::chunk_name(bundle_at, 's', 0, "../x")), "nothing outside the pin");
	check(!sg::serve(l.r.io, l.pin, sg::chunk_name(bundle_at + 1, 'a', 0, "up-2")), "nothing for another pin");

	// A learner with nothing: every pinned byte crosses once, in ceil(length / 64 KiB) chunks.
	replica learner;
	learner.recover(std::nullopt);
	source src;
	const auto pin = run(sg::fetch_pin(src.fetch(), l.b));
	const auto pin_bytes = src.bytes;
	const auto counts = run(sg::stage(learner.io, pin, src.fetch()));
	std::uint64_t chunks = 0;
	for (const auto* list : {&pin.strata, &pin.files}) {
		for (const auto& f : *list) { chunks += (f.length + 65535) / 65536; }
	}
	check(counts.fetched == l.total && src.bytes - pin_bytes == l.total, "a fresh learner fetches every pinned byte once: " + std::to_string(counts.fetched) + " of " + std::to_string(l.total));
	check(counts.kept == 0 && counts.chunks == chunks, "in " + std::to_string(chunks) + " chunks, " + std::to_string(counts.chunks) + " fetched");
	check(set_bytes > 4 * 65536, "the set's share spans more than four chunks: " + std::to_string(set_bytes) + " B");
	check(sg::staged(learner.io) == pin, "the marker names the pin");
	learner.recover(l.b);
	learner.apply(last);
	check(learner.of() == l.r.of(), "the learner's digest equals the leader's after catch-up");
	check(learner.of() == group_test::replay(the_log, last), "and a replay's");

	// The same install through the driver of every other case.
	replica second;
	second.recover(std::nullopt);
	source again;
	install_and_check(second, again, "a second fresh learner", 1);
}

// A failed fetch (a deadline's timed_out, the peer's io_error) fails the install with what it
// staged kept: the retry fetches exactly the rest.
void retry() {
	auto& l = leader();
	replica learner;
	learner.recover(std::nullopt);
	const auto pin = run(sg::fetch_pin(source{}.fetch(), l.b));
	std::uint64_t held = 0;
	for (const auto code : {std::errc::timed_out, std::errc::io_error}) {
		source src;
		const std::uint64_t at = code == std::errc::timed_out ? 3 : 6;  // the failing fetch, mid-window
		src.fail = [&](std::uint64_t n) { return n == at ? std::make_error_code(code) : std::error_code{}; };
		bool failed = false;
		try {
			(void)run(sg::stage(learner.io, pin, src.fetch(), 4));
		} catch (const sg::install_failed& e) {
			failed = e.code == std::make_error_code(code);
		}
		check(failed, "a fetch ending " + std::make_error_code(code).message() + " fails the install with it");
		check(!sg::staged(learner.io), "a failed stage leaves no marker");
		const auto now = staged_bytes(learner);
		check(now > held, "the stage kept what came before the failure");
		held = now;
	}
	source src;
	const auto counts = run(sg::stage(learner.io, pin, src.fetch(), 4));
	check(counts.kept == held && counts.fetched == l.total - held && src.bytes == l.total - held,
		  "the retry fetches only what the learner lacks or holds short: " + std::to_string(src.bytes) + " B for " + std::to_string(l.total - held) + " lacking");
	check(counts.refetched.empty(), "and refetches nothing it kept");
	learner.recover(l.b);
	learner.apply(last);
	check(learner.of() == l.r.of(), "the retried install's digest equals the leader's");
}

// A learner that was a replica (applied through 10, checkpointed there) holds the set's older
// files: sealed volumes whole and the current one short. It keeps them and fetches the rest.
void held() {
	auto& l = leader();
	replica learner;
	learner.create();
	learner.apply(old_at);
	const auto old = learner.checkpoint();
	learner.recover(old);
	std::uint64_t own = 0;
	for (const auto& [name, f] : old.files) {
		if (l.b.files.count(name)) { own += std::min(f.length, l.b.files.at(name).length); }
	}
	check(own > 0, "the old replica holds " + std::to_string(own) + " B of the bundle's files");
	source src;
	const auto pin = run(sg::fetch_pin(src.fetch(), l.b));
	src.bytes = 0;
	const auto counts = run(sg::stage(learner.io, pin, src.fetch()));
	check(counts.kept == own && counts.fetched == l.total - own && src.bytes == l.total - own, "it fetches " + std::to_string(src.bytes) + " B, the total less the " + std::to_string(own) + " B it holds");
	learner.recover(l.b);
	learner.apply(last);
	check(learner.of() == l.r.of(), "its digest equals the leader's");
}

// Held bytes that differ from the leader's (a flipped byte in the old replica's sealed volume)
// fail the file's CRC32C: that file is fetched whole, and nothing else again.
void damaged() {
	auto& l = leader();
	replica learner;
	learner.create();
	learner.apply(old_at);
	const auto old = learner.checkpoint();
	learner.recover(old);
	std::string sealed;
	for (const auto& [name, f] : old.files) {
		if (f.crc && l.b.files.count(name)) { sealed = name; }
	}
	check(!sealed.empty(), "the old replica holds a sealed volume");
	{
		auto f = learner.io.open_existing("app." + sealed);
		char c = 0;
		f->read_at(3, std::span<char>(&c, 1));
		c ^= 0x20;
		f->write_at(3, std::string_view(&c, 1));
	}
	source src;
	const auto pin = run(sg::fetch_pin(src.fetch(), l.b));
	src.bytes = 0;
	const auto counts = run(sg::stage(learner.io, pin, src.fetch()));
	std::uint64_t own = 0;
	for (const auto& [name, f] : old.files) {
		if (l.b.files.count(name)) { own += std::min(f.length, l.b.files.at(name).length); }
	}
	const auto length = l.b.files.at(sealed).length;
	check(counts.refetched == std::set<std::string>{sealed}, "the damaged file is fetched again: " + sealed);
	check(counts.kept == own - length && counts.fetched == l.total - own + length && src.bytes == counts.fetched,
		  "it fetches the lacking " + std::to_string(l.total - own) + " B and the damaged file's " + std::to_string(length) + " B: " + std::to_string(src.bytes));
	learner.recover(l.b);
	learner.apply(last);
	check(learner.of() == l.r.of(), "the repaired install's digest equals the leader's");
}

// A crash at every mutating operation of the stage, consensus's persist, the activation and
// the recovery that finishes it, a process crash and power losses at four seeds: the learner
// recovers to the old replica (its own bundle at 10) or to the complete new one, which then
// catches up. Then the activating recovery is crashed at each of its operations and recovered.
void crash() {
	auto& l = leader();
	const auto old_digest = group_test::replay(the_log, old_at);
	const auto new_digest = group_test::replay(the_log, last);
	auto fresh = [&](replica& r) {
		r.create();
		r.apply(old_at);
		const auto b = r.checkpoint();
		r.recover(b);
		return b;
	};
	// One clean run counts the learner's mutating operations from the stage to the end of the
	// recovery's activation.
	std::uint64_t total = 0;
	{
		replica r;
		fresh(r);
		const auto before = r.fs.mutations();
		source src;
		(void)run(sg::stage(r.io, l.pin, src.fetch()));
		r.recover(l.b);
		total = r.fs.mutations() - before;
	}
	check(total > 20, "the install mutates " + std::to_string(total) + " times");
	std::uint64_t runs = 0, olds = 0, news = 0;
	for (std::uint64_t n = 0; n <= total; ++n) {
		for (const int power : {0, 1, 2, 3, 4}) {  // a process crash, or a power loss at seed 1-4
			for (const bool persists : {true, false}) {
				replica r;
				const auto old = fresh(r);
				std::optional<sg::bundle> restored = old;
				r.fs.power_off_after(n);
				try {
					source src;
					(void)run(sg::stage(r.io, l.pin, src.fetch()));
					// consensus persists the install once the stage validates; without `persists`, the
					// crash falls between the stage and the persist.
					if (persists) {
						restored = l.b;
						r.recover(restored);
					}
				} catch (...) {
				}
				r.stop();
				if (power) {
					r.fs.crash(n * 37 + std::uint64_t(power) * 2 + (persists ? 1 : 0));
				} else {
					r.fs.power_on();
				}
				const std::string what = "crash at " + std::to_string(n) + (power ? " (power, seed " + std::to_string(power) + ")" : " (process)") + (persists ? "" : ", not persisted");
				try {
					r.recover(restored);
					if (restored == old) {
						check(r.of() == old_digest, what + ": the old replica");
						++olds;
					} else {
						r.apply(last);
						check(r.of() == new_digest, what + ": the complete new one");
						++news;
					}
					check(!(restored == l.b) || !sg::staged(r.io), what + ": no activation left pending");
				} catch (const std::exception& e) {
					check(false, what + ": recovery failed: " + e.what());
				}
				++runs;
			}
		}
	}
	check(olds > 0 && news > 0, "both outcomes reached: " + std::to_string(olds) + " old, " + std::to_string(news) + " new of " + std::to_string(runs));
	std::cout << "crash: " << runs << " runs over " << total << " operations, " << olds << " old, " << news << " new\n";

	// The recovery that activates, crashed at each of its operations and recovered again.
	std::uint64_t activation = 0;
	{
		replica r;
		fresh(r);
		(void)run(sg::stage(r.io, l.pin, source{}.fetch()));
		const auto before = r.fs.mutations();
		r.recover(l.b);
		activation = r.fs.mutations() - before;
	}
	for (std::uint64_t n = 0; n <= activation; ++n) {
		for (const int power : {0, 1, 2, 3, 4}) {
			replica r;
			fresh(r);
			(void)run(sg::stage(r.io, l.pin, source{}.fetch()));
			r.fs.power_off_after(n);
			try {
				r.recover(l.b);
			} catch (...) {
			}
			r.stop();
			if (power) {
				r.fs.crash(n * 41 + std::uint64_t(power));
			} else {
				r.fs.power_on();
			}
			const std::string what = "activation crash at " + std::to_string(n) + (power ? " (power, seed " + std::to_string(power) + ")" : " (process)");
			try {
				r.recover(l.b);
				r.apply(last);
				check(r.of() == new_digest, what + ": the complete new replica");
			} catch (const std::exception& e) {
				check(false, what + ": recovery failed: " + e.what());
			}
		}
	}
	std::cout << "crash: activation " << activation << " operations, each crashed and recovered\n";
}

// The crash case on a fresh learner, its index deferred (no manifest log until its first
// commit), as group's grow and replace paths install: a crash at every mutating operation from
// the stage through the activating recovery leaves the empty replica or the complete new one.
void deferred() {
	auto& l = leader();
	const auto empty_digest = group_test::replay(the_log, 0);
	const auto new_digest = group_test::replay(the_log, last);
	std::uint64_t total = 0;
	{
		replica r;
		r.recover(std::nullopt);
		const auto before = r.fs.mutations();
		(void)run(sg::stage(r.io, l.pin, source{}.fetch()));
		r.recover(l.b);
		total = r.fs.mutations() - before;
	}
	check(total > 20, "the deferred install mutates " + std::to_string(total) + " times");
	std::uint64_t runs = 0, olds = 0, news = 0;
	for (std::uint64_t n = 0; n <= total; ++n) {
		for (const int power : {0, 1, 2, 3, 4}) {
			for (const bool persists : {true, false}) {
				replica r;
				r.recover(std::nullopt);
				check(!strata::index_detail::log_holding(r.io), "the fresh learner's index is deferred");
				std::optional<sg::bundle> restored;
				r.fs.power_off_after(n);
				try {
					(void)run(sg::stage(r.io, l.pin, source{}.fetch()));
					if (persists) {
						restored = l.b;
						r.recover(restored);
					}
				} catch (...) {
				}
				r.stop();
				if (power) {
					r.fs.crash(n * 43 + std::uint64_t(power) * 2 + (persists ? 1 : 0));
				} else {
					r.fs.power_on();
				}
				const std::string what = "deferred crash at " + std::to_string(n) + (power ? " (power, seed " + std::to_string(power) + ")" : " (process)") + (persists ? "" : ", not persisted");
				try {
					r.recover(restored);
					if (!restored) {
						check(r.of() == empty_digest, what + ": the empty replica");
						++olds;
					} else {
						r.apply(last);
						check(r.of() == new_digest, what + ": the complete new one");
						++news;
					}
					check(!restored || !sg::staged(r.io), what + ": no activation left pending");
				} catch (const std::exception& e) {
					check(false, what + ": recovery failed: " + e.what());
				}
				++runs;
			}
		}
	}
	check(olds > 0 && news > 0, "both outcomes reached: " + std::to_string(olds) + " empty, " + std::to_string(news) + " new of " + std::to_string(runs));
	std::cout << "deferred: " << runs << " runs over " << total << " operations, " << olds << " empty, " << news << " new\n";
}

// A segment body that rotted behind a valid footer (F7). The source's pin checks each segment
// whole and refuses it; a pin made before the rot fails the source's check_pin; a learner whose
// staged bytes match the pin's CRC32C yet fail strata's checks refuses them at check_stage, and
// its activation of such a stage marks it damaged and sweeps, never escaping as corrupt_segment.
std::string segment_of(replica& r, const std::string& prefix = "") {
	for (const auto& n : r.fs.list()) {
		if (!n.starts_with(prefix)) { continue; }
		const auto p = strata::index_detail::parse_name(n.substr(prefix.size()));
		if (p && p->type == strata::index_detail::parsed_name::segment) { return n; }
	}
	return {};
}
void rot(replica& r, const std::string& name) {
	auto f = r.io.open_existing(name);
	const auto at = f->size() / 2;
	char c = 0;
	f->read_at(at, std::span<char>(&c, 1));
	c = char(c ^ 0x5a);
	f->write_at(at, std::string_view(&c, 1));
	f->sync();
}
template <class E, class F>
bool throws(F&& f) {
	try {
		f();
	} catch (const E&) {
		return true;
	} catch (...) {}
	return false;
}
void rotted() {
	auto& l = leader();
	{
		replica r;
		r.create();
		r.apply(bundle_at);
		const auto b = r.checkpoint();
		const auto pin = sg::make_pin(*r.ix, r.io, b);
		check(!throws<std::exception>([&] { sg::check_pin(r.io, pin); }), "a sound source's pin passes its check");
		const auto seg = segment_of(r);
		check(!seg.empty(), "the source holds a segment");
		rot(r, seg);
		check(throws<strata::corrupt_index>([&] { sg::check_pin(r.io, pin); }), "a pin whose segment rotted since fails its check");
		check(throws<strata::corrupt_segment>([&] { (void)sg::make_pin(*r.ix, r.io, b); }), "a source whose segment body rotted refuses to pin it");
	}
	replica learner;
	learner.create();
	learner.apply(old_at);
	const auto old = learner.checkpoint();
	learner.recover(old);
	const auto before = learner.of();
	(void)run(sg::stage(learner.io, l.pin, source{}.fetch()));
	const auto staged_seg = segment_of(learner, "is.");
	check(!staged_seg.empty(), "the stage holds a segment");
	rot(learner, staged_seg);
	// The pin a rotted source would send: its CRC32C taken from the rotted bytes.
	auto bad = l.pin;
	for (auto& f : bad.strata) {
		if ("is." + f.name == staged_seg) { f.crc = sg::install_detail::crc_of(*learner.io.open_existing(staged_seg), f.length); }
	}
	check(throws<strata::corrupt_segment>([&] { sg::check_stage<strata::document_layout>(learner.io, bad); }), "the learner's check refuses a stage whose segment fails strata's checks");
	{
		const auto bytes = sg::encode(bad);
		auto m = learner.io.open_existing("ip");
		m->truncate(0);
		for (std::size_t done = 0; done < bytes.size();) { done += m->write_at(done, std::string_view(bytes).substr(done)); }
		m->sync();
	}
	check(throws<sg::replica_damaged>([&] { learner.recover(l.b); }), "the activation of a stage strata refuses answers replica_damaged");
	check(sg::marked_damaged(learner.io), "the replica is marked damaged");
	const auto names = learner.fs.list();
	check(std::none_of(names.begin(), names.end(), [](const std::string& n) { return n.starts_with("is.") || n.starts_with("ia.") || n == "ip"; }), "the refused stage is swept");
	learner.recover(old);
	check(learner.of() == before, "the replica's own content is as it was");
}

// Fetches answered on other threads, as the transport's loop answers them: the stage resumes on
// its own scheduler. Run under TSan.
void threads() {
	auto& l = leader();
	replica learner;
	learner.recover(std::nullopt);
	std::mutex mu;
	std::condition_variable cv;
	std::deque<std::function<void()>> work;
	bool stop = false;
	std::vector<std::thread> pool;
	for (int t = 0; t < 4; ++t) {
		pool.emplace_back([&] {
			for (;;) {
				std::function<void()> w;
				{
					std::unique_lock lock(mu);
					cv.wait(lock, [&] { return stop || !work.empty(); });
					if (work.empty()) { return; }
					w = std::move(work.front());
					work.pop_front();
				}
				w();
			}
		});
	}
	std::atomic<std::uint64_t> bytes{0};
	sg::fetcher fetch = [&](std::string name, sg::fetch_done done) {
		std::lock_guard lock(mu);
		work.push_back([&, name = std::move(name), done = std::move(done)] {
			auto got = sg::serve(l.r.io, l.pin, name);
			if (got) { bytes += got->size(); }
			done({}, std::move(got));
		});
		cv.notify_one();
	};
	task::blocking_scheduler s;
	bool finished = false;
	std::exception_ptr error;
	std::optional<sg::install_counts> counts;
	task::spawn(task::scheduler_ref::of(s), sg::stage(learner.io, l.pin, fetch, 8), [&](std::exception_ptr e, std::optional<sg::install_counts> c) {
		error = e;
		counts = std::move(c);
		finished = true;
	});
	s.drive([&] { return finished; }, [] { return true; });
	{
		std::lock_guard lock(mu);
		stop = true;
	}
	cv.notify_all();
	for (auto& t : pool) { t.join(); }
	check(!error && counts && counts->fetched == l.total && bytes == l.total, "a stage answered on four threads fetches every byte once");
	learner.recover(l.b);
	learner.apply(last);
	check(learner.of() == l.r.of(), "and its digest equals the leader's");
}

}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases = {
		{"values", values}, {"retry", retry}, {"held", held}, {"damaged", damaged}, {"threads", threads}, {"crash", crash}, {"deferred", deferred}, {"rotted", rotted}};
	try {
		for (const auto& [name, fn] : cases) {
			if (only.empty() || only == name) {
				fn();
				std::cout << name << (failures ? ": failures so far " + std::to_string(failures) : ": ok") << '\n';
			}
		}
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << '\n';
		return 1;
	}
	return failures ? 1 : 0;
}
