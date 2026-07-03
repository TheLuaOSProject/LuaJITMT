# GC2 TG registry helper slice

This slice routes the global GC2 thread-group registry through helper
accessors:

- `gc2_tg_list_acq()` reads the lock-free TG list root with acquire ordering.
- `gc2_tg_list_store_rlx()` initializes the registry root.
- `gc2_tg_list_cas()` wraps CAS-prepend and physical unlink CAS operations.
- `gc2_n_threads_*()` initializes, reads, increments, and decrements the live
  registered-TG count.

Runtime users in `lj_tg.c`, `lj_gc2.c`, `lj_gc.c`, `lj_safepoint.c`,
`lj_dispatch.c`, and `lib_threading.c` now call helper accessors instead of
spelling ad hoc atomics against `GC2State.tg_list` or `GC2State.n_threads`.
Per-node `TGState.next_tg` remains covered by `lj_tg_next_*()` helpers.

Production access to the global registry fields must stay behind the helper
surface, with helper bodies as the single raw-access point. The safepoint,
threading, and VM safepoint cases own the observable behavior instead of
source-text matching.

Validation:

- `tools/ci/lua_test.sh m3_safepoint_handshake`
- `tools/ci/lua_test.sh m4_threading_api`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m3_vm_safepoint`
- `git diff --check`
