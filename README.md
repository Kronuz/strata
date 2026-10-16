# strata

[![CI](https://github.com/Kronuz/strata/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/strata/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Status: in development](https://img.shields.io/badge/status-in%20development-orange.svg)

An embeddable, MIT-licensed search engine in C++20, built to replace Xapian inside
[Xapiand](https://github.com/Kronuz/Xapiand). Writes produce immutable segments, reads
skip whole blocks with block-max pruning, and every query is a coroutine, so a host
event loop never blocks on it.

![strata: Xapiand on top, strata core and replication groups in the middle, small libraries below](docs/strata.svg)

## ✨ Highlights

- 🧱 **Immutable segments** with crash-safe commits, verified against power loss,
  disk-full, and media faults.
- ⚡ **Block-max pruning** over compressed postings, scored with BM25 in Lucene's or
  Xapian's parameterization.
- 🧵 **Coroutine queries** on the caller's reactor: no thread of its own, no blocking
  reads.
- 🔁 **Replication groups**: one consensus group per shard, built from
  [consensus](https://github.com/Kronuz/consensus) and
  [journal](https://github.com/Kronuz/journal).
- 🧩 **Built from small libraries**: [fst](https://github.com/Kronuz/fst),
  [termdict](https://github.com/Kronuz/termdict),
  [bitpack](https://github.com/Kronuz/bitpack),
  [docset](https://github.com/Kronuz/docset),
  [analysis](https://github.com/Kronuz/analysis), and more; see
  [cpp-libraries](https://github.com/Kronuz/cpp-libraries).
- 📏 **Ratcheted against Lucene, Tantivy, and Xapian**: each phase must match or beat
  them on speed, memory, and size before the next begins.

## 🚧 Status

strata is under active development and its API changes between phases. Phases 0
through 2 have passed their gates. Phase 3 (index lifecycle) has reported its
durability rows, and its performance rows are being measured. Queries don't yet meet the
≤ 1.0× target. [docs/PLAN.md](docs/PLAN.md) has the phases, and
[docs/gates/](docs/gates) has every measured row.

## 📦 Installation

Requires a C++20 compiler and CMake 3.18 or newer; the libraries it builds on are
fetched at configure time. CI builds it on Linux (x86_64, arm64), macOS (arm64,
x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(strata
  GIT_REPOSITORY https://github.com/Kronuz/strata.git
  GIT_TAG        main)
FetchContent_MakeAvailable(strata)

target_link_libraries(my_app PRIVATE strata::strata)
```

## 🚀 Quick start

```sh
git clone https://github.com/Kronuz/strata.git && cd strata
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build
```

The tests are the current usage examples: `test/index_test.cc` covers indexing, commit,
and recovery; `test/search_test.cc` covers the scorers; `test/multisegment_test.cc`
covers search across segments.

## 🎯 When to use it

- ✅ Following or contributing to the engine, or embedding it experimentally.
- ❌ Production search today: use Xapiand on its Xapian core, Lucene, or Tantivy until
  strata's Phase 3 gate passes.

## 📚 Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): layers, segments, and the read and
  write paths.
- [docs/PLAN.md](docs/PLAN.md): phases, ratchets, and gates.
- [docs/REPLICATION.md](docs/REPLICATION.md) and
  [docs/DISTRIBUTION.md](docs/DISTRIBUTION.md): replication groups, placement, and
  membership.
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) and [docs/TESTING.md](docs/TESTING.md): how
  it is measured and tested.
- [AGENTS.md](AGENTS.md): repository map and working rules.

## 🤝 Contributing

Read [AGENTS.md](AGENTS.md) first: strata is a clean-room implementation, and every
change goes through its phase gates. CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
