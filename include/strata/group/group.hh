// strata::group: one replication group per shard on consensus's Driver (cluster-release D2, D3,
// D11, D16, D18; docs/mechanisms/replication-group.md). A `loop` owns one Driver and its Sink;
// each `group` binds a consensus group to a strata index it borrows (its catalog slot's, D169;
// docs/mechanisms/group-catalog.md), an application file set and an application that turns
// committed commands into file effects, a strata transaction and its own state. Apply runs on
// the group's chore, off the loop; the loop only enqueues.
#pragma once

#include "strata/core/index.hh"
#include "strata/group/answer.hh"
#include "strata/group/bundle.hh"
#include "strata/group/file_set.hh"

#include "consensus/driver.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace strata::group {

namespace cs = kronuz::consensus;
namespace jn = kronuz::journal;

// A committed entry as the application sees it.
struct entry {
	cs::Index index = 0;
	cs::Term term = 0;
	cs::EntryKind kind = cs::EntryKind::Command;
	std::string_view command;
};

// The application (Xapiand's command, D3): every function of the entry's starting state, so a
// replica, a replay and a retry agree. `effects` writes the file set positionally (ENOSPC
// throws std::system_error and the entry retries from its start); `transaction` is the strata
// write, if any; `advance` moves the state whatever strata answered.
template <class A>
concept application = requires(A& a, const typename A::state& s, typename A::state& m, const entry& e, file_set& f, std::string_view bytes) {
	typename A::layout;
	a.effects(s, e, f);
	{ a.transaction(s, e) } -> std::same_as<std::optional<typename basic_index<typename A::layout>::transaction>>;
	a.advance(m, e);
	{ a.encode(s) } -> std::same_as<std::string>;
	{ a.decode(bytes) } -> std::same_as<typename A::state>;
};

// A group a loop dispatches its Sink's notices to; every call is on the loop.
class member {
public:
	virtual ~member() = default;
	virtual void committed(cs::Committed range) = 0;
	virtual void fenced(std::string reason) = 0;
	virtual void install_needed(cs::NodeId peer) = 0;
};

class loop;

// The loop's Sink: notices to their group. It must not throw (DRIVER.md): what it does is enqueue.
struct dispatch {
	loop* owner;
	void operator()(cs::GroupId, cs::Send) const;
	void operator()(cs::GroupId, cs::Committed) const;
	void operator()(cs::GroupId, cs::RoleChanged) const {}
	void operator()(cs::GroupId, cs::ActivateInstall) const;
	void operator()(cs::GroupId, cs::CheckpointPublished) const {}
	void operator()(cs::GroupId, cs::RepairCompleted) const {}
	void operator()(cs::GroupId, cs::SnapshotNeeded) const;
	void operator()(cs::GroupId, cs::SnapshotTransmit) const {}
	void operator()(cs::GroupId, cs::SnapshotReleased) const {}
	void operator()(cs::GroupId, cs::Fenced) const;
};

// One reactor loop's Driver and its groups. Its calls are the loop's (on its scheduler).
class loop {
public:
	using driver_type = cs::Driver<dispatch>;

	loop(task::scheduler_ref scheduler, jn::completion_ref io, jn::Host& host, std::uint32_t index = 0, std::uint64_t seed = 0)
		: sink_{this}, driver_(cs::Seams{scheduler, io, host, index, seed}, sink_), host_(&host), scheduler_(scheduler) {}
	loop(const loop&) = delete;
	loop& operator=(const loop&) = delete;

	driver_type& driver() noexcept { return driver_; }
	jn::Host& host() noexcept { return *host_; }
	task::scheduler_ref scheduler() const noexcept { return scheduler_; }
	task::task<void> run(task::cancel_token stop = {}) { return driver_.run(std::move(stop)); }

	// The transport's (quest 4); a lone voter sends nothing.
	std::function<void(cs::GroupId, cs::Send)> send;
	// The host's view of a group's followers (health): one applied through `index`, if it knows
	// one. A leader stalled on space hands leadership to it (D3); unset, it hands off to no one.
	std::function<std::optional<cs::NodeId>(cs::GroupId, cs::Index)> applied_past;

	void attach(cs::GroupId id, member& m) {
		if (!members_.emplace(id, &m).second) { throw std::logic_error("strata::group: the group is attached to this loop"); }
	}
	void detach(cs::GroupId id) noexcept { members_.erase(id); }
	member* find(cs::GroupId id) const noexcept {
		auto it = members_.find(id);
		return it == members_.end() ? nullptr : it->second;
	}

private:
	dispatch sink_;
	driver_type driver_;
	jn::Host* host_;
	task::scheduler_ref scheduler_;
	std::unordered_map<cs::GroupId, member*> members_;
};

inline void dispatch::operator()(cs::GroupId id, cs::Send s) const {
	if (owner->send) { owner->send(id, std::move(s)); }
}
inline void dispatch::operator()(cs::GroupId id, cs::Committed c) const {
	if (auto* m = owner->find(id)) { m->committed(std::move(c)); }
}
inline void dispatch::operator()(cs::GroupId id, cs::SnapshotNeeded s) const {
	if (auto* m = owner->find(id)) { m->install_needed(s.peer); }
}
inline void dispatch::operator()(cs::GroupId id, cs::ActivateInstall a) const {
	owner->driver().activation_failed(id, a.token, "strata::group: install is not built yet (group-install)");
}
inline void dispatch::operator()(cs::GroupId id, cs::Fenced f) const {
	if (auto* m = owner->find(id)) { m->fenced(std::move(f.reason)); }
}

