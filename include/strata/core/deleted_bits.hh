// A segment's live-docs bits, copied on write (shared-reader D3): one allocation holds a share
// count, the word count, and the words, one bit per ordinal, set when deleted. share() hands
// the same words to a reader's snapshot; the owner's next write to shared bits clones them
// first, so a snapshot's bits never change. Sharing, writing, and dropping happen under the
// index's write lease alone (owner.hh), whose handoff orders one holder's changes before the
// next's, so the count is a plain integer: shared is exactly "count above one".
#pragma once

#include "strata/core/owner.hh"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace strata {

class deleted_bits {
public:
	deleted_bits() noexcept = default;
	// Bits for `words` words, all clear.
	explicit deleted_bits(std::size_t words) {
		if (!words) { return; }
		p_ = allocate(words);
		std::memset(p_ + header, 0, words * sizeof(std::uint64_t));
	}
	deleted_bits(deleted_bits&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
	deleted_bits& operator=(deleted_bits&& o) noexcept {
		if (this != &o) {
			drop();
			p_ = std::exchange(o.p_, nullptr);
		}
		return *this;
	}
	// Copies are explicit (share() or clone()), so no share is taken by accident.
	deleted_bits(const deleted_bits&) = delete;
	deleted_bits& operator=(const deleted_bits&) = delete;
	~deleted_bits() { drop(); }

	// The same words, counted: a writer clones them before its next write.
	deleted_bits share() const noexcept {
		deleted_bits b;
		if (p_) {
			detail::owner_only();
			++p_[0];
			b.p_ = p_;
		}
		return b;
	}
	// A private copy of the words.
	deleted_bits clone() const {
		deleted_bits b;
		if (p_) {
			b.p_ = allocate(size());
			std::memcpy(b.p_ + header, p_ + header, size() * sizeof(std::uint64_t));
		}
		return b;
	}

	std::size_t size() const noexcept { return p_ ? std::size_t(p_[1]) : 0; }  // words
	bool empty() const noexcept { return !p_; }
	const std::uint64_t* data() const noexcept { return p_ ? p_ + header : nullptr; }
	const std::uint64_t* begin() const noexcept { return data(); }
	const std::uint64_t* end() const noexcept { return data() + size(); }
	std::uint64_t operator[](std::size_t w) const noexcept { return p_[header + w]; }
	bool test(std::uint64_t ord) const noexcept { return (p_[header + (ord >> 6)] >> (ord & 63)) & 1; }
	bool shared() const noexcept { return p_ && p_[0] > 1; }

	// The words to write: cloned first when shared, so no sharer sees the write.
	std::uint64_t* mutable_words() {
		if (shared()) { *this = clone(); }
		return p_ ? p_ + header : nullptr;
	}
	void set(std::uint64_t ord) { mutable_words()[ord >> 6] |= std::uint64_t(1) << (ord & 63); }
	void reset(std::uint64_t ord) { mutable_words()[ord >> 6] &= ~(std::uint64_t(1) << (ord & 63)); }

private:
	static constexpr std::size_t header = 2;  // the share count, then the word count
	static std::uint64_t* allocate(std::size_t words) {
		auto* p = static_cast<std::uint64_t*>(::operator new((header + words) * sizeof(std::uint64_t)));
		p[0] = 1;
		p[1] = words;
		return p;
	}
	void drop() noexcept {
		if (!p_) { return; }
		if (p_[0] > 1) { detail::owner_only(); }
		if (--p_[0] == 0) { ::operator delete(p_); }
		p_ = nullptr;
	}
	std::uint64_t* p_ = nullptr;
};

}  // namespace strata
