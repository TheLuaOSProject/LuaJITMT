## GC2 handshake epoch retire helpers

Retired strings, table nodes/arrays, trace vectors/traces, scoped trace flushes,
ctype retired tables, CLibrary cache entries, and mcode retirement all stamp
objects with the current GC2 handshake epoch. The retire producers now query
that epoch through `lj_gc2_retire_epoch()` instead of reading the raw handshake
epoch helper directly.

The safepoint handshake coverage now checks all production C files for raw GC2
handshake-state field access, so future retire paths must use the `gc2_hs_*()`
helper family instead of loading `g->gc2.hs_epoch` directly.
It also rejects direct `gc2_hs_epoch_acq()` calls in the SMR retire producers,
keeping epoch ownership behind GC2's public retire API.
