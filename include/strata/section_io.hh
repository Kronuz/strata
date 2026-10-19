// Section I/O shared by every writer of segment files (layers.md, L4 and L7): outputs and
// spills, the counting and checksumming streams, the sink that lays sections out in file
// order, and the handles readers verify owned sections through.
#pragma once

#include "hashes.hh"
#include "residency.hh"
#include "strata/budget.hh"
#include "strata/format.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata {

// Receives a segment file's bytes in order.
struct output {
	virtual ~output() = default;
	virtual void write(const char* data, std::size_t size) = 0;
	void write(std::string_view bytes) { write(bytes.data(), bytes.size()); }
};

// Holds a section produced alongside another until it is appended.
struct spill : output {
	virtual void copy_to(output& out) = 0;
};

// Appends to a string, charging its reallocations to `memory` when given.
class string_output final : public output {
public:
	explicit string_output(std::string& s, flush_charge* memory = nullptr) : s_(s), memory_(memory) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (!memory_) {
			s_.append(data, size);
			return;
		}
		const std::size_t before = s_.capacity();
		s_.append(data, size);
		if (s_.capacity() > before) { memory_->grow(s_.capacity() - before); }
	}

private:
	std::string& s_;
	flush_charge* memory_;
};

// A temporary file written once through out(), then read back whole (a mapping): a
// reordering merge's forward index and its stored records.
struct scratch {
	virtual ~scratch() = default;
	virtual output& out() = 0;
	virtual std::string_view bytes() = 0;
};

class string_scratch final : public scratch {
public:
	explicit string_scratch(flush_charge* memory = nullptr) : out_(s_, memory) {}
	output& out() override { return out_; }
	std::string_view bytes() override { return s_; }

private:
	std::string s_;
	string_output out_;
};

class string_spill final : public spill {
public:
	string_spill() = default;
	explicit string_spill(flush_charge* memory) : memory_(memory) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (!memory_) {
			s_.append(data, size);
			return;
		}
		const std::size_t before = s_.capacity();
		s_.append(data, size);
		if (s_.capacity() > before) {
			memory_->grow(s_.capacity() - before);
			charged_ += s_.capacity() - before;
		}
	}
	void copy_to(output& out) override {
		out.write(s_);
		std::string().swap(s_);
		if (memory_) { memory_->shrink(std::exchange(charged_, 0)); }
	}

private:
	std::string s_;
	flush_charge* memory_ = nullptr;
	std::uint64_t charged_ = 0;
};

namespace detail {
// Holds a spilled section in chunks that never move (64 KiB doubling to 1 MiB): growth
// copies nothing, and copy_to() frees each chunk once written.
class chunked_spill final : public spill {
public:
	explicit chunked_spill(flush_charge* memory = nullptr) : memory_(memory) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		while (size) {
			if (fill_ == size_) {
				size_ = chunk_size(chunks_.size());
				chunks_.push_back(std::unique_ptr<char[]>(new char[size_]));
				fill_ = 0;
				if (memory_) { memory_->grow(size_); }
			}
			const std::size_t n = std::min(size, size_ - fill_);
			std::memcpy(chunks_.back().get() + fill_, data, n);
			fill_ += n, data += n, size -= n;
		}
	}
	void copy_to(output& out) override {
		for (std::size_t c = 0; c < chunks_.size(); ++c) {
			const std::size_t n = chunk_size(c);
			out.write(chunks_[c].get(), c + 1 == chunks_.size() ? fill_ : n);
			chunks_[c].reset();
			if (memory_) { memory_->shrink(n); }
		}
		chunks_.clear();
		fill_ = size_ = 0;
	}

private:
	static std::size_t chunk_size(std::size_t c) noexcept { return std::size_t(64) << 10 << std::min<std::size_t>(c, 4); }
	std::vector<std::unique_ptr<char[]>> chunks_;
	std::size_t fill_ = 0, size_ = 0;  // the last chunk's
	flush_charge* memory_ = nullptr;
};
}  // namespace detail

// Writes to a file; throws std::runtime_error on failure.
class file_output final : public output {
public:
	explicit file_output(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
		if (!f_) { throw std::runtime_error("strata: cannot create " + path); }
	}
	~file_output() override {
		if (f_) { std::fclose(f_); }
	}
	file_output(const file_output&) = delete;
	file_output& operator=(const file_output&) = delete;
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (std::fwrite(data, 1, size, f_) != size) { throw std::runtime_error("strata: write failed"); }
	}
	// Flushes and closes; throws when the data did not reach the file.
	void close() {
		const bool ok = std::fflush(f_) == 0;
		std::fclose(std::exchange(f_, nullptr));
		if (!ok) { throw std::runtime_error("strata: write failed"); }
	}

