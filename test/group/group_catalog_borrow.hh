// What a catalog slot holds for a group (D169), for the tests that run groups without a catalog:
// the directory, the index opened over it and the group that borrows both, destroyed in that
// order reversed. emplace() destroys the last ones first, so a reopen meets no directory lock.
#pragma once

#include "strata/core/budget.hh"
#include "strata/group/group.hh"

#include <array>
#include <memory>
#include <optional>

namespace group_test {

template <class G>
class borrowed {
public:
	using index_type = typename G::index_type;

	borrowed() = default;
	borrowed(const borrowed&) = delete;
	borrowed& operator=(const borrowed&) = delete;
	~borrowed() { reset(); }

	// A keyed index on `storage`, opened (or created deferred, D112) as group::open_index does.
	template <class App>
	G& emplace(strata::group::loop& l, typename G::config c, std::unique_ptr<kronuz::journal::IO> directory, strata::byte_budget& storage, const std::array<char, 16>& uuid, App& app, task::scheduler_ref chore) {
		reset();
		typename index_type::options o;
		o.keyed = true;
		o.storage = &storage;
		io_ = std::move(directory);
		ix_.emplace(G::open_index(*io_, o, uuid));
		g_ = std::make_unique<G>(l, std::move(c), *io_, *ix_, app, chore);
		return *g_;
	}
	// The host's close after the group's, as the catalog's close and retire do: the index closed,
	// then it and the directory gone; the group stays until reset().
	void close_index() {
		ix_->close();
		ix_.reset();
		io_.reset();
	}
	void reset() {
		g_.reset();
		ix_.reset();
		io_.reset();
	}

	G* get() const noexcept { return g_.get(); }
	G* operator->() const noexcept { return g_.get(); }
	G& operator*() const noexcept { return *g_; }
	explicit operator bool() const noexcept { return g_ != nullptr; }
	index_type& index() noexcept { return *ix_; }

private:
	std::unique_ptr<kronuz::journal::IO> io_;
	std::optional<index_type> ix_;
	std::unique_ptr<G> g_;
};

}  // namespace group_test
