# The field check at each map

> Status: CURRENT (2026-11-24) as built on `quest/xapiand/field-check-at-map`; the field table is
> `field-table.md`, the key field `docs/design/fields-and-keys.md` D7, the cost rows
> `docs/perf/eager-open.md`.

## What it does

A segment's field ids must lie in its manifest's field table, and none may be the key field (a key
field holds no terms, D7). Stage 1 checks every id a transaction names, so a segment this process
wrote is right by construction. A segment read from disk is right only if its manifest says so:
a manifest resealed with a field dropped, or a file under another index's manifest, names ids the
table lacks. Such a segment is refused with `corrupt_segment` wherever it maps.

## Where it runs

The manifest's claim a mapped file must meet is its `vouch` (`include/strata/core/index.hh`):
the structure mask and version range (F2, F20), and the field table's size and key field. `vouch_of`
copies all four from the manifest a record is made under, and `vouch::check` runs them on the
decoded segment in `open_body_of` (`index_open.hh`), the one function every map goes through:

| Map | Caller | Vouch |
|---|---|---|
| Eager open (`eager_segments`, `defer_io`) | `load_files` | the opened manifest's |
| A lazy segment's first read | `open_segment::seg()` | the opened manifest's, kept on the record |
| A merge's input | `schedule` (`seg()`): a refused input is marked damaged and left out | as above |
| A wake (`promote`) and a read after `demote` | the twin's `seg()` | the original record's |
| A heal's reread | `heal` | the durable head's |
| A checkpoint install | `install` | the installed manifest's |

A segment this process wrote (a flush, a merge's output) keeps the default vouch, which bounds no
field (`fields` all ones): its ids passed stage 1, and the table in memory may be ahead of any
manifest. A refusal at a first read marks the record failed (cold-tier D11), so it is not mapped
again until `heal()`.

The claim is kept on the record, one per segment of every open index, at no memory cost: the
vouch is 32 bytes (16 on main), and the record's small members (`version` and three flags) share
the one word each used to pad alone, so `open_segment` stays 200 bytes, as on main. The check
itself keeps nothing: the counted heap per index equals main's with Xapiand's loop
(`docs/perf/eager-open.md`, "The record's size").

## The plugin's part

`basic_segment::check_fields(size, key)` (`reader.hh`) calls each plugin reader's
`check_fields(size, key)` when it has one and compiles to nothing for one without, so the core's
layouts (keys, stored) pay nothing. Prospect's inverted reader forwards to its dictionary, which
checks the field directory and every pooled term's id. The check belongs at the map, not the open:
a lazy open, a merge and a wake never ran Xapiand's open-time `verify_fields` loop, and with the
check at the map that loop is redundant.

## Evidence

`test/field_check_map_test.cc`: a stored plugin whose records are field ids, its reader checking
them; a manifest resealed with the last field dropped, and one with that field made the key,
each refused at an eager open, at the lazy first read of the one segment that names it (the others
read), and at a merge's input map (the others merge, the refused one is kept and marked); a good
index checked once per segment and unchanged; a segment written in-process remapped unchecked.
