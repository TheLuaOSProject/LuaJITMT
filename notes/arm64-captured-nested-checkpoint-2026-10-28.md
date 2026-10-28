# Captured nested-call checkpoint

Port-branch production source `f040711ab` combines bounded negative dynamic
`string.byte` indices, ordinary parameter-passed nested calls, and captured
nested callees. The captured workload matches the inventory topology: the
loop captures `f`, which reads captured `g`, calls it, and multiplies its result
by one half. The existing rooted upvalue helper supplies the function value;
no raw shared upvalue cell is dereferenced by generated code.

The independent semantic and post-register-allocation passes bind helper
function roots, source PCs, upvalue indices, caller arguments, function
identity guards, return values and snapshot geometry. Review added explicit
IR constant-boundary checks before indexing and prototype-membership checks
before reading source bytecode. Positive dynamic function aliases and negative
unrelated identities are tested separately.

## Validation

- The permanent leaf contract passed ARM64 and arm64e assertion and release
  profiles, including its newly wired nested C fixture. Each profile observed
  one single-child snapshot in the parameter case and six in the captured
  case; neither case exercised a two-child snapshot chain. Native identity
  exits, exact trace reuse after collection, live-peer execution, independent
  malformed-source rejection, and all prior leaf fixtures passed.
- The rooted-upvalue contract passed all four profiles, including existing
  closed-cell reads/writes, source continuations, GC retention and concurrent
  publication.
- Ordinary ARM64 and signed x64 each passed 509 source and 509 imported stock
  cases, their ordinary Lua companions, and all 260 inventory comparisons.
- The sampled ARM64 native-root exit witness count is now 16 of 20 rows,
  versus 18 on x64. Captured nested calls add the new ARM64 witness. Lua
  varargs and multiple results remain the sampled differences; iterator and
  I64 rows remain inconclusive on both targets. These observations do not
  establish full-body compilation or complete architecture parity.

Evidence is under
`/Users/frityet/Scratch/ljmt-negative-nested-20261028/`: `captured-four-profile.log`,
`upvalue-four-profile.log`, `ordinary-matrix.log`, both architecture inventory
TSVs, and their four stock corpus logs. The preceding focused negative-byte
oracle and parameter-nested proofs are recorded separately in this directory.

The complete experimental gate and combined M7 matrix were last run at
`60d304989`, before this group. Main remains at `49523abd`; the earlier
intermittent JIT-enabled harness TDUP failure is still unresolved and has no
runtime fix. Deeper live snapshot chains, general calls/returns, trace
topology and FFI restrictions remain work toward the parity goal.
