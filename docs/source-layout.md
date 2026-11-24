# Source layout

Where the code lives, for the headers that were restructured. `docs/SPLITTING.md` in cpp-libraries
is the rule: a piece is a unit someone opens alone, and a file over 1,000 lines stays whole only
when it is one coherent unit.

## index.hh and index_*.hh

`include/strata/core/index.hh` keeps its path and is the header callers include. It holds the
namespace-scope types (verdict, the errors, `index_detail`, `basic_index_options`, `io_spill`,
`file_sink`, `lent_frame`) and `class basic_index` with its data members, nested types and member
declarations, then includes one header per mechanism holding the member definitions. The pieces
sit beside it with its name as prefix, as strata's other headers are flat. Each includes `index.hh`
and compiles alone. `docs/mechanisms/index-layout.md` says why each mechanism is cut where it is.

| File | Holds |
|---|---|
| `index.hh` | the namespace-scope types, `basic_index`'s body, and the includes of the pieces below |
| `index_open.hh` | create, open and recovery, the segment loaders, heal, warm (demote, promote), close |
| `index_apply.hh` | apply, stage 1's evaluation and its undo log, the writer calls' lease, chores and fence |
| `index_buffer.hh` | the write buffer: lookups of its copies, memory accounting, whether a transaction fits |
| `index_flush.hh` | freeze and flush: the buffer written as segments, their files and identities |
| `index_turns.hh` | the turn log: attach, join, leave, group commit, replay, durable waits |
| `index_commit.hh` | commit: the manifest log's edit, its roll-over and the durable writes |
| `index_checkpoint.hh` | checkpoints, release, install, retention |
| `index_storage.hh` | storage accounting, reclamation and its posted deletes, drains |
| `index_merge.hh` | background merges: scheduling, the merge job, publication, abandonment |
| `index_read.hh` | lookups, scans, reads at a position, the shared segment reads, digest and check |
| `index_publish.hh` | shared readers: the views published, retired and freed |

`index.hh` is about 2,970 lines, 2,100 of them `basic_index`'s body: about 1,100 lines of nested
types and the declarations with their comments. Step 2 of the restructure decides which state
leaves the class as a subobject.