// The replica fenced: a reopen recovers it (cluster-release D3, D16 step 2).
struct group_fenced : std::runtime_error {
	using std::runtime_error::runtime_error;
};

template <class App>
	requires application<App>
class group final : public member {
public:
	using layout = typename App::layout;
	using index_type = basic_index<layout>;
	using state_type = typename App::state;
	using transaction = typename index_type::transaction;

	struct config {
		cs::GroupId id = 0;
		cs::GroupOptions options;
		jn::Identity identity{};          // the journal group's, at create
		std::optional<std::string> closed;  // the last close's closed_record(), if the host kept it
	};

	// The index a group recovers on, which its host opens and lends (the catalog's open_index):
	// the durable one, else a deferred create, written first by the first checkpoint's commit; a
	// crash before it recreates the index from the log (D112).
	static index_type open_index(jn::IO& directory, const typename index_type::options& o, const std::array<char, 16>& uuid) {
		try {
			return index_type::open(directory, o);
		} catch (const index_not_created&) {
			return index_type::create_deferred(directory, o, uuid);
		}
	}

	// Both borrowed, for the group's life (D169): `index` is opened over `directory`, which the
	// file set shares under its prefix; the host closes both after the group's close. The chore
	// runs apply, the capture, the checkpoint's syncs and commit, and recovery, one item at a time.
	group(loop& l, config c, jn::IO& directory, index_type& index, App& app, task::scheduler_ref chore)
		: loop_(&l), id_(c.id), log_bound_(cs::checkpoint_detail::maximum_size(c.options.limits)), replacement_pool_(c.options.admission.replacement_pool.logical_bytes),
		  handoff_after_(c.options.timing.election_max * 1'000'000), opening_(std::make_unique<config>(std::move(c))), files_(directory), app_(&app), chore_(chore), ix_(&index) {
		handoff_.self = this;
		handoff_.fire = &handoff_fired;
	}
	group(const group&) = delete;
	group& operator=(const group&) = delete;
	// Destroyed with its chore idle and no call in flight: never opened, after close() or a failed
	// open(), or once fenced and the chore has drained (a stopped process). Only open() joins the
	// loop's table and close() or a failed open() leaves it, both on the loop, so a host may
	// destroy a group it never opened, closed or failed to open on any thread (the catalog does).
	~group() override {
		if (!attached_) { return; }
		if (handoff_.armed()) { loop_->scheduler().cancel(handoff_); }
		loop_->detach(id_);
	}

	cs::GroupId id() const noexcept { return id_; }
	loop& owner() const noexcept { return *loop_; }

	// Opens the group: joins the loop's table, then recovers it when the host's table holds it,
	// else creates it. On the loop. A failed open fences the group, closes its row, so the id opens
	// again on this Driver, and tears down as close() does before it throws. Started on the loop,
	// its Driver call enters before a request started after it, which so rides the reopen's turn
	// (consensus quiescence.md).
	task::task<void> open() {
		if (task::current() != loop_->scheduler()) { co_await task::schedule_on{loop_->scheduler()}; }
		if (!opening_) { throw std::logic_error("strata::group: the group was opened"); }
		loop_->attach(id_, *this);  // another group with this id: refused, nothing begun
		attached_ = true;
		auto& driver = loop_->driver();
		std::exception_ptr failed;
		bool row = false;
		try {
			driver.open(id_, std::move(opening_->options));
			row = true;
			// Recovery goes first: the ranges Core delivers meanwhile wait behind it.
			hold_.store(true);
			std::optional<bundle> restored;
			if (const auto status = loop_->host().status(id_); status == jn::GroupStatus::stored || status == jn::GroupStatus::closed) {
				// With the last close's record, a matching one resumes its term with no election.
				co_await driver.recover(
					id_,
					[&](jn::ArtifactReader& r) {
						std::string bytes(std::size_t(r.descriptor().length), '\0');
						for (std::size_t at = 0; at < bytes.size();) {
							const auto n = std::min<std::size_t>(bytes.size() - at, 65536);
							at += r.read_at(at, std::span<char>(bytes.data() + at, n));
						}
						restored = decode(bytes);
					},
					std::move(opening_->closed));
			} else {
				co_await driver.create(id_, opening_->identity);
			}
			co_await on_chore(
				[this, b = std::move(restored)]() mutable {
					try {
						recover(std::move(b));
					} catch (...) {
						fence_local(error_text(std::current_exception()));  // before any range applies
						throw;
					}
				},
				true);
		} catch (...) {
			failed = std::current_exception();
		}
		opening_.reset();
		if (!failed) { co_return; }
		closing_.store(true);
		fence_local(error_text(failed));
		co_await task::schedule_on{loop_->scheduler()};
		// The open's error is the answer; a close that fails too leaves the row, which a reopen
		// then refuses ("open on this Driver").
		if (row) {
			try {
				co_await driver.close(id_);
			} catch (...) {}
		}
		co_await leave();
		std::rethrow_exception(failed);
	}

