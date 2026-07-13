# x64 trace-exit STOPREQ rethrow target

## Failure

The optimized `t-vm-safepoint` fixture deterministically crashed when a fresh
STOPREQ interrupted its final long-running trace.  The fault was an
out-of-bounds `TraceVec` lookup in `vm_exit_interp`: the negated Lua error code
`-LUA_ERRRUN` (`0xfffffffe`) was being consumed as a trace number.

An isolated behavioral bisect identified `7d09a64c` (`Cooperate with JIT during
GC2 sweep`) as the first failing commit.  Its parent passes the same binary.
The failure does not require an active GC cycle or worker: immediately before
the final call the fixture was in IDLE with the JIT gate open and zero workers.

## Cause

`vm_exit_interp` used `jae >9` to branch from its negated-error check to a later
numeric DynASM label that rethrows from the correct C frame.  Cooperative JIT
entry recovery inserted new `9:` labels between that branch and the intended
destination.  DynASM therefore resolved the forward reference to the first new
label, inside JLOOP recovery.  That path expects `RD` to contain a trace number,
but the trace-exit protected callback had returned `-LUA_ERRRUN` after its
native-leave safepoint delivered STOPREQ.

This was a control-flow label collision, not a GC/JIT lifetime race.  The
trace-exit protected callback, SMR read section, and outer error propagation
were otherwise following their intended sequence.

## Fix and invariant

The error check and rethrow block now use the named DynASM label
`vm_exit_rethrow`.  Future local numeric labels added to the normal trace-exit
dispatch path cannot redirect error control flow.

Invariant: every `RD` value in the inclusive negated Lua error range branches
directly to `vm_exit_rethrow` before any bytecode decode, trace-vector access,
or JIT entry-recovery logic.

## Verification

- The original optimized fixture failed on every immediate retry before the
  change.
- The rebuilt fixture passed 50 consecutive 20-second-bounded executions.
- Disassembly shows the unsigned error branch targeting the generated
  `lj_vm_exit_rethrow` symbol directly.

