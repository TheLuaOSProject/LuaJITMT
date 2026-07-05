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
package loader paths, `jit.profile.stop`, FFI string/copy/fill helpers,
VM-event failure reporting, JIT-token waits, the FFI CLibrary loader,
`debug.debug` native console I/O, FFI native-call helpers, profiling stop, and
the CLI frontend, I/O library native stdio wrappers, channel parks, and FFI
callback native boundaries.

Site-specific wrappers were intentionally left in place when they carry behavior
beyond the common helper, such as extra pending-poll handling or additional
cleanup before throwing a fresh STOPREQ.

## Table Retire Helpers

`lj_tab` node and array retirement records share the same Treiber-list metadata:
retired head, next link, retire epoch, and armed bit. The typed accessors and
push loops now use local macro generators for those identical parts, while the
payload fields, free checks, and reclaim loops stay explicit for node and array
ownership semantics.
