// strata::group's install (contract strata:group-install; cluster-release D16; quest group-heal):
// a replica behind the leader's checkpoint takes the leader's bundle, a strata checkpoint pinned
// at or past it, and each file-set file to the bundle's length, in one transfer.
// docs/mechanisms/group-install.md.
//
// The leader pins (make_pin) and serves 64 KiB chunks by name (serve). The learner stages every
// file under staging names beside the replica (stage), keeping what it already holds whole or as
// a prefix, then marks the stage complete. Activation (activate) installs strata from the staged
// files, moves the set's files into place and sweeps; recovery repeats it (resume) after a crash,
// so the replica is either the old one or the complete new one.
#pragma once

#include "strata/core/index.hh"
#include "strata/group/bundle.hh"
#include "strata/group/file_set.hh"
#include "strata/group/repair.hh"

#include "task.hh"

#include <atomic>
#include <coroutine>
#include <crc32c/crc32c.hh>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace strata::group {

// An install the learner cannot finish now: the source failed, holds no such pin, or sent bytes
// that fail their CRC32C. Retryable: what was staged stays, so a retry fetches only the rest.
struct install_failed : std::runtime_error {
	install_failed(const std::string& what, std::error_code c = {}) : std::runtime_error(what), code(c) {}
	std::error_code code;
};
// A pin whose bytes fail their checks.
struct corrupt_pin : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// A file the install moves: its name (strata's, or the set's without its prefix), its length,
// and the CRC32C of its bytes [0, length).
struct install_file {
	std::string name;
	std::uint64_t length = 0;
	std::uint32_t crc = 0;
	bool operator==(const install_file&) const = default;
};

// What an install moves: the leader's bundle, the strata checkpoint generation pinned at or past
// it, the files strata reads to open that generation, and the set's files the bundle names.
struct install_pin {
	bundle base;
	std::uint64_t generation = 0;
	std::vector<install_file> strata, files;
	bool operator==(const install_pin&) const = default;
};

// What a stage moved: bytes fetched from the source and bytes the learner already held.
struct install_counts {
	std::uint64_t fetched = 0, kept = 0, chunks = 0;
	std::set<std::string> refetched;  // files whose held bytes failed the CRC32C
};

