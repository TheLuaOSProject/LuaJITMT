2026-06-20

- Extracted M5/M6 local-cell probe bodies into `tests/lib/local_cell_probes.lua`.
- Added `tests/lib/suite_cell_ops.lua` as the suite-facing runner/assertion helper.
- M5 now delegates bytecode/result behavior checks to the helper and keeps stock tests in the case.
- M6 now delegates local-cell JIT dump matching and runtime guards to the helper.
- Focus validation: `tools/ci/lua_test.sh m5_cell_ops m6_jit_cell_ops`.
