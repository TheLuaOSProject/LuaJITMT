String Table Retry Yield
========================

`strtab_wait()` no longer sleeps for a fixed 1 ms. Contenders now do a short CPU
pause loop and yield through `lj_thr_yield(L)`, keeping the wait native and
safepoint-visible without parking interners for millisecond-scale intervals.

This does not change the current string-table protocol: readers still publish a
TG-local active header/depth while traversing a table generation, and resizers
still claim `StrTabHdr.resize` before draining active readers. The change only
narrows the retry wait used when an interner races a resize claim, secondary
rehash, or active-generation drain.
