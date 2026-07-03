## GC2 handshake epoch retire helpers

Retired strings, table nodes/arrays, trace vectors/traces, scoped trace flushes,
ctype retired tables, CLibrary cache entries, and mcode retirement all stamp
objects with the current GC2 handshake epoch. The retire producers now query
that epoch through `lj_gc2_retire_epoch()` instead of reading the raw handshake
epoch helper directly.

The retire-epoch ownership rule is documented here and beside
`lj_gc2_retire_epoch()`: SMR retire producers must use GC2's public retire API
instead of reading `g->gc2.hs_epoch` or calling the low-level handshake helper
directly. Safepoint and retire behavior remains covered by the GC2/JIT/FFI
fixture cases rather than source-text matching.
