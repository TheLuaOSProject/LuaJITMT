# Rooted boxed CALLXS admission (2026-07-18)

## Status

The one production generic FFI recorder now admits pointer/reference, enum,
signed 64-bit, and unsigned 64-bit results to CALLXS on x64. There is no
signature catalogue, result-shape dispatcher, production wrapper, or test-only
activation gate: these result classes use the same ABI-driven argument
recorder, native-frame lifecycle, and CALLXS emission as the unboxed scalar
classes.

At this checkpoint bool remained interpreted and aggregate results remained
future work. Subsequent same-day tranches admitted replay-safe bool and
ABI-indirect aggregate results; this note is retained as the historical boxed
admission checkpoint. No file under `plan/` was changed.

## Recorder protocol

The recorder captures the exact raw child CType ID, immutable return info,
size, and raw IR type before recording arguments. This is necessary because
vararg inference may grow and relocate the CType table. Raw-child lookup strips
outer attributes exactly as the interpreter does, while preserving reference
and enum identity. Consequently attributed integer, pointer, and enum
declarations produce the same raw box CType as a cold call; explicit C
references remain references because `CTF_REF` is not an outer attribute node.
No classification consults a stale CType pointer.

After all argument guards and conversions, a boxed result follows this order:

```text
preallocation replay snapshot
IR_CNEW with the exact result CType ID
precompute ADD(box, sizeof(GCcdata)) payload address
append box in a hidden Lua slot
XSAVE snapshot containing the box exactly once
remove the hidden logical slot
distinct entry-rejection replay snapshot without the box
publish native frame with the direct result-root pointer
entry guard
CALLXS with the raw ABI result type
one matching raw-type XSTORE into the precomputed payload
construct the caller-visible result state
native leave
```

The allocation is deliberately `IR_CNEW`, not sinkable `IR_CNEWI`. The XSAVE
slot materializes and roots the allocation before native-frame publication;
the direct frame root then covers ACTIVE, nested SUSPENDED, and POSTCALL states.
The separate replay snapshot restores only the original callable and arguments
if native entry is rejected. A checked append prevents overflowing the trace
slot array.

There is no guard, allocation, helper, conversion, poll, or throwing operation
between CALLXS and the result XSTORE/native leave. The raw store is suitable for
these leaf cdata classes and preserves the foreign return bits exactly,
including high-bit `uint64_t` values. This protocol must not be copied to
managed-edge aggregates without a write-barrier design.

## Compatibility and cost

The public LuaJIT API and ABI are unchanged. Generated values keep the
interpreter's exact resolved CType identity: outer scalar/pointer/enum
attributes are stripped, enums retain their enum node, and references retain
their reference node. The generated path allocates the result box immediately
before the foreign call instead of immediately after it; this enables safe
rooting without adding a signature-specific wrapper or a post-return
allocation. The only post-return addition is the raw payload store.

As previously authorized for the beta line, custom `lua_Alloc` remains
temporarily ignored by the GC2/internal allocator policy. This admission uses
that existing internal allocation path. Restoring a safe custom-allocator
contract is still required after the beta correctness tranches and is not being
silently treated as complete.

## Evidence

- The authentic Lua fixture mechanically requires CALLXS for pointer,
  reference, enum, i64, u64, high-bit u64, and attributed integer/pointer/enum
  declarations. JIT-disabled baselines require exact `ffi.typeof()` and
  `ffi.alignof()` equality after outer attributes are stripped.
- The generated ABI catalogue requires the generic path for all 320 admitted
  rows: 211 unboxed scalar rows and 109 newly admitted boxed rows.
- IR inspection proves one exact `IR_CNEW` root, inclusion only in the XSAVE
  snapshot, omission from both replay snapshots, and an immediate matching
  CALLXS-to-XSTORE-to-leave sequence.
- A remote thread performs a complete collection and trace flush while a
  pointer-return CALLXS is ACTIVE; both requests complete before the foreign
  call is released, the exact pointer survives, and the foreign effect occurs
  once.
- Generated callbacks exercise boxed outer and nested results, distinct roots
  in ACTIVE/SUSPENDED frames, allocation and full collection inside the
  callback, normal unwind, and callback-error unwind.
- Boxed paths cover forced epochs, entry replay, POSTCALL restoration, fresh
  STOPREQ abandonment, and native error-state preservation without foreign-call
  replay.
- The focused boxed/scalar lifecycle and complete ABI catalogue pass under
  AddressSanitizer and UndefinedBehaviorSanitizer. The UBSan sweep also exposed
  and removed pre-existing unaligned typed accesses in callback and x86
  machine-code byte streams.
- The authentic matrix passes with allocation sinking enabled and explicitly
  disabled, so correctness does not depend on sink optimization.

Clean default, `LUAJIT_DISABLE_JIT`, and `LUAJIT_DISABLE_FFI` builds and runtime
smokes pass independently of the sanitizer builds.

## Next work at this checkpoint

Replay-safe bool and ABI-indirect aggregate results were the immediate
follow-ups and are now covered by their later notes. Broader
protected/continuation and root/tail trace shapes, direct/multi-register
aggregate ABI results, cross-platform deep validation, and performance work
remain b1.2.1 tasks. They must remain generic ABI-lowering work and must not
reintroduce explicit C-signature matching. The temporary custom allocator
exception and separate Lua `atomic` library remain scheduled after the core
correctness tranches.
