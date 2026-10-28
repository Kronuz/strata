# The field table

> Status: CURRENT (2026-10-27) as built on `phase8/field-table`; the design is
> `docs/design/fields-and-keys.md` (D2, D7), step F1. Prospect's use of it is F2.

## What it does

An index has one **field table**: an append-only list of declarations (a name, a role, what the
postings hold, whether the field has norms). A field's id is its position. The core owns the table
and replicates it with the log, so every replica applying the same transactions assigns the same
ids. The core names no search type: a layout's plugin parts hold `field_ref`s and the core resolves
them in stage 1.

A transaction (`basic_transaction`) gains two members. `fields` is the list of declarations its parts
name by position (`field_ref::declared(i)`). `expected_fields` is a mark, `{size, chain}`, of the table
the host read its ids from (`field_ref::id(n)`). A host that never read the table uses only
`declared(i)`, so it predicts no id.

## How stage 1 works

`evaluate()` runs these steps before the operations, inside the undo log:

1. **The mark.** With `expected_fields`, the table must hold at least `size` declarations and its chain at
   `size` must equal the mark's, else the verdict is `fields_conflict` carrying the table's current
   mark, and nothing is declared. The chain is `c0 = 0`, `c(i+1) = xxh64(c(i) || declaration i)`, kept
   per declaration in memory (8 bytes a field) and recomputed at open.
2. **The declarations**, in order, each by name: an invalid one (`field_problem`: name length, role,
   postings kind, a key field with postings or norms) or a different declaration of a known name
   rejects `invalid` with `verdict::field` naming the entry; an identical one answers the known id; a
   new name takes the next id after the key rules (only in a keyed index, at most one key field). The
   first change logs an `undo_step::fields` with the old size, so a rejection truncates the table back
   and the next transaction reuses the ids.
3. **The resolution** (`field_resolution`): the id of each entry of `fields`, and `known`, the mark's
   size. `validate_context::field(ref)` answers a part's reference: `declared(i)` needs `i` below the
   entry count, `id(n)` needs `n` below `known`, so an `id` without a mark never resolves. A plugin's
   `validate` calls it; its optional `resolve(part&, const field_resolution&)` rewrites the buffered
   copy's references to ids (the buffer stores ids).

```
host table T (size 5, chain c5) ──mark──▶ txn { fields:[y], parts: id(2), declared(0) }
                                            │ stage 1
        table now {0..6}? chain at 5 == c5? ─┴─ no: fields_conflict (nothing declared)
        yes: y found or appended (id 6); id(2) < 5 ok; declared(0) = 6; rejected => truncate to 6 entries
```

Why a mark and not predicted ids: two transactions built against one table that each declare a new
field would both predict the same id. `declared(i)` carries no id, and the mark's chain catches the
rest: after a crash drops applied declarations and later ones reuse their ids under other names, the
chain at the host's size differs, so its transaction is refused instead of binding terms to the wrong
field. A concurrent declarer only appends beyond `size`, so ordinary concurrency never conflicts.

## The file

`fields-<gen>.kv` is append-only: the files family's magic and version, then blocks `[count LEB]
[per declaration: name length LEB, name, role, postings, flags][CRC32C of the block]`. The manifest's
`fields` reference (`generation`, `size`, `crc`) names the prefix it reads; the CRC is the last block's.
`decode_fields` bounds every length by the bytes left, refuses a name twice, a second key field,
an empty block and any declaration `field_problem` refuses. `index::commit` appends one block when the
table grew, to the file the manifest names, and syncs it with the other new files before the manifest
renames; a manifest committed earlier still reads its shorter prefix (checkpoints). A new file
(generation from `next_fields`) is written whole when the table replaced the durable one (`install`), or
when the file has bytes past the manifest's size (a torn append): no byte any manifest may name is
overwritten. Open reads the prefix, recomputes the chain and the id map; `check()` re-reads the prefix
and compares; `retain()` keeps the file while a manifest names it.

An append-only file keeps T8's bytes written near the table's size (one block per commit, five bytes of framing) where rewriting the table whole at each commit writes its square; `bench/field_table`
measures it.

## What reads the table

- `index::fields()` and `fields_mark()`: the last applied table (apply runs on the writer's thread,
  so never an in-flight declaration). A view carries a snapshot, shared between views until the
  table's epoch moves.
- The digest has one record per declaration (`F`, id, name, options), in id order.
- `install` adopts the source's table and marks it for a new file.

## Where it is tested

`test/fields_test.cc`: a property test against a model (stale tables, repeated and conflicting new
names, mixed references; every resolved term's field is the one the host named), rollback then reuse,
the lost tail, each stage-1 rule, the append-only file and replicas (replay, checkpoints, install),
a crash at every mutating operation of a declaring commit, readers during declarations, and the
decoder. `fuzz/fuzz_fields.cc` fuzzes the decoder. `tools/mutants/fields.py` breaks one rule at a time
and expects `fields_test` to fail.

## Not here

Typed values (phase-4 D3's value half), the prospect plugin's field parts (F2), and replacing
`key_prefix` by the key field's routing (F2, F3). In an inverting layout stage 1 does not copy a part,
so F2's inverter takes the resolution when it tokenizes.
