# GC2 TBAR black gate

Traced numeric table stores can emit `TBAR` for string-key stores even when the
stored value is not collectable. On x64 the inline legacy half of `TBAR` already
uses the table black bit as the once-per-cycle gate: the first barrier clears
black and queues the table on `grayagain`; later barriers skip until traversal
makes it black again.

The GC2 half did not share that gate. While GC2 MARK was active, every traced
`TBAR` called `lj_gc2_barrier_tab_g()` and requeued the already-marked table
into SSB. A `t["k"..i] = i` loop therefore left mark completion with one grey
table revisit per iteration. Locally, `tab_hash_write` timed out at 60s before
printing, and a 200k-iteration probe drained about 199k grey entries in the
next explicit GC step.

The x86/x64 JIT `TBAR` emission now uses the same entry black-bit gate for its
GC2 helper call. The generic C GC2 table barrier remains conservative, because
GC2 can mark a table before the legacy color bits catch up. Once a traced `TBAR`
has cleared black and linked legacy `grayagain`, later traced barriers skip the
duplicate GC2 helper until traversal makes the table black again. The same 200k
probe now converts thousands of SSB entries instead of one per iteration, and
`aux/bench/bench.lua tab_hash_write` completes at about 22 ns/op locally.

The regression guard is `tools/ci/lua_test.sh m6_jit_tbar_gc2_black_gate`.
