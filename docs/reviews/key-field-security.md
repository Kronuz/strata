# Security review: F3 (delete key_prefix), decoder safety

Subject: strata `phase8/key-field` strata:d4e43a9 (base strata:5e6af07), prospect `phase8/key-field` prospect:6330ddf. Reviewer: security. Date 2026-10-28.

## Verdict

APPROVE. No blockers. One low finding (SR1) with an optional hardening; one informational note.

## What was run

- ASan+UBSan build (LLVM 22, Debug -O1, STRATA_FUZZ=ON), own tree, `quiet light f3sec`: ctest 57 of 57 pass.
- `fuzz_files` 241 s from the checked-in corpus: 6.0M runs, no crash, no new finding.
- Probes (kept in `.scratch/`): `probe_manifest.cc`, `probe_keysbit.cc`, `build_probe.sh`, `manifest_v6.bin`, `manifest_v7.bin`, `fuzzsec.log`.
- Not read: prospect's full test diff beyond the F3 hunks; prospect was reviewed by diff only (no prospect build).

## Focus 1: the manifest decoder at 0x80000007

Verdict: sound.

- The field is removed from encode and decode symmetrically (`files.hh` encode line ~251, decode ~296); every later field is read through the bounded `detail::reader` (`get` via LEB128 decode against `end_`, `bytes`/`count` against bytes left). No new length is introduced, one is removed (`sized()` for the prefix is gone), so the attack surface shrank.
- Order is CRC, then version (`reader` ctor): a v6 manifest is refused as `unsupported_index`, never misparsed. Probe: the base-commit fixture `manifest_v6.bin` through the new decoder gives `unsupported`. A v7 file with legacy `01 51` bytes inserted and a valid CRC gives `corrupt` (trailing or shifted fields), not a misread.
- Probe `probe_manifest` (ASan+UBSan, CRC resealed after every edit): every truncation, 3 insertions at every offset (including a 10-byte max varint and 40 x 0x80), and 300k random 1 to 3 byte mutations: 95k ok, 12k unsupported, 194k corrupt, no sanitizer report, no other exception type. Encode/decode round trip of the v7 fixture is byte-identical.
- Old versions are refused, not migrated. That fits the pre-freeze series (a build refuses another build's files); the cost is that a v6 index must be rebuilt, which the design accepts (formats open until release).

## Focus 2: keys bit off is now a valid keyless index (SR1)

### SR1 (low): a CRC-valid manifest with the keys bit cleared opens, and the next merge drops the keys

Attacker: someone with local file access who can write a manifest with a valid CRC (the CRC is not a MAC), or a software bug that writes wrong `configured`. Not a random bit flip: CRC-32C catches those at open before this code runs, so the honest-fault case is covered.

Probe `probe_keysbit.cc` (faultfs index, 40 keys of 40 distinct first bytes, two segments, manifest rewritten with `configured &= ~keys`):

1. Clearing `keys` alone: refused at open, `corrupt_index "configured structure bit 7 without its owner"` (index_sort requires keys). Good.
2. Clearing `keys` and `index_sort` (what a deliberate editor does): opens. Existing segments still hold their keys sections, so `find_key`, `get` and `scan` still return all 40. Replace by key gives `invalid`; an anonymous add is accepted; `check()` passes.
3. After `force_merge`, the merged segment is written with the manifest's structures (keys off): `find_key`/`get`/`scan` return 0 of 40. The documents survive; their keys are gone, and `check()` still passes. Reopening with `keyed = true` then throws "the index is keyless".
4. Keys bit set on a keyless index: refused at open (`configured & ~mask`, "structures or versions"). Good.

No memory unsafety in any case: ASan and UBSan stayed silent through open, apply, commit, check, merge and reopen. The result is silent loss of the key association (a wrong read: the key no longer finds its document), not a crash or an out-of-bounds read.

Acceptable under the trusted-cluster threat model (perimeter and honest faults only): reaching it needs a deliberate, CRC-correct edit of a file inside the perimeter, and the same editor could equally delete segments or rewrite counters, which the manifest never defended (it is not authenticated). D8's dropped check does not widen an honest-fault path, because a single bit flip fails the CRC first.

Optional hardening, cheap (not required to land): in `check_readable`, refuse `(m.mask & keys) && !(m.configured & keys)`. Creation sets `mask` to the structures the index may hold and a keyless index's mask lacks keys (probe case 4 shows it), so a keyed index stripped of the bit alone would fail as it did before D8. A deliberate editor can still clear `mask` too, so this narrows, not closes. I did not check whether `mask` can legitimately exceed `configured` elsewhere (for example after a planned structure removal); verify before adding.

Install path: `install` copies `theirs.configured` over `m_.configured` (index.hh ~1535), as it copied `key_prefix` before. A peer's keyless manifest installed onto a keyed index therefore turns it keyless, as before. Unchanged by F3; same trusted-peer model.

## Focus 3: key bytes and leftover prefix assumptions

- `git grep` on strata `include src tools bench fuzz` for `key_prefix`, `'Q'`, `"Q"`: only bench generators (`bench/kv/kvgen.hh`, `kvdriver.hh`, which make Q-led keys; harmless) and the mutant that tests the rule. `starts_with` uses in `index.hh` are file names and the reserved metadata prefix, not keys.
- `wbuf::key_prefix` renamed `key_head` (the first 8 bytes as a big-endian sort prefix). It reads `unsigned char`, so 0x80 to 0xff sort correctly, and keys shorter than 8 bytes zero-pad: `"\0"` and `""` share a head but the full-key compare (`entry_from`) breaks the tie. No finding.
- Keys: `key_of` accepts 1 to `max_term_bytes` bytes of any value; empty means keyless; oversize is invalid. `replace_key`/`delete_key` reject empty keys (`o.key.empty()`) and keyless indexes. The index_test added for all 256 first bytes plus NUL-NUL and a 41-byte Q-led key passes buffered, flushed, reopened, merged, erased and scanned in byte order, under ASan.
- Prospect: keys are also text terms. Routing is by the key field's role, not by a leading byte; prospect's `format13_test` covers every first byte as key and as text term through flush, merge and reopen, and the mutants V and W (route by `Q` or 0xff) are reported to die. Diff review found no remaining prefix argument (`inverted_plugin::check`, `values_plugin::check`, `fuzz_segment.cc` all migrated). Prospect was not rebuilt here; its ledger reports ASan, UBSan and TSan 49 of 49 and CI 8 of 8.
- Mixed state worth knowing: `check_segment` no longer takes the prefix, so the "key lacks the prefix" damage class is gone; a segment whose keys are all well-formed bytes cannot be flagged by prefix any more. That is the intent of F3, not a regression.

## Non-blocking

1. SR1's optional hardening above.
2. `probe_keysbit` case A cleared both bits and the merge dropped keys silently; if the lead wants a louder signal for this class, `merge` could assert the output's structures cover the keys sections of its inputs (a reader of any input with `present` keys under `!(structures & keys)`). Design decision, not a security need.

## Not run

TSan (no concurrency change); prospect build and tests (reviewed by diff, ledger reports sanitizer runs); other platforms (CI); fuzz targets other than `fuzz_files` (`fuzz_stored`, `fuzz_fields` untouched by this change); a replica `install` probe of a keyless manifest onto a keyed index. Build directory deleted; probes and the fuzz log stay in `.scratch/`.
