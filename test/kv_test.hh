// What the core's lifecycle tests share: the document layout's index as a key-value store, its
// operations, and the checks. Core headers only (docs/design/core-split.md); the programs
// mirror prospect's lifecycle suites, which index through the search layout.
#pragma once

#include "strata/core/faultfs.hh"
#include "strata/core/index.hh"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

inline int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;
using kv_index = basic_index<document_layout>;
using operation = kv_index::operation;
using transaction = kv_index::transaction;
using document = kv_index::document;

// A 16-byte index uuid from a short name, zero padded.
inline std::array<char, 16> uuid_of(const char* name) {
	std::array<char, 16> id{};
	for (std::size_t i = 0; i < id.size() && name[i]; ++i) { id[i] = name[i]; }
	return id;
}

inline document doc(const std::string& key, const std::string& body) {
	document d;
	d.key = key;
	d.stored = body;
	return d;
}
inline operation add(const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::add;
	o.doc = doc(key, body);
	return o;
}
inline operation replace(const std::string& key, const std::string& body, std::optional<std::uint64_t> expected = std::nullopt) {
	operation o;
	o.type = operation::replace_key;
	o.key = key;
	o.doc = doc(key, body);
	o.expected_version = expected;
	return o;
}
inline operation erase(const std::string& key) {
	operation o;
	o.type = operation::delete_key;
	o.key = key;
	return o;
}
inline operation set_meta(const std::string& key, const std::string& value) {
	operation o;
	o.type = operation::set_meta;
	o.meta_key = key;
	o.meta_value = value;
	return o;
}
inline transaction tx(std::vector<operation> ops) {
	transaction t;
	t.ops = std::move(ops);
	return t;
}

inline bool throws(const std::function<void()>& f) {
	try {
		f();
	} catch (...) {
		return true;
	}
	return false;
}

// docid -> (version, stored) over every live document in every segment.
using contents_t = std::map<std::uint64_t, std::pair<std::uint64_t, std::string>>;
inline contents_t contents(const kv_index& ix) {
	contents_t out;
	for (const auto& s : ix.segments()) {
		for (ordinal_t o = 0; o < s.seg->core().doc_count(); ++o) {
			if (!s.live(o)) { continue; }
			const auto docid = s.seg->core().docid(o);
			if (out.count(docid)) { std::fprintf(stderr, "two live copies of docid %llu\n", (unsigned long long)docid); }
			out[docid] = {s.seg->get<keys_plugin>().version(o), std::string(s.seg->get<stored_plugin>().stored(o))};
		}
	}
	return out;
}
