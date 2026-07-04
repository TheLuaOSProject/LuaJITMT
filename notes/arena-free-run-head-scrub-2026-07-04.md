# Arena Free-Run Head Scrub

Date: 2026-07-04

`arena_insert_run()` must keep removing exact-address duplicates before
publishing a run. Split-tail and realloc-shrink paths can otherwise put the
same cell into a free bin more than once, which is unsafe once the address is
reused.

A broader experiment that validated every existing bin node during insertion
was correct but too expensive: `tab_insert_newkey` regressed past the stock
comparison gate at about 3.4x. The committed shape only scrubs stale leading
nodes during insertion, keeps the duplicate-address walk, and leaves full
per-node validation to `arena_find_run()`, which already must validate before
reuse.

The regression fixture in `tests/t-arena-realloc.c` seeds a bin head with a
stale allocated-cell record and verifies that freeing another same-bin run
drops the stale head instead of chaining it behind the new run.
