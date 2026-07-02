FFI Runtime Retry Yield
=======================

Runtime FFI publication waits no longer park for a fixed 1 ms:

- cdata FINREG slot claims;
- FINREG generation claims in `lj_ctype.c`;
- callback blacklist duplicate-publication claims;
- CLibrary cache entry publication claims.

These are short CAS publication windows with either a current Lua state or a
safe NULL fallback. They now use `lj_thr_retry_yield()`, which does a short CPU
pause loop and then yields as native time through `lj_thr_yield()`.

The ctype parser/cdef token wait is intentionally unchanged in this slice. It
serializes parser/CTState mutation and is part of the cdef carve-out rather than
the runtime FFI paths targeted here.
