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
assertion runtime.

The fixtures cache one `ffi.typeof` object when comparing two callable
identities: parsing the same function-pointer type string twice can allocate
distinct CType IDs. They also retain the library object for the full lifetime
of the casts. Interpreter-only first iterations are excluded from native
pause hooks; an error before trace entry is not evidence of a native exit.

## Frozen checkpoint validation

Source checkpoint `0761fb79e5547bf23fb8cfc8c1b4269d64c339e8` passes all seven
focused fixtures in all four profiles:

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `f64e3a8db59b85d49dba4dc72e2e3445f5794b6a7b36ac7cc6d1a3ca4cdacd4e` |
| arm64e assertion/BTI | `f2042f932d10ce9eb74b89c64455932a6513d31f8cf4c9a412f032e7c209eda7` |
| ARM64 release | `b3632a4fd62a99aa1ce8a509635ca9c37265f79f22b173f4693d1f3b1573e7ea` |
| arm64e release/BTI | `dc011d13e9e470bf4cad1a122dcdf670f43b6ec654051a15f836972ea24d83a4` |

The broader M7 scalar CALLXS suite passes in all four profiles, including its
existing scalar, float, pointer, boxed/fixed I64/U64, U32/CALLM, variadic,
callback, flush and C++ unwind coverage. Logs:
`/tmp/ljmt-table-call-four-profile.log` and
`/tmp/ljmt-table-call-m7-<profile>.log`.

Ordinary ARM64 assertion profile
`b884fdda38e7adf40b507283693454b510acb1bc613a4d40089e5fe8dd1b5b4a`
passes nine table-call smoke cases, rooted cells/upvalues, bit operations,
509 stock source cases and 509 original-bytecode imports. The smoke checks
exact helper counts, values, errno and each runner's actual JFORL trace number.
Its first draft incorrectly assumed trace 1 was always reused after a flush;
that temporary harness assumption was corrected without changing production
source. Native execution evidence comes from the C fixtures above, rather
than published trace metadata alone.

Signed x64/Rosetta assertion profile
`cff9d90b08c2eb9469d631dcba0ae26538ab795bd8be0e2f355a9f0ec6bf2f4f`
passes both 509-case stock corpora. This is bounded compatibility evidence,
not a full x64 umbrella or native Intel performance result. The matrix checks
unchanged source and restores ordinary ARM64 assertion profile
`e32b9bbb7a2ae98eec54b263baf8039802ab063d5f508d8109adba7de09c6664`.
Code-sign verification passes; selected test-helper symbols and out-of-line
atomic imports are absent. Logs: `/tmp/ljmt-table-call-matrix.log` and the
matching ordinary, stock-source/import and x64 logs. Complete experimental
and bootstrap umbrellas were not rerun for this target-local increment.

## Main checkout

Main `v2.1` was fast-forwarded to the source checkpoint and rebuilt with the
ordinary bootstrap/experimental/assertion flags, without test helpers.
Profile `e1b8247dd7bc9ce35191e244fa13562da667b8eb5e0f4ad9d53f4f9fa4685d4b`
passes the same nine table-call smoke cases, bit operations, rooted
cells/upvalues and both 509-case stock corpora. Code-sign and atomic/test-helper
symbol checks pass. Log: `/tmp/ljmt-table-call-main.log`.

- Runtime SHA-256: `a752a8bac9f412d82d54aee99b57055961daf76d87a0975a9598d145f0e4b2fa`
- Archive SHA-256: `94899f430ce85418df5ba3f875ecd2c57f171d30b0493da2bfd14b305bfa56cf`
- Build-signature SHA-256: `1df01456604c0713dc4d2d4019c6b4b958182adbd0b2897fabc9c73e2a7b7f5f`

## Scope

The full ARM64 port remains incomplete and opt-in. This does not open general
Lua calls/metamethods, arbitrary table/GC recurrences, side traces with foreign
effects, aggregate/sret CALLXS, or unsupported argument/result forms. It
composes the bounded table-hit certificate with the already admitted scalar
ABI and native-frame protocol.

A next bounded composition is table-loaded owned I64/U64 arguments. Their
payload proofs currently accept SLOAD or a prior boxed CALLXS result, not the
certified rooted CData VLOAD. Extending those independent source/CTYPE/use and
XSAVE proofs remains future work; the table-callable evidence does not certify
it automatically.
