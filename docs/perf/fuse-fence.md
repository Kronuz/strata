# Fuse filter construction failure fencing an index

Cause: not duplicate hashes (keys are strictly increasing and fusefilter dedupes anyway) and not
bad luck for one seed. fusefilter::filter8::build sizes its table from the key count, and near
some counts (11,509 keys with "user%08d" keys, 11,435 to 11,509 range seen with random keys) the
table has too few slots for any of the 100 seeds to peel, so build threw. A merge of about
11.5k keys (100k records at 5 MiB) hits it; which keys a background merge picks decides whether
the count lands there, which is why it looked timing dependent.

Fix (Kronuz/fusefilter branch grow-on-failure): after the 100 seeds fail, grow the table one
segment and retry, up to 8 times. The header records the shape and seed, so readers are
unchanged; any set that built before gets the same bytes (the first 100 attempts are
unchanged). Strata pins the fixed commit.

Test: test/keys_fuse_test.cc (fails with the old pin: "construction failed"; passes with the
fix). core_kv_test and format14_test pass unchanged.

Not done: a set that needs many seeds (36 to 100 attempts near these counts) is slow to build;
sizing for those counts could be improved in fusefilter (changes bytes for them).
