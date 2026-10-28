# Fixed two-result Lua leaf calls on ARM64

## Scope

The bounded leaf-call recorder now admits a pure Lua leaf returning exactly
two fixed values when the caller uses a fixed `BC_CALL` result window. The
source leaf ends in `BC_RET` with `D=3`, and the caller uses `BC_CALL` with
`B=3`. The argument field `C` remains independently checked against the
recorded incoming argument count and the child frame's normalized parameter
window.

Open result counts, padded result counts, variable returns, and multi-result
early-return leaves remain refused. The admitted multi-result case is a
terminal fixed return with both source result slots defined.

## Proof and restoration

The source recognizer checks the terminal return opcode, result slots, and
frame bounds. Recorder preflight and call setup bind the exact source result
count to the caller's `B` field while preserving the existing `C` argument
checks. The semantic validator replays the source return and caller
continuation, requiring each returned value to reach its corresponding caller
slot. The allocated post-RA validator reconstructs the same result window and
source return independently, then checks snapshot values and allocated
restoration state. Direct two-result leaf calls use the bounded inline-frame
snapshot form (zero child ID) and are validated from its exact `SNAP_FRAME`,
callee identity, frame link, source return PC, and both mapped results.
Captured/nested call replay remains one-result; two-result leaves containing
an upvalue read or nested call are refused.

The native fixture uses `local a,b=fn(i+.5); sum=sum+a+b`, compares against
interpreter-only peers, forces identity exits with alternate leaves, reuses
the same trace after garbage collection, and mutates the second result
binding, caller result count, return source, child PC, and frame geometry.
The ordinary Lua companion verifies trace publication and exact trace reuse;
it does not claim native execution. The nested-call companion also keeps the
same selected self-linked root through identity replacement, full GC, and a
bounded concurrent equivalent-callee writer. Its trace selector retries
`jit.util.traceinfo` when the safe read lease is temporarily unavailable: raw
bytecode still named the same JFORL ID and a subsequent read showed the root
self-linked. It fails if the raw selected ID changes or the trace remains
unavailable after bounded retries.

## Validation

Evidence is under `/Users/frityet/Scratch/ljmt-leaf-multires-20261028/`.
The final frozen-source four-profile contract is
`final-four-profile.log`. It passed ARM64 and ARM64e in assert and release
profiles. Each profile reported `native=16`, `semantic-negative=100`,
`postRA-negative=108`, and `cold-retries=0`; the ordinary companion reported
13 publications, 13 exact-trace reuses, and three refusal shapes.
The two exact `bench/jit-parity.lua` inventory samples also passed under the
experimental ARM64 profile: `leaf-multires` and `nested-leaf` each reported 13
semantic checks, one live root, and 13 observed exits from that root. Logs are
`integrated/inventory-slice.log` and `integrated/restore-default.log`.

The targeted CALLXS regression check is `callxs-regression-check.log`. Its
arm64-assert run passed `callxs-admission`, `callxs-table-admission`, and
`callxs-table-lifecycle` after restoring the CALLXS snapshot-slot equality
check.
