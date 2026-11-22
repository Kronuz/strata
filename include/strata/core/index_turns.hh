// basic_index's turn log: attach, join and leave, group commit, replay and durable waits (standalone-turn-log)
#pragma once

#include "strata/core/index.hh"

namespace strata {

template <class Layout>
void basic_index<Layout>::await_durable(files::position at) const {
	if constexpr (command::loggable<Layout>) {
		if (!opts_.log || !at.index) { return; }
		try {
			opts_.log->await(opts_.log_group, at.index);
		} catch (const kronuz::journal::group_closed&) {
			throw index_closed("strata: the index closed before its write's wait returned");
		}
	}
}

template <class Layout>
void basic_index<Layout>::attach_log(bool created) {
	if constexpr (command::loggable<Layout>) {
		const std::optional<index_detail::turn_member> member = index_detail::turn_group_of(m_.attributes);
		if (!opts_.log) {
			if (member) { throw log_mismatch("strata: a logged index opens only on its turn-log host"); }
			return;
		}
		const std::uint64_t g = opts_.log_group;
		if (!g) { throw std::invalid_argument("strata: a logged index needs a group"); }
		if (member && (member->host != opts_.log->identity() || member->group != g)) {
			throw log_mismatch("strata: the index's turn_group names another host or group");
		}
		const kronuz::journal::GroupStatus status = opts_.log->status(g);
		if (status == kronuz::journal::GroupStatus::lost) { throw log_lost("strata: the turn log lost the index's group"); }
		if (status == kronuz::journal::GroupStatus::none) {
			join(member.has_value(), created);
			return;
		}
		if (!member) { throw log_mismatch("strata: the group is another index's"); }
		if (status == kronuz::journal::GroupStatus::closed) { opts_.log->reopen(g); }
		const std::vector<kronuz::journal::TurnRecord> records = opts_.log->records(g);
		std::optional<std::pair<std::uint64_t, std::string>> table;
		if (records.empty()) {
			table = opts_.log->table_record(g);
			if (table && opts_.log->sequence(g) > table->first) { table.reset(); }
		}
		std::optional<command::checkpoint_record> cp;
		if (table) {
			// The reopen (D7): the table's record covers every batch through its base.
			cp = command::decode_checkpoint(table->second);
			if (cp->uuid != m_.uuid) { throw log_mismatch("strata: the host's record of the group is another index's"); }
			if (cur_.applied < cp->at) { throw log_lost("strata: the index's head is older than its turn-log checkpoint"); }
			if (cp->at < cur_.applied) { throw log_mismatch("strata: the index's head is past the host's record of it"); }
			kronuz::journal::TurnRecord r;
			r.group = g;
			r.sequence = std::max(table->first, opts_.log->retired(g)) + 1;  // after what a closed life retired
			r.kind = kronuz::journal::TurnRecordKind::Checkpoint;
			r.covers = table->first;
			r.payload = table->second;
			opts_.log->stage(std::move(r));
		}
		const std::uint64_t staged = seal_log(table ? table->first : ~std::uint64_t(0));
		if (table) {
			logged_->checkpointed = cp->at;
			logged_->last_checkpoint = staged;
			logged_->checkpoint_payload = std::move(table->second);
			opts_.log->attach(g);
			return;
		}
		if (cur_.applied.index > staged) { throw log_lost("strata: the index's head names turn-log records its log lacks"); }
		if (records.empty() && staged > cur_.applied.index) { throw log_lost("strata: the host released the group's records; reopen the host"); }
		if (!records.empty() && records.front().kind == kronuz::journal::TurnRecordKind::Checkpoint) {
			const command::checkpoint_record front = command::decode_checkpoint(records.front().payload);
			if (front.uuid != m_.uuid) { throw log_mismatch("strata: the turn log's group holds another index's checkpoint"); }
			if (cur_.applied < front.at) { throw log_lost("strata: the index's head is older than its turn-log checkpoint"); }
			logged_->checkpointed = front.at;
			logged_->last_checkpoint = records.front().sequence;
			logged_->checkpoint_payload = records.front().payload;
		}
		replay_log(records);
		opts_.log->release(g);
		opts_.log->attach(g);
	}
}

template <class Layout>
std::uint64_t basic_index<Layout>::seal_log(std::uint64_t durable)
	requires(command::loggable<Layout>)
{
	auto st = std::make_shared<index_detail::log_state>();
	st->group = opts_.log_group;
	st->command_bytes = opts_.command_bytes;
	st->block = opts_.log->block_bytes();
	st->visible.store(visible_, std::memory_order_relaxed);
	const std::uint64_t staged = opts_.log->seal(
		st->group,
		[st, log = opts_.log](std::uint64_t room, bool first) {
			auto r = st->seal(room, first);
			if (r) { log->sealed(); }
			return r;
		},
		[st](std::uint64_t s) { st->durable_through(s); }, st.get());
	st->synced.store(std::min(staged, durable), std::memory_order_release);
	st->next = staged + 1;
	logged_ = std::move(st);  // from here the destructor unseals it
	return staged;
}

template <class Layout>
void basic_index<Layout>::join(bool member, bool created)
	requires(command::loggable<Layout>)
{
	if (cur_.applied != files::position{}) {
		if (member) { throw log_lost("strata: the index's head names turn-log records its host lacks"); }
		throw std::invalid_argument("strata: an index with log positions cannot join a turn log");
	}
	opts_.log->create(opts_.log_group);
	seal_log(~std::uint64_t(0));
	join_owed_ = true;
	if (member) {
		stage_checkpoint();
		return;
	}
	attributes_owed_ = m_.attributes;
	attributes_owed_->push_back(index_detail::turn_group_attribute(opts_.log->identity(), opts_.log_group));
	std::sort(attributes_owed_->begin(), attributes_owed_->end(), [](const auto& a, const auto& b) { return a.tag < b.tag; });
	changed();
	if (!created) { commit(); }
}

template <class Layout>
void basic_index<Layout>::leave() {
	if constexpr (command::loggable<Layout>) {
		if (!logged_ || fenced_ || opts_.log->failed()) { return; }
		commit();
		if (logged_->last_checkpoint + 1 != logged_->next) { stage_checkpoint(true); }
		await_log_through(logged_->last_checkpoint);
		opts_.log->close(logged_->group, logged_->last_checkpoint, logged_->checkpoint_payload);
	}
}

template <class Layout>
void basic_index<Layout>::replay_log(const std::vector<kronuz::journal::TurnRecord>& records)
	requires(command::loggable<Layout>)
{
	replaying_ = true;
	struct done {
		bool& on;
		~done() { on = false; }
	} ended{replaying_};
	for (const auto& r : records) {
		if (r.kind != kronuz::journal::TurnRecordKind::Batch) {
			// The front checkpoint is attach_log's; any other kind is another build's log.
			if (&r == &records.front() && r.kind == kronuz::journal::TurnRecordKind::Checkpoint) { continue; }
			throw unsupported_index("strata: a turn-log record of a kind strata does not write");
		}
		const std::vector<std::string_view> commands = command::decode_batch(r.payload);
		for (std::size_t i = 0; i < commands.size(); ++i) {
			const files::position at{r.sequence, i + 1};
			const transaction t = command::decode<Layout>(commands[i]);
			for (int chores = 0;; ++chores) {
				const verdict v = apply(t, at);
				if (v.result == verdict::accepted || v.result == verdict::skipped) { break; }
				if (v.result == verdict::no_space) { throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "strata: replay: no space"); }
				// Each owed answer comes at most once per transaction: map, flush, drain.
				if (chores == 3) { throw replay_diverged(at, v.result); }
				if (v.result == verdict::flush_owed) {
					flush_point();
				} else if (v.result == verdict::drain_owed) {
					(void)reclaim_for_space();
				} else if (v.result == verdict::map_owed) {
					promote();
				} else {
					throw replay_diverged(at, v.result);
				}
			}
		}
	}
}

