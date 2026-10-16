# Many small indexes (note, not yet a design)

Xapiand's shape is millions of small indexes scattered over a cluster (a social network:
one per user, group, or conversation), some medium ones, and a few very large ones.
strata so far optimizes the large case. This note records what the small case needs, so
a later phase can design and measure it.

## What a small index costs today

Per open index, independent of its size:

1. **Files and descriptors**: owner lock, manifest, metadata, each segment, each live-docs
   file. Even one segment is several descriptors.
2. **Mappings**: one per segment once segments are mapped. Linux caps a process at
   `vm.max_map_count` (65,530 by default), so a host cannot hold a million mapped indexes.
3. **Pinned per-segment state**: the term index copy, verified-region state, key filter,
   lengths, norms, stats, all decoded at open.
4. **Format overhead**: header, table of contents, 4 KiB section alignment, and a region
   checksum per section. A 3-document segment still pays kilobytes.
5. **Machinery per index**: an executor group, a memory permit, a storage permit, and
   (clustered) a consensus group and journal unless they are shared.

## Directions to measure

1. **Measure first.** Open time, resident bytes, descriptors, and mappings per idle
   index, for 1, 10, 100, and 1,000 documents. A working target: an idle open index
   under 1 KB of memory and no descriptor, open under 100 µs, a million per host.
2. **Open lazily, close idle.** An LRU of open indexes behind a cheap handle. Cold indexes
   hold nothing; opening reads one manifest. Verification stays per region on first
   read, never at open.
3. **Pack small indexes together.** Below a size threshold an index's segments live in a
   shared container file (as Lucene's compound files, but across indexes), or as rows in
   the core KV layer (the layers split, `layers.md`): rows keyed by index and term in
   one shared LSM-style store. An index graduates to its own segment files when it
   grows. This turns millions of files and mappings into a handful.
4. **A small-segment format profile.** Below some document count, skip skip lists,
   impacts, region checksums per section (one whole-file checksum instead), the key
   filter, and alignment padding. Format 13's section groups and structure flags (Phase
   4 D2) are the place for it.
5. **Share the machinery.** One executor group, budget permit, and journal per host or
   shard, not per index: C2's shared journal already holds many groups in one turn log.
   Clustered, many small indexes share one consensus group instead of one each.
6. **Share schemas.** Most small indexes of one application have the same schema;
   store it once and reference it.

## Where it fits

After Phase 4's format 13 (the format profile) and alongside the KV-rows gate (packing),
with measurements on Linux, FreeBSD, and macOS. The distributed side (placement and
shared groups) belongs in DISTRIBUTION.md's open decisions.