namespace install_detail {

inline constexpr char magic[8] = {'S', 'T', 'G', 'I', 'N', 'S', 0, 1};
// Staging names beside the replica: strata's under `is.`, the set's under `ia.` (a set name and
// its prefix stay within NAME_MAX), and the complete stage's marker `ip`. Neither strata nor the
// set's recovery reads or removes them.
inline constexpr std::string_view strata_prefix = "is.", files_prefix = "ia.", marker = "ip", marker_tmp = "ip.tmp";
inline constexpr std::size_t chunk = 65536;

inline bool strata_name(std::string_view n) {
	const auto p = index_detail::parse_name(n);
	return p && (p->type == index_detail::parsed_name::manifest || p->type == index_detail::parsed_name::segment);
}

inline void put_file(std::string& out, const install_file& f) {
	bundle_detail::put(out, f.name.size(), 2);
	out += f.name;
	bundle_detail::put(out, f.length, 8);
	bundle_detail::put(out, f.crc, 4);
}

inline std::unique_ptr<kronuz::journal::File> open_if(kronuz::journal::IO& io, std::string_view name) {
	try {
		return io.open_existing(name);
	} catch (const std::system_error& e) {
		if (e.code() == std::errc::no_such_file_or_directory) { return nullptr; }
		throw;
	}
}
inline void remove_if(kronuz::journal::IO& io, std::string_view name) {
	try {
		io.remove(name);
	} catch (const std::system_error& e) {
		if (e.code() != std::errc::no_such_file_or_directory) { throw; }
	}
}

// Reads [0, length) of `f` into `crc`, copying it to `to` when given; the bytes read.
inline std::uint64_t read_prefix(kronuz::journal::File& f, std::uint64_t length, kronuz::crc32c::Crc32c& crc, kronuz::journal::File* to = nullptr) {
	std::string buf(std::size_t(std::min<std::uint64_t>(length, chunk)), '\0');
	std::uint64_t at = 0;
	while (at < length) {
		const auto got = f.read_at(at, std::span<char>(buf.data(), std::size_t(std::min<std::uint64_t>(length - at, buf.size()))));
		if (!got) { break; }
		crc.update(buf.data(), got);
		if (to) {
			for (std::size_t done = 0; done < got;) { done += to->write_at(at + done, std::string_view(buf.data() + done, got - done)); }
		}
		at += got;
	}
	return at;
}
inline std::uint32_t crc_of(kronuz::journal::File& f, std::uint64_t length) {
	kronuz::crc32c::Crc32c crc;
	if (read_prefix(f, length, crc) != length) { throw install_failed("strata::group: an install file ends before its length"); }
	return crc.value();
}
// The first staged file of `p` whose bytes fail its CRC32C, by its reason: strata's while they are
// staged, and each set file not yet moved into place.
inline std::optional<std::string> staged_damage(kronuz::journal::IO& dir, const install_pin& p, bool strata_staged) {
	auto check = [&](std::string_view prefix, const install_file& f, bool required) -> std::optional<std::string> {
		const auto name = std::string(prefix) + f.name;
		auto file = open_if(dir, name);
		if (!file) {
			if (required) { return "strata::group: a staged file is missing: " + name; }
			return std::nullopt;
		}
		kronuz::crc32c::Crc32c crc;
		if (read_prefix(*file, f.length, crc) != f.length || crc.value() != f.crc) { return "strata::group: a staged file fails its CRC32C: " + name; }
		return std::nullopt;
	};
	if (strata_staged) {
		for (const auto& f : p.strata) {
			if (auto bad = check(strata_prefix, f, true)) { return bad; }
		}
	}
	for (const auto& f : p.files) {
		if (auto bad = check(files_prefix, f, false)) { return bad; }
	}
	return std::nullopt;
}

// A directory's names under `prefix`, the prefix taken off: strata reads a staged checkpoint
// through it as an index directory of its own.
class prefixed_io final : public kronuz::journal::IO {
public:
	prefixed_io(kronuz::journal::IO& io, std::string_view prefix) : io_(io), prefix_(prefix) {}

	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return io_.acquire_owner(create); }
	void open_existing_read_only_into(std::string_view n, kronuz::journal::File& f) override { io_.open_existing_read_only_into(path(n), f); }
	bool open_reclaim_candidate_read_only_into(std::string_view n, kronuz::journal::File& f) override { return io_.open_reclaim_candidate_read_only_into(path(n), f); }
	std::unique_ptr<kronuz::journal::File> open_reclaim_candidate_read_only(std::string_view n) override { return io_.open_reclaim_candidate_read_only(path(n)); }
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view n) override { return io_.open_existing(path(n)); }
	std::unique_ptr<kronuz::journal::File> open_existing_read_only(std::string_view n) override { return io_.open_existing_read_only(path(n)); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view n) override { return io_.create_exclusive(path(n)); }
	void replace(std::string_view a, std::string_view b) override { io_.replace(path(a), path(b)); }
	void remove(std::string_view n) override { io_.remove(path(n)); }
	void sync_directory() override { io_.sync_directory(); }
	std::optional<kronuz::journal::EntryFootprint> entry_footprint(std::string_view n) override { return io_.entry_footprint(path(n)); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override {
		struct cursor final : kronuz::journal::DirectoryCursor {
			std::unique_ptr<kronuz::journal::DirectoryCursor> base;
			std::string prefix;
			std::optional<std::string> next() override {
				while (auto n = base->next()) {
					if (n->starts_with(prefix)) { return n->substr(prefix.size()); }
				}
				return std::nullopt;
			}
		};
		auto c = std::make_unique<cursor>();
		c->base = io_.scan_directory();
		c->prefix = prefix_;
		return c;
	}

private:
	std::string path(std::string_view n) const { return prefix_ + std::string(n); }
	kronuz::journal::IO& io_;
	std::string prefix_;
};

