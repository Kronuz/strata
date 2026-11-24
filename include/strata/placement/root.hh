// The root authority's application on strata::group (cluster-release D5, D7; contract
// strata:placement-records; docs/mechanisms/placement-root.md). Its state is the strata index of
// records.hh's records; a command is a list of conditional record writes applied as one strata
// transaction. The leader prepares every command from the records it reads (D106's prepare,
// submit.hh): node ids, group ids and placement are chosen there, and apply only checks and writes.
#pragma once

#include "strata/core/index.hh"
#include "strata/group/group.hh"
#include "strata/group/submit.hh"
#include "strata/placement/placement.hh"
#include "strata/placement/records.hh"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace strata::placement {

using index_type = basic_index<document_layout>;

// --- the application ---------------------------------------------------------------------

struct root {
	using layout = document_layout;
	struct state {
		bool operator==(const state&) const = default;
	};

	template <class Files>
	void effects(const state&, const group::entry&, Files&) {}

	// A command that fails decode_command is a transaction strata refuses as invalid (an
	// operation without a key): it writes nothing and its proposer hears `invalid`.
	std::optional<index_type::transaction> transaction(const state&, const group::entry& e) const {
		if (e.kind != kronuz::consensus::EntryKind::Command) { return std::nullopt; }
		index_type::transaction t;
		command c;
		try {
			c = decode_command(e.command);
		} catch (const corrupt_record&) {
			index_type::operation o;
			o.type = index_type::operation::delete_key;
			t.ops.push_back(std::move(o));
			return t;
		}
		for (auto& w : c.writes) {
			index_type::operation o;
			o.type = w.kind == write::put ? index_type::operation::replace_key : index_type::operation::delete_key;
			o.key = w.key;
			o.expected_version = w.expected;
			if (w.kind == write::put) {
				o.doc.key = std::move(w.key);
				o.doc.stored = std::move(w.value);
			}
			t.ops.push_back(std::move(o));
		}
		return t;
	}

	void advance(state&, const group::entry&) const {}
	std::string encode(const state&) const { return std::string(1, char(record_format)); }
	state decode(std::string_view bytes) const {
		if (bytes.size() != 1 || std::uint8_t(bytes[0]) != record_format) { throw group::corrupt_bundle("strata::placement: root state"); }
		return {};
	}
};
static_assert(group::application<root>);

// --- reads -------------------------------------------------------------------------------

// A record and its strata version.
template <class R>
struct versioned {
	std::uint64_t version = 0;
	R record;
};

// The index plus this leader's commands placed and not applied (submit.hh's `pending`), each
// assumed to apply: the view a command is prepared from. A pending command that loses its race
// makes the next one conflict, and that one is prepared again.
class view {
public:
	struct value {
		std::uint64_t version = 0;
		std::string bytes;
	};

	explicit view(const index_type& ix, const std::vector<std::string>& pending = {}) : ix_(&ix) {
		for (const auto& p : pending) {
			command c;
			try {
				c = decode_command(p);
			} catch (const corrupt_record&) {
				continue;  // refused at apply: it writes nothing
			}
			for (auto& w : c.writes) {
				const auto before = get(w.key);
				if (w.kind == write::put) {
					overlay_[w.key] = value{before ? before->version + 1 : 1, std::move(w.value)};
				} else {
					overlay_[w.key] = std::nullopt;
				}
			}
		}
	}

	std::optional<value> get(std::string_view key) const {
		if (auto it = overlay_.find(key); it != overlay_.end()) { return it->second; }
		auto found = ix_->find_key(key);
		if (!found) { return std::nullopt; }
		return value{found->second, *ix_->get(key)};
	}

	// The live records in [from, to) in key order: f(key, version, bytes).
	template <class F>
	void scan(std::string_view from, std::string_view to, F&& f) const {
		std::vector<std::pair<std::string, value>> base;
		ix_->scan(from, to, [&](std::string_view k, std::string_view v, std::uint64_t version) { base.push_back({std::string(k), value{version, std::string(v)}}); });
		auto o = overlay_.lower_bound(from);
		const auto o_end = overlay_.lower_bound(to);
		std::size_t b = 0;
		while (b < base.size() || o != o_end) {
			if (o == o_end || (b < base.size() && base[b].first < o->first)) {
				f(std::string_view(base[b].first), base[b].second.version, std::string_view(base[b].second.bytes));
				++b;
				continue;
			}
			if (b < base.size() && base[b].first == o->first) { ++b; }
			if (o->second) { f(std::string_view(o->first), o->second->version, std::string_view(o->second->bytes)); }
			++o;
		}
	}

