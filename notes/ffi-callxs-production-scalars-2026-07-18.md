# Production generic scalar CALLXS (2026-07-18)

## Status

The generic x64 FFI call recorder is now enabled in ordinary production builds
for the result classes whose complete native lifecycle and result handoff are
nonallocating. The compile-time `LJ_FFI_CALLXS_TEST_ACTIVATE` wall no longer
exists. This supersedes the default-gate status recorded in
`ffi-explicit-ccall-safety-gate-2026-07-10.md`,
`ffi-generic-call-audit-2026-07-18.md`, and
`ffi-authentic-generic-callxs-2026-07-18.md`; those notes remain historical
certificates which preceded production activation. No file under `plan/` was
changed.

Non-x64 targets explicitly retain interpreted fallback. Production admission
is guarded by `LJ_TARGET_X64`, matching the current project and validation
scope rather than exposing an uncertified lowering on another architecture.

This remains one declaration-independent recorder and lowerer. It does not
match declarations against an explicit FFI-shape catalogue, introduce a
signature enum, or dispatch through per-signature wrappers. Existing CType
conversion constructs one generic `CARG` tree and x64 lowers one `IR_CALLXS`.

## Production admission boundary

The enabled result classes are:

- void;
- signed and unsigned 8-, 16-, and 32-bit integers;
- float and double.

Arguments already accepted by the generic path include scalar integers and
floating point, pointers, enums, 64-bit cdata, mixed GPR/XMM layouts, and C
varargs. Argument temporaries are materialized before native publication and
are included in the exact XSAVE root.

The following result classes still fail closed to the interpreter before
XSAVE, native entry, or the foreign side effect:

- bool;
- pointer and reference results;
- enums;
- i64/u64 cdata results;
- aggregates, complex values, vectors, and other multi-register results.

This is a result-handoff boundary, not a declaration or function-name matcher.
The interpreted fallback retains exact values and side effects.

## Exact lifecycle

Every admitted generated call uses the previously certified sequence:

```text
XSAVE exact logical frame and roots
CALLS lj_ffi_native_trace_enter(exact finalized trace, raw function)
non-recordable rejection guard
CALLXS generic CARG tree
pure, nonthrowing scalar normalization
post-return Lua caller snapshot
CALLS lj_ffi_native_trace_leave
non-recordable post-call guard
```

Generated callbacks suspend and restore the frame, nested callbacks preserve
ownership, remote `jit.flush()` treats the pinned frame as a certified root,
and forced exits release the exact trace pin without replaying CALLXS. Native
leave is the first C call after the foreign return, so errno/LastError capture
still precedes every other runtime helper.

## Frame topology

Production admission requires a physical Lua frame which resolves to a valid
Lua caller with one `lj_record_ret()` adjustment. Its saved opcode must be
`CALL`, `CALLM`, or `ITERC`; the post-call helper independently rejects an
invalid prototype/PC, terminal link, or unsupported physical caller.

The VM erases an inlined `CALLT` or `CALLMT` by reusing its Lua frame, but
deliberately retains the outer `CALL`/`CALLM`/`ITERC` PC. That topology has the
same one-adjustment post-return view and is now explicitly supported rather
than accidentally admitted. Tests mechanically authenticate the source
bytecodes and require a CALLXS trace for:

- ordinary `CALL`;
- a real multiple-result-argument `CALLM`;
- cdata used directly by generic-for `ITERC`;
- inlined `CALLT`;
- inlined producer plus `CALLMT`.

The result-count matrix also exercises an ignored result, the normal single
result, excess fixed results filled with nil, and an open result forwarded into
another Lua call. Each case requires CALLXS and an exact foreign side-effect
count.

All five admitted frame shapes have an exact saved-opcode certificate. Physical
`CALLM` and `ITERC`, plus both reused-frame tail forms, additionally have
separate finalized-trace, caller-snapshot, forced epoch, and throwing
fresh-STOPREQ tests. Their exact foreign counters prove the completed call is
never replayed and every retained frame/pin is released. The STOPREQ count is
checked per topology because a loop-root `ITERC` and an entry-root `CALLM`
reach the generated call a different number of times before the fresh request
is observed.

