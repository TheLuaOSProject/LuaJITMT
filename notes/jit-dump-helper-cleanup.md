JIT dump helper cleanup
=======================

Context
-------

Historical context: the Lua test suite previously matched generated
JIT/bytecode dump text. Those checks have since been removed from active tests
because they pinned implementation spelling instead of behavior.

Fix
---

Superseded: the shared JIT dump helper module and the repeated dump marker
probes were deleted. The same invariants now use runtime behavior, trace
existence, C fixtures, and code-adjacent comments.

Validation
----------

* `tools/ci/lua_test.sh m6_jit_xbar_xpoll m6_jit_table_store_helper m6_jit_gc2_readiness m6_jit_gcstep_guard`
* `tools/ci/lua_test.sh m6_jit`
* `tools/ci/lua_test.sh m7_ffi_jit_cnew`
