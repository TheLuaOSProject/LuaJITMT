# Rooted boxed CALLXS result protocol (2026-07-18)

## Status

The production generic-FFI native frame now has the precise GC-root substrate
needed for pointer/reference, enum, and i64/u64 CALLXS results. This commit does
not yet admit those result classes: the recorder passes a null result root for
the existing void and unboxed scalar paths, so their generated behavior is
unchanged. Boxed results remain interpreted until the recorder and authentic
ABI/lifecycle fixtures land together.

No file under `plan/` was changed.

## Frame-root refinement

The earlier scalar-production note required a pre-call `IR_CNEW`, an
XSAVE-materialized root, a distinct entry-replay snapshot, and a nonthrowing
post-call `XSTORE`. The implementation design now adds a second, direct root:
each `LJFFINativeFrame` publishes the exact `GCcdata *result_root` before it
publishes ACTIVE native state.

This is additive rather than a replacement for the planned hidden XSAVE slot:

- the hidden slot protects the allocation and gives the XSAVE snapshot an
  ordinary materialized Lua root before native-frame publication;
- the direct frame pointer remains authoritative while the result is not in the
  caller-visible Lua stack, including callback relocation and POSTCALL restore;
- the separate replay snapshot prevents an entry rejection from restoring the
  hidden box as a callable or extra argument.

The eventual recorder order is therefore:

```text
validate and convert arguments
replay snapshot
IR_CNEW exact return CType
append hidden result-root slot
XSAVE snapshot
remove hidden logical slot
entry-rejection replay snapshot
publish frame(trace, geometry, result_root)
entry guard
CALLXS
one nonthrowing XSTORE into the box payload
construct caller-visible result
postcall snapshot
native leave
```

There may be no allocation, poll, guard, throwing helper, or result conversion
between CALLXS and XSTORE/leave. Bool remains a separate tranche because its
truth normalization needs a post-side-effect guard and matching snapshot
contract. Aggregate/container results also remain out of scope for this leaf
cdata protocol because managed payload edges would need a barrier design.

## Exact lifetime

`result_root` is an atomically published payload word covered by the native
frame's even/odd sequence. It is copied, snapshotted, and cleared with the rest
of the frame, but structural validation never dereferences it.

GC2 marks it through `gc2_mark_thread_root_obj_status()`, which preserves the
MARK/WEAK/SWEEP admission and recovery rules. It then acquires a second lease
requiring the immutable GC type to be exactly `GCcdata`; a stale or corrupted
frame word that happens to name another live allocation is stable invalidity,
not an accepted result root:

- ACTIVE: the certified parked-frame scan authenticates the trace first, then
  marks the result root and materialized stack;
- SUSPENDED: every continuation, including lower recursive callback frames,
  marks both its exact trace graph and its own result root;
- POSTCALL: owner-side scanning retains the box until protected snapshot
  restoration finishes and `lj_ffi_native_trace_exit_cleanup()` pops it.

The owner scan validates that every lower frame is SUSPENDED and only the top
may be ACTIVE, SUSPENDED, or POSTCALL. An unstable snapshot, invalid topology,
trace failure, result-root admission failure, or final sequence change queues a
root-scan retry and propagates failure to the native-parked caller. A consumed
safepoint request therefore cannot be acknowledged from a partial root scan.
Remote shape certification remains a non-dereferencing trace/pin check and does
not mark the cdata itself.

On x64 the added pointer grows `LJFFINativeFrame` from 88 to 96 bytes and the
16-frame diagnostic snapshot from 1424 to 1552 bytes. These are private runtime
structures, not LuaJIT public API/ABI objects.

## Evidence in this substrate landing

- structural frame tests copy, snapshot, pop, clear, and finalize non-null
  poison result-root words without dereferencing them;
- the exact ACTIVE scanner marks a real cdata which is absent from the Lua
  stack;
- callback SUSPENDED and POSTCALL owner scans retain a real unstacked cdata
  through complete major GC2 cycles;
- a forced semantic-admission retry makes the parked scanner refuse completion,
  records the retry, and succeeds only after the exact root can be admitted;
- a live object of the wrong immutable GC type is rejected after safe allocator
  admission rather than being dereferenced through the raw frame word;
- ordinary scalar CALLXS still passes a typed null root.

The focused lifecycle suites pass, as do clean default,
`LUAJIT_DISABLE_JIT`, and `LUAJIT_DISABLE_FFI` builds.

The next landing will implement the hidden-slot/replay-snapshot recorder
sequence and switch pointer/reference, enum, and i64/u64 authentic ABI fixtures
from interpreted fallback to mandatory CALLXS.