// The awaited fetches of one window: every callback stores its answer, and the last resumes the
// stage on its own scheduler. The count starts one high so no answer resumes it mid-issue.
struct window_awaiter {
	struct slot {
		std::string name;
		std::error_code error;
		std::optional<std::string> bytes;
	};
	const std::function<void(std::string, std::function<void(std::error_code, std::optional<std::string>)>)>* fetch;
	std::vector<slot>* slots;
	std::atomic<std::size_t> left{0};
	std::coroutine_handle<> handle;
	task::scheduler_ref home;

	bool await_ready() const noexcept { return slots->empty(); }
	bool await_suspend(std::coroutine_handle<> h) {
		handle = h;
		home = task::current();
		left.store(slots->size() + 1, std::memory_order_relaxed);
		for (auto& s : *slots) {
			auto done = [this, &s](std::error_code e, std::optional<std::string> b) {
				s.error = e;
				s.bytes = std::move(b);
				if (left.fetch_sub(1, std::memory_order_acq_rel) == 1) { home.post(handle); }
			};
			try {
				(*fetch)(s.name, done);
			} catch (...) {
				done(std::make_error_code(std::errc::invalid_argument), std::nullopt);
			}
		}
		return left.fetch_sub(1, std::memory_order_acq_rel) != 1;
	}
	void await_resume() const noexcept {}
};

}  // namespace install_detail

// --- the pin -------------------------------------------------------------------------------

// Little-endian: magic and version (8), the generation (8), the bundle (4 + its bytes), strata's
// files then the set's (each 4 + per file 2 + name + 8 + 4), and a CRC32C of all of it (4).
inline std::string encode(const install_pin& p) {
	namespace d = bundle_detail;
	const auto base = encode(p.base);
	std::string out(install_detail::magic, sizeof install_detail::magic);
	d::put(out, p.generation, 8);
	d::put(out, base.size(), 4);
	out += base;
	for (const auto* list : {&p.strata, &p.files}) {
		d::put(out, list->size(), 4);
		for (const auto& f : *list) { install_detail::put_file(out, f); }
	}
	d::put(out, kronuz::crc32c::crc32c(out), 4);
	return out;
}

// Decodes a pin and checks it names what an install may write: strata's manifest and segment
// names, the set's names, and exactly the files the bundle names at its lengths and CRC32Cs.
inline install_pin decode_pin(std::string_view bytes) {
	bundle_detail::reader r{bytes};
	try {
		if (std::memcmp(r.take(sizeof install_detail::magic).data(), install_detail::magic, sizeof install_detail::magic) != 0) { throw corrupt_pin("strata::group: pin magic or version"); }
		install_pin p;
		p.generation = r.get(8);
		p.base = decode(r.take(std::size_t(r.get(4))));
		for (auto* list : {&p.strata, &p.files}) {
			const auto count = r.get(4);
			if (count > bytes.size()) { throw corrupt_pin("strata::group: pin file count"); }
			for (std::uint64_t i = 0; i < count; ++i) {
				install_file f;
				f.name = std::string(r.take(std::size_t(r.get(2))));
				f.length = r.get(8);
				f.crc = std::uint32_t(r.get(4));
				list->push_back(std::move(f));
			}
		}
		const auto body = r.at;
		if (std::uint32_t(r.get(4)) != kronuz::crc32c::crc32c(bytes.substr(0, body)) || r.at != bytes.size()) { throw corrupt_pin("strata::group: pin CRC32C"); }
		std::set<std::string, std::less<>> seen;
		for (const auto& f : p.strata) {
			if (!install_detail::strata_name(f.name) || !seen.insert(f.name).second) { throw corrupt_pin("strata::group: pin strata name"); }
		}
		if (p.files.size() != p.base.files.size()) { throw corrupt_pin("strata::group: pin files differ from its bundle"); }
		for (const auto& f : p.files) {
			auto it = p.base.files.find(f.name);
			if (it == p.base.files.end() || it->second.length != f.length || (it->second.crc && *it->second.crc != f.crc)) { throw corrupt_pin("strata::group: pin files differ from its bundle"); }
		}
		return p;
	} catch (const corrupt_bundle& e) {
		throw corrupt_pin(e.what());
	}
}

