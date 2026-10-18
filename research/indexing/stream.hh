// The corpus sample as a token stream, shared by the research prototypes: each document's
// tokens in order (the key term "Q" + id first, at position 0), pointing into the file.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace research {

struct tok {
	const char* p;
	std::uint32_t len, pos;  // the key term has position 0
};
struct doc_toks {
	std::uint32_t first, count;
	std::string_view id, key;  // key: "Q" + id
};
struct stream {
	std::string file, keys;
	std::vector<tok> toks;
	std::vector<doc_toks> docs;
	std::uint64_t text_bytes = 0;
};

inline std::string_view bytes_of(const tok& t) { return std::string_view(t.p, t.len); }

inline stream load(const char* path, std::size_t max_docs) {
	stream s;
	std::ifstream in(path, std::ios::binary);
	if (!in) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
	in.seekg(0, std::ios::end);
	s.file.resize(std::size_t(in.tellg()));
	in.seekg(0);
	in.read(s.file.data(), std::streamsize(s.file.size()));
	std::string_view all(s.file);
	// Keys first, so their bytes never move.
	std::vector<std::pair<std::uint32_t, std::uint32_t>> lines;
	for (std::size_t at = 0; at < all.size() && (!max_docs || lines.size() < max_docs);) {
		std::size_t nl = all.find('\n', at);
		if (nl == std::string_view::npos) { nl = all.size(); }
		lines.push_back({std::uint32_t(at), std::uint32_t(nl - at)});
		at = nl + 1;
	}
	s.keys.reserve(lines.size() * 64);
	std::vector<std::pair<std::uint32_t, std::uint32_t>> key_spans;
	for (const auto& [at, len] : lines) {
		std::string_view line = all.substr(at, len);
		auto i = line.find("\"id\": \"");
		if (i == std::string_view::npos) { key_spans.push_back({0, 0}); continue; }
		i += 7;
		const std::string_view id = line.substr(i, line.find('"', i) - i);
		key_spans.push_back({std::uint32_t(s.keys.size()), std::uint32_t(id.size() + 1)});
		s.keys.push_back('Q');
		s.keys.append(id);
	}
	for (std::size_t n = 0; n < lines.size(); ++n) {
		const auto [at, len] = lines[n];
		std::string_view line = all.substr(at, len);
		if (!key_spans[n].second) { continue; }
		doc_toks d;
		d.first = std::uint32_t(s.toks.size());
		d.key = std::string_view(s.keys).substr(key_spans[n].first, key_spans[n].second);
		d.id = d.key.substr(1);
		s.toks.push_back({d.key.data(), std::uint32_t(d.key.size()), 0});
		if (auto t = line.find("\"text\": \""); t != std::string_view::npos) {
			t += 9;
			const std::string_view text = line.substr(t, line.find('"', t) - t);
			s.text_bytes += text.size();
			const char* p = text.data();
			const char* end = p + text.size();
			std::uint32_t pos = 0;
			while (p < end) {
				const char* q = static_cast<const char*>(std::memchr(p, ' ', std::size_t(end - p)));
				if (!q) { q = end; }
				if (q > p) { s.toks.push_back({p, std::uint32_t(q - p), ++pos}); }
				p = q + 1;
			}
		}
		d.count = std::uint32_t(s.toks.size()) - d.first;
		s.docs.push_back(d);
	}
	return s;
}

}  // namespace research