	// Proposes a command. `none` answers at ProposalPlaced; `quorum` when this replica applies
	// the placement, with apply's verdict, or `superseded` when another term's entry is there.
	// `placed`, if any, hears the placement on the loop first.
	task::task<answer> propose(std::string command, ack level = ack::quorum, task::cancel_token token = {}, std::function<void(cs::Placement)> placed = {}) {
		if (closing_.load()) { throw cs::closed{}; }
		if (level == ack::none) {
			const auto at = co_await place(std::move(command), std::move(token));
			if (placed) { placed(at); }
			co_return answer{at, std::nullopt};
		}
		pending p{*this};
		p.placement = co_await place(std::move(command), std::move(token));
		if (placed) { placed(p.placement); }
		co_await p;
		co_return answer{p.placement, std::move(p.result)};
	}

	// A linearizable read (D4): the Driver's quorum probe gives a read index at or past every
	// write committed before the call; `fn` runs on the chore once this replica applied it,
	// between two entries, and its result is the answer.
	template <class F>
	task::task<std::invoke_result_t<F&, index_type&>> read_applied(F fn, task::cancel_token token = {}) {
		using R = std::invoke_result_t<F&, index_type&>;
		const auto ready = co_await loop_->driver().read(id_, std::move(token));
		co_await applied_at(ready.index);
		if constexpr (std::is_void_v<R>) {
			co_await on_chore([&] { fn(*ix_); });
		} else {
			std::optional<R> out;
			co_await on_chore([&] { out.emplace(fn(*ix_)); });
			co_return std::move(*out);
		}
	}

	// A checkpoint (D16): capture on the chore between two entries, sync the named files and
	// the directory, commit strata, publish, then unlink what the bundle omits. One at a time:
	// consensus refuses a second while one is pending, which would fail a close racing it.
	// Once the close tore down, it answers `closed`.
	task::task<cs::CheckpointPublished> checkpoint(task::cancel_token token = {}) {
		auto serial = co_await checkpoints_.lock();
		co_return co_await checkpoint_held(std::move(token));
	}

	// Closes the group once a checkpoint covers everything committed (D11): proposals are
	// refused from here, then checkpoints repeat until one covers the commit index. A `quorum`
	// waiter whose placement this replica never applied answers `closed`: not acknowledged. It
	// holds the checkpoints through the teardown, so a checkpoint racing it answers `closed`.
	// At rest, it keeps consensus's closed state (closed_record) for a reopen without election.
	// The teardown always runs: a fenced group publishes nothing and throws group_fenced, a
	// failed checkpoint throws its error, and either way its log and last bundle stand. On return
	// the chore is idle and the group touches neither the index nor the directory again (both stay
	// the host's, D169); a second close returns at once.
	task::task<void> close() {
		closing_.store(true);
		auto serial = co_await checkpoints_.lock();
		if (torn_down()) { co_return; }
		std::exception_ptr failed;
		if (!fenced_.load()) {
			try {
				for (;;) {
					auto published = co_await checkpoint_held();
					co_await task::schedule_on{loop_->scheduler()};
					auto s = loop_->driver().state(id_);
					if (!s || published.through >= s->committed) { break; }
				}
				closed_record_ = loop_->driver().closed_state(id_);
			} catch (...) {
				failed = std::current_exception();
			}
		}
		co_await task::schedule_on{loop_->scheduler()};
		try {
			co_await loop_->driver().close(id_);
		} catch (...) {
			if (!failed) { failed = std::current_exception(); }
		}
		co_await leave();
		if (failed) { std::rethrow_exception(failed); }
		if (fenced_.load()) { throw group_fenced("strata::group: closed fenced: " + fence_reason()); }
	}

	// open() and close() for a synchronous host (the catalog's open_payload and close_payload):
	// block the calling thread until they end. The caller must be none of what they wait on: the
	// loop, the group's chore, or a holder of the index's write lease, which apply and commit take.
	void open_blocking() { blocking(open(), "open"); }
	void close_blocking() { blocking(close(), "close"); }

	// The host freed space: a stalled apply retries.
	void resume() {
		{
			std::lock_guard lock(mu_);
			stalled_ = false;
		}
		kick();
	}

	// --- observers ------------------------------------------------------------------------

	cs::Index applied() const noexcept { return applied_.load(std::memory_order_acquire); }
	// Reads open once applied reaches strata's durable head P_H: below it, strata's documents
	// may name file bytes the replay has not rewritten (D16 recovery step 4).
	bool readable() const noexcept { return applied() >= readable_at_.load(std::memory_order_acquire); }
	bool stalled() const {
		std::lock_guard lock(mu_);
		return stalled_;
	}
	bool fenced() const noexcept { return fenced_.load(); }
	// Peers this leader's log no longer reaches: they catch up only by install (group-install),
	// which this group does not serve, so they stay behind until it lands.
	std::set<cs::NodeId> installs_needed() const {
		std::lock_guard lock(mu_);
		return installs_needed_;
	}
	std::string fence_reason() const {
		std::lock_guard lock(mu_);
		return fence_reason_;
	}
	// The chore's state, for a host or a test once the chore is idle.
	index_type& strata() { return *ix_; }
	file_set& files() noexcept { return files_; }
	const state_type& state() const noexcept { return state_; }
	const bundle& recovered() const noexcept { return recovered_; }
	// consensus's closed state (RFTCLOS1) at the close, if it rested: the host hands it to the
	// reopen's config.closed, so the reopened replica resumes its term (quiescence.md).
	const std::optional<std::string>& closed_record() const noexcept { return closed_record_; }
	files::position durable_head() const noexcept { return head_; }

