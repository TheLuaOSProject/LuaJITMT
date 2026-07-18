# Generic FFI native-frame publication substrate (2026-07-18)

## Status and scope

This change adds the dormant per-thread-group publication structure needed by
future generic traced foreign calls. It deliberately does not activate traced
`IR_CALLXS`, consume `IR_XSAVE` staging, enter native state, acquire a trace
pin, scan a Lua stack, change a callback mirror, or relax any GC/JIT/safepoint
gate. The unconditional generic `crec_call()` blacklist remains in place.

The names and protocol describe any foreign call. There is no C-signature
matcher, explicit shape enum, per-shape recorder, or generated wrapper family.
Capacity exhaustion will make future lowering side-exit before the call and
therefore will not restrict the Lua or FFI calling paradigms that work.

This is an implementation refinement documented outside `plan/`; no plan file
was edited.

## Per-TG representation

Each FFI-enabled `TGState` ends with:

- one 64-bit `ffi_native_seq` generation;
- one 32-bit `ffi_native_depth`;
- sixteen `LJFFINativeFrame` records.

The fields are last in `TGState`, and `GG_State.main_tg` is itself last in
`GG_State`. Thus no existing TG, VM, global-state, dispatch, callback, or ABI
offset changes. On the Linux x86_64 compiler used for validation, a frame is 88
bytes, a full snapshot is 1,424 bytes, and the new fields begin at TG offset
26,552 by consuming the old tail padding. `sizeof(TGState)` grows from 26,560
to 27,984 bytes. The bounded 1,424-byte net cost per FFI-enabled TG avoids the roughly
15--20 KiB cost of reserving the full callback nesting maximum. A seventeenth
future traced foreign call takes a pre-call interpreter side exit without
altering the sequence, depth, payload, native state, or call semantics.

Every remotely visible payload field has an atomic accessor. The exact trace
pointer is authoritative; its trace number is diagnostic only. Stack roots,
base, top, and JIT base are represented as offsets rather than raw stack
pointers so a later callback-induced stack relocation can resolve them against
the current stack. This structural layer only carries those offsets; it does
not claim that they are valid or dereferenceable.

## Publication and snapshots

Push and pop are single-owner, allocation-free operations:

1. validate the stable sequence, depth, slot, and frame;
2. release-publish the next odd sequence;
3. execute a writer barrier so the odd word precedes payload mutation on the
   supported x86_64 targets;
4. atomically fill or clear the payload and publish the new depth;
5. release-publish the next even sequence.

The final even generation makes the preceding payload visible. Sequence wrap
publishes a permanently odd poison and aborts rather than allowing an ABA.
Malformed owner input, underflow, or an occupied destination is an internal
lifetime violation and also fail-stops. Capacity exhaustion is the sole normal
push failure and does not touch any publication.

A reader acquire-loads the initial generation, copies every active payload word
atomically, and acquire-loads the generation again. Results are:

- `EMPTY` for a stable zero-depth stack;
- `STABLE` for a same-even, structurally valid active stack;
- `RETRY` for an odd generation or any generation change;
- `INVALID` for stable malformed depth or payload.

`RETRY` and `INVALID` leave the caller's output object untouched. A stable
malformed snapshot can only veto future GC/reclaim work; it must never be used
as positive root or lifetime authority.

The seqlock alone is not a stack-storage lease. Even a snapshot which later
detects relocation could already have dereferenced freed old storage. The
future exact scanner therefore may resolve and scan these offsets only while a
consumed-poll/native-park certificate keeps the carrier and its stack stable,
unless a separate stack-storage lease is implemented first.

## Lifecycle and conservative behavior

Both embedded-main and heap TG initialization clear every frame before the
first global-state, carrier, or TLS publication. Detach checks the sequence,
depth, and all slots before clearing `cur_L`, XSAVE staging, JIT base, native
state, or callback mirrors. Main, heap, retry, and terminal physical teardown
repeat the same fail-stop check. A leaked frame is never repaired by clearing
it during destruction.

This slice leaves all conservative behavior unchanged:

- broad native/JIT stack scanning remains authoritative;
- `lj_tg_any_jit_active()` remains unchanged;
- trace-flush remote acknowledgement stays vetoed for active JIT/native TGs;
- trace-quiescence waits remain in place;
- `GCtrace.native_pins` are neither acquired nor released;
- `ffi_xsave_root`, `ffi_call_func`, callback state, `in_native`, and `jit_base`
  are not modified by frame push, pop, or snapshot.

## Deterministic evidence

`tests/t-ffi-native-frames.c`, built with its isolated test helper flag, proves:

- initialization produces a stable empty snapshot;
- sixteen correlated frames publish in order and pop in exact reverse order;
- each successful transition increments the even generation by two;
- capacity failure leaves generation, depth, all frames, and unrelated native,
  callback, XSAVE, and JIT mirrors unchanged;
- a forced odd window returns `RETRY` without touching output;
- a forced generation change after copying returns `RETRY`;
- stable out-of-range depth and stable malformed payload return `INVALID`
  without touching output;
- a completely unwound TG passes lifecycle finalization and normal VM close.

The focused `m7_ffi_native_frames` gate builds the runtime and fixture with
`-Werror`, runs the deterministic probe, and restores a default build. Callback
install/runtime, foreign carrier detach, terminal orphan cleanup, and threaded
shutdown gates also pass with this substrate.

The broader pre-existing `t-ffi-ccall-native-helpers` fixture currently lets
an expected STOPREQ escape its nested Lua `pcall`. A clean archive of pushed
commit `387dbfd4` reproduces the identical failure without this change, so it is
not a native-frame regression. It remains a real FFI/error-unwind defect to
diagnose before activating generic traced calls.

## Next activation slices

The next safe work remains staged:

1. validate and consume XSAVE geometry into stack-relative offsets while the
   ordinary JIT-execution proof still protects the exact trace;
2. acquire the exact native trace pin and publish a dormant authentic frame;
3. add a certified GC2 scanner under a native-park/consumed-poll certificate,
   while retaining the broad fallback;
4. retain the frame and pin through callbacks, unwind, forced post-call exit,
   and exact snapshot restoration;
5. only after those paths pass deterministic and stress gates, wire the generic
   `CALLXS` enter/leave path and remove the recorder blacklist.
