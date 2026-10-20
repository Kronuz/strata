# core-split
- Task: split the repo into two build targets, `strata::core` (the lifecycle, the keys and stored sections, the document layout) and `strata::search` (inverted, postings, scoring, queries, the search layout). The first step toward two libraries; the repository split waits for the search engine's name.
- Branch / worktree: phase4/core-split from main 76fa331 (tag format-13), at /Users/kronuz/code/Kronuz/strata-wt/core-split
- State: starting. Plan: (1) move headers into include/strata/core/ and include/strata/search/, callers migrated by script; (2) split the files that hold both (format, reader, writer, merge, check, index) at the plugin seam; (3) make basic_index<document_layout> compile with the core alone; (4) the boundary test and a core-only KV test; (5) suites, sanitizers, revert and mutant checks.
- Done: nothing yet.
- Next: step 1.
- Blocked on: nothing.