	// --- the loop's notices ---------------------------------------------------------------

	void committed(cs::Committed range) override {
		if (range.entries.empty()) { return; }
		push(new range_work(*this, std::move(range)));
	}
	void fenced(std::string reason) override { fence_local(std::move(reason)); }
	void install_needed(cs::NodeId peer) override {
		std::lock_guard lock(mu_);
		installs_needed_.insert(peer);
	}

private:
	// --- the chore ------------------------------------------------------------------------

	enum class step : std::uint8_t { done, more, stall };
	// A chore item. A range is the queue's, freed once applied; a call is its awaiter's, and
	// finish() or abort() resumes the awaiter after the queue let go of it.
	struct work {
		virtual ~work() = default;
		virtual step run() = 0;
		virtual void finish() {}
		virtual void abort(std::exception_ptr) {}
		work* next = nullptr;
		bool queued_owns = true;
	};
	// The chore's queue, linked through its items: an idle group holds no block for it.
	struct work_queue {
		work *head = nullptr, *tail = nullptr;
		bool empty() const noexcept { return !head; }
		work* front() const noexcept { return head; }
		void push_back(work* w) noexcept {
			w->next = nullptr;
			(tail ? tail->next : head) = w;
			tail = w;
		}
		void push_front(work* w) noexcept {
			w->next = head;
			head = w;
			if (!tail) { tail = w; }
		}
		work* pop_front() noexcept {
			auto* w = head;
			head = w->next;
			if (!head) { tail = nullptr; }
			w->next = nullptr;
			return w;
		}
		// Everything, or everything after the front, leaves for the queue returned.
		work_queue take(bool keep_front) noexcept {
			work_queue out;
			if (keep_front && head) {
				out.head = head->next;
				out.tail = out.head ? tail : nullptr;
				head->next = nullptr;
				tail = head;
			} else {
				std::swap(out, *this);
			}
			return out;
		}
	};

	struct range_work final : work {
		range_work(group& g, cs::Committed c) : self(&g), range(std::move(c)) {}
		step run() override {
			const auto r = self->apply_entry(range.entries[next]);
			if (r != step::done) { return r; }
			if (++next < range.entries.size()) { return step::more; }
			self->notify_applied(range.entries.back().index);
			return step::done;
		}
		group* self;
		cs::Committed range;
		std::size_t next = 0;
	};

	// A call the chore runs for an awaiting task, which resumes on its own scheduler.
	struct call_work final : work {
		std::function<step()> fn;
		std::coroutine_handle<> handle;
		task::scheduler_ref home;
		std::exception_ptr error;
		step run() override {
			try {
				return fn();
			} catch (...) {
				error = std::current_exception();
				return step::done;
			}
		}
		void finish() override { home.post(handle); }
		void abort(std::exception_ptr e) override {
			error = std::move(e);
			home.post(handle);
		}
	};
	struct chore_awaiter {
		group* self;
		std::unique_ptr<call_work> w;
		bool front = false;
		call_work* raw = nullptr;
		bool await_ready() const noexcept { return false; }
		void await_suspend(std::coroutine_handle<> h) {
			w->handle = h;
			w->home = task::current();
			w->queued_owns = false;
			raw = w.get();
			self->push(raw, front);
		}
		void await_resume() const {
			if (raw->error) { std::rethrow_exception(raw->error); }
		}
	};
	// Resumes once the chore is idle: no item running, none to come once the group refuses them.
	struct idle_awaiter {
		group* self;
		std::coroutine_handle<> handle;
		task::scheduler_ref home;
		idle_awaiter* next = nullptr;
		bool await_ready() const {
			std::lock_guard lock(self->mu_);
			return !self->running_;
		}
		bool await_suspend(std::coroutine_handle<> h) {
			handle = h;
			home = task::current();
			std::lock_guard lock(self->mu_);
			if (!self->running_) { return false; }
			next = std::exchange(self->idle_, this);
			return true;
		}
		void await_resume() const noexcept {}
	};
	idle_awaiter chore_idle() { return idle_awaiter{this, {}, {}}; }

