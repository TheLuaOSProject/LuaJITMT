# Scalar FPMATH admission — 2026-10-28

The integrated FPMATH slice admits the ARM64 native scalar math operations
`math.floor`, `math.ceil` and `math.sqrt` for a one-argument call recorded in a
root `LOOP` or `FORL` trace. The recorder validates the exact fast-function
identity and immutable synthetic bytecode slot, caller/callee argument
geometry, and return-slot bound. The scalar IR proofs independently allow only
`IRFPM_FLOOR`, `IRFPM_CEIL` and `IRFPM_SQRT` with a prior numeric operand and a
numeric result. Their post-RA proof separately checks the same operation,
operand, result class and allocated value. Other `IR_FPMATH` operations remain
rejected because they may lower through runtime helpers.

The fast-function identity guard is verified as an exact function constant
guard. The numeric operation is admitted as a pure scalar operation, like
arithmetic, rather than being associated with a trace-global “last builtin”
marker. This permits CSE and composition of multiple math operations while
preserving independent operation and allocation proofs.

The native fixture exercises captured and parameter-passed builtin functions
while a peer MT is live, `floor`/`ceil`/`sqrt` composition in one trace, and a
floor input that exits the checked integer return path. The forced exit is
followed by a full collection and a native re-entry. Semantic and post-RA
mutations independently reject an unsupported helper-backed FPMATH operation,
a forward operand reference, and an identity guard for `math.sin`. Existing
register, spill, frame and snapshot mutations remain in the same contract. An
ordinary Lua companion compares interpreter and JIT results over fractional,
negative, zero and large values.

## Evidence

Validated on native macOS arm64 and arm64e assertion and release builds with
`tools/ci/arm64_jit_scalar_capability_contract.sh`. In each of the four
profiles, floor, ceil, sqrt, parameter-passed and composed cases published
native traces and passed the semantic and post-RA negative mutations. The
guarded floor case published again after the forced exit and full GC. The
ordinary companion matched interpreter and JIT results for all three
operations. The release fixture build omits LuaJIT assertion hooks to match
the release archive; select this with `LJ_ARM64_SCALAR_ASSERT=0`.

Follow-up edge checks compare ordinary interpreter and JIT results for NaN,
positive and negative infinity, signed zero (using reciprocal sign), negative
sqrt inputs, negative fractional boundaries, both sides of the signed int32
range, and finite values above exact 32-bit and 53-bit integer boundaries.
Native floor traces also take both the positive and negative out-of-int32
guard exits. Negative sqrt and positive-infinity sqrt inputs publish native
traces and match the interpreter with NaN-aware comparison; the negative and
infinity loop shapes can exit/re-enter repeatedly, so this is a semantic
boundary witness, not an exit-free performance claim.

## Native ABS, min/max and numeric NEG follow-up

The follow-up source slice also admits the existing ARM64 `FABS`, `FCSEL` and
`FNEG` lowerings for numeric `IR_ABS`, same-type integer or numeric `IR_MIN` /
`IR_MAX`, and numeric `IR_NEG`. ABS and NEG must carry the exact aligned
`GG_State.g.ksimd` mask load for their opcode; the post-RA proof treats that
load as an unallocated constant and requires that every use is the matching
operation. MIN/MAX operands remain in source order so the target keeps the VM's
NaN and signed-zero behavior. Fast-function admission checks exact immutable
function identity, caller geometry, and result bounds; min/max supports the
one-argument passthrough and fixed multi-argument calls.

The live-peer native fixture checks captured ABS/MIN/MAX, one- and three-arg
min/max, integer and numeric min/max, numeric NEG, exact `INT_MIN` ABS
widening, NaNs in first/middle/last argument positions, infinities, and both
signed-zero input orders. The composition witness remains the existing
floor/sqrt trace, and `math_four` now composes floor, sqrt, ABS and three-arg
min in one native loop. The source gate validates each call's exact immutable
function slot, caller argument geometry and result bounds independently; it
does not impose a trace-global fast-function count. Semantic and post-RA
corruptions cover wrong SIMD mask provenance, forward operands, illegal flags
and register/spill placement, including attempts to allocate a real mask load
in a valid FPR or free NUM spill pair. The ordinary companion compares
ABS/MIN/MAX JIT results with the interpreter across NaN, infinities, signed
zero and large boundaries.

The focused scalar fixture and ordinary companion passed on native macOS in
all four arm64/arm64e assertion/release profiles, including successful native
trace publication while a peer MT remains live. The four-profile string-length
contract also passed after combining its `IR_FLOAD` admission with the SIMD
mask proof, exercising the shared semantic and post-RA cases.

The floor/ceil/sqrt and ABS/min/max/numeric NEG slices are integrated into
`v2.1`. The combined full gate, stock corpora and independent main rebuild pass;
see the [combined checkpoint](arm64-math-byte-branch-checkpoint-2026-10-28.md).
Neither slice establishes full ARM64 LuaJIT parity or lockless-VM completion.