	std::optional<versioned<directory_record>> directory() const { return read(directory_key(), decode_directory); }
	std::optional<versioned<groups_record>> groups() const { return read(groups_key(), decode_groups); }
	std::optional<versioned<node_record>> node(node_id id) const { return read(node_key(id), decode_node); }
	std::optional<versioned<load_record>> load(node_id id) const { return read(load_key(id), decode_load); }
	std::optional<versioned<namespace_record>> namespace_at(std::string_view path) const { return read(namespace_key(path), decode_namespace); }

	// The active nodes and their loads; every node has a load record (add_node writes both).
	std::vector<candidate> active() const {
		std::vector<candidate> out;
		scan(nodes_begin(), nodes_end(), [&](std::string_view k, std::uint64_t, std::string_view v) {
			if (decode_node(v).state != node_state::active) { return; }
			const auto id = *node_of(k);
			const auto l = load(id);
			if (!l) { throw corrupt_record("strata::placement: a node without a load record"); }
			out.push_back({id, l->record.groups, l->record.leaders});
		});
		return out;
	}

private:
	template <class D>
	std::optional<versioned<std::invoke_result_t<D, std::string_view>>> read(const std::string& key, D decode) const {
		auto v = get(key);
		if (!v) { return std::nullopt; }
		return versioned<std::invoke_result_t<D, std::string_view>>{v->version, decode(v->bytes)};
	}

	const index_type* ix_;
	std::map<std::string, std::optional<value>, std::less<>> overlay_;
};

// --- operations: commands prepared at the leader ------------------------------------------

namespace detail {

inline group::prepared refuse(verdict::outcome why, std::uint64_t current = 0) {
	verdict v;
	v.result = why;
	v.current_version = current;
	return {{}, false, v};
}
inline group::prepared chosen(const command& c) { return {encode(c), true, std::nullopt}; }
inline write put(std::string key, std::uint64_t expected, std::string value) { return {write::put, std::move(key), expected, std::move(value)}; }
inline write erase(std::string key, std::uint64_t expected) { return {write::erase, std::move(key), expected, {}}; }

// The directory rewritten: its version, the directory version, moves.
inline write touch_directory(const view& v, std::optional<std::uint64_t> next_node = std::nullopt) {
	auto d = v.directory();
	directory_record r = d ? d->record : directory_record{};
	if (next_node) { r.next_node = *next_node; }
	return put(directory_key(), d ? d->version : 0, encode(r));
}

// Load writes for the members of `before` and `after` whose counts differ.
inline void load_changes(const view& v, const std::vector<shard>& before, const std::vector<shard>& after, command& c) {
	std::map<node_id, std::pair<std::int64_t, std::int64_t>> delta;  // groups, leaders
	const auto count = [&](const std::vector<shard>& shards, std::int64_t sign) {
		for (const auto& s : shards) {
			for (std::size_t i = 0; i < s.members.size(); ++i) {
				auto& d = delta[s.members[i].node];
				d.first += sign;
				if (i == 0) { d.second += sign; }
			}
		}
	};
	count(before, -1);
	count(after, +1);
	for (const auto& [id, d] : delta) {
		if (d.first == 0 && d.second == 0) { continue; }
		auto l = v.load(id);
		if (!l) { throw corrupt_record("strata::placement: a member without a load record"); }
		auto r = l->record;
		r.groups = std::uint64_t(std::int64_t(r.groups) + d.first);
		r.leaders = std::uint64_t(std::int64_t(r.leaders) + d.second);
		c.writes.push_back(put(load_key(id), l->version, encode(r)));
	}
}

}  // namespace detail

// A new node: the directory's next id, its record and a zero load. `assigned` hears the id.
inline group::prepared add_node(const view& v, node_record n, node_id* assigned = nullptr) {
	const auto d = v.directory();
	const node_id id = d ? d->record.next_node : 1;
	n.directory_version = (d ? d->version : 0) + 1;
	command c;
	c.writes.push_back(detail::touch_directory(v, id + 1));
	c.writes.push_back(detail::put(node_key(id), 0, encode(n)));
	c.writes.push_back(detail::put(load_key(id), 0, encode(load_record{})));
	if (assigned) { *assigned = id; }
	return detail::chosen(c);
}

