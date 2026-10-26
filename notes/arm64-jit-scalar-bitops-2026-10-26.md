# ARM64 scalar bit operations (2026-10-26)

The experimental ARM64 JIT admits the scalar 32-bit bit library in self-linked
LOOP/FORL roots. This extends the existing operation certificates and uses the
stock recorder, optimizer and ARM64 emitter. It adds no runtime lock, shared
scratch state or alternate implementation of the operations.

## Scope and independent proofs

The new producer set is BNOT, BSWAP, BAND, BOR, BXOR, BSHL, BSHR, BSAR and BROR.
Scalar ROL records as BROR of an integer NEG. TOBIT requires a NUM input and the
exact double bias `0x4338000000000000`. The stock narrowing pass also emits
wrapping integer ADD/SUB and can replace superseded overflow guards with
canonical NOPs. Both semantic and post-allocation passes validate these forms
independently, including flags, bounded earlier references, types, PHIs,
snapshots, registers and spill ownership. The FORL index/step/range proof stays
intact while allowing unrelated body arithmetic.

Fast-function entry is restricted to these scalar builtin identities and an
ordinary CALL or CALLM with exactly one trailing scalar result in the current
root. The caller frame, root prototype, return PC, argument/result extent,
recorder state and immutable synthetic fast-function bytecode slot are checked.
The existing exact identity guards remain mandatory. This does not admit
arbitrary fast functions or general multires, tail calls or nested Lua frames.

The allocation proof matches bounded stock fusions: shifted-register logic and
arithmetic, ORN/EON, shifted NOT, paired shifts, contiguous-mask UBFM,
complementary-shift EXTR, adjacent BAND/zero comparisons and doubled operands.
An unlocated value must have a matching fusion at every IR use, with actual
locations for the underlying operands. Snapshot sources are independently
checked after resolving renames.

A real constant-shift trace exposed an existing allocator behavior: a
snapshot-only NUM<-INT conversion can lack an FPR/spill because exit restoration
reconstructs it from the integer input. The certificate now admits exactly that
one-level conversion in root traces and checks the input's location at the
same snapshot, including its own renames. Every other snapshot still requires
its normal physical location. No guardless-snapshot exemption was added.

## Focused validation

`tests/t-arm64-jit-bitops.c` and `.lua` pass on the ARM64 assertion/helper build.
The fixture records first, then measures a fresh invocation; each of its 52
native cases must publish exactly once and exit exactly once through the final
loop exit. This avoids counting a first-run type mismatch as execution of the
bit-operation body. Full return tuples match the interpreter.

Coverage includes LOOP and FORL with dynamic counts -1, 0, 1, 15, 31, 32, 33
and 63; constant shifts; wrapping arithmetic; 24 live integer results and
spills; fractional/large/non-finite TOBIT inputs; and signed zero. Zero may
remain an integer and therefore need no TOBIT instruction. Thirteen additional
pure scalar cases demonstrate actual unlocated instruction fusions and check
the corresponding instruction family in emitted code. A same-typed BSWAP
substitution stays semantically valid but must fail physical admission. The complete run observed 25 fused
values, 12 rematerialized snapshot entries and 423 integer spill entries.
The generated code contains the expected W-register logic, byte-swap,
variable-shift/rotate and negation encodings.

Semantic and physical clones reject unsupported operations, types, guards,
forward operands, BROL, altered TOBIT bias, reserved registers and invalid
spill slots. An unlocated fused value remains invalid as a snapshot source.
Changing a callee after six iterations forces a real guard exit which restores
one snapshot-only NUM<-INT value and returns the interpreter's result.

Deterministic post-admission requests exercise profile exit, STOPREQ unwind,
clean native state, resumed entry and a full GC followed by native re-entry.
This is not a new remote-flush test. String coercion, cdata bit64, bit.tohex
allocation and a FUNCF tailcall remain interpreted. An idle secondary Lua
thread keeps the existing rooted lookup/upvalue paths active throughout.