private:
	std::FILE* f_;
};

// A temporary file holding a spilled section; removed when copied or destroyed.
class file_spill final : public spill {
public:
	explicit file_spill(std::string path) : path_(std::move(path)), f_(std::fopen(path_.c_str(), "w+b")) {
		if (!f_) { throw std::runtime_error("strata: cannot create " + path_); }
	}
	~file_spill() override { release(); }
	file_spill(const file_spill&) = delete;
	file_spill& operator=(const file_spill&) = delete;
	using output::write;
	void write(const char* data, std::size_t size) override {
		if (std::fwrite(data, 1, size, f_) != size) { throw std::runtime_error("strata: spill write failed"); }
	}
	void copy_to(output& out) override {
		if (std::fflush(f_) != 0 || std::fseek(f_, 0, SEEK_SET) != 0) { throw std::runtime_error("strata: spill read failed"); }
		std::vector<char> buf(1 << 20);
		for (std::size_t n; (n = std::fread(buf.data(), 1, buf.size(), f_)) > 0;) { out.write(buf.data(), n); }
		if (std::ferror(f_)) { throw std::runtime_error("strata: spill read failed"); }
		release();
	}

private:
	void release() {
		if (f_) {
			std::fclose(f_);
			std::remove(path_.c_str());
			f_ = nullptr;
		}
	}
	std::string path_;
	std::FILE* f_;
};

// Tracks bytes written and pads to an alignment.
class counting_output final : public output {
public:
	explicit counting_output(output& out) : out_(out) {}
	using output::write;
	void write(const char* data, std::size_t size) override {
		out_.write(data, size);
		size_ += size;
	}
	std::uint64_t size() const noexcept { return size_; }
	void align(std::size_t alignment) {
		static constexpr char zeros[64] = {};
		while (size_ % alignment) { write(zeros, std::min<std::size_t>(sizeof zeros, alignment - size_ % alignment)); }
	}

private:
	output& out_;
	std::uint64_t size_ = 0;
};

// One section's bytes, checksummed as they pass (the whole section and each region).
// Small writes gather in 64 KiB first; finish() passes the rest, so nothing else may write
// to the same output before it.
class section_stream {
public:
	explicit section_stream(counting_output& out, std::uint8_t region_shift = format::default_region_shift)
		: out_(out), offset_(out.size()), shift_(region_shift), size_(std::size_t(1) << region_shift) {}
	section_stream(const section_stream&) = delete;
	section_stream& operator=(const section_stream&) = delete;
	void write(std::string_view bytes) {
		if (pending_.size() + bytes.size() > gather) { drain(); }
		if (bytes.size() >= gather) {
			pass(bytes);
		} else {
			pending_.append(bytes);
		}
		length_ += bytes.size();
	}
	std::uint64_t size() const noexcept { return length_; }
	format::section_entry finish(format::section kind) {
		drain();
		if (region_fill_) { regions_.push_back(region_.value()); }
		return {kind, offset_, length_, crc_.value(), std::move(regions_), shift_};
	}

private:
	static constexpr std::size_t gather = 64 << 10;
	void drain() {
		if (pending_.empty()) { return; }
		pass(pending_);
		pending_.clear();
	}
	// Bytes to the output and both checksums: the section's and each region's.
	void pass(std::string_view bytes) {
		out_.write(bytes);
		crc_.update(bytes.data(), bytes.size());
		for (std::size_t at = 0; at < bytes.size();) {
			const std::size_t n = std::min(bytes.size() - at, size_ - region_fill_);
			region_.update(bytes.data() + at, n);
			region_fill_ += n;
			at += n;
			if (region_fill_ == size_) {
				regions_.push_back(region_.value());
				region_ = {};
				region_fill_ = 0;
			}
		}
	}

	counting_output& out_;
	std::uint64_t offset_, length_ = 0;
	std::uint8_t shift_;
	std::size_t size_;
	hashes::Crc32c crc_, region_;
	std::size_t region_fill_ = 0;
	std::vector<std::uint32_t> regions_;
	std::string pending_;  // writes gathered, not yet passed
};

// What a plugin's build or merge produced (L4, L6).
struct built {
	format::structure_bits structures = 0;  // the structure bits it built (F4)
};