// --- the source (the leader) ---------------------------------------------------------------

// Pins strata's checkpoint for bundle `b`, at or past it (strata committed before `b` published),
// and lists every file with its length and CRC32C: the manifest log holding the generation (its
// prefix now), its segments, and the set's files to the bundle's lengths. On the chore, between
// entries. The pin keeps the generation's files until release_pin.
template <class Layout>
install_pin make_pin(basic_index<Layout>& ix, kronuz::journal::IO& dir, const bundle& b) {
	install_pin p;
	p.base = b;
	p.generation = ix.checkpoint();
	try {
		if (ix.applied().index < b.position) { throw std::logic_error("strata::group: strata is behind the bundle it would pin (P_H < P_b)"); }
		const auto holder = index_detail::log_holding(dir, p.generation);
		if (!holder) { throw files::generation_not_retained(p.generation); }
		auto add = [&](std::vector<install_file>& out, std::string name, std::string path, std::optional<std::uint64_t> length, std::optional<std::uint32_t> crc) {
			auto f = dir.open_existing(path);
			install_file e{std::move(name), length.value_or(f->size()), 0};
			e.crc = crc ? *crc : install_detail::crc_of(*f, e.length);
			out.push_back(std::move(e));
		};
		const auto log = index_detail::manifest_name(*holder);
		add(p.strata, log, log, std::nullopt, std::nullopt);
		for (const auto& [id, s] : basic_index<Layout>::read_generation(dir, p.generation).segments) {
			const auto name = index_detail::segment_name(id);
			add(p.strata, name, name, s.size, std::nullopt);
		}
		for (const auto& [name, f] : b.files) { add(p.files, name, std::string(bundle_detail::prefix) + name, f.length, f.crc); }
	} catch (...) {
		ix.release(p.generation);
		throw;
	}
	return p;
}
template <class Layout>
void release_pin(basic_index<Layout>& ix, const install_pin& p) {
	ix.release(p.generation);
}
// A pin lives in strata's manifest, so it outlives its source's process; no install does. The
// generations the index in `dir` keeps pinned, for a reopened source to release; a deferred
// index (no manifest yet) keeps none.
inline std::vector<std::uint64_t> held_pins(kronuz::journal::IO& dir) {
	if (!index_detail::log_holding(dir)) { return {}; }
	return index_detail::read_state(dir).pins;
}

// Names on the fetch lane, the group's own routing ahead of them: a pin by its bundle's
// position, and up to 64 KiB of a pin's file by class (`s` strata, `a` the set), offset and name.
inline std::string pin_name(std::uint64_t position) { return std::to_string(position) + "/pin"; }
inline std::string chunk_name(std::uint64_t position, char kind, std::uint64_t offset, std::string_view name) {
	return std::to_string(position) + "/" + kind + "/" + std::to_string(offset) + "/" + std::string(name);
}

// What `name` asks of pin `p`, read from `dir`: the pin itself, or up to 64 KiB of one of its
// files from an offset below its pinned length; nullopt for anything else.
inline std::optional<std::string> serve(kronuz::journal::IO& dir, const install_pin& p, std::string_view name) {
	const auto head = std::to_string(p.base.position) + "/";
	if (!name.starts_with(head)) { return std::nullopt; }
	name.remove_prefix(head.size());
	if (name == "pin") { return encode(p); }
	if (name.size() < 3 || (name[0] != 's' && name[0] != 'a') || name[1] != '/') { return std::nullopt; }
	const bool strata = name[0] == 's';
	name.remove_prefix(2);
	const auto slash = name.find('/');
	if (slash == std::string_view::npos) { return std::nullopt; }
	const auto offset = index_detail::number(name.substr(0, slash));
	name.remove_prefix(slash + 1);
	const auto& list = strata ? p.strata : p.files;
	const install_file* f = nullptr;
	for (const auto& e : list) {
		if (e.name == name) { f = &e; }
	}
	if (!offset || !f || *offset >= f->length) { return std::nullopt; }
	const auto n = std::size_t(std::min<std::uint64_t>(install_detail::chunk, f->length - *offset));
	auto file = dir.open_existing(strata ? f->name : std::string(bundle_detail::prefix) + f->name);
	std::string out(n, '\0');
	for (std::size_t done = 0; done < n;) {
		const auto got = file->read_at(*offset + done, std::span<char>(out.data() + done, n - done));
		if (!got) { throw std::runtime_error("strata::group: a pinned file ends before its length"); }
		done += got;
	}
	return out;
}

