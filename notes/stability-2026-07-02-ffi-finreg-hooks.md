# Stability Fixes: FFI FINREG, Native Waits, Hooks

Date: 2026-07-02

This pass fixed several stability failures found while stress-running
`tests/t-ffi-gc-finreg.lua`, M4 hook tests, and M8 weak/finalizer fixtures.

## Stack/base preservation

- Safepoint acknowledgements now preserve `L->base` and `L->top` when called
  from C/VM helper paths.
- Channel native waits use a small saved-frame wrapper so blocking futex waits
  cannot return with a stack frame rewritten by a safepoint.
- Channel send stores stack `TValue *` arguments as stack offsets while it may
  block, then restores the pointer after stack growth or relocation.
- Thread spawn copies caller arguments and start roots from saved stack offsets,
  not raw caller stack pointers. Protected stack-copy calls and root-vector
  allocation can both move the parent stack.
- `ffi.gc()` and GC2 finalizer dispatch preserve `base`/`top` around paths that
  may allocate, grow the stack, or run protected calls.
- Worker entry refreshes `L->glref` from the worker `TGState`; protected C
  calls also repair a missing `glref` from `tg_hint` before entering assembly.

The original failure signatures were crashes returning from channel receive and
`ffi.gc()` with stale or null `L->base` values. Later stress runs also exposed
spawned workers receiving or returning corrupted arguments after parent stack
movement; the offset-based spawn copy fixed that class of stale pointer.

## Cdata FINREG ordering

- `lj_cdata_setfin()` anchors temporary key/finalizer values using stack
  offsets instead of raw stack pointers across allocation.
- Clearing a cdata finalizer now retires matching ordered FINREG nodes
  immediately. This removes stale ordered-node references after `ffi.gc(cd,
  nil)` and avoids relying on a later GC scan to discover the tombstone.
- Ordered FINREG scans now re-resolve the current weak-table slot by cdata key
  instead of trusting the original slot pointer, which can be invalid after a
  table resize.

The focused FINREG race passed 300 assert-build iterations after these changes,
and full M7 FFI coverage passed with both interpreter and JIT FINREG cases.

## Concurrent hook callbacks

`HOOK_ACTIVE` was a single global bit. Two OS threads could both enter debug
hooks, then the first to leave cleared the bit while the second was still inside
its hook, tripping `callhook: active hook flag removed`.

Actual hook callback entry/leave now uses an atomic active-callback counter and
keeps `HOOK_ACTIVE` set until the last concurrent callback exits. Existing
one-way guard uses of `hook_enter()` during close/GC/vmevent paths remain
unchanged.

## JIT trace boundaries

Generic NYI trace stitching moved Lua stack frames, entered `lj_vm_cpcall()`,
then restored the frame using raw stack pointers. In this fork, protected calls
can safepoint and relocate the stack, so the stitch undo now restores via saved
stack offsets.

Threading fast functions are now treated as non-recordable by the generic NYI
recorder. These calls can block, publish stack roots, poll safepoints, and
change thread ownership; compiling a trace boundary immediately before or
across them caused intermittent JIT-only corruption in
`tests/t-ffi-gc-finreg.lua`. This preserves JIT for surrounding Lua code while
keeping synchronization primitives on the interpreter path until dedicated
recorders exist.

## Current-stack weak semantics

The conservative legacy-GC traversal for owned remote states initially scanned
all owned stacks up to `maxstack`. That protected running remote workers from
clear/shrink races, but it also treated the current main state as remote and
marked stale stack slots, keeping weak-kv hash entries alive for an extra cycle.
Traversal now scans full stacks only for owned non-current states; the current
state uses the ordinary frame-derived live range and retains stock weak-table
semantics.

## Test updates

- `tests/t-gc2-traverse.c` now expects immediate ordered FINREG retirement after
  `ffi.gc(cd, nil)`.
- `tests/t-m8-ffi-weak-newindex.c` treats weak-value telemetry as monotonic,
  matching the broader C API/VM weak-newindex fixtures. The semantic assertions
  remain that the value is marked and SSB work is produced.

Current verification included:

- `tests/t-ffi-gc-finreg.lua 3 72` looped 3000 times with normal JIT enabled.
- `src/luajit -joff tests/t-weak-modes.lua`
- `tools/ci/lua_test.sh m4_chan_stress m4_threading_api m4_threading_stress
  m4_threading_shutdown m4_threading_coroutine m4_threading_hooks m7_ffi`
- `tools/ci/lua_test.sh m8_weak m9_m10_gc`
- `tools/ci/lua_test.sh m5_hookmask_atomic m5_hook_state_atomic
  m4_threading_hooks`