// One owned section, resolved once when a reader is constructed (L7): verifying a range
// is one add and the segment's shared region state, with no lookup by kind.
struct section_handle {
	std::string_view bytes;
	std::uint64_t base = 0;                                // the section's file offset
	residency::verified_regions* regions = nullptr;  // the segment's, heap-held and stable
	bool verify(std::uint64_t offset, std::uint64_t length) const { return regions->verify({base + offset, length}); }
};

// Lays out a segment's sections in file order (F5): each at the previous one's aligned
// end, streamed sections as they arrive, in rank order, and pinned ones held until
// finish(), which writes them in rank order. Records each table-of-contents entry. A
// section produced alongside another goes to a spill through defer() and is copied in by
// place(). Held bytes are charged to `memory` while held.
class section_sink {
public:
	explicit section_sink(output& out, std::uint8_t region_shift = format::default_region_shift, spill* held = nullptr, flush_charge* memory = nullptr)
		: out_(out), shift_(region_shift), held_(held), memory_(memory) {}
	section_sink(const section_sink&) = delete;
	section_sink& operator=(const section_sink&) = delete;
	~section_sink() {
		if (memory_) { memory_->shrink(std::exchange(pinned_bytes_, 0)); }
	}

	// Starts a streamed section at the next aligned offset; end() records it.
	section_stream& begin() {
		out_.align(format::section_alignment);
		return open_.emplace(out_, shift_);
	}
	void end(format::section kind) {
		streamed(kind);
		toc_.push_back(open_->finish(kind));
		open_.reset();
	}
	// A whole section: the header first, a pinned one held for the tail, others streamed.
	void write(format::section kind, std::string_view bytes) {
		if (kind != format::section::header && format::rule_of(kind)->verify == format::check::pinned) {
			pinned_.push_back({kind, std::string(bytes)});
			if (memory_) {
				memory_->grow(pinned_.back().second.capacity());
				pinned_bytes_ += pinned_.back().second.capacity();
			}
			return;
		}
		if (kind == format::section::header && !toc_.empty()) { throw std::logic_error("strata: the header comes first"); }
		begin().write(bytes);
		if (kind == format::section::header) {
			toc_.push_back(open_->finish(kind));
			open_.reset();
			return;
		}
		end(kind);
	}

	// A section streamed into `held` while another is written.
	class deferred {
	public:
		deferred(spill& held, std::uint8_t shift) : held_(held), out_(held), stream_(out_, shift) {}
		section_stream& stream() noexcept { return stream_; }

	private:
		friend class section_sink;
		spill& held_;
		counting_output out_;
		section_stream stream_;
	};
	std::unique_ptr<deferred> defer(spill& held) const { return std::make_unique<deferred>(held, shift_); }
	// The spill the core gave this sink for deferred sections.
	spill& held() const {
		if (!held_) { throw std::logic_error("strata: this sink has no spill"); }
		return *held_;
	}
	void place(deferred& d, format::section kind) {
		streamed(kind);
		out_.align(format::section_alignment);
		auto entry = d.stream_.finish(kind);
		entry.offset = out_.size();
		d.held_.copy_to(out_);
		toc_.push_back(std::move(entry));
	}

	// Writes the held pinned sections in rank order; then the table and footer follow.
	void finish() {
		std::stable_sort(pinned_.begin(), pinned_.end(), [](const auto& a, const auto& b) { return format::rule_of(a.first)->rank < format::rule_of(b.first)->rank; });
		for (auto& [kind, bytes] : pinned_) {
			out_.align(format::section_alignment);
			section_stream st(out_, shift_);
			st.write(bytes);
			toc_.push_back(st.finish(kind));
			if (memory_) {
				memory_->shrink(bytes.capacity());
				pinned_bytes_ -= bytes.capacity();
			}
			std::string().swap(bytes);
		}
		pinned_.clear();
	}

	counting_output& out() noexcept { return out_; }
	std::vector<format::section_entry>& toc() noexcept { return toc_; }

private:
	// A streamed section must follow every earlier one in rank (a writer's logic error).
	void streamed(format::section kind) {
		const std::uint8_t rank = format::rule_of(kind)->rank;
		if (rank == format::unranked || rank <= last_rank_ || format::rule_of(kind)->verify == format::check::pinned) {
			throw std::logic_error(std::string("strata: section out of file order: ") + format::name_of(kind));
		}
		last_rank_ = rank;
	}

	counting_output out_;
	std::uint8_t shift_;
	spill* held_;
	flush_charge* memory_;
	std::optional<section_stream> open_;
	std::vector<format::section_entry> toc_;
	std::vector<std::pair<format::section, std::string>> pinned_;
	std::uint64_t pinned_bytes_ = 0;
	std::uint8_t last_rank_ = 0;
};

}  // namespace strata
