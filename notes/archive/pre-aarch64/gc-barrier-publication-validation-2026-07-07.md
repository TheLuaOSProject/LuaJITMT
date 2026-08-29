# GC publication/barrier validation

This slice extends the stale-root preflight discipline from root scanning into
publication and barrier entry points:

- `lj_gc_pubobjroot()` now validates the object/allocation before deriving a
  synthetic TValue tag from `gct`.
- Legacy inline TValue barriers call the GC root validator before `tviswhite()`
  or `gcV()` can read an object header.
- GC2 TValue barriers skip stale tagged snapshots before dirtying parent tables,
  marking children, or publishing remembered pairs.
- FINREG cdata guards accept fixed-size raw cdata with live-memory/header proof
  even before CTState exists, while variable/aligned cdata still uses
  `lj_cdata_validate()` to prove the allocation base.

`tests/t-gc2-alloc-account.c` now feeds a canonical non-object pointer through
the public root and active-mark barrier helpers to keep these paths no-op
instead of header-reading stale memory.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `git diff --check`
- `tools/ci/lua_test.sh m3_safepoint_handshake`
- `tools/ci/lua_test.sh m6_jit_table_store_helper`
- `tools/ci/lua_test.sh m7_ffi_finreg`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m9_m10_gc`
