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

## Coverage Model

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

2026-07-03 follow-up: the helper now initializes the fresh `GCfunc` and its
fresh closed `GCupval` before publishing them as one root-pending chain. This
does not change closure or upvalue identity and does not remove the required
heap `USTORE`; it only avoids publishing the pair as two independent new root
heads. Focused same-session `BENCH_SCALE=0.05` samples still stayed around
100 ns/op versus stock around 40 ns/op, so the remaining gap is still the
expected allocation/cell cost until a future snapshot-aware closure-sinking or
batch allocation design exists.

2026-07-03 follow-up: the JIT numeric helper now has a strict bump-pair fast
path for the fresh `GCfunc` plus fresh closed `GCupval`. It runs only for the
main arena-internal TG, before MT activation, with no GC2 workers, no custom
allocator, enough local accounting headroom, no traversable free-run bin that
could satisfy either object, and enough room in the current traversable bump
run. Otherwise it falls back to the existing generic helper path.

The fast path preserves stock Lua closure semantics: every `FNEW` still creates
a distinct closure and distinct upvalue cell, mutable captures republish the
fresh cell into the parent slot, and `debug.upvalueid()`/`debug.setupvalue()`
observe ordinary per-closure identity. It only avoids the helper allocator
round trips and publishes the initialized pair as one pending-root chain under
the same single-thread predicates as the empty-table inline bump path.

Focused coverage: `m6_jit_fnew_bump` exercises the traced escaped-closure fast
path, distinct upvalue identities, debug mutation isolation, and deterministic
accounting fallback. Repeated same-session stock comparisons put
`closures_upval` around 2.30x stock, down from the pre-slice 2.66x sample but
not at the earlier unstable 1.47x best sample; the other M9 comparison rows
remain under threshold.

2026-07-03 x64 JIT inline follow-up: `IRCALL_lj_func_newL_gc1num_forjit` now
lowers to a bounded x64 inline bump-pair path in `lj_asm_x86.h` when the
recorder shape is the strict one-upvalue numeric local-cell case. Every
predicate miss jumps to the existing C helper fallback. A focused `-jdump=im`
still shows the `CALLA` marker in IR, but the loop mcode's straight-line path
allocates and initializes the fresh `GCfunc`/`GCupval`, performs the heap slot
store, publishes the pending-root chain, and jumps over the helper call; the
helper call remains only in the fallback block.

The inline path is intentionally narrower than the C helper: it requires
`mark_active == 0`. If active marking or generational remembering is visible,
the trace jumps to the C helper so the existing GC2 and legacy barriers own
`fn -> pt/env/uv` and `uv -> tv` publication. Sweep-time black allocation does
not imply active barriers, so the inline path handles it by setting the arena
mark bits while still fully initializing fields and edges before the TG
pending-root release publication. The focused C fixture covers the fallback
boundary by forcing mark-active state around a traced numeric FNEW loop and
checking that the C helper path is used while closure/upvalue identity remains
ordinary Lua identity. It also forces `alloc_black` without active marking and
checks that traced allocation stays inline, protecting the sweep-time mark-bit
case.

Focused validation after the inline follow-up:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m6_jit_fnew_bump m6_jit_cell_ops m3_gc_root_pending`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `LJ_BENCH_STOCK_FILTERS=closures_upval LJ_BENCH_STOCK_SCALE=0.02 tools/ci/lua_test.sh m9_bench_stock_compare`

The final stock comparison guard reported `closures_upval` geomean `2.116886`.

2026-07-03 follow-up: generic closed-upvalue construction and the one-upvalue
bump helper now skip the upvalue-payload publication helper when the copied
payload is non-GC. This does not change Lua closure or upvalue identity and does
not remove any collector edge: numeric captures such as `closures_upval` have no
`uv->tv` child object, while table/string/cdata/thread/function captures still
run the GC2 and legacy publication barrier before the upvalue is linked into
the pending root chain.

The C bump helper now also skips fresh-closure object-edge publication wrappers
for `fn -> proto`, `fn -> env`, and `fn -> uv` while `TG.mark_active` is clear.
The helper initializes the closure and upvalue before pending-root publication,
so an inactive collector observes the complete edge set when it later traverses
that new root. With active marking or generational remembering, the helper
falls back to the normal GC2/legacy publication behavior for each edge.

Focused validation after this C-helper edge cleanup:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m6_jit_fnew_bump`
- `tools/ci/lua_test.sh m5_upvalue_publish_gc m6_jit_cell_ops m3_gc_root_pending`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `LJ_BENCH_STOCK_FILTERS=closures_upval LJ_BENCH_STOCK_SCALE=0.02
  tools/ci/lua_test.sh m9_bench_stock_compare`

The focused stock comparison reported `closures_upval` geomean `1.901021`.
