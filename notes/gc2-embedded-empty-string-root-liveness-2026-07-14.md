# GC2 embedded empty-string root liveness (2026-07-14)

## Scope

This checkpoint fixes a deterministic GC2 grey-drain livelock triggered by a
Lua root containing the canonical empty string embedded in `global_State`.
It changes neither `plan/` nor the collector architecture: GC2 remains the
only collector, and the fix adds no lock, wait, or legacy-GC fallback.

## Symptom

The canonical table-resize `gcmark` stress case finished in about 0.14 seconds
when `LJ_M5_TAB_RESIZE_TRAVERSAL_MODES` was unset or non-empty, but consumed one
CPU indefinitely when the variable was exported as an empty string. Both JIT
and `-joff` reproduced the same behavior. A clean `c8026a47` worktree also
reproduced it, proving that it predated the HugeTab reader-handoff checkpoint.

The empty environment value is semantically significant: `os.getenv()` returns
Lua's canonical empty string, `&g->strempty`, and the stress script retains it
on the main stack.

## Root cause

`gc2_mark_thread_root_obj_worker_status()` routed the stack root through
`gc2_retain_candidate_status()`. That generic path required allocator/arena
membership. `strempty` is an immortal string object embedded directly in
`global_State`, so admission incorrectly returned `GC2_MARK_DEAD`.

Thread traversal interpreted the rejected strong stack root as a concurrent
root race, set `stack_retry`, and republished the main thread one-for-one. The
unbounded grey drain therefore repeatedly traversed the same thread and could
never empty. Debugger sampling observed 120 consecutive traversals of that
thread and the rejected object at the exact `global_State.strempty` address.

## Fix

Central candidate retention now recognizes the exact address of
`g->strempty` before allocator membership checks. It validates that the header
type is exactly `~LJ_TSTR` and still rejects an incompatible expected type.
For a valid root it reports `GC2_MARK_LIVE_ALREADY`.

This status is deliberate: the object is an immortal leaf, so it needs no
mark transition, grey publication, traversal, ownership handoff, or reclamation
coordination. Handling it in central retention covers ordinary, worker, table,
stack, and JIT-root callers instead of adding another call-site exception.

## Regression coverage

`tests/t-gc2-traverse.c` now verifies in an active major cycle that:

- generic status marking accepts `strempty` and reports its exact string type;
- expected string type succeeds and an expected function type fails;
- admission changes neither `marks_this_round` nor `grey_pushed`; and
- the cycle reaches idle normally.

The original canonical stress command with the empty variable now completes:

```sh
LJ_M5_TAB_RESIZE_STRESS_CASES=gcmark \
  LJ_M5_TAB_RESIZE_STRESS_TIMEOUT=10s \
  src/luajit tools/test.lua m5_tab_resize_stress
```

The exact direct `-joff` variant also completes under ten seconds, and the
helper-enabled C traversal fixture passes with strict compiler warnings.

Post-fix validation also passed:

- 50 consecutive optimized JIT runs and 50 consecutive `-joff` runs of the
  exact empty-environment reproducer;
- 20 JIT plus 20 `-joff` reproducer runs under target-only Clang ASAN;
- 20 JIT plus 20 `-joff` reproducer runs under target-only Clang UBSAN;
- `m6_jit_token`, including secondary-TG recording and explicit exits;
- `m6_jit_gc2_readiness`, including traced allocations during active MARK and
  SWEEP; and
- `m3_gc2_recovery` in its default and assertion/paranoia profiles.

The sanitizer runs used abort-on-first-error settings and produced no report.
An explicit clean default rebuild followed them to avoid the documented test
harness sanitizer-profile contamination issue.

## Remaining scope

This checkpoint covers the known embedded canonical empty string. Other
embedded/non-arena objects retain their existing explicit validation rules and
remain subject to the broader GC2 root-admission audit. It does not change the
temporary documented policy that GC2 ignores custom `lua_Alloc` callbacks.
