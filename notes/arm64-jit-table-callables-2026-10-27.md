# ARM64 shared-table FFI callables

This increment composes the existing bounded rooted table-hit operation with
the scalar CALLXS protocol in FORL roots. A CData callable fetched by TGETS,
TGETV or TGETB can be used by the compiled loop. The ABI classifier also
resolves one native-width, non-reference pointer to a function, while keeping
the original CData CType guard distinct from the underlying function type.
References, data pointers, nested pointers and non-function children do not
gain admission. Empty fixed argument lists are matched against both the CType
and source CALL arity. Guarded boolean SLOADs require exactly TYPECHECK mode.

## Source and lifetime proofs

The semantic and allocated passes independently track the current TG scratch
roots. Both require the exact bounded hit helper, input/output modes, argument
aliases, success guard and adjacent guarded CData VLOAD. Scratch provenance is
cleared at calls and LOOP/XPOLL boundaries. The separate full rooted helper
used for the mutable CData `__call` lookup retains its own existing proof.

Each pass binds the retry and VLOAD-guard snapshots to the same source TGET,
restored receiver/key slots or exact constant key, and a PC no earlier than
the preceding completed foreign call. The first later snapshot must contain
the fetched CData in the TGET destination; intervening bytecodes may only be
the existing side-effect-free call setup operations. The XSAVE source proof
then requires the exact fetched CData in the saved callable slot and matches
each argument carrier to its saved Lua source. The allocated pass separately
checks scratch registers, full-width spills, snapshot locations and existing
native-call survival rules.

The helper publishes the complete result before releasing its object lease.
After later scratch overwrites, the existing published JIT activity and GC2
phase gate protect the live value until XSAVE/native-frame publication or
snapshot restoration. Registers and spills are not new enumerated GC roots.
No helper, GC gate, emitter, pointer authentication path or x64 production
mechanism is replaced by this increment.

Variadic wrapper metadata names the normalized CT_FUNC, not the guarded outer
pointer CType. Both admission passes verify that relationship. The pointer
bits continue through the existing authenticated indirect-call path unchanged.

## Focused evidence

`tools/ci/arm64_jit_table_call_contract.sh` builds profile-bound runtimes,
archives, helper libraries and fixtures. Its default matrix covers ARM64 and
arm64e/BTI with assertions enabled and disabled. Cases are also registered in
the existing M7 scalar CALLXS suite.

The initial ARM64 assertion profile
`e88cfc77925ab7df6d0a1f917e610e8d3a2adf395de00cec897dac9f010e44fb`
passes all seven focused fixtures:

- Shared callable replacement by a peer, followed by GC, executes the new
  function identity. Replacing the entry with a number or nil while the trace
  is active permits the already-loaded call, then exits and raises through
  the interpreter without another native effect.
- A forced POSTCALL exit resumes after CALL, preserves errno and executes
  each requested foreign effect exactly once. Native frames, staging,
  callbacks and trace pins are clean afterwards.
- The lifetime fixture pauses at a second rooted hit after it overwrites the
  target's scratch root. A peer removes the target's table edge and requests
  GC. The phase transition is deferred while JIT activity remains published;
  the target still executes through a native frame and is eventually reclaimed
  after its remaining roots and trace are released.
- An authentic two-call trace independently passes semantic and allocated
  admission. It rejects 23 structural mutations in both passes and four
  additional allocated-layout mutations, including malformed source PCs,
  stale scratch, missing boolean/table guards, wrong callable roots and
  partial/out-of-frame CData spills.
- Cached function-pointer CData executes zero-argument calls through string,
  integer and both boolean TGETV keys and TGETB, plus a TGETS variadic call.
  Native observations, exact helper counts, errno and cleanup are required.
- The existing variadic fixture retains its direct-function cases and adds a
  cached pointer variant. Independent clones reject substituting the outer
  pointer CType ID for the normalized function wrapper ID.
- Existing scalar and pointer admission regressions pass.

Log: `/tmp/ljmt-table-call-focus.log`. The contract restored the ordinary ARM64
assertion runtime. Four-profile and broader compatibility validation is pending
at this initial checkpoint.

The fixtures cache one `ffi.typeof` object when comparing two callable
identities: parsing the same function-pointer type string twice can allocate
distinct CType IDs. They also retain the library object for the full lifetime
of the casts. Interpreter-only first iterations are excluded from native
pause hooks; an error before trace entry is not evidence of a native exit.

## Scope

The full ARM64 port remains incomplete and opt-in. This does not open general
Lua calls/metamethods, arbitrary table/GC recurrences, side traces with foreign
effects, aggregate/sret CALLXS, or unsupported argument/result forms. It
composes the bounded table-hit certificate with the already admitted scalar
ABI and native-frame protocol.
