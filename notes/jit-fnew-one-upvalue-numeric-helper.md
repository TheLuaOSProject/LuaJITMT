# JIT one-upvalue numeric FNEW helper

Date: 2026-07-02

## Context

The `closures_upval` regression was dominated by traced closure creation. For the
common pattern:

```lua
for i = 1, n do
  local x = i
  local f = function()
    x = x + 1
    return x
  end
  s = s + f()
end
```

the recorder previously emitted a `lj_func_syncslot_forjit` call to copy the
numeric local into the interpreter frame, then called the generic
`lj_func_newL_gc_forjit` helper. This preserved semantics, but kept the hot loop
on the generic multi-upvalue path.

## Change

Added `lj_func_newL_gc1num_forjit`, used only when the callee prototype has a
single local cell upvalue and the captured value is numeric in the trace. The
helper still allocates a real `GCfunc` and `GCupval`, publishes ordinary GC
edges, initializes upvalue metadata, and promotes the stack slot to a cell for
mutable captures.

This is not closure sinking. Side exits continue to see ordinary heap objects.
Non-numeric captures, inherited upvalues, multiple upvalues, already promoted
cells, and generic/local-cell cases keep using the existing path.

## Invariant checks

- Same-trace calls from either FNEW helper are recognized by prototype, not by a
  single runtime closure identity. Since 2026-07-03, the same-trace helper case
  trusts the constant prototype argument directly and no longer emits a
  redundant `func.pc` reload/guard for the freshly allocated closure.
- The interpreter helper remains the only FNEW helper that performs
  `lj_gc_check_fixtop`; trace assembly continues to emit the allocation check
  before `CALLA`.
- `m6_jit_cell_ops` now asserts the specialized helper for numeric
  assigned-before-FNEW traces and absence of the old sync helper in that IR.
- The first `UGET` in an immediate one-upvalue numeric helper call forwards the
  numeric helper argument only before any child call boundary and before any
  earlier child `USTORE`. The same proof also removes the closed-upvalue guard
  from the immediate child `UREFC`: `lj_func_newL_gc1num_forjit()` always
  creates a fresh closed cell for this prototype. Mutable updates still
  emit a real heap `USTORE`, so side exits, escaped closures,
  `debug.setupvalue`, and later reads observe the ordinary heap upvalue cell.

## Local result

`BENCH_FILTER=closures_upval BENCH_SCALE=0.05 ./aux/bench/run.sh compare
/usr/bin/luajit src/luajit`:

- baseline stock: 42.18 ns/op
- current fork: 53.15 ns/op
- ratio: 1.26x

2026-07-03 follow-up sample after initial-UGET forwarding and fresh-closure
prototype reload removal:

- `BENCH_SCALE=0.05 src/luajit aux/bench/bench.lua closures_upval`: 72.01 ns/op
- `BENCH_SCALE=0.05 /usr/bin/luajit aux/bench/bench.lua closures_upval`: 43.86 ns/op

This slice reduces the traced IR/mcode shape (`mcode` 537 -> 492 bytes in the
local dump) but does not solve the larger allocation/GC-side closure gap.

2026-07-03 follow-up: the same helper proof now removes the dynamic
closed-upvalue guard on immediate child `UREFC`. The focused dump shrank again
from 492 to 465 bytes of mcode.

The same audit found a correctness bug in the original numeric helper: if the
trace stored closures from multiple loop iterations, `BASE[slot]` could still
hold the previous iteration's promoted cell and the helper reused it. That made
escaped closures alias incorrectly (`t[100]()` returned `4`-shape state instead
of `101`). `lj_func_newL_gc1num_forjit()` now always snapshots the numeric
argument into a fresh `GCupval` and republishes that cell to the parent slot for
mutable captures. `m6_jit_cell_ops` covers escaped closures, distinct
`debug.upvalueid()`, and `debug.setupvalue()` isolation.

The correctness fix restores required per-closure upvalue allocation cost:
same-session `BENCH_SCALE=0.05 ... closures_upval` measured 100.64 ns/op versus
stock 44.04 ns/op. The remaining gap is therefore real allocation/GC publication
work, not an optional guard.
