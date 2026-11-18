> Status: PROTOTYPE (2026-11-04) key and stored bytes inline in the lane record, behind `STRATA_WBUF_INLINE` on `explore/wbuf-bytes`; not landed.

# Write buffer bytes per record: where they go on main

Question: the hunt list says ~290 B per record against 177 B designed; where do the bytes go on current main (`356acbc`), and what single change recovers most?

Row: `kv_proto_bufmem --value 100` (document layout, 16 B keys, 100 B values, nothing flushed), Release, dev Mac (macOS x86_64). Resident bytes per record is RSS(300k) - RSS(100k) over 200,000 records; `heap` is malloc's size-in-use over the same span. Strict hold, 5 interleaved rounds of base, base again (A/A), inline.

## The 290 B figure is stale

The arena (lanes of records in chunks, write buffer items 2-4) and 8-byte map slots have landed. Main costs 231-237 B resident per record (heap 238-246), not 290. The gap to the 177 B design is ~55 B, not 113 B.

## Attribution on main (300k records, bytes per record)

| Piece | Bytes | Why |
|---|---:|---|
| Lane record | 80.4 | 32 B header + `document` of two `std::string` (24 B each on libc++) |
| Value heap string | 112 | 100 B value + NUL, malloc quantum 16; the 16 B key is SSO inside the document |
| Version map | 14.0 | 8 B slots, 524,288 slots at 300k (11 B at 7/8 full) |
| Position index | 21.0 (12 B resident) | 12 B entries in doubling blocks; the unwritten tail of a block is never touched, so it is virtual, not resident |
| Docid map | 0 | not built for key-addressed puts |
| Sum (heap) | ~227 | RSS marginal 231-237 |

The design's 177 B is 32 header + 16 key + 100 value + 12 index + ~11 map + 8 lengths; main holds the same pieces but spends 192 B on key and value (80 B record + 112 B heap) where the design spends ~156 B inline.

## Experiment 1: key and stored bytes inline in the record (`STRATA_WBUF_INLINE`)

For `document_layout` the record holds `[u32 key len][u32 stored len][key][stored]` after its header instead of a `document`: no per-value malloc, no 16 B malloc rounding, no two string headers. A record too big for a 1 KiB chunk (value above ~900 B) keeps a heap `document` (`heap_flag`). The flush borrows the stored bytes straight from the pinned lane through `writer::add_stored_view`. `record::key()` and `stored()` replace `doc().key` and the part casts at the 16 call sites; the digest builds a temporary document.

Resident bytes per record, 5 rounds (A/A = base twice):

| Arm | Resident B/record | Range | Heap B/record |
|---|---:|---|---:|
| base | 231.1, 231.6, 236.5, 236.6, 236.4 | 231.1-236.6 | 237.8-245.7 |
| base (A/A twin) | 236.4, 231.4, 231.0, 231.3, 231.3 | 231.0-236.4 | 237.8-243.1 |
| inline | 203.6, 198.4, 198.6, 198.7, 198.8 | 198.4-203.6 | 204.5-209.7 |

Inline is 32 B (14%, lower is better) smaller resident, from ~233 to ~199 B. The A/A twins agree within 2.3%; the inline range sits wholly below both. The lane grows from 80.4 to 160.8 B per record and the heap value (112 B) disappears.

Identity: the key-order scan digest (`KV_SCAN_DIGEST`, FNV over every key and value) is identical to base on `readseq` and `seekrandom` with 100 B values (`2b32a84591bc1ba5`, `3eceede7d18fcd30`; 100,000 records, 16 MB memory so buffers flush) and with 1500 B values (`b13a7d21edc9ed05`, `d28bcee36aa961f3`; 20,000 records, exercising the heap-record fallback and the flush borrow). bufmem's own scan digest over 300,000 records is `15a6928fd4ce9082` in all 15 runs.

## What remains (resident, inline)

199 B against 177 B designed: 160.8 lane (156 B payload + chunk slack) + 14 version map + 12 position index + ~12 unaccounted (malloc and table-growth transients, not attributed). Not chased.

## Not measured, not done

1. Throughput of the inline layout (fillrandom, gets, scans): only bytes and identity were checked. Reads touch one contiguous record instead of a record plus a heap string, so a gain is plausible but unmeasured.
2. Unit tests, sanitizers, TSan. A rejected transaction's `truncate()` runs the new destructor (heap record); untested.
3. Other layouts (inverting, field-carrying) keep `document` records: the switch is `document_layout` only. "One mechanism" asks for the same shape everywhere; a finished version needs a view-based document for all layouts.
4. The `STRATA_WBUF_DUMP` hook in `~buffer` and the bufmem heap and scan-digest lines are measurement code; drop them on landing.

## Verdict

1. The loss is smaller than recorded: main is ~233 B, not 290 B; the arena and slot compaction already landed.
2. The biggest remaining piece is the per-value heap string plus the document's string headers (192 B for a 116 B payload). Inlining them recovers 32 B (14%) and gets to 199 B; the remaining ~22 B over the design is the map, the index and chunk slack.
3. Worth finishing only with a view-based document for every layout, plus the throughput check; as an `document_layout`-only switch it breaks the one-shape rule.