Root-level tailcalls, protected/continuation-rooted calls, direct vararg-frame
callers, terminal returns, and non-Lua immediate callers still reject. Non-Lua
frames farther below an otherwise valid Lua caller remain representable by the
ordinary snapshot traversal. Broader protected and continuation support needs
an explicit reconstructed post-return view and different cleanup sequencing;
adding more saved-opcode guesses is not sufficient.

## Concurrent CType read correction

The production audit found that `crec_call()` computed the result IR type with
the older `crec_ct2irt()` helper before rejecting enums. That helper follows an
enum child through the relocatable CType table without the recorder snapshot
contract. Rejected declarations are still concurrent reads, so production
activation changes this call site to `crec_ct2irt_snapshot()`. Scalar behavior
is identical, while enum child relocation now retries or aborts recording
instead of racing the table.

## Why boxed results remain interpreted

The existing post-CALLXS `IR_CNEWI` code is not an activation path. CNEWI can
allocate or throw while the native frame is still ACTIVE, before central
POSTCALL cleanup recognizes ownership. It is also immutable/CSE-able, so it
cannot represent a preallocated result box whose payload is filled later.

The next boxed tranche must instead use pre-call `IR_CNEW`, make that object an
exact XSAVE-materialized root, and use a nonthrowing post-call `XSTORE`. It also
needs a distinct pre-entry replay snapshot: placing the box in the ordinary
callable slot before XSAVE would make entry rejection restore cdata where the
interpreter expects the FFI function and retry `FUNCC` incorrectly. Bool stays
closed for a separate reason: its result guard and false-result snapshot must
be kept distinct from the later post-call snapshot and `FIXGUARDSNAP` repair.

## Deterministic evidence

The production suite now requires XSAVE and CALLXS in a normal build. The
larger generated ABI catalogue retains all 320 behavior assertions and gives
each row an explicit result-class oracle: all 211 admitted scalar rows must
contain CALLXS, while all 109 pointer/i64/u64 result rows must contain none.
The catalogue advances a full GC2/JIT retirement grace period between its
artificially rapid flush/re-record cycles; this prevents transient retired
mcode pressure in the test harness from being mistaken for a recorder result
and is not a runtime precondition for CALLXS. Focused validation includes:

- production scalar CALLXS IR and ABI/result matrix;
- callback suspend/nest/error unwind;
- remote flush while a generated call is blocked;
- exact finalized trace constants and pins;
- CALL/CALLM/ITERC/CALLT/CALLMT topology;
- repeated forced POSTCALL exits and fresh STOPREQ throws;
- forced POSTCALL restoration for u32-to-number, double, float, signed and
  unsigned 8-/16-bit normalization, and void effects, each with an exact
  foreign counter proving that the completed call was not replayed;
- remote generated-STOPREQ publication gated on both the ACTIVE native bit and
  a nonzero XSAVE-backed native-frame depth, so an interpreted warm-up call
  cannot satisfy the concurrency oracle;
- errno/LastError preservation across a generated scalar call followed by an
  aligned cdata allocation, alongside interpreted boxed and VLA fallbacks;
- rejected entry and exactly-once interpreter fallback;
- JIT XSAVE materialization;
- VM safepoint and handshake suites.

Focused ASan and UBSan builds instrumented both the runtime and native test
fixtures. Repeated callback, remote-flush, forced-postcall, STOPREQ,
error-state, production-smoke, and full 320-row ABI-matrix runs completed with
no sanitizer diagnostics. Host code generators were deliberately left
unsanitized; setting a global sanitizer `CFLAGS` without the corresponding host
link flags fails in host `minilua` before it exercises the target runtime.
Clean default, `LUAJIT_DISABLE_JIT`, and `LUAJIT_DISABLE_FFI` feature builds
also produce the executable plus static and shared libraries. The FFI-disabled
profile retains its pre-existing unused-helper and broad IR-switch warnings,
but has no production-CALLXS compile or link failure.

The immediate next FFI work is the replay-safe pre-rooted boxed result protocol,
followed by bool, protected/continuation/root-tail frames, and descriptor-driven
multi-register results. Performance tuning remains a b1.2.1 target; this
activation adds no signature dispatch or global
lock, but it still pays the exact native enter/leave helpers around each
generated foreign call.