// --- the learner ---------------------------------------------------------------------------

// One fetch on the fetch lane (consensus transport/fetch.h's shape, the peer chosen): `done`
// runs once with an error, or the bytes, or nullopt when the source holds none.
using fetch_done = std::function<void(std::error_code, std::optional<std::string>)>;
using fetcher = std::function<void(std::string name, fetch_done done)>;

// The pin for `image`, the bundle consensus delivered, from the source. Coroutines here take their
// arguments by value, so a caller's temporaries may go before they finish; `dir` outlives them.
inline task::task<install_pin> fetch_pin(fetcher fetch, bundle image) {
	std::vector<install_detail::window_awaiter::slot> one{{pin_name(image.position), {}, {}}};
	co_await install_detail::window_awaiter{&fetch, &one};
	auto& s = one.front();
	if (s.error) { throw install_failed("strata::group: the install's pin fetch failed: " + s.error.message(), s.error); }
	if (!s.bytes) { throw install_failed("strata::group: the source holds no pin at the bundle"); }
	auto p = decode_pin(*s.bytes);
	if (!(p.base == image)) { throw install_failed("strata::group: the source's pin is for another bundle"); }
	co_return p;
}

// Stages pin `p` beside the replica in `dir`: each file under its staging name, keeping the bytes
// already staged or, for a set file, held under its own name, as far as they go, and fetching the
// rest in 64 KiB chunks, `window` at a time. A file whose kept bytes fail its CRC32C is fetched
// whole once more. Each file is synced, then the marker names the pin: the stage is complete.
// A fetch error, a missing answer or a chunk of the wrong size throws install_failed with the
// bytes before it kept; anything staged for another pin is removed first.
inline task::task<install_counts> stage(kronuz::journal::IO& dir, install_pin p, fetcher fetch, std::size_t window = 4) {
	namespace d = install_detail;
	if (!window) { throw std::invalid_argument("strata::group: an install window of zero"); }
	install_counts counts;
	struct plan {
		const install_file* file;
		char kind;
		std::string staged;
		std::unique_ptr<kronuz::journal::File> f;
		kronuz::crc32c::Crc32c crc;
		std::uint64_t held = 0, at = 0;
	};
	std::vector<plan> plans;
	std::set<std::string> wanted;
	for (const auto& f : p.strata) { plans.push_back({&f, 's', std::string(d::strata_prefix) + f.name, {}, {}, 0, 0}); }
	for (const auto& f : p.files) { plans.push_back({&f, 'a', std::string(d::files_prefix) + f.name, {}, {}, 0, 0}); }
	for (const auto& pl : plans) { wanted.insert(pl.staged); }

	// A marker left by an earlier stage goes before any staged byte changes; so does what no
	// file of this pin keeps.
	if (d::open_if(dir, d::marker)) {
		dir.remove(d::marker);
		dir.sync_directory();
	}
	d::remove_if(dir, d::marker_tmp);
	{
		std::vector<std::string> stale;
		for (auto c = dir.scan_directory(); auto n = c->next();) {
			if ((n->starts_with(d::strata_prefix) || n->starts_with(d::files_prefix)) && !wanted.count(*n)) { stale.push_back(*n); }
		}
		for (const auto& n : stale) { dir.remove(n); }
	}

	// What each file keeps: its staged bytes, or a set file's own, up to the pinned length.
	auto keep = [&](plan& pl) {
		pl.crc = {};
		if (auto f = d::open_if(dir, pl.staged)) {
			if (f->size() > pl.file->length) { f->truncate(pl.file->length); }
			pl.held = d::read_prefix(*f, std::min(f->size(), pl.file->length), pl.crc);
			pl.f = std::move(f);
		} else {
			pl.f = dir.create_exclusive(pl.staged);
			pl.held = 0;
			if (pl.kind == 'a') {
				if (auto own = d::open_if(dir, std::string(bundle_detail::prefix) + pl.file->name)) {
					pl.held = d::read_prefix(*own, std::min(own->size(), pl.file->length), pl.crc, pl.f.get());
				}
			}
		}
		pl.at = pl.held;
		counts.kept += pl.held;
	};
	auto fetch_rest = [&](std::vector<plan*> todo) -> task::task<void> {
		struct piece {
			plan* pl;
			std::uint64_t offset;
			std::size_t size;
		};
		std::vector<piece> pieces;
		for (auto* pl : todo) {
			for (auto at = pl->at; at < pl->file->length; at += d::chunk) {
				pieces.push_back({pl, at, std::size_t(std::min<std::uint64_t>(d::chunk, pl->file->length - at))});
			}
		}
		for (std::size_t first = 0; first < pieces.size(); first += window) {
			const auto last = std::min(pieces.size(), first + window);
			std::vector<d::window_awaiter::slot> slots;
			for (auto i = first; i < last; ++i) { slots.push_back({chunk_name(p.base.position, pieces[i].pl->kind, pieces[i].offset, pieces[i].pl->file->name), {}, {}}); }
			co_await d::window_awaiter{&fetch, &slots};
			// In order: the bytes before a failed chunk are written, none after it.
			for (auto i = first; i < last; ++i) {
				auto& s = slots[i - first];
				auto& pc = pieces[i];
				if (s.error) { throw install_failed("strata::group: an install fetch failed: " + s.error.message(), s.error); }
				if (!s.bytes) { throw install_failed("strata::group: the source no longer holds the pin's " + pc.pl->file->name); }
				if (s.bytes->size() != pc.size) { throw install_failed("strata::group: an install chunk of the wrong size"); }
				const std::string_view bytes = *s.bytes;
				for (std::size_t done = 0; done < bytes.size();) { done += pc.pl->f->write_at(pc.pl->at + done, bytes.substr(done)); }
				pc.pl->crc.update(bytes.data(), bytes.size());
				pc.pl->at += bytes.size();
				counts.fetched += bytes.size();
				++counts.chunks;
			}
		}
	};

	for (auto& pl : plans) { keep(pl); }
	std::vector<plan*> todo;
	for (auto& pl : plans) { todo.push_back(&pl); }
	co_await fetch_rest(todo);
	// Kept bytes that fail the CRC32C: the file again, whole; fetched bytes that fail it: the
	// source's fault, and the next try starts the file over.
	std::vector<plan*> again;
	for (auto& pl : plans) {
		if (pl.crc.value() == pl.file->crc) { continue; }
		pl.f->truncate(0);
		pl.crc = {};
		counts.kept -= pl.held;
		if (!pl.held) { throw install_failed("strata::group: an install file fails its CRC32C: " + pl.file->name); }
		pl.held = pl.at = 0;
		counts.refetched.insert(pl.file->name);
		again.push_back(&pl);
	}
	co_await fetch_rest(again);
	for (auto* pl : again) {
		if (pl->crc.value() != pl->file->crc) {
			pl->f->truncate(0);
			throw install_failed("strata::group: an install file fails its CRC32C: " + pl->file->name);
		}
	}
	for (auto& pl : plans) {
		pl.f->sync();
		pl.f.reset();
	}
	dir.sync_directory();
	{
		auto m = dir.create_exclusive(d::marker_tmp);
		const auto bytes = encode(p);
		for (std::size_t done = 0; done < bytes.size();) { done += m->write_at(done, std::string_view(bytes).substr(done)); }
		m->sync();
	}
	dir.replace(d::marker_tmp, d::marker);
	dir.sync_directory();
	co_return counts;
}

