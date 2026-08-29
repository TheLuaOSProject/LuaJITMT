# FNEW pre-MT tracing policy audit

Date: 2026-07-03

The remaining `closures_upval` gap is not just GC pacing. An isolated loop that
creates a closure over one numeric local and immediately calls it measured about
70 ns/op with GC enabled and 47 ns/op with GC stopped on the fork, versus about
39 ns/op and 18 ns/op on stock LuaJIT in the same container.

`-jdump=im` shows why the shapes differ:

- Stock LuaJIT aborts the outer trace on `BC_FNEW` and traces the inner closure
  body (`UREFO`/`ULOAD`/`USTORE`) separately.
- The fork traces the outer loop and emits
  `CALLA lj_func_newL_gc1num_forjit` for each closure allocation. Since the
  2026-07-03 recorder slice, the immediate numeric `UGET` forwards the helper
  argument and the same-trace fresh closure no longer reloads `func.pc`, but
  mutable `USET` still writes the real heap upvalue cell.

A tempting performance policy is to make pre-MT `FNEW` stock-like and only
record helper-backed `FNEW` once `mt_entering || mt_active` is visible. That is
not a small mechanical cleanup in the current tree: `m6_jit_cell_ops` explicitly
asserts source and loaded local-cell `CNEW/FNEW` creation traces before MT
activation, including the one-upvalue numeric helper. Changing the policy would
need a matching test rewrite that proves active-MT FNEW creation still traces
and that pre-MT fallback remains stock-semantics compatible.

Current conclusion: keep the helper-backed FNEW tracing policy. Small
same-trace proof reductions are acceptable when they preserve heap closure and
upvalue semantics, but do not make pre-MT `FNEW` stock-like as a benchmark
shortcut unless the active-MT tracing coverage and compatibility policy are
changed deliberately.
