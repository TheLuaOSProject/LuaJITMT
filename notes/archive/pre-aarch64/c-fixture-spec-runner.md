C fixture spec runner
=====================

Context
-------

Several Lua suite cases compile and run multiple C fixtures after one build.
The repeated `compile_and_run_c(t, t:tmp(...), ...)` calls obscure the behavior
the cases are actually covering.

Fix
---

`suite_build.run_c_fixture_specs()` accepts fixture specs with output names,
C files, and per-fixture options, merged with common options. The M5 JIT trace
publication and M7 FFI callback runtime cases now use it for their C fixture
batches.

Validation
----------

* `tools/ci/lua_test.sh m5_jit_trace_publish m7_ffi_callback_runtime`