	// The end of close() and of a failed open(): the group leaves the loop's table on the loop and
	// tears down, so the host may destroy it on any thread once it returns.
	task::task<void> leave() {
		if (task::current() != loop_->scheduler()) { co_await task::schedule_on{loop_->scheduler()}; }
		if (handoff_.armed()) { loop_->scheduler().cancel(handoff_); }
		loop_->detach(id_);
		attached_ = false;
		co_await tear_down();
	}
	// On the loop: items refused from here, the queue answered `closed`, every waiter failed; once
	// the chore is idle the file set closes, exclusive by then.
	task::task<void> tear_down() {
		work_queue left;
		{
			std::lock_guard lock(mu_);
			closed_ = true;
			if (!running_) { left = queue_.take(false); }  // stalled or held: no drain will drop them
		}
		while (!left.empty()) { drop(left.pop_front(), true); }
		fail_waiters(std::make_exception_ptr(cs::closed{}));
		co_await chore_idle();
		files_.close();
	}
	bool torn_down() const {
		std::lock_guard lock(mu_);
		return closed_;
	}

	void blocking(task::task<void> work, const char* what) {
		const auto here = task::current();  // a scheduler pumped inline is not runs_here()
		if (loop_->scheduler().runs_here() || chore_.runs_here() || here == loop_->scheduler() || here == chore_ || ix_->lease().held_here()) {
			throw std::logic_error(std::string("strata::group: a blocking ") + what + " on the loop, the group's chore or under the index's write lease waits on itself");
		}
		std::mutex m;
		std::condition_variable cv;
		bool done = false;
		std::exception_ptr error;
		task::spawn(loop_->scheduler(), std::move(work), [&](std::exception_ptr e) {
			std::lock_guard lock(m);
			error = e;
			done = true;
			cv.notify_one();  // under the lock: the waiter's frame outlives it
		});
		std::unique_lock lock(m);
		cv.wait(lock, [&] { return done; });
		if (error) { std::rethrow_exception(error); }
	}

	// The awaiter keeps the item alive until it resumes: the queue hands it back, never frees it.
	chore_awaiter on_chore(std::function<void()> fn, bool front = false) {
		auto w = std::make_unique<call_work>();
		w->fn = [f = std::move(fn)] {
			f();
			return step::done;
		};
		return chore_awaiter{this, std::move(w), front};
	}
	chore_awaiter on_chore_step(std::function<step()> fn, bool front = false) {
		auto w = std::make_unique<call_work>();
		w->fn = std::move(fn);
		return chore_awaiter{this, std::move(w), front};
	}

	void push(work* w, bool front = false) {
		bool closed = false;
		{
			std::lock_guard lock(mu_);
			closed = closed_;
			if (!fenced_.load() && !closed) {
				if (front) {
					queue_.push_front(w);
					hold_.store(false);
				} else {
					queue_.push_back(w);
				}
				w = nullptr;
			}
		}
		if (w) { drop(w, closed); }
		kick();
	}
	// A refused item: a range is freed; a call answers `closed` once the close tore down, else
	// group_fenced.
	static void drop(work* w, bool closed) {
		if (w->queued_owns) {
			delete w;
		} else if (closed) {
			w->abort(std::make_exception_ptr(cs::closed{}));
		} else {
			w->abort(std::make_exception_ptr(group_fenced("strata::group: the replica fenced")));
		}
	}
	void kick() {
		{
			std::lock_guard lock(mu_);
			if (running_ || stalled_ || hold_.load() || queue_.empty()) { return; }
			running_ = true;
		}
		task::spawn(chore_, drain());
	}
	task::task<void> drain() {
		for (;;) {
			work* w = nullptr;
			work_queue left;  // a fence kept the running item, or the close tore down: they go now
			idle_awaiter* idle = nullptr;
			bool closed = false;
			{
				std::lock_guard lock(mu_);
				closed = closed_ && !fenced_.load();
				if (fenced_.load() || closed_) { left = queue_.take(false); }
				if (queue_.empty() || stalled_ || hold_.load()) {
					running_ = false;
					idle = std::exchange(idle_, nullptr);
				} else {
					w = queue_.front();
				}
			}
			if (!w) {
				// Neither touches the group: a resumed idle waiter may destroy it.
				while (!left.empty()) { drop(left.pop_front(), closed); }
				while (idle) {
					auto* i = std::exchange(idle, idle->next);
					i->home.post(i->handle);
				}
				co_return;
			}
			step r;
			try {
				r = w->run();
			} catch (...) {
				fence(error_text(std::current_exception()));
				r = step::done;
			}
			{
				std::lock_guard lock(mu_);
				if (r == step::stall) { stalled_ = true; }
				if (r == step::done) { queue_.pop_front(); }  // fence_local keeps the running item
			}
			if (r == step::stall && loop_->applied_past) { task::spawn(loop_->scheduler(), stall_on_loop(loop_, id_)); }
			if (r == step::done) {
				if (w->queued_owns) {
					delete w;
				} else {
					w->finish();
				}
			}
			co_await task::yield();
		}
	}

	// --- apply (D3, D18) ------------------------------------------------------------------

