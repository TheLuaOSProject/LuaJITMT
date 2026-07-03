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
  single runtime closure identity.
- The interpreter helper remains the only FNEW helper that performs
  `lj_gc_check_fixtop`; trace assembly continues to emit the allocation check
  before `CALLA`.
- `m6_jit_cell_ops` now asserts the specialized helper for numeric
  assigned-before-FNEW traces and absence of the old sync helper in that IR.

## Local result

`BENCH_FILTER=closures_upval BENCH_SCALE=0.05 ./aux/bench/run.sh compare
/usr/bin/luajit src/luajit`:

- baseline stock: 42.18 ns/op
- current fork: 53.15 ns/op
- ratio: 1.26x
