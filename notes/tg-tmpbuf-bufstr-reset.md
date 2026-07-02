# TG tmpbuf BUFSTR append state

Traced x86/x64 buffer concatenation uses the running `TGState.tmpbuf` through
`IR_BUFHDR`/`IR_BUFPUT`/`IR_BUFSTR`. `lj_buf_tostr_tg()` must match stock
`lj_buf_tostr()` and leave the write pointer unchanged after materializing the
interned string. Loop traces can emit `BUFSTR` on every iteration and then keep
appending to the same `BUFHDR ... APPEND` chain; resetting in `BUFSTR` drops
the accumulated prefix and returns only the last segment.

`IR_BUFHDR RESET` remains the point that starts each traced concat chain from an
empty buffer. The C guard checks both sides: a traced materialized concat leaves
the final append length visible, and the next traced concat starts from a fresh
reset rather than appending to the prior chain.

This is a correctness/invariant guard, not the cause of the large
`tab_hash_write` full-GC cliff. The cliff was caused by GC2 table-barrier
requeueing while active marking was enabled; see `notes/gc2-tbar-black-gate.md`.

The guard is `tools/ci/lua_test.sh m6_jit_tmpbuf_thread_format`, which includes
`tests/t-jit-tg-tmpbuf-reset.c`.
