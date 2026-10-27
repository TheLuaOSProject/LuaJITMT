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
The frozen-checkpoint profile/umbrella/ordinary/stock matrix is still pending
at this source checkpoint.

## Limits

This opens exact fixed integer argument conversions in the existing CALLXS
certificate. It does not certify arbitrary integer conversion graphs, enum
or bool conversion extensions, general CALLM adaptation, aggregate calls,
general table PHIs or arbitrary Lua/metamethod calls. The complete ARM64 port
remains experimental and incomplete.
