> Status: CURRENT (2026-11-04) workstream perf/wbuf-inline, developer; done, ready for review.

# wbuf-inline: the write buffer's records hold their key and stored bytes inline, for every layout

Task: finish explore/wbuf-bytes (strata:bec2c09, `docs/perf/wbuf-bytes.md`) as one shape: no switch, no
`document_layout`-only path. Branch perf/wbuf-inline off main 9e344d8. Results: `docs/perf/wbuf-inline.md`;
how it works: `docs/mechanisms/write-buffer.md`.

## State

Code, tests, proof and notes done and pushed. CI not watched (the lead watches). Not landed.

## Done

1. strata:55773d2 the inline record for every layout (`index.hh` record, `push_record`, `lane_fits`,
   flush), `writer::add_stored_view`, `stored_builder::add_view`; tests for both forms, sizes through a
   chunk's edge, rejected batches, the room check with big records; `kv_proto_bufmem` by layout; the
   `wbuf` set in `bench/kv/ab.py`.
2. strata:58d40e5 flush test probes; the mechanism note.
3. strata:48fd9f1 and the commit after it: perf note, raw data, exploration-log row.
4. Proof: bytes per record per layout with an A/A twin (macOS 5 rounds, Linux 3); A/B and A/A of ten kv
   rows, 11 trials: all ties, B/A 0.987-1.075 against an A/A of 0.958-1.057; 21 scan digests identical,
   fallback sizes included; ctest 69/69 in Release, ASan+UBSan and TSan on macOS; 70/70 Release and
   ASan+UBSan on the local Linux VM; two mutants caught (room check ignoring stored bytes: abort with
   length_error; flush dropping a byte: write_buffer_test and core_kv_test fail).
5. No fuzz target: nothing here decodes untrusted bytes; the record body is written and read by the
   same code from lengths it wrote.

## Next

1. Review (code reviewer): the writer.hh change (an inline record's views are checked against the lease;
   the whole-document path is unchanged) and the lane room check.
2. The lead: CI, and the owner's rulings below.

## Blocked on

1. Owner/architect: layouts whose parts carry more than stored bytes (field-carrying parts with data,
   prospect's `flush_threads > 1` whole-document mode) keep whole `document` records, so they get no
   saving. Inline for them needs a per-plugin serializer for the buffered part. Options: (a) leave
   them whole (as built); (b) a `buffered_size/put/get` hook per plugin, with the flush materializing
   a document for them; (c) drop the whole-document mode in prospect. Recommendation: (a) until a
   measurement on prospect shows the cost.
2. Lead: the buffer's memory estimate (`doc_bytes`, `estimate_for`) is unchanged, so flush points match
   main; charging real strides would flush later and change flush cadence.
3. Prospect (not in this repository): repin needs `inline_part` nowhere (the default keeps its parts
   whole); untested here. Its inverting layout's records now hold the key alone; not measured.

## Design vs code

See "Design vs code" in `docs/mechanisms/write-buffer.md`: the design's 24-byte header and 143 B
record versus 32-byte header and 156 B; parts beyond key and stored kept whole; the unchanged
estimate. Also: values past about 900 B stay whole because a record must fit the 1 KiB first chunk.
