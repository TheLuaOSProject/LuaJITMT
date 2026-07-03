# gcnext fixture cleanup

## 2026-06-20

- Replaced the last non-macro `gcnext()` traversal in
  `tests/t-gc2-alloc-account.c` with:
  - `gcref_acq(g->gc.root)` for the root head
  - `lj_obj_gcw_acq(o)` for `nextgc` traversal
- Historical state: `tools/ci/m6_jit_alloc_account.sh` once rejected
  `gcnext()` outside the macro definition in `src/lj_obj.h`. That implementation-text assertion
  check has been removed; the useful rule is that shared root/next traversal
  should use acquire helpers so fixture code does not normalize raw shared-link
  reads.

## Validation target

- Use `tools/ci/lua_test.sh m6_jit_alloc_account`.
