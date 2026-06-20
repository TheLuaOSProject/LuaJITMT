JIT dump helper cleanup
=======================

Context
-------

The Lua test suite intentionally matches generated JIT/bytecode dumps as
behavioral output. The repeated mechanics around temp dump paths, default
`-jdump=ir` invocation, timeout, and marker assertions belong in shared suite
helpers rather than individual case bodies.

Fix
---

`tests/lib/suite_jit.lua` now returns the dump path from `run_ir_dump_probe()`
and exposes helpers for one-shot dump probes with single or multiple expected
markers. `tests/suites/m6_jit.lua` uses those helpers for the repeated XBAR,
table-store, GC2 readiness, and GCSTEP dump probes.

Validation
----------

* `tools/ci/lua_test.sh m6_jit_xbar_xpoll m6_jit_table_store_helper m6_jit_gc2_readiness m6_jit_gcstep_guard`
* `tools/ci/lua_test.sh m6_jit`
* `tools/ci/lua_test.sh m7_ffi_jit_cnew`
