# The core and search targets

strata is two build targets in one repository, the first step toward two libraries (the
repository split waits for the search engine to have a name). The line is layers.md's: a
lifecycle core plus section plugins (L1, L12).

| Target | Directory | Holds |
|---|---|---|
| `strata::core` | `include/strata/core/` | the index and its lifecycle (segments, manifests, merges, merge policy, checkpoints, budgets, files, executors, faultfs); the `keys` and `stored` plugins; the document layout; the generic segment writer and document; the file frame and section table |
| `strata::search` | `include/strata/search/` | the `inverted` plugin (terms, postings, positions, lengths, norms), its builder and merge, the inverter, scoring, queries, aggregation, sorting, collapse, doc values, reordering, the search layout, and the aliases that name it (`index`, `segment`, `document`, `operation`, `transaction`, `index_options`, `check_report`, `segment_writer`, `merge_source`) |
| `strata::strata` | | both; what the tools, tests, and benchmarks link |

Files moved because the directory is the target. The six files that held both sides were split
at the plugin seam: `format.hh` (the frame and section table stay; skip data, ordinal blocks,
seeds, and term info go to `search/format.hh`), `reader.hh`, `writer.hh`, `merge.hh`, `check.hh`,
and `index.hh`. Callers' includes were rewritten by name; `search/index.hh` is the header for the
search layout's index.

## What the core asks of a layout

The core never names a search type. It folds over a layout's entries and calls optional
static members of each plugin, found with `requires`:

| Hook | Used by |
|---|---|
| `P::builder` | the generic writer (`basic_segment_writer<Layout>`) builds each configured plugin's sections in layout order |
| `P::inverter` | the buffer that inverts documents as they arrive at one flush thread; none: documents wait whole and flush through the writer (what `document_layout` does) |
| `P::records(reader, core, live, emit)`, `P::records(docid, part, emit)` | the digest, for segments and for buffered documents |
| `P::buffered(part&)` | normalizes a document as it is buffered |
| `P::total_length(reader)`, `P::length(reader, ordinal)` | the length statistics the manifest records |
| `P::consistent(structures)`, `P::refuse(part, configured)` | the writer's structure checks |
| `P::report`, `P::check(reader, core, report, key_prefix)` | `check_segment` and `basic_index::check()` |
| `order_source<Layout>` | the merge's order source and its settings, in `index_options` |

## The boundary

`strata::core` has its own include root, a directory of the build tree holding a link to
`include/strata/core` and nothing else, so a consumer of core alone cannot reach a search header by
`strata/search/...`. It links none of search's libraries (`docset`, `bitpack`, `analysis`, and
`analysis`'s `unicode` and `snowball`); `fst` reaches core through `termdict`, which links it. Five checks:

1. `core_boundary` compiles every core header in a translation unit of its own against that root and
   those libraries; it builds with the rest, so a core header that includes search fails the build,
   and its ctest entry rebuilds it. `core_boundary_negative` proves a search header is not found there.
2. `core_includes` (`cmake/check_core_includes.cmake`) reads every core header's includes and fails on
   any that is not another `strata/core/` header or a library core owns. It catches `"../search/x.hh"`,
   which the linked root resolves physically into `include/strata/search`; `core_includes_negative`
   proves it on a fixture.
3. Configure fails if `strata_core`'s transitive `INTERFACE_LINK_LIBRARIES` contain any of the
   search-only libraries, however they arrive.
4. `order_source_negative`: a unit that names the search layout without `search/reorder.hh` does not
   compile (`search/reader.hh` declares `order_source<search_layout>`, `reorder.hh` defines it), so no
   program can see two layouts of `index_options`.
5. `core_kv_test` links `strata::core` alone and drives `basic_index<document_layout>` through a
   seeded key-value workload: puts, conditional puts, deletes, `get`, flushes, merges, commits,
   reopens, a checkpoint installed into a second index, the digest, `check()`, and the sections every
   segment holds. `index_test` runs the same `get` on the search layout and checks that a search
   index opened as the document layout is refused (`unsupported_index`).

## What in the core still names search

1. `format.hh`'s section enumeration, structure bits, owners, and rule table list the search kinds
   (`terms`, `postings`, `lengths`, ...) by number and rank. It is one numbering space that every
   plugin claims from (layers.md L1); no search code or type is included.
2. `keys` is built on the `termdict` library (a sorted dictionary with a block index) and
   `fusefilter`, as the inverted plugin's term dictionary is. Core links `termdict`; it is a
   separate library, not search code.
3. The manifest carries a length statistic (`total_length`, `live_length`). The core fills it
   through the plugins' `total_length` and `length` hooks; it is 0 for `document_layout`.
4. `no_more_docs`, the ordinal sentinel, is the core's own (`format.hh`); search asserts it equals
   `docset`'s.

## The key-value harness on the core

`bench/kv/kv_strata.cc` (branch perf/core-vs-rocksdb) links `strata::core` alone with
`basic_index<document_layout>`; with its includes renamed (`strata/core/budget.hh`,
`strata/core/index.hh`, `strata/core/pool_executor.hh`) it compiles and runs. Its two reaches into
private state have this public core API:

1. get: `basic_index::get(key)` returns the live stored record (the buffered copy, then the newest
   segment holding the key). It replaces the harness's `key_slot`, `segs_`, and `active_->buf` walk.
2. scan: there is none yet. Over flushed data, `segments()` with `keys_plugin::reader::scan` and
   `stored_plugin::reader::stored` is public and ordered per segment; the harness merges the segments'
   scans itself. Buffered keys have no sorted view (rocksdb-survey K3), so a scan sees them only after
   `refresh()`. A core `scan(from, n)` merging segments and the buffer is the missing piece.
