# Aligned HFA stack groups (2026-10-29)

The stack-HFA classifier now uses an ABI property instead of one explicit
two-float shape: all eight FPR arguments must already be consumed, and the
HFA must start and end at an eight-byte boundary. This adds four-float and
one/two/three/four-double groups to the existing two-float support. It needs
no emitter change. Each member's ordinary scalar store has the exact size and
offset from the independently validated signature plan; the next argument
begins at the same offset as the interpreter ABI.

Partial-register-bank overflow remains closed because scalar argument
lowering cannot move an entire group to the stack when some FPRs remain.
The one-float and three-float stack shapes remain closed because they require
group padding beyond their member stores. An unaligned preceding stack
payload remains closed too. Existing exact CType/member-offset checks and
source cdata lifetime validation are unchanged.

The aggregate contract passes all four ARM64/ARM64e assertion/release
profiles for all five added shapes. Each has eight preceding doubles and a
trailing double, lane-weighted results, exact C effect counts, native finish
and selected-root exit checks, full-GC native reuse, alternate-call fallback,
and private semantic/post-RA carrier/source/map/allocation mutations. It
also checks the ABI offsets explicitly and rejects three-float and partial
FPR-bank stack signatures. The test resets standard hotloop counters before
each independent stack probe; prior rejected-call penalties must not carry
into a new bounded warmup. Earlier missing HFA-root fixture logs are retained;
their precise counter state was not captured, so no production liveness fix
is claimed.

Ordinary binaries without test helpers compare each shape with an
interpreter-only oracle and an attached peer. New ARM64 builds publish one
selected loop root with two CALLXS operations and observe 52 selected-root
exits per shape. Each executes exactly 2,304 primary calls and 64 alternate
calls, with equal results before/after GC. The prior ARM64 build has no root
for these shapes. In the matching x64 probe only the one-double struct has
a native root; the other four also fall back on x64. Therefore only the
one-double row establishes an ARM64-only compilation gap in this sample.
The other rows validate the general ABI rule, without claiming a measured
x64 native deficit. Strict Darwin x64 compilation of the touched shared
call/assembler sources and C fixtures passes.

Evidence is `/Users/frityet/Scratch/ljmt-wide-hfa-stack-20261029/`:
`five-shape-contract.log`, `baseline-five-arm64.log`,
`baseline-five-x64.log`, `ordinary-validation.log`, `probe.lua`,
`fixture-final.dylib`, and immutable `arm64/` and `x86_64/` bundles and probe
logs. The baseline bundles come from integer-stack checkpoint `be1bd66b0`.
Source and SHA-256 manifests record the final tested files. Earlier four-shape
logs are diagnostic history, not the final five-shape result.
