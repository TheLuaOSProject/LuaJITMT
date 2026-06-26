## GC2 handshake epoch retire helpers

Retired strings, table nodes/arrays, trace vectors/traces, scoped trace flushes,
ctype retired tables, and mcode retirement all stamp objects with the current
GC2 handshake epoch. This slice routes those production loads through
`gc2_hs_epoch_acq()`.

The safepoint handshake guard now checks all production C files for raw GC2
handshake-state field access, so future retire paths must use the `gc2_hs_*()`
helper family instead of loading `g->gc2.hs_epoch` directly.
