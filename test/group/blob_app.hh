// A small application on strata::group shaped like Xapiand's (cluster-release D3, D12, D16):
// documents whose stored record is a locator into a volume of the file set, blobs pushed into
// the current volume at apply, chunked uploads into staging files that a final entry consumes
// and retires, volume rotation at an entry, and a lower-term upload retired at the first entry of
// a higher term. Its state is the volume counter, the current volume's length and the open
// uploads. Every function reads the entry's starting state only, so effects are positional.
#pragma once

#include "strata/core/index.hh"
#include "strata/group/group.hh"
#include "strata/group/submit.hh"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace blob {

using index_type = strata::basic_index<strata::document_layout>;
using transaction = index_type::transaction;
using operation = index_type::operation;

inline constexpr std::uint64_t rotate_at = 48;  // a volume rotates once it holds this many bytes

struct upload {
	std::uint64_t term = 0, length = 0;
	bool operator==(const upload&) const = default;
};
struct state {
	std::uint64_t volume = 0, length = 0;  // the volume counter, the current volume's length
	std::map<std::uint64_t, upload> uploads;
	bool operator==(const state&) const = default;
};

inline std::string volume_name(std::uint64_t n) { return "vol-" + std::to_string(n); }
inline std::string staging_name(std::uint64_t id) { return "up-" + std::to_string(id); }
inline std::string locator(std::uint64_t volume, std::uint64_t offset, std::uint64_t length) {
	return std::to_string(volume) + ":" + std::to_string(offset) + ":" + std::to_string(length);
}

// Commands: fields joined by '|' (values never hold one).
//   P|key|value          put: the value pushed into the current volume, the key's record its locator
//   C|key|value|version  the same, if the key's version is `version` (refused otherwise)
//   D|key                delete
//   U|id|offset|bytes    an upload's chunk, into its staging file
//   F|id|key             an upload's final entry: its bytes into the volume, the staging retired
//   A|id                 an upload aborted: its staging retired
//   I|key                a document strata finds invalid (its key differs from the operation's)
// Requests the leader prepares (`prepare`), never in the log:
//   M|key|value          merge: a put at the version the leader predicts (its expectation)
inline std::vector<std::string_view> fields(std::string_view c) {
	std::vector<std::string_view> out;
	for (std::size_t at = 0;;) {
		const auto bar = c.find('|', at);
		out.push_back(c.substr(at, bar == std::string_view::npos ? std::string_view::npos : bar - at));
		if (bar == std::string_view::npos) { return out; }
		at = bar + 1;
	}
}
inline std::uint64_t number(std::string_view s) { return std::stoull(std::string(s)); }

struct app {
	using layout = strata::document_layout;
	using state = blob::state;

	// Every entry this process applied, for the test's oracle: index -> (term, kind, command).
	struct seen_entry {
		std::uint64_t term;
		kronuz::consensus::EntryKind kind;
		std::string command;
		bool operator==(const seen_entry&) const = default;
	};
	std::map<std::uint64_t, seen_entry>* seen = nullptr;
	bool diverged = false;
	// Called after an entry's file effects, before its strata write: a test starts work there.
	std::function<void(const strata::group::entry&)> after_effects;

	// The bytes a put or final pushes, from the starting state.
	template <class Files>
	std::optional<std::string> pushed(const state& s, const strata::group::entry& e, Files& files) const {
		if (e.kind != kronuz::consensus::EntryKind::Command) { return std::nullopt; }
		auto f = fields(e.command);
		if (f[0] == "P" || f[0] == "C") { return std::string(f[2]); }
		if (f[0] == "F") {
			auto it = s.uploads.find(number(f[1]));
			if (it == s.uploads.end() || it->second.term < e.term) { return std::nullopt; }
			return files.read(staging_name(it->first), 0, std::size_t(it->second.length));
		}
		return std::nullopt;
	}

	template <class Files>
	void effects(const state& s, const strata::group::entry& e, Files& files) {
		if (seen) {
			auto [it, fresh] = seen->try_emplace(e.index, seen_entry{e.term, e.kind, std::string(e.command)});
			if (!fresh && !(it->second == seen_entry{e.term, e.kind, std::string(e.command)})) { diverged = true; }
		}
		// An upload of a lower term fails at the first entry of a higher one.
		for (const auto& [id, u] : s.uploads) {
			if (u.term < e.term) { files.retire(staging_name(id), e.index); }
		}
		if (auto bytes = pushed(s, e, files)) {
			files.write(volume_name(s.volume), s.length, *bytes);  // positional: from the state
			if (s.length + bytes->size() >= rotate_at) { files.seal(volume_name(s.volume)); }
		}
		if (e.kind != kronuz::consensus::EntryKind::Command) { return; }
		auto f = fields(e.command);
		if (f[0] == "U") {
			const auto id = number(f[1]), offset = number(f[2]);
			auto it = s.uploads.find(id);
			if (it == s.uploads.end() ? offset == 0 : it->second.term >= e.term) { files.write(staging_name(id), offset, f[3]); }
		} else if (f[0] == "F" || f[0] == "A") {
			files.retire(staging_name(number(f[1])), e.index);
		}
		if (after_effects) { after_effects(e); }
	}

