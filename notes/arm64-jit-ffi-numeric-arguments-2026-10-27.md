# Ordinary Lua numbers as fixed C integer arguments

## Diagnosis and change

The preceding checkpoint interpreted `fn(t.value)` when the shared table
contained a fractional Lua number and the fixed C argument was an integer.
All eight signed/unsigned 8/16/32/64-bit cases produced the correct interpreted
value but declined CALLXS admission. Baseline logs:
`/tmp/ljmt-ffi-narrow-baseline.log` and
`/tmp/ljmt-ffi-narrow-types-red.log`.

The recorder and ARM64 emitter already implement the required conversions.
For widths below eight bytes the recorder converts NUM to I64 with
`IRCONV_ANY`, then takes the low word. Small register arguments extend that
word according to the declared signedness; small Darwin stack arguments keep
their compact width. Eight-byte arguments convert directly to I64 or U64.
The small register-extension instruction encodes I8/U8/I16/U16 as its source
type even though its actual input IR is INT. That exact stock shape must not
be mistaken for an arbitrary source-type mismatch.

Only `src/lj_asm_arm64_admit.h` changes production behavior. Independent
semantic and allocated graph walks recognize the exact conversion modes,
types and fixed integer ABI consumer, bind the full chain to the original
Lua NUM in the CALLXS input snapshot, and reject native integer intermediates
in Lua snapshots or unrelated uses. The source proof is required even for a
direct NUM SLOAD argument. Existing register-bank, spill, liveness, native-frame
and stack-area checks still apply. No recorder, emitter, shared VM/GC2 or x64
production changes are needed.

NaN and out-of-range results follow this architecture's existing interpreter
conversion helpers. The tests compare their exact typed argument bits; they
do not impose a C cast with undefined out-of-range behavior or a different
architecture's integer-conversion result.

## Focused evidence

The native fixture has eight unary runner prototypes and a ninth stack
runner with eight leading integer register arguments followed by all eight
integer widths/signs in compact Darwin stack slots. An active waiting peer
keeps the multithreaded path exercised. Every C invocation checks its argument
bits against an interpreter oracle, including calls after warmup.

Each runner executes a base fractional case, 15 width-specific cases, a
forced POSTCALL exit and same-trace reuse: 162 runs of 24 calls in total.
Cases include signed fractions, negative zero, width boundaries, high bits,
huge positive/negative numbers, NaN and infinities. Checks cover exact C
counts, errno, native frame/L/callee/trace identity, pins, XSAVE and callback
cleanup. Each forced exit verifies the continuation immediately after its
CALL and exact call counts exclude replay.

Interpreter oracle functions are separate prototypes permanently disabled
with `jit.off(fn, true)`, and their finish hook must observe zero native
CALLXS frames. An initial fixture using only `jit.off()` failed this check:
already-published traces can still execute. That red evidence is preserved in
`/tmp/ljmt-numeric-runtime-oracle-red.log`.

The admission fixture clones nine authentic traces. All nine semantic views
and all nine allocated views pass. It independently rejects 86 semantic and
106 allocated mutations covering conversion source/mode/type, ABI signedness,
original snapshot identity, exposed native carriers, NORESTORE, truncated
input frames, stack argument order, register bank and spill alignment/extent.
The first fixture draft incorrectly required a snapshot at the rooted helper
CALLS instruction; authentic traces instead bracket it with root and VLOAD
snapshots. The locator was corrected while retaining the exact CALLXS XSAVE
and original NUM slot checks. Production admission was not relaxed for this.

Both new fixtures pass ARM64 assertion profile
`ce41d12a09d95ee041b87285f139bb183fe124b11267f8b68fae3c301974ca03`.
Log: `/tmp/ljmt-numeric-new-focused.log`. They are registered in the expanded
20-fixture table-call contract and the broader M7 scalar CALLXS suite.

## Frozen checkpoint validation

Source checkpoint `f05b4658b8a4ed1cc88dddfa240ef66d0b46dcae` passes all 20
focused fixtures in all four profiles. Each profile includes all 162 numeric
runs, nine forced post-call exits, 18 authentic admission positives, 86
semantic negatives and 106 allocated negatives described above.

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `ce46ff483f9940904f737fc3772301a94e0f4e24fe8cace2a57b52d3d6a5644e` |
| arm64e/BTI assertion | `b5e1df84bebae01dd0c8d86d0f3ec58dabbd3e66b7a5948620a50ef9b7b78278` |
| ARM64 release | `8ef002f2b35efc8af8918b55708f84c6474d78de8de30f1ac02c591842bdddb1` |
| arm64e/BTI release | `6baed1e382954a193c4c0441f874ab8c5ddbeb0433e39c7580a892a8602617f8` |

