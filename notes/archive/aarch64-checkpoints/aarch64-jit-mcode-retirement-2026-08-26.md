# macOS ARM64 JIT mcode retirement proof (2026-08-26)

## Status and scope

The existing `t-jit-mcode-retire` lifecycle fixture now runs under the current
experimental macOS ARM64 JIT without widening ARM64 trace admission. The x64
workload and assertions are unchanged. ARM64 selects a separate workload only
when all of the first-loop gates have their proved values:

- root recording and `BC_LOOP` native entry are open;
- side and stitch recording remain closed;
- `JFUNCF` and stitch native entry remain closed;
- the build has the explicit ARM64 experimental opt-in and JIT support.

This is a structural retirement proof for the one admitted spill-free integer
root family. It is not evidence for side traces, function-entry traces, stitch
traces, FFI traces, or any other ARM64 IR shape.

## Exact ARM64 trace topology

The ARM64 branch defines two independent functions with the same admitted
body:

```lua
local i,x=0,0
while i<n do i=i+1 x=x+i end
return x
```

Both run with `n = 20` and must return `210`. A C-side driver calls each
function 40 times, so no Lua numeric `for` loop can record an unsupported
`FORL` trace and accidentally provide the retirement evidence.

The first trace becomes the native pin target. The identical peer stays
unpinned. This two-root topology is necessary to preserve the full pre-existing
fixture contract:

1. before trace retirement drains, the unpinned peer makes the mature mcode
   reference classification ordinary and same-epoch retryable;
2. after the peer is reclaimed, the pinned root is the only remaining area
   reference and the stable pinned-only scan may be memoized;
3. final native unpin changes the release sequence and permits both the exact
   trace body and mcode area to be reclaimed in that same explicit epoch.

The fixture obtains each function's exact `GCproto` and requires its published
trace to be runnable, root-owned, linked to that same prototype, backed by
mcode, and marked `TRACE_ARM64_INT_LOOP_ADMITTED`. The area scan must pin the
first exact body, while the peer must remain unpinned.

## Lifecycle assertions retained

The ARM64 execution retains the existing no-throw and lifetime assertions:

- an mcode allocation has a preowned active retirement sidecar;
- a JIT-root mark preserves that sidecar through the forced activation
  collision;
- `lj_trace_flushall_gc()` allocates nothing while allocator growth is denied;
- flush transfers the active area and trace bodies to retired ownership;
- young completed epochs do not reclaim the area and stable scans are
  memoized;
- a mature ordinary trace reference keeps the scan retryable;
- mature trace reclamation removes the unpinned peer but retains the pinned
  body and its mcode;
- the final unpin allows trace and mcode reclamation without another epoch
  increment;
- the retired mcode list is empty and `szallmcarea` returns to zero.

## Native contract

`tools/ci/arm64_jit_mcode_retire_contract.sh`:

- serializes source rebuilds with `src/.lj-test-run.lock`;
- failure-safely restores an ordinary thin `arm64` experimental build;
- builds with `LUAJIT_MT_ARM64_BOOTSTRAP`,
  `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`, `LUA_USE_ASSERT`,
  `LJ_TRACE_TEST_HELPERS`, and `LUAJIT_MCODE_TEST`;
- checks the resulting archive is thin ARM64 and verifies all granular gate
  macros from the production preprocessor view;
- source-checks the exact `while` roots, rejects a Lua `FORL` fallback, and
  pins the runnable/admission/prototype assertions;
- compiles `tests/t-jit-mcode-retire.c` separately with warnings as errors;
- runs the complete fixture.

Validated on native Apple ARM64:

```text
t-jit-mcode-retire OK: no-throw mcode flush retires by epoch
arm64_jit_mcode_retire_contract OK: admitted root pin held trace and mcode through explicit epoch reclaim
```

The same fixture translation unit also compiled for macOS x86_64 with
`-Wall -Wextra -Werror`, confirming that the unchanged x64 workload excludes
the ARM64-only helpers cleanly.

## Important boundary

The fixture advances `completed_epoch` explicitly through white-box reclaim
helpers. It proves the finite-epoch retirement logic and the pin-release
notification, but it does not run two real live safepoint-handshake generations.
It also does not exercise concurrent `FLUSHJ`, trace-number reuse, stale
bytecode entry, or replacement races. Those live grace and reuse cases remain
separate follow-up work.