	step apply_entry(const cs::Entry& e) {
		const entry view{e.index, e.term, e.kind, e.payload};
		if (phase_ == phase::effects) {
			// 1. File effects from the entry's starting state, whatever strata will answer.
			try {
				app_->effects(state_, view, files_);
			} catch (const std::system_error& x) {
				if (x.code() == std::errc::no_space_on_device) { return step::stall; }
				throw;
			}
			phase_ = phase::strata;
		}
		verdict v;
		if (phase_ == phase::strata) {
			// 2. The strata write: a retryable answer repeats only this.
			if (auto t = app_->transaction(state_, view)) {
				const auto r = write(*t, e.index, v);
				if (r != step::done) { return r; }
			}
			phase_ = phase::effects;
		}
		// 3. The application state, verdict-independent; 4. the applied position, after strata's
		// write is visible; 5. the placement's quorum waiter.
		app_->advance(state_, view);
		applied_term_ = e.term;
		applied_.store(e.index, std::memory_order_release);
		answer_waiter(e.index, e.term, v);
		wake_readers(e.index);
		return step::done;
	}
	// One strata attempt at entry `at`'s position (index, 1).
	step write(const transaction& t, cs::Index at, verdict& v) {
		try {
			v = ix_->apply(t, files::position{at, 1});
		} catch (const index_fenced&) {
			throw;
		} catch (...) {
			// A stage-1 throw rolled back with the position unmoved: retried after the chore
			// yields; the third at one entry fences the replica.
			if (++throws_ >= 3) { throw; }
			return step::more;
		}
		throws_ = 0;
		switch (v.result) {
		case verdict::flush_owed:
			ix_->refresh();
			return step::more;
		case verdict::drain_owed:
			ix_->reclaim_for_space();
			return step::more;
		case verdict::map_owed:
			ix_->promote();
			return step::more;
		case verdict::no_space:
			// Retryable with the position unmoved (strata-nospace-position's contract).
			return step::stall;
		case verdict::skipped:
			// Only replay at or below P_H skips; above it, strata moved its position on a
			// retryable answer, which the contract forbids.
			if (files::position{at, 1} > head_) { throw group_fenced("strata::group: strata skipped an entry above its durable head"); }
			return step::done;
		default:
			return step::done;
		}
	}

	// The journal's closed-group answer (journal group_closed: the group closed before the
	// record was durable) is the group's `closed`, as consensus's own is.
	task::task<cs::Placement> place(std::string command, task::cancel_token token) {
		try {
			co_return co_await loop_->driver().propose(id_, std::move(command), std::move(token));
		} catch (const jn::group_closed&) {
			throw cs::closed{};
		}
	}

	// --- applied waiters (D4) -------------------------------------------------------------

	struct applied_waiter {
		group* self;
		cs::Index index;
		std::coroutine_handle<> handle;
		task::scheduler_ref home;
		std::exception_ptr error;
		bool await_ready() {
			std::lock_guard lock(self->mu_);
			if (self->refused_) { error = self->refused_; }
			return error || self->applied_.load() >= index;
		}
		void await_suspend(std::coroutine_handle<> h) {
			handle = h;
			home = task::current();
			std::lock_guard lock(self->mu_);
			if (self->refused_ || self->applied_.load() >= index) {
				error = self->refused_;
				home.post(handle);
				return;
			}
			self->readers_.emplace(index, this);
		}
		void await_resume() const {
			if (error) { std::rethrow_exception(error); }
		}
	};
	applied_waiter applied_at(cs::Index index) { return applied_waiter{this, index, {}, {}, {}}; }
	// After applied moved to `through`: the reads waiting at or below it resume.
	void wake_readers(cs::Index through) {
		std::lock_guard lock(mu_);
		auto end = readers_.upper_bound(through);
		for (auto it = readers_.begin(); it != end; ++it) { it->second->home.post(it->second->handle); }
		readers_.erase(readers_.begin(), end);
	}

	// --- quorum waiters (D4) --------------------------------------------------------------

	struct outcome {
		cs::Term term = 0;
		verdict v;
	};
	struct pending {
		explicit pending(group& g) : self(&g) {
			std::lock_guard lock(self->mu_);
			since = self->applied_.load();
			self->outstanding_.insert(since);
		}
		~pending() {
			std::lock_guard lock(self->mu_);
			if (linked) { self->waiters_.erase(placement.index); }
			unclaim();
		}
		// Registered or answered: apply need not keep outcomes for it any more.
		void unclaim() {
			if (!counted) { return; }
			counted = false;
			self->outstanding_.erase(self->outstanding_.find(since));
			const auto floor = self->outstanding_.empty() ? ~cs::Index(0) : *self->outstanding_.begin();
			self->recent_.erase(self->recent_.begin(), self->recent_.upper_bound(floor));
		}
		bool await_ready() {
			std::lock_guard lock(self->mu_);
			if (self->refused_) {
				error = self->refused_;
				return true;
			}
			if (auto it = self->recent_.find(placement.index); it != self->recent_.end()) {
				take(it->second);
				unclaim();
				return true;
			}
			return false;
		}
		void await_suspend(std::coroutine_handle<> h) {
			std::lock_guard lock(self->mu_);
			handle = h;
			home = task::current();
			// Between await_ready and here the chore may have applied it, or the group refused.
			if (auto it = self->recent_.find(placement.index); it != self->recent_.end()) {
				take(it->second);
				unclaim();
				home.post(handle);
				return;
			}
			if (self->refused_) {
				error = self->refused_;
				unclaim();
				home.post(handle);
				return;
			}
			self->waiters_.emplace(placement.index, this);
			linked = true;
			unclaim();
		}
		void await_resume() {
			if (error) { std::rethrow_exception(error); }
		}
		void take(const outcome& o) {
			if (o.term != placement.term) {
				error = std::make_exception_ptr(cs::superseded{});
			} else {
				result = o.v;
			}
		}
		group* self;
		cs::Placement placement;
		std::optional<verdict> result;
		std::exception_ptr error;
		std::coroutine_handle<> handle;
		task::scheduler_ref home;
		cs::Index since = 0;  // applied when it started: its placement is above
		bool linked = false, counted = true;
	};
	void answer_waiter(cs::Index index, cs::Term term, const verdict& v) {
		std::lock_guard lock(mu_);
		if (auto it = waiters_.find(index); it != waiters_.end()) {
			auto* p = it->second;
			waiters_.erase(it);
			p->linked = false;
			p->take(outcome{term, v});
			p->home.post(p->handle);
			return;
		}
		// A proposal not yet registered is placed above the applied index it started at.
		if (!outstanding_.empty() && index > *outstanding_.begin()) { recent_[index] = outcome{term, v}; }
	}

