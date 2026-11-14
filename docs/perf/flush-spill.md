# A flush within its budget

A flush draws its working set from the indexing budget on top of the buffer it flushes. Two
mechanisms keep the peak near the budget:

1. The positions section, produced beside the postings, goes to `seg-<id>.spill` through the
   index's IO (`index_detail::io_spill`, `index.hh`) after a 1 MiB gather, instead of being held
   in memory until the postings end. A spill within the gather never creates a file.
2. Under a budget the buffer's limit is the smaller of its share and the capacity less the
   flush's working set (`buffer_limit()`, `flush_working_pct`), so the flush fits in the room
   the buffer leaves.

## Measurement (prospect 1M keyed, one thread, budget 1,039 MiB, gate VM, light jobs)

| strata | job | peak RSS | ratio | budget peak | segments |
| --- | --- | --- | --- | --- | --- |
| main 397c02d5 (in-memory spill, trigger at the capacity) | 261113-171217-033927 | 1,578 MiB | 1.52x | | 2 |
| io_spill alone, 92408e2e | 261113-171233-02281e | 1,343 MiB | 1.29x | 1,249 MiB | 2 |

The io_spill saves 235 MiB (15%) of peak RSS; the spilled section is 284 MiB (positions of
the first segment). What remains is the builder's scratch: in the 92408e2e run the flush
started with the buffer charged at 1,040 MiB and RSS at 1,071 MiB, and RSS peaked at 1,343 MiB,
a working set of 272 MiB, 26% of the buffer's charge (the budget's own charge rose 209 MiB,
20%; the rest is allocations the flush does not charge). `flush_working_pct` is that 26%.
