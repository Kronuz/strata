// The group tests' oracle (quest group-core): a replica's digest (documents with versions,
// resolved through their locators; the file set; the application state) and a replay from
// empty of a log's committed commands through the application alone, on a fresh strata index
// and an in-memory file set, sharing no code with the group's apply, checkpoint or recovery.
#pragma once

#include "blob_app.hh"

#include "faultlab/faultfs.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace group_test {

namespace sg = strata::group;
namespace fl = kronuz::faultlab;
using group_type = sg::group<blob::app>;

// An in-memory file set with file_set's apply interface.
struct mem_files {
	struct file {
		std::string bytes;
		std::optional<std::uint32_t> crc;
	};
	std::map<std::string, file> live;
	void write(std::string_view name, std::uint64_t offset, std::string_view bytes) {
		auto& f = live[std::string(name)];
		if (f.bytes.size() < offset + bytes.size()) { f.bytes.resize(offset + bytes.size(), '\0'); }
		f.bytes.replace(offset, bytes.size(), bytes);
	}
	std::string read(std::string_view name, std::uint64_t offset, std::size_t n) { return live.at(std::string(name)).bytes.substr(offset, n); }
	void seal(std::string_view name) {
		auto& f = live.at(std::string(name));
		if (!f.crc) { f.crc = kronuz::crc32c::crc32c(f.bytes); }
	}
	void retire(std::string_view name, std::uint64_t) { live.erase(std::string(name)); }
};

struct digest {
	std::map<std::string, std::pair<std::uint64_t, std::string>> documents;  // key: version, bytes
	std::map<std::string, std::pair<std::optional<std::uint32_t>, std::string>> files;
	std::string state;
	bool operator==(const digest&) const = default;
};
inline const std::vector<std::string> keys = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n"};

template <class Read>
void documents(const blob::index_type& ix, Read&& read, digest& out) {
	for (const auto& key : keys) {
		auto found = ix.find_key(key);
		if (!found) { continue; }
		auto stored = ix.get(key);
		std::string loc(*stored);
		const auto a = loc.find(':'), b = loc.find(':', a + 1);
		const auto volume = std::stoull(loc.substr(0, a)), offset = std::stoull(loc.substr(a + 1, b - a - 1)), length = std::stoull(loc.substr(b + 1));
		out.documents[key] = {found->second, read(blob::volume_name(volume), offset, std::size_t(length))};
	}
}

inline digest of(group_type& g, blob::app& app) {
	digest out;
	documents(g.strata(), [&](const std::string& n, std::uint64_t o, std::size_t l) { return g.files().read(n, o, l); }, out);
	for (const auto& [name, f] : g.files().capture()) { out.files[name] = {f.crc, g.files().read(name, 0, std::size_t(f.length))}; }
	out.state = app.encode(g.state());
	return out;
}

// A replay from empty of the log's committed commands, through the application alone;
// `verdicts`, if given, gets strata's answer at each index that writes.
inline digest replay(const std::map<std::uint64_t, blob::app::seen_entry>& log, std::uint64_t through, std::map<std::uint64_t, strata::verdict::outcome>* verdicts = nullptr) {
	fl::filesystem fs;
	fl::io io(fs);
	strata::basic_index_options<strata::document_layout> o;
	o.keyed = true;
	auto ix = blob::index_type::create(io, o, {'r', 'e', 'f'});
	blob::app app;
	blob::state s;
	mem_files files;
	for (std::uint64_t i = 1; i <= through; ++i) {
		const auto& e = log.at(i);
		const sg::entry view{i, e.term, e.kind, e.command};
		app.effects(s, view, files);
		if (auto t = app.transaction(s, view)) {
			const auto v = ix.apply(*t, strata::files::position{i, 1});
			if (verdicts) { (*verdicts)[i] = v.result; }
		}
		app.advance(s, view);
	}
	digest out;
	documents(ix, [&](const std::string& n, std::uint64_t o, std::size_t l) { return files.read(n, o, l); }, out);
	for (const auto& [name, f] : files.live) { out.files[name] = {f.crc, f.bytes}; }
	out.state = app.encode(s);
	return out;
}

}  // namespace group_test
