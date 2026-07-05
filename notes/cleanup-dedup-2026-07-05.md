# Cleanup Dedup Pass - 2026-07-05

This pass kept behavior unchanged and removed repeated fork-local scaffolding.

## Closure Bump Allocation

`lj_func.c` had the same allocator-readiness predicate in the closed-upvalue,
one-upvalue closure, and no-upvalue closure bump paths. It now uses a single
local `func_bump_alloc_ready()` helper. The helper documents why these fast
paths are only valid for the main-TG, arena-backed, no-MT/no-worker window.

The FNEW bump fixture now covers the predicate directly:

- `mt_entering` blocks the one-upvalue, upvalue-cell, and no-upvalue bump paths.
- registered GC2 workers block one-upvalue bump allocation.
- disabling the arena allocator shim blocks one-upvalue bump allocation.

## Safepoint Wrappers

Several native-call sites repeated identical `had_stopreq`/fresh-stop wrappers.
`lj_safepoint_had_stopreq()` now lives next to the existing fresh-stop helpers,
and exact duplicate local wrappers were removed from base `print`, `loadfile`,
package loader paths, `jit.profile.stop`, and FFI string/copy/fill helpers.

Nullable or site-specific wrappers were intentionally left in place when they
carry behavior beyond the common helper, such as extra pending-poll handling or
`L == NULL` tolerance.
