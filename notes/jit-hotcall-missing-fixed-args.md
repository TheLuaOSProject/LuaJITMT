# FUNCF hot-call missing fixed arguments

## Failure

The GC2 paranoia stock run aborted in `rec_check_slots()` while recording
`lib/string/format/num.lua` case 5. The VM's fourth parameter was `nil`, but
the recorder had specialized the same slot as `IRT_STR`.

The minimal shape is a four-parameter Lua function called repeatedly with
three arguments from an interpreted caller. A concatenation used for the third
argument leaves a string scratch value in the next stack slot.

`vm_hotcall` invokes the recorder at `BC_FUNCF` before the fall-through
`BC_IFUNCF` prologue clears missing fixed parameters. At this boundary
`L->top - L->base` is the exact incoming argument count. The lockless local-cell
recorder eagerly materializes future `CGET` sources during root setup; without
accounting for the pre-prologue boundary, it loaded the stale concatenation
scratch as parameter four. The VM then cleared that slot before the recorder's
first bytecode consistency check.

## Fix

After root snapshot 0 is created, the recorder takes a pre-promotion demand
pass over the straight-line `CGET` sources that local-cell entry setup would
otherwise materialize. For each undefined source that is both a fixed
parameter and beyond the exact hot-call argument count published through
`L->top`, it emits a guarded nil `SLOAD` and canonicalizes that recorder slot to
`TREF_NIL`. Missing parameters that this entry region never reads produce no
IR. Actual arguments, including explicit `nil` arguments, are handled by the
ordinary materialization pass. Extra arguments remain outside the
fixed-parameter snapshot as before.

The snapshot-before-guard order is required. If the same function-entry trace
later receives a real value in a previously missing slot, the nil guard exits
through snapshot 0 without restoring nil over the real argument. Directly
seeding `TREF_NIL` without an entry guard would incorrectly let that later call
execute the trace under the recorded missing-argument assumption.

The demand pass runs before pending local-cell promotion helpers. This keeps a
failed entry guard free of speculative upvalue-cell allocation and lets the
ordinary post-promotion pass materialize provided sources. A parameter used
only by a future mutable-local-cell `FNEW` capture is intentionally not
specialized: the generated promotion helper executes after `BC_JFUNCF` has
cleared missing parameters (or preserved supplied ones) and reads the runtime
slot dynamically.

The assertion-only slot checker also retains the trace-start prototype when
its initial VM frame marker is C/fast and therefore has no Lua prototype. A
pending mutable-cell slot remains owned by that trace-start prototype even
after recording enters a C/fast frame and `J->pt` becomes null. This fallback
is deliberately narrow: it supplies metadata only for the initial trace frame;
each nested Lua frame marker replaces it with that frame's own prototype in the
existing traversal.

This is recorder state reconciliation for the existing VM entry ordering. It
does not relax `rec_check_slots()`, disable local-cell materialization, or add a
Lua-call/JIT bailout. `BC_JFUNCF` still performs the normal VM-side missing
parameter clear before entering generated code.

## Regression and history

The focused fixture drives a consumed missing fourth argument with adjacent
string scratch, then alternates missing and real fourth arguments through that
same live trace without a flush. It separately escapes a child closure whose
mutable fourth parameter is consumed only by `FNEW`, proving the generated
promotion path captures both missing nil and a later real value without
specializing the parent slot. Error unwinding closes the escaped capture after
the hot-call trace because a direct return would encounter the independently
unrecordable `UCLO` bytecode. It also covers an all-missing fixed-parameter
suffix from a JIT-disabled caller.

A generated 128-parameter callee consumes the supplied first parameter and the
missing 128th parameter while leaving parameters 2 through 127 unused. The
fixture requires exactly one guarded nil `SLOAD`, exactly two total guarded
`SLOAD`s, and at most 12 IR instructions. This bounds entry work by demanded
sources; the discarded blanket implementation generated 129 IR instructions
for this case. The demand implementation currently emits three IR instructions:
the two loads and the return-frame guard.

The M6 gate runs the fixture and the original stock format case under
`LUA_USE_ASSERT` plus `LJ_GC2_PARANOIA`, then restores the default build
profile.

Commit-level isolation found `edddec97` passing and its child `830297de`
failing. The earlier commit had removed a blanket function-entry materializer
skip while broad JIT gates still hid the defect; `830297de` restored normal JIT
recording and made the stale pre-prologue read reachable. The fix retains that
recording and replaces neither gate.
