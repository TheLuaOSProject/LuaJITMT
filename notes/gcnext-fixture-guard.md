# gcnext fixture cleanup

## 2026-06-20

- Replaced the last non-macro `gcnext()` traversal in
  `tests/t-gc2-alloc-account.c` with:
  - `gcref_acq(g->gc.root)` for the root head
  - `lj_obj_gcw_acq(o)` for `nextgc` traversal
- Added a guard to `tools/ci/m6_jit_alloc_account.sh` that rejects `gcnext()`
  outside the macro definition in `src/lj_obj.h`.

## Validation target

- `tools/ci/m6_jit_alloc_account.sh`