template <class Layout>
void basic_index<Layout>::flush_point() {
	if (logged_ && !replaying_) {
		commit();
	} else {
		refresh();
	}
}

template <class Layout>
void basic_index<Layout>::stage_checkpoint(bool force) {
	if constexpr (command::loggable<Layout>) {
		if (!logged_ || replaying_) { return; }
		const files::position x = cur_.applied;
		if (!force && !join_owed_ && (x == files::position{} || x == logged_->checkpointed)) { return; }
		command::checkpoint_record cp;
		cp.uuid = m_.uuid;
		cp.generation = m_.generation;
		cp.at = x;
		logged_->checkpoint(command::encode_checkpoint(cp), x, visible_);
		join_owed_ = false;
		opts_.log->wake();
	}
}

template <class Layout>
void basic_index<Layout>::detach_log() noexcept {
	if constexpr (command::loggable<Layout>) {
		if (logged_ && opts_.log) { opts_.log->unseal(logged_->group, logged_.get()); }
	}
	logged_.reset();
}

template <class Layout>
void basic_index<Layout>::join_log(std::string_view cmd) {
	if constexpr (command::loggable<Layout>) {
		bool opened = false;
		try {
			if (opts_.log_seam) { opts_.log_seam(); }
			for (;;) {
				const auto j = logged_->join(cmd, visible_, cur_.applied);
				if (!j.full) {
					opened = j.opened;
					break;
				}
				const auto* st = logged_.get();
				opts_.log->await_sealed(logged_->group, [st, s = j.full] { return st->handed.load(std::memory_order_acquire) >= s; });
			}
		} catch (...) {
			fenced_ = true;
			throw;
		}
		changed();
		if (opened) { opts_.log->wake(); }
	}
}

template <class Layout>
void basic_index<Layout>::await_log() {
	if (logged_) { await_log_through(cur_.applied.index); }
}

template <class Layout>
void basic_index<Layout>::await_log_through(std::uint64_t s) {
	if constexpr (command::loggable<Layout>) {
		if (logged_->synced.load(std::memory_order_acquire) >= s) { return; }
		try {
			opts_.log->await(logged_->group, s);
		} catch (...) {
			fenced_ = true;  // fail-stop (D9)
			throw;
		}
	}
}

}  // namespace strata
