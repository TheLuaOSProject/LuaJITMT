JIT token secondary TG fixture
==============================

Context
-------

After `lj_thr_get_tg_fallback(g)` started rejecting TLS thread groups whose
`tg->gl` belongs to another `global_State`, `tests/t-jit-token.c` exposed a
stale test shortcut: it used a zeroed synthetic `TGState` and only filled in
`tid`. That no longer models a valid secondary thread group for the VM.

Fix
---

The fixture now initializes the synthetic secondary TG with
`lj_tg_init_thread(g, &secondary, NULL, 0)`, sets its test `tid` and allocator
owner id, verifies `G2TG(g)` sees it while TLS points at it, and finishes it
with `lj_tg_fini_thread()` after restoring TLS.

Validation
----------

* `tools/ci/lua_test.sh m6_jit_token`
