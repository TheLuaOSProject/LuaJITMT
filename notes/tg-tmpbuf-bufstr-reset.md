# TG tmpbuf BUFSTR reset

Traced x86/x64 buffer concatenation uses the running `TGState.tmpbuf` through
`IR_BUFHDR`/`IR_BUFPUT`/`IR_BUFSTR`. `lj_buf_tostr_tg()` now resets the TG
tmpbuf write pointer after materializing the interned string, matching the
expected buffer-finalization invariant for repeated traced concatenations.

This is a correctness/invariant guard, not the cause of the large
`tab_hash_write` full-GC cliff. The cliff was caused by GC2 table-barrier
requeueing while active marking was enabled; see `notes/gc2-tbar-black-gate.md`.

The guard is `tools/ci/lua_test.sh m6_jit_tmpbuf_thread_format`, which includes
`tests/t-jit-tg-tmpbuf-reset.c`.
