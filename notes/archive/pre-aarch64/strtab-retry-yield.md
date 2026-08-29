String Table Retry Yield
========================

`strtab_wait()` no longer sleeps for a fixed 1 ms. Contenders now do a short CPU
pause loop and yield through `lj_thr_yield(L)`, keeping the wait native and
handshake-visible without parking interners for millisecond-scale intervals.

This does not change the current string-table protocol: readers still publish a
TG-local active header/depth while traversing a table generation, and resizers
still claim `StrTabHdr.resize` before draining active readers. The change only
narrows the retry wait used when an interner races a resize claim, secondary
rehash, or active-generation drain.

`strtab_wait()` deliberately does not raise STOPREQ itself. Some callers reach
it after allocating an unpublished `GCstr`, after allocating a replacement
`StrTabHdr`, or while holding the old header's resize claim. A fresh STOPREQ
longjmp from that helper would leak the unpublished object or leave the claim
set. The cleanup-safe rule is: wait natively so GC2 handshakes can see and ack
the thread, then let the surrounding string operation reach its normal cleanup
or publication point before any interrupting error is raised.