	// --- checkpoint (D16) -----------------------------------------------------------------

	// Step 1, between two entries: P_b is the applied position. If strata's position is below
	// it (chunks, NoOps), an empty transaction there moves it; it retries like any entry.
	std::function<step()> capture_work(bundle& b) {
		return [this, &b] {
			const cs::Index at = applied_.load();
			if (ix_->applied() < files::position{at, 1}) {
				verdict v;
				const auto r = write(transaction{}, at, v);
				if (r != step::done) { return r; }
			}
			b.position = at;
			b.term = applied_term_;
			b.files = files_.capture();
			b.state = app_->encode(state_);
			return step::done;
		};
	}
	// A checkpoint, its serializer held. The close tears down under it, so a checkpoint after the
	// teardown is refused at its capture (`closed`) and none is torn down mid-way.
	task::task<cs::CheckpointPublished> checkpoint_held(task::cancel_token token = {}) {
		bundle b;
		co_await on_chore_step(capture_work(b));
		// Nothing applied past the last bundle: it is this checkpoint (consensus refuses a
		// capture at or below its base).
		if (b.position <= published_.load()) { co_return cs::CheckpointPublished{0, published_.load()}; }
		struct source {
			std::string bytes;
			std::size_t at = 0;
			cs::Index through;
			cs::Term term;
			cs::CaptureMetadata metadata() const { return {0, through, term, 0}; }
			std::size_t read(std::span<char> out) {
				const auto n = std::min(out.size(), bytes.size() - at);
				std::copy_n(bytes.data() + at, n, out.data());
				at += n;
				return n;
			}
		};
		auto bytes = encode(b);
		const auto cap = bytes.size();
		// The admission reserves at least the bundle and the log's bound from its replacement
		// pool: one that cannot hold them would wait forever, so it fails here, before any sync.
		if (log_bound_ + cap > replacement_pool_) {
			throw std::length_error("strata::group: a checkpoint cannot fit the admission's replacement pool (size it to the group's limits)");
		}
		try {
			co_await on_chore([&] { sync_and_commit(b); });
		} catch (...) {
			fence(error_text(std::current_exception()));
			throw;
		}
		auto published = co_await loop_->driver().checkpoint(id_, cap, source{std::move(bytes), 0, b.position, b.term}, std::move(token));
		published_.store(std::max(published_.load(), published.through));
		co_await on_chore([&] { files_.unlink_retired(b.position); });
		co_return published;
	}
	// Steps 2 and 3: the named files and the directory synced, then strata committed (P_H >= P_b).
	void sync_and_commit(const bundle& b) {
		files_.sync(b.files);
		ix_->commit();
	}

	// --- recovery (D16) -------------------------------------------------------------------

	// strata at its head P_H, the borrowed index's (its durable head once opened, or past it when
	// an earlier group over it closed); the file set as the bundle names it; the application state
	// loaded. Core then delivers every committed entry above P_b: file effects re-run, strata
	// skips at or below P_H.
	void recover(std::optional<bundle> restored) {
		recovered_ = restored.value_or(bundle{});
		head_ = ix_->applied();
		if (recovered_.position > head_.index) { throw group_fenced("strata::group: the bundle is past strata's durable head (P_b > P_H)"); }
		files_.recover(recovered_.files);
		state_ = restored ? app_->decode(recovered_.state) : state_type{};
		applied_term_ = recovered_.term;
		readable_at_.store(head_.index, std::memory_order_release);
		applied_.store(recovered_.position, std::memory_order_release);
		published_.store(recovered_.position);
	}

	// --- fencing --------------------------------------------------------------------------

