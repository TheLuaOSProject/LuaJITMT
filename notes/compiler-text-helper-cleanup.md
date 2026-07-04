Compiler-Text Helper Cleanup
============================

Context
-------

Historical context: the Lua test suite previously matched generated compiler
text. That coverage moved to runtime behavior, trace existence, C fixtures, and
code-adjacent comments.

Fix
---

Superseded: the shared generated-text helper module and repeated marker probes
were deleted.

Validation
----------

* `tools/ci/lua_test.sh m6_jit_xbar_xpoll m6_jit_table_store_helper m6_jit_gc2_readiness m6_jit_gcstep_pacing`
* `tools/ci/lua_test.sh m6_jit`
* `tools/ci/lua_test.sh m7_ffi_jit_cnew`
