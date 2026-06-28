# X64 Closed-Upvalue Store Helper For All TValue Types

## Summary

Linux/x64 JIT lowering now routes every `IR_USTORE` into `IR_UREFC` through
`lj_func_storeuv_forjit()`, not only GC-valued stores. The helper performs a
release copy of the full `TValue`, which is the right publication primitive for
closed upvalue/local-cell storage.

## Why

The previous x64 backend gate only used the helper for GC-valued stores:

```c
IR_USTORE && irt_isgcv(ir->t) && IR(ir->op1)->o == IR_UREFC
```

That left numeric and primitive closed-upvalue stores on raw backend paths:

- numeric stores used a direct `MOVSDto`;
- primitive/constants could materialize raw TValue words;
- GC64 split stores could update payload/tag words directly.

Those paths were fast, but they bypassed the release-copy store used by the VM
and by GC-valued JIT stores. The safer semantic rule is: once a store targets a
closed upvalue cell, publish the complete TValue through the helper regardless
of value type. `IR_OBAR` remains GC-only because only GC values need the
`lj_gc_pubuv` barrier.

## Related Debug API Follow-Up

`lua_getlocal()` now acquire-loads unwrapped closed local-cell values, matching
`lua_getupvalue()`. Ordinary owner-claimed raw stack locals still use `copyTV`.

## Guards

- `m5_cell_ops` source-guards the broadened x64 helper condition and the
  `lua_getlocal()` acquire load.
- `m5_cell_ops` now has a traced primitive closed-local-cell behavior probe.
- `m6_jit_cell_ops` now checks mcode dumps for `->lj_func_storeuv_forjit` on
  numeric and primitive closed-upvalue stores.
- `m6_jit_barrier_xpoll` remains the ordering guard for GC-valued stores:
  helper call first, then `lj_gc_pubuv`.

## Validation

- `make -C src clean && make -C src -j2`
- `tools/ci/lua_test.sh m5_cell_ops`
- `tools/ci/lua_test.sh m6_jit_cell_ops`
- `tools/ci/lua_test.sh m6_jit_barrier_xpoll`
- `tools/ci/lua_test.sh m5_concurrent_objects`
- `git diff --check`