	static std::string error_text(std::exception_ptr e) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception& x) {
			return x.what();
		} catch (...) {}
		return "unknown";
	}
	// A failure apply cannot retry: the group fences here and in Core.
	void fence(std::string reason) {
		const auto at = applied_.load();
		fence_local(reason);
		task::spawn(loop_->scheduler(), report_failure(at, std::move(reason)));
	}
	task::task<void> report_failure(cs::Index at, std::string reason) {
		loop_->driver().application_failed(id_, at, std::move(reason));
		co_return;
	}
	void fence_local(std::string reason) {
		work_queue dropped;
		{
			std::lock_guard lock(mu_);
			if (fenced_.exchange(true)) { return; }
			fence_reason_ = reason;
			// The running item, if any, stays: drain() ends at its next look.
			dropped = queue_.take(running_);
		}
		fail_waiters(std::make_exception_ptr(cs::fenced(reason)));
		while (!dropped.empty()) { drop(dropped.pop_front(), false); }
	}
	// Every `quorum` waiter, linked now or later, resumes with the first such error.
	void fail_waiters(std::exception_ptr error) {
		std::lock_guard lock(mu_);
		if (!refused_) { refused_ = std::move(error); }
		for (auto& [i, p] : waiters_) {
			p->linked = false;
			p->error = refused_;
			p->home.post(p->handle);
		}
		waiters_.clear();
		for (auto& [i, r] : readers_) {
			r->error = refused_;
			r->home.post(r->handle);
		}
		readers_.clear();
	}

	// --- a stalled leader's hand-off (D3) -------------------------------------------------

	struct handoff_timer : wheel::timer_entry {
		group* self = nullptr;
	};
	static void handoff_fired(wheel::timer_entry& e, bool expired) noexcept {
		if (expired) { static_cast<handoff_timer&>(e).self->hand_off(); }
	}
	// Holds the loop and the id, not the group, which a stopped process may destroy first.
	static task::task<void> stall_on_loop(loop* l, cs::GroupId id) {
		if (auto* g = dynamic_cast<group*>(l->find(id))) { g->arm_handoff(); }
		co_return;
	}
	void arm_handoff() {
		if (!handoff_.armed()) { loop_->scheduler().arm(handoff_, loop_->scheduler().now() + handoff_after_); }
	}
	// One election timeout into a stall, a leader hands leadership to a follower applied past the
	// stalled entry. The timer re-arms each timeout while the stall lasts, whatever the role: a
	// replica elected while stalled hands off within one timeout of leading.
	void hand_off() noexcept {
		if (!stalled() || fenced_.load()) { return; }
		const auto s = loop_->driver().state(id_);
		if (s && s->role == cs::Role::Leader) {
			std::optional<cs::NodeId> target;
			try {
				target = loop_->applied_past(id_, applied() + 1);
			} catch (...) {}
			if (target) { task::spawn(loop_->scheduler(), transfer_on_loop(loop_, id_, *target)); }
		}
		arm_handoff();
	}
	// A refused or aborted transfer is tried again at the next timeout while the stall lasts.
	static task::task<void> transfer_on_loop(loop* l, cs::GroupId id, cs::NodeId target) {
		try {
			co_await l->driver().transfer_leadership(id, target);
		} catch (...) {}
	}

	// Holds the loop and the id, not the group, which a stopped process may destroy first.
	static task::task<void> notify_applied_on_loop(loop* l, cs::GroupId id, cs::Index through) {
		l->driver().applied(id, cs::Applied{through});
		co_return;
	}
	void notify_applied(cs::Index through) { task::spawn(loop_->scheduler(), notify_applied_on_loop(loop_, id_, through)); }

	loop* loop_;
	cs::GroupId id_;
	std::size_t log_bound_;            // the checkpoint's least reservation past its bundle
	std::uint64_t replacement_pool_;   // the admission's pool that reservation comes from
	std::uint64_t handoff_after_;      // one election timeout, ns: a stalled leader's hand-off
	handoff_timer handoff_;            // armed on the loop only while stalled
	bool attached_ = false;            // in the loop's table: the loop's, from open() until close() or its failure
	std::unique_ptr<config> opening_;  // what open() needs, freed once it returns
	std::optional<std::string> closed_record_;  // the loop's: taken at close
	file_set files_;
	App* app_;
	task::scheduler_ref chore_;

	// The chore's alone; the index is borrowed (D169).
	index_type* ix_;
	state_type state_{};
	bundle recovered_;
	files::position head_{};  // P_H at the last recovery
	cs::Term applied_term_ = 0;
	enum class phase : std::uint8_t { effects, strata } phase_ = phase::effects;
	unsigned throws_ = 0;

	// Shared, under mu_ or atomic.
	mutable std::mutex mu_;
	work_queue queue_;
	idle_awaiter* idle_ = nullptr;  // close's, waiting for the chore to go idle
	bool running_ = false, stalled_ = false, closed_ = false;  // closed_: torn down, chore items refused
	std::atomic<bool> hold_{false}, fenced_{false}, closing_{false};
	task::async_mutex checkpoints_;
	std::string fence_reason_;
	std::exception_ptr refused_;  // fenced or closed: every waiter's answer
	std::map<cs::Index, pending*> waiters_;
	std::multimap<cs::Index, applied_waiter*> readers_;
	std::map<cs::Index, outcome> recent_;
	std::multiset<cs::Index> outstanding_;  // the `since` of each proposal not yet registered
	std::set<cs::NodeId> installs_needed_;
	std::atomic<cs::Index> applied_{0}, readable_at_{0}, published_{0};  // published_: the last bundle's P_b
};

}  // namespace strata::group