The standalone Lua file also supports an ordinary runtime without C probes:
it checks the same results and published trace metadata. Only the C harness
claims exact entry/exit counts, malformed-view checks and forced lifecycle
coverage.

`tools/ci/arm64_jit_bitops_contract.sh` builds the ARM64/arm64e assertion/release
matrix and is included in the experimental gate. The full matrix and combined
regressions are pending at this implementation checkpoint. Local development
log: `/tmp/ljmt-bitops-focused.log`; the tracked fixtures are the reproducible
validation source. All temporary production diagnostic prints were removed.

## Limits

FUNCF/side bit operations, BROL, bit64/cdata, STRTO and unrelated integer or
floating-point operations remain closed. This is one bounded extension of the
experimental port; the general JIT, FFI, profiler and sustained concurrency gaps
in `README.md` remain open.

The first combined run stopped in the older generic admission fixture because
it still expected every NOP to be an unsupported opcode. The fixture now
accepts an unused canonical NOP, rejects a NOP used as a value or carrying
operands, and distinguishes valid wrapping ADD/SUB from wrong PHI/guard flags.
The complete generic admission contract passes after that test-only update;
that correction did not change production sources.

## Loop-boundary certificate correction

A subsequent review found that the shifted-operand certificate also needs the
emitter's `mayfuse` loop boundary. The first recorded iteration can contain an
unlocated shift fused into its local consumer, but that shift cannot also fuse
into a consumer after `IR_LOOP`. The use walk now tracks the consumer's active
loop boundary and applies it only to `asm_fuseopm` paths; the separate `canfuse`
transformations keep their existing rules. No emitter behavior changes.

The regression clones the real `allocated_fusion_shift_xor` trace and changes
only the body XOR's shifted operand to the unlocated first-iteration shift.
Semantic admission still accepts it. The previous physical checker incorrectly
accepted this clone; the corrected checker rejects it. Snapshots, PHIs,
locations and frame size remain untouched, and the restored original passes.
The full focused run still passes all 52 native cases with the same layout
totals, plus one independently required cross-loop rejection. This establishes
a certificate gap, not a demonstrated native wrong-code result.

The broad run at `8b605112` was deliberately stopped after its current numeric
contract completed, and its ordinary runtime restoration finished. It had
already passed all four bitops profiles and the rooted-value/callback/side
contracts reached so far; it is not claimed as a complete gate pass. Final
combined validation will use a new frozen source checkpoint including this
correction. Local before/after logs are
`/tmp/ljmt-bitops-boundary-before.log` and
`/tmp/ljmt-bitops-boundary-after.log`.

## Existing NUM fusion follow-up

The older NUM MUL-to-ADD/SUB certificate had the equivalent missing `mayfuse`
boundary. A separate diagnostic against the unchanged main runtime at
`d124b6fa` reproduced semantic and physical acceptance after redirecting only
the loop-body ADD to an unlocated first-iteration MUL. The original has two
real FMADD instructions and remains accepted after restoring the clone.

The physical use walk now applies the same loop boundary to NUM fusion.
`tests/t-arm64-jit-fusion-boundary.c` retains the diagnostic as a required
negative, with exact original results and emitted FMADD checks. It runs beside
the bitops fixture in all four assertion/release ARM64/arm64e profiles. All
four pass, as do the existing ARM64 scalar/FMA and numeric-spill contracts,
including lifecycle, snapshots and flush/reuse. The original 52 bitops cases
retain their layout totals and INT cross-loop rejection. The source change is
one additional physical-admission condition; emitter code remains unchanged.

The complete combined matrix passed at `bbf3f5ba` before this NUM tightening.
It was not repeated after the one-condition follow-up; the four-profile and
numeric regression results above describe its final validation scope. Logs:
`/tmp/ljmt-fma-boundary-before.log`,
`/tmp/ljmt-bitops-final-four-profile.log`,
`/tmp/ljmt-bitops-final-scalar.log`, and
`/tmp/ljmt-bitops-final-spills.log`.
