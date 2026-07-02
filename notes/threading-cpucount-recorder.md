# Threading cpucount recorder

`threading.cpucount()` now has a dedicated JIT recorder. It emits a runtime
`lj_thr_cpucount()` helper call instead of stitching through the generic NYI
fast-function boundary.

The helper is recorded as `CALLS`, not as a pure `CALLN`, so the query remains
at the Lua call site inside hot loops. This avoids baking in or hoisting an OS
CPU-count query while still removing the trace-stitch boundary.

Focused guard:

- `tools/ci/lua_test.sh m6_jit_threading_nyi_boundary`