Both complete umbrellas pass on that unchanged checkpoint:

- `tools/ci/arm64_jit_fail_closed_gate.sh`, including all four broader M7
  scalar CALLXS profiles with the new fixtures, rooted effects, construction,
  GC lifetime, publication/entry/exit/flush, callbacks and unwind checks.
- `tools/ci/arm64_bootstrap_gate.sh`, including the 387 applicable stock cases,
  assertion interpreter, GC2 hard assist, TG hooks, coroutine/dead-resume,
  aggregate/variadic interpreter FFI and callback/errno unwind checks.

Logs: `/tmp/ljmt-numeric-four-profile.log`,
`/tmp/ljmt-numeric-experimental.log` and `/tmp/ljmt-numeric-bootstrap.log`.

Ordinary ARM64 assertion profile
`9ee91aeabe90f28c6c29cf21d7eddacd6c706ee4fe4c7fdd52ed3fba6bbfd35c`
passes 53 new smoke runs: nine runners over five values with same-trace reuse,
plus eight direct NUM SLOAD prototypes. They require published FORL traces,
exact typed argument bits, C counts and errno, without test hooks. All 49
preceding held-result/scalar-table/nested-read/I64-argument/callable smoke runs,
bit operations and rooted cells/upvalues also pass. The original fractional
table-to-C-int boundary now requires successful compilation and passes.

Both stock source and original-bytecode import corpora pass 509 cases on
ARM64. Signed x64/Rosetta assertion profile
`df9ecfcc1b30286d82dcd89eccdfbb0af618f7cccf059053a07425b0eefbc961`
passes both 509-case corpora too. This remains bounded compatibility evidence,
not a complete x64 umbrella or a native Intel performance measurement.

The matrix verifies unchanged source and restores ordinary ARM64 assertion
profile `15735080c07de805aa0096f7fc02d3269ec7aca3ae22a4b8d34720dc0a85d1af`.
That runtime repeats the 53 new smoke runs. Code-sign checks pass; selected
test-helper symbols and out-of-line atomic imports are absent. Log:
`/tmp/ljmt-numeric-matrix.log` and matching ordinary/stock/x64 logs.

An ordinary follow-up probe confirms the next remaining composition gap:
`abs(fabs(t.value))` returns the correct result through interpretation but
does not compile its CALLM chain. Its NUM-identity control
`fabs(fabs(t.value))` compiles, as does a separate ordinary CALL that first
stores the NUM result in a local before passing it to `abs`. Both CALLM
admission passes still admit only identity or their existing float adaptation
at the producer/consumer boundary. Log: `/tmp/ljmt-numeric-callm-boundary.log`.

## Main checkout

Main `v2.1` was fast-forwarded to the source checkpoint and rebuilt with
ordinary bootstrap/experimental/assertion flags and no test hooks. Profile
`217e4e65b85ec0c8d493a7ca56ad6a068bfee67a3e0b743ff3298619779ed38d`
passes all 102 smoke runs above, the held/accumulated/fractional-narrowing
boundary checks, bit operations, rooted cells/upvalues, and both 509-case
stock corpora. Code-sign and atomic/test-helper symbol checks pass.
Log: `/tmp/ljmt-numeric-main.log`.

- Runtime SHA-256: `d21b7157fca77776735bc965816e9025d8f8945e3a89c2ca153d62e80e74f538`
- Archive SHA-256: `37824b11dc0d06b7760c4014eff3b732c01fbca47f638849d0c9d6c6201ba818`
- Build-signature SHA-256: `7b1a297a784fc583bd7cc244dcc5eb1493f7ea54e8e55983a1b352764731d570`

## Limits

This opens exact fixed integer argument conversions in the existing CALLXS
certificate. It does not certify arbitrary integer conversion graphs, enum
or bool conversion extensions, general CALLM adaptation, aggregate calls,
general table PHIs or arbitrary Lua/metamethod calls. The complete ARM64 port
remains experimental and incomplete.

The stack runner supplies the same Lua NUM to all eight converted positions.
Its raw captures therefore cannot distinguish every same-width signed/unsigned
argument permutation. The independent layout/source proofs and rejection of
a wrong stack operand are covered, but this fixture is not an exhaustive permutation
or mixed-source stack test.
