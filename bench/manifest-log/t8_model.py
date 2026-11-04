#!/usr/bin/env python3
"""fields-and-keys T8 under the manifest log (docs/design/manifest-log.md D3, D7, revision 1).

T8 declares N fields one per commit (bench/field_table, names "field-<i>") and bounds the bytes
written for the table at 4x the final table. Arithmetic, from the encodings: a declaration as
the field table writes it (fields.hh:101-106: the name with its LEB128 length, then role,
postings and norms bytes); journal's framing at 512-byte blocks (62-byte block headers, 24-byte
fragment headers, a block tail under 25 bytes padded); records as D3 shapes them. The edit's
scalars are the one estimate: every scalar (revision 0's D3, ~45 bytes, the review's figure)
or a presence mask with the scalars that changed (revision 1: T8's commits change the revision
alone). The architect's model (.scratch/mlogrev/t8_model.py at strata:057c47e) used a 20-byte
declaration and a 200-byte rest of state; this one counts both from the encodings.

Columns: table bytes (declarations in edits plus the table in every snapshot) over the final
table's bytes, the measure T8 bounds in the log's terms; all bytes the log writes over the final
table file (encode_fields: header, count, declarations, CRC); and main's fields-* bytes over the
same file (one appended block per commit: count, declaration, CRC), from the same arithmetic.
"""

BLOCK, HDR, FRAG = 512, 62, 24


def leb(v):
    n = 1
    while v >= 0x80:
        v >>= 7
        n += 1
    return n


def framed(payload, pos):
    """Bytes a record of `payload` bytes takes appended at log offset `pos`."""
    start, rest = pos, payload
    while rest:
        if pos % BLOCK == 0:
            pos += HDR
        room = BLOCK - pos % BLOCK
        if room < FRAG + 1:
            pos += room
            continue
        n = min(rest, room - FRAG)
        pos += FRAG + n
        rest -= n
    return pos - start


def framed_fresh(payload):
    """framed(payload, 0) in closed form: a record from a log's start fills each block with one
    fragment of up to 426 bytes (512 less the 62-byte header and a 24-byte fragment header)."""
    per = BLOCK - HDR - FRAG
    k = max(1, -(-payload // per))
    return (k - 1) * BLOCK + HDR + FRAG + (payload - (k - 1) * per)


def decl_bytes(i):
    name = len("field-%d" % i)
    return leb(name) + name + 3


def run(n_fields, scalars_all, trigger, floor=16 << 10, replay_floor=128):
    """trigger: 'snap' rolls over past max(floor, 2 x the log's own snapshot) (revision 0);
    'now' past max(floor, 2 x a snapshot of the current state) in bytes, or past
    max(replay_floor, the state's entries) in edits since the snapshot (revision 1)."""
    table = 0  # the table's declaration bytes
    gen = 1
    rest_of_state = 1 + 4 + 40 + 2  # kind, generation, files version and scalars, empty metadata
    snap_payload = lambda: rest_of_state + leb(n_decl) + table
    n_decl = 0
    log = framed_fresh(snap_payload())
    snap = log
    written_all, written_table = log, table
    edits = 0  # since the snapshot
    for i in range(n_fields):
        d = decl_bytes(i)
        gen += 1
        if scalars_all:
            head = 1 + leb(gen) + 45  # every scalar, the review's estimate
        else:
            head = 1 + leb(gen) + 1 + leb(gen)  # presence mask, the revision (about the generation)
        edit = head + 1 + leb(n_decl) + 1 + d  # sections, the table size it extends, count, the declaration
        table += d
        n_decl += 1
        size = framed(edit, log)
        now = framed_fresh(snap_payload())
        limit = max(floor, 2 * (snap if trigger == "snap" else now))
        over = log + size > limit or (trigger == "now" and edits + 1 > max(replay_floor, n_decl))
        if over:
            log = framed_fresh(snap_payload())  # the rollover: a fresh log holding the snapshot
            snap = log
            edits = 0
            written_all += log
            written_table += table
        else:
            log += size
            edits += 1
            written_all += size
            written_table += d
    final_table = leb(n_decl) + table
    final_file = 9 + leb(n_decl) + table + 4
    main = 9 + sum(1 + decl_bytes(i) + 4 for i in range(n_fields))
    # main also writes a manifest per commit: about 70 bytes for an index with no segment
    # (an estimate from files.hh:246-269's fields at these values), uncounted by T8.
    main_all = main + 70 * n_fields
    return written_table / final_table, written_all / final_file, main / final_file, main_all / final_file


if __name__ == "__main__":
    print("T8: bytes written over the final table, declaring N fields one per commit (bound: 4x)")
    for label, scalars_all, trigger in (("revision 0: every scalar, 2 x the log's snapshot", True, "snap"),
                                        ("changed scalars, 2 x the log's snapshot", False, "snap"),
                                        ("every scalar, 2 x a snapshot of the current state", True, "now"),
                                        ("revision 1: changed scalars, 2 x a snapshot of the current state", False, "now")):
        for n in (2000, 100000):
            t, a, m, ma = run(n, scalars_all, trigger)
            print(f"{label}; {n:>6} fields: table bytes {t:4.2f}x, all log bytes {a:5.2f}x the table file; "
                  f"main's fields-* {m:4.2f}x, with its manifests {ma:5.2f}x (estimate)")