	// Qualified: the member's own name would change the unqualified one's meaning (GCC).
	std::optional<blob::transaction> transaction(const state& s, const strata::group::entry& e) const {
		if (e.kind != kronuz::consensus::EntryKind::Command) { return std::nullopt; }
		auto f = fields(e.command);
		operation o;
		if (f[0] == "P" || f[0] == "C" || f[0] == "F") {
			std::uint64_t length = 0;
			std::string key;
			if (f[0] == "F") {
				auto it = s.uploads.find(number(f[1]));
				if (it == s.uploads.end() || it->second.term < e.term) { return std::nullopt; }
				length = it->second.length;
				key = std::string(f[2]);
			} else {
				length = f[2].size();
				key = std::string(f[1]);
			}
			o.type = operation::replace_key;
			o.key = key;
			o.doc.key = key;
			o.doc.stored = locator(s.volume, s.length, length);
			if (f[0] == "C") { o.expected_version = number(f[3]); }
		} else if (f[0] == "D") {
			o.type = operation::delete_key;
			o.key = std::string(f[1]);
		} else if (f[0] == "I") {
			o.type = operation::replace_key;
			o.key = std::string(f[1]);
			o.doc.key = std::string(f[1]) + "!";
			o.doc.stored = "-";
		} else {
			return std::nullopt;
		}
		index_type::transaction t;
		t.ops.push_back(std::move(o));
		return t;
	}

	// The state moves whatever strata answered (D3): the next locator depends on it.
	void advance(state& s, const strata::group::entry& e) const {
		std::erase_if(s.uploads, [&](const auto& u) { return u.second.term < e.term; });
		if (e.kind != kronuz::consensus::EntryKind::Command) { return; }
		auto f = fields(e.command);
		std::uint64_t pushed_bytes = 0;
		bool pushes = false;
		if (f[0] == "P" || f[0] == "C") {
			pushed_bytes = f[2].size();
			pushes = true;
		} else if (f[0] == "F") {
			if (auto it = s.uploads.find(number(f[1])); it != s.uploads.end()) {
				pushed_bytes = it->second.length;
				pushes = true;
			}
		}
		if (pushes) {
			s.length += pushed_bytes;
			if (s.length >= rotate_at) {
				++s.volume;
				s.length = 0;
			}
		}
		if (f[0] == "U") {
			const auto id = number(f[1]), offset = number(f[2]);
			auto it = s.uploads.find(id);
			if (it == s.uploads.end() && offset == 0) { it = s.uploads.emplace(id, upload{e.term, 0}).first; }
			if (it != s.uploads.end()) { it->second.length = std::max(it->second.length, offset + f[3].size()); }
		} else if (f[0] == "F" || f[0] == "A") {
			s.uploads.erase(number(f[1]));
		}
	}

	// The leader's predictor (D106): the key's version from the applied index, moved by the
	// leader's own commands not applied yet. `stale` makes the next predictions miss by one (a
	// predictor that misses one of apply's checks), so a merge spends its placements.
	unsigned stale = 0;
	static std::uint64_t moved(std::uint64_t v, std::string_view command, std::string_view key) {
		auto f = fields(command);
		if (f.size() < 2 || f[1] != key) { return v; }
		if (f[0] == "P") { return v + 1; }
		if (f[0] == "D") { return 0; }
		if (f[0] == "C") {
			const auto e = number(f[3]);
			return (e == 0 ? v == 0 : e == v) ? v + 1 : v;
		}
		return v;
	}
	strata::group::prepared prepare(index_type& ix, const std::vector<std::string>& pending, std::string_view request) {
		auto f = fields(request);
		if (f[0] != "M") { return {std::string(request), false, std::nullopt}; }
		const auto found = ix.find_key(f[1]);
		std::uint64_t v = found ? found->second : 0;
		for (const auto& c : pending) { v = moved(v, c, f[1]); }
		if (stale) {
			--stale;
			++v;
		}
		return {"C|" + std::string(f[1]) + "|" + std::string(f[2]) + "|" + std::to_string(v), true, std::nullopt};
	}

	std::string encode(const state& s) const {
		std::ostringstream out;
		out << s.volume << ' ' << s.length << ' ' << s.uploads.size();
		for (const auto& [id, u] : s.uploads) { out << ' ' << id << ' ' << u.term << ' ' << u.length; }
		return out.str();
	}
	state decode(std::string_view bytes) const {
		state s;
		std::istringstream in{std::string(bytes)};
		std::size_t n = 0;
		in >> s.volume >> s.length >> n;
		for (std::size_t i = 0; i < n; ++i) {
			std::uint64_t id;
			upload u;
			in >> id >> u.term >> u.length;
			s.uploads.emplace(id, u);
		}
		if (!in) { throw strata::group::corrupt_bundle("blob app: application state"); }
		return s;
	}
};
static_assert(strata::group::application<app>);

}  // namespace blob
