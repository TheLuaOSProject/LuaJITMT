# X64 Closed-Upvalue Store Helper For All TValue Types

2026-07-04 follow-up: primitive closed-upvalue stores, and exact DUALNUM integer
stores if the recorder emits them, now use the same one-word raw TValue publish
shape as numeric stores. GC-valued stores still use the helper before
`lj_gc_pubuv`.

2026-07-02 follow-up: numeric closed-upvalue stores no longer use this
all-helper bridge. They lower to one aligned 64-bit x64 store, which preserves
the no-tear slot invariant without paying a C call. At that point,
primitive/integer stores still used the helper until they had a dedicated
single-word TValue encoding path, and GC-valued stores still used the helper
before `lj_gc_pubuv`.

## Summary

This note originally documented routing every Linux/x64 `IR_USTORE` into
`IR_UREFC` through `lj_func_storeuv_forjit()`, not only GC-valued stores. The
helper performs a release copy of the full `TValue`, which remains the right
publication primitive for GC-valued stores before `lj_gc_pubuv`.

Primitive values and exact DUALNUM integers do not create GC-object edges and
fit in a single raw `TValue` word on x64. The backend now materializes that raw
word and stores it directly into the closed cell, avoiding the helper call while
preserving the no-tear slot invariant. Numeric stores already used the same
single-word property through `MOVSD`.

## Why

The previous x64 backend gate only used the helper for GC-valued stores:

```c
IR_USTORE && irt_isgcv(ir->t) && IR(ir->op1)->o == IR_UREFC
```

That left numeric and primitive closed-upvalue stores on raw backend paths:

- numeric stores used a direct `MOVSDto`;
- primitive/constants could materialize raw TValue words;
- GC64 split stores could update payload/tag words directly.

Those paths were fast, but the split payload/tag stores could expose torn
`TValue` states. The safer semantic rule is: once a store targets a closed
upvalue cell, publish the complete TValue as one aligned slot update. Non-GC
values can do that directly in generated code. GC-valued stores keep the helper
so the release copy is still ordered before the GC edge publication. `IR_OBAR`
remains GC-only because only GC values need the `lj_gc_pubuv` barrier.

## Related Debug API Follow-Up

`lua_getlocal()` now acquire-loads unwrapped closed local-cell values, matching
`lua_getupvalue()`. Ordinary owner-claimed raw stack locals still use `copyTV`.

## Coverage

- `m5_cell_ops` documents the closed-upvalue helper condition and the
  `lua_getlocal()` acquire load through comments near the implementation.
- `m5_cell_ops` now has a traced primitive closed-local-cell behavior probe.
- `m6_jit_cell_ops` now checks traced numeric and primitive closed-upvalue store
  behavior. It deliberately does not parse `-jdump` output or assert helper
  names; the helper/direct-store split is an implementation invariant documented
  here and in code comments, while the suite covers Lua-visible values and trace
  viability.
- `m6_jit_barrier_xpoll` remains the ordering guard for GC-valued stores:
  helper call first, then `lj_gc_pubuv`.

## Validation

2026-07-04:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m6_jit_cell_ops m6_jit_barrier_xpoll m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh run_stock_tests -- lang/upvalue/closure.lua`
- `tools/ci/lua_test.sh run_stock_tests -- opt/fwd/upval.lua`
- `cd tests/stock/test && LUA_PATH='../../../tests/lib/?.lua;../../../src/?.lua;../../../src/jit/?.lua;;' ../../../src/luajit misc/wbarrier_obar.lua`
- `git diff --check`

Original helper-path validation:

- `make -C src clean && make -C src -j2`
- `tools/ci/lua_test.sh m5_cell_ops`
- `tools/ci/lua_test.sh m6_jit_cell_ops`
- `tools/ci/lua_test.sh m6_jit_barrier_xpoll`
- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh m5_concurrent_objects`
- `git diff --check`
