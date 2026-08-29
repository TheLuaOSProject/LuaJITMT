# Active-MT local-cell FORL roots

Secondary TGs were rejecting root numeric-loop traces for protos with local-cell
bytecode after MT activation. The broad `BC_FORL` NYI avoided earlier snapshot
shape risk, but it also prevented valid worker traces for local-cell loops:

- `m6_jit_token` could not build the expected worker numeric-loop trace.
- `m7_ffi_jit_cnew` recorded zero worker traces for `ffi.new()` loops.
- `t-gc2-traverse` lost the table-store-helper barrier trace after a prior
  threading spawn had activated MT mode.

`rec_for_loop()` already initializes the hidden numeric-loop slots and the
`CGET`-visible `FORL_EXT` slot before recording the loop body. Side traces that
start inside the loop can recover the narrowed visible loop value through
`rec_for_ext_cget()`. Keeping the root trace interpreted is therefore too broad;
it blocks ordinary Lua/JIT/FFI semantics in secondary TGs.

The remaining unsafe/pathological shape is side-trace chaining from another
side trace under an active-MT local-cell root. Those traces replay a snapshot
that has already replayed local-cell state, and table/FFI helper exits can then
form long helper/result-shape churn chains. Hot-side admission now leaves those
side-of-side exits interpreted while still allowing the root and first-level
side traces. This preserves the useful hot path without consuming trace slots
on repeated local-cell replay shapes.

Focused verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `src/luajit tools/test.lua m6_jit_token m7_ffi_jit_cnew`
- `LJ_M6_JIT_SECONDARY_TRACE_LIMIT=128 LUA_PATH='./tests/lib/?.lua;./src/?.lua;./src/jit/?.lua;;' timeout 20s ./src/luajit ./tests/t-jit-secondary.lua`

`tests/t-jit-secondary.lua` now includes a secondary-TG local-cell numeric
`for` loop that must record a root trace and a first-level side trace while an
escaped closure still observes the final visible loop value. That keeps the
coverage attached to the active-MT FORL/local-cell snapshot shape instead of
only proving ordinary worker numeric loops.

Broader GC scaffold progressed past the previous `t-gc2-traverse` trace absence
and exposed a later `test_async_weak` assertion in `t-gc2-worker-scheduler`;
that weak-table scheduler failure is tracked separately.