// The pin of a complete stage in `dir`, if any; a marker that fails its checks is none.
inline std::optional<install_pin> staged(kronuz::journal::IO& dir) {
	auto f = install_detail::open_if(dir, install_detail::marker);
	if (!f) { return std::nullopt; }
	try {
		return decode_pin(index_detail::read_all(*f));
	} catch (const corrupt_pin&) {
		return std::nullopt;
	}
}

// Removes every staging name and the marker, the marker first.
inline void sweep(kronuz::journal::IO& dir) {
	namespace d = install_detail;
	if (d::open_if(dir, d::marker)) {
		dir.remove(d::marker);
		dir.sync_directory();
	}
	std::vector<std::string> names;
	for (auto c = dir.scan_directory(); auto n = c->next();) {
		if (n->starts_with(d::strata_prefix) || n->starts_with(d::files_prefix) || *n == d::marker_tmp) { names.push_back(*n); }
	}
	for (const auto& n : names) { dir.remove(n); }
	if (!names.empty()) { dir.sync_directory(); }
}

// Makes the replica in `dir` the complete stage's pin `p`, once consensus holds its bundle as the
// replica's checkpoint: strata installs the pinned generation from the staged files (old or new
// content across a crash), unless it already holds it; the set's staged files replace its own;
// the stage is swept. Idempotent: recovery repeats it after a crash. The group's recovery from
// the bundle follows (truncation to the bundle's lengths, its state).
template <class Layout>
void activate(kronuz::journal::IO& dir, const install_pin& p, basic_index<Layout>& ix) {
	namespace d = install_detail;
	const bool strata_staged = d::open_if(dir, std::string(d::strata_prefix) + p.strata.front().name) != nullptr;
	// Staged bytes rested since the stage synced them: each is read back against its CRC32C
	// first. A file that fails marks the replica damaged, and the stage goes for a new one.
	if (auto bad = d::staged_damage(dir, p, strata_staged)) {
		mark_damaged(dir, *bad);
		sweep(dir);
		throw replica_damaged(*bad);
	}
	if (strata_staged) {
		// A deferred index commits its old state first: strata's install writes its copies
		// before its manifest, which a crash must not leave in a directory with no manifest log.
		if (!index_detail::log_holding(dir)) { ix.commit(); }
		d::prefixed_io staged_io(dir, d::strata_prefix);
		ix.install(staged_io, p.generation);
		// strata holds it now: the staged manifest log goes, durably, before the segments it
		// names, so a repeat after a crash finds the whole staged checkpoint or none of it.
		dir.remove(std::string(d::strata_prefix) + p.strata.front().name);
		dir.sync_directory();
		for (const auto& f : p.strata) { d::remove_if(dir, std::string(d::strata_prefix) + f.name); }
		dir.sync_directory();
	}
	for (const auto& f : p.files) {
		const auto from = std::string(d::files_prefix) + f.name;
		if (d::open_if(dir, from)) { dir.replace(from, std::string(bundle_detail::prefix) + f.name); }
	}
	dir.sync_directory();
	clear_damaged(dir);  // the replica holds the pin's bytes now (repair.hh)
	sweep(dir);
}

// Recovery's first step: a complete stage whose pin's bundle is the one consensus restored was
// persisted as this replica's install, and activation finishes; a stage at or below the restored
// bundle is stale and swept; a stage above it waits for consensus to retry. True when activated.
template <class Layout>
bool resume(kronuz::journal::IO& dir, const bundle& restored, basic_index<Layout>& ix) {
	auto p = staged(dir);
	if (p && p->base == restored) {
		activate(dir, *p, ix);
		return true;
	}
	if (!p || p->base.position <= restored.position) { sweep(dir); }
	return false;
}

}  // namespace strata::group
