# gcnext fixture cleanup

## 2026-06-20

- Replaced the last non-macro `gcnext()` traversal in
  `tests/t-gc2-alloc-account.c` with:
  - `gcref_acq(g->gc.root)` for the root head
  - `lj_obj_gcw_acq(o)` for `nextgc` traversal
- Shared root/next traversal uses acquire helpers so fixture code does not
  normalize raw shared-link reads. The rule matters even in tests because these
  fixtures become examples for production collectors and allocation accounting.

## Validation target

- Use `tools/ci/lua_test.sh m6_jit_alloc_account`.