// A node's record replaced (its addresses, pin, state): refused when the node is absent.
inline group::prepared update_node(const view& v, node_id id, node_record n) {
	const auto cur = v.node(id);
	if (!cur) { return detail::refuse(verdict::version_conflict, 0); }
	const auto d = v.directory();
	n.directory_version = (d ? d->version : 0) + 1;
	command c;
	c.writes.push_back(detail::touch_directory(v));
	c.writes.push_back(detail::put(node_key(id), cur->version, encode(n)));
	return detail::chosen(c);
}

// A node removed: refused while it is absent or a member of any shard.
inline group::prepared remove_node(const view& v, node_id id) {
	const auto cur = v.node(id);
	const auto l = v.load(id);
	if (!cur || !l) { return detail::refuse(verdict::version_conflict, 0); }
	if (l->record.groups != 0) { return detail::refuse(verdict::version_conflict, cur->version); }
	command c;
	c.writes.push_back(detail::touch_directory(v));
	c.writes.push_back(detail::erase(node_key(id), cur->version));
	c.writes.push_back(detail::erase(load_key(id), l->version));
	return detail::chosen(c);
}

// A namespace for `path`, its shards placed on the active nodes: refused (`version_conflict`)
// when the path exists, `invalid` for a bad path, no shards, rf 0 or no active node.
// `placed` hears the record.
inline group::prepared create_namespace(const view& v, std::string_view path, std::size_t shards, std::uint32_t rf, std::uint64_t settings_version = 0, namespace_record* placed = nullptr) {
	if (!valid_path(path) || shards == 0 || rf == 0) { return detail::refuse(verdict::invalid); }
	if (const auto cur = v.namespace_at(path)) { return detail::refuse(verdict::version_conflict, cur->version); }
	auto nodes = v.active();
	if (nodes.empty()) { return detail::refuse(verdict::invalid); }
	const auto g = v.groups();
	group_id next = g ? g->record.next_group : 1;
	namespace_record ns;
	ns.rf = rf;
	ns.settings_version = settings_version;
	for (auto& members : place(nodes, shards, rf)) { ns.shards.push_back(shard{next++, 1, std::move(members)}); }
	command c;
	c.writes.push_back(detail::put(namespace_key(path), 0, encode(ns)));
	c.writes.push_back(detail::put(groups_key(), g ? g->version : 0, encode(groups_record{next})));
	detail::load_changes(v, {}, ns.shards, c);
	if (placed) { *placed = ns; }
	return detail::chosen(c);
}

// A namespace's record replaced (repair: members, roles, incarnations), the loads following:
// refused when the path is absent or the new record names an unknown node.
inline group::prepared replace_namespace(const view& v, std::string_view path, const namespace_record& next) {
	if (!valid_path(path)) { return detail::refuse(verdict::invalid); }
	const auto cur = v.namespace_at(path);
	if (!cur) { return detail::refuse(verdict::version_conflict, 0); }
	for (const auto& s : next.shards) {
		for (const auto& m : s.members) {
			if (!v.load(m.node)) { return detail::refuse(verdict::invalid); }
		}
	}
	std::string bytes = encode(next);
	if (!valid_record(namespace_key(path), bytes)) { return detail::refuse(verdict::invalid); }
	command c;
	c.writes.push_back(detail::put(namespace_key(path), cur->version, std::move(bytes)));
	detail::load_changes(v, cur->record.shards, next.shards, c);
	return detail::chosen(c);
}

// A namespace deleted, its members' loads decremented: refused when the path is absent.
inline group::prepared delete_namespace(const view& v, std::string_view path) {
	if (!valid_path(path)) { return detail::refuse(verdict::invalid); }
	const auto cur = v.namespace_at(path);
	if (!cur) { return detail::refuse(verdict::version_conflict, 0); }
	command c;
	c.writes.push_back(detail::erase(namespace_key(path), cur->version));
	detail::load_changes(v, cur->record.shards, {}, c);
	return detail::chosen(c);
}

}  // namespace strata::placement
