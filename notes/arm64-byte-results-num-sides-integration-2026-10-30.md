# Byte result windows and numeric FORL children

The integration branch now contains fixed multiple `string.byte` results
(`96ff1fb8a`), dynamic positive/negative end proofs for those result windows
(`4be9406fa`), and the reviewed NUM FORL root-child implementation
(`58aa53554`). The latter fixes constant-payload validation to read the actual
IR array instead of a temporary one-word copy. The byte changes retain a
separate bounds proof for every loaded byte and leave raw U8 snapshot values
closed.

At integrated source `eaaf15231`, the side admission contract passed native
arm64 and arm64e assertion and release profiles. It restored ordinary ARM64
assertions without test helpers, profile
`b3f9a827f1e5a16c8701c962c47387f9a20e38e051b77576e68f0e7394011013`.
Evidence is in
`/Users/frityet/Scratch/ljmt-wide-byte-ffi-complete-20261030/`:
`numeric-sides-integration-source.txt` and
`numeric-sides-integration-four-profiles-explicit.log`. An earlier invocation
with an unsupported profile selector stopped before building and is not a
validation result.

The complete gate on earlier integration `d0ee5eb8c` stopped in the ARM64e
assertion cdata-call recorder race fixture: the worker did not reach the
expected post-recorder VM capture seam. Its ordinary ARM64 restoration
passed. Three separately rebuilt replays with the same source and profile
then passed. The sticky atomic pause protocol prevents a reached pause from
being missed; the fixture did not distinguish a missed recorder attempt from
an armed hook that never reached VM capture. Test entry and diagnostics are
being investigated; the three passes do not supersede the failed full gate.
See `complete-gate.log`, `gate-restoration.log`,
`capture-race-rebuild.log`, `capture-race-rebuilt-{1,2,3}.log`, and
`capture-race-restoration.log` in the same evidence directory. The earlier
`cdata-race-replay-*` files only contain attempts to invoke already-removed
temporary executables and are not test runs.

The main parity branch has subsequently promoted the full-gate-tested
`789c1e229` checkpoint described below. Later focused results are not a full
x86_64 parity claim.

## Empty-arm integration and ordinary matrix

Source `50eecd54d` also merges the bounded empty-arm leaf from `1b9018141`.
The complete Lua-leaf contract passed all four profiles, including the new
join-map checks, and restored ordinary ARM64 assertions (profile
`d1925b0b5699d81f61dc401eaa97a0b26913a7065266faabb6f3331e333f4d27`).
The source and log are `empty-leaf-integration-source.txt` and
`empty-leaf-integration-four-profiles.log` in the directory above.

The same production and tests, with notes at `8b3cf5a57`, subsequently passed
an ordinary ARM64 assertion build and strict bare-`-Werror` x86_64 assertion
build. Each passed all 509 stock source cases, all 509 original v2-bytecode
cases, the 260 workload semantic checks, topology probes, and ten fixed plus
forty dynamic-end multiple-byte native witness rows. Eighteen of the twenty
workload rows had native root-exit witnesses on each architecture. The expanded
inventory witnesses the empty-arm root on ARM in its first two phases; it
still exposes the sequential two-condition leaf gap and does not establish
native execution in every input phase.

Evidence and frozen ordinary executables, matching JIT modules, archives and
hashes are in
`/Users/frityet/Scratch/ljmt-byte-results-num-sides-ordinary-20261030/`.
`ordinary-matrix.sh` finished with `ordinary matrix passed at 8b3cf5a57`.
The ordinary ARM64 restoration profile was
`a2f8c31052621c9c536f3b876d51ebe50f0b86bbfffe423808583bc175083c15`.

## Dynamic first/end integration

The capture-fixture correction was integrated at `789c1e229`, the source for
the new full gate in
`/Users/frityet/Scratch/ljmt-byte-results-num-sides-complete-20261030/`.
That gate passed with `gate exit=0; restore exit=0`. Its ordinary ARM64
restoration profile is
`b875c2070dd756928f88b4ac421837f3d7bc5a3d72f1cc2073abd750add5e27c`.
Its source excludes the following later byte extension.

Source `0f22ba712` merges `eec23e603`: fixed multiple byte results with dynamic
positive first/end parameters and the exact stock adjacent-ADD/STRREF fusion
proof. The integrated byte contract passed all four profiles, including cold
NaN/infinity/extreme-bound cases and independent allocation mutations.
Evidence is `dynamic-start-multiple-integration-source.txt` and
`dynamic-start-multiple-integration-four-profiles.log` in the earlier
wide-byte-FFI directory. It restored ordinary ARM64 assertions, profile
`af78e57c5048f444e8e1a7d9c6ce09eab3cad42e37d6e6097bc95e2c8f69bf1a`.
The component note records its ordinary forty-row matched ARM64/x86_64 probe
and both 509-case stock corpora on each architecture.

## Nonnegative FORL index arguments

Integrated source `140d1f21d` adds the source-proven FORL index widening from
`069c49fdd`. It admits the optimizer's non-sign-extending I64/U64 conversion
only for independently proven nonnegative induction values, including source
control-flow, initialization, hidden-slot writes and exact argument uses.
Explicit CSET writes and invalid source opcodes are rejected. The native
fixture checks the actual C argument bits, including the ninth stack argument.

All 27 combined FFI fixtures pass on ARM64 and ARM64e assertion and release
profiles. Evidence is `forl-index-integration-source.txt` and
`forl-index-integration-four-profiles.log` in the wide-byte-FFI directory.
The ordinary ARM64 restoration profile is
`7c71a4675a94dfc2063e8668a5f12de5829b54fbb705927f5dc8802d7c6d3017`.

The same source passed the ordinary ARM64 assertion and strict bare-`-Werror`
x86_64 assertion matrix in
`/Users/frityet/Scratch/ljmt-forl-index-byte-ordinary-20261030/`.
Each architecture passed 509 source and 509 original v2-bytecode cases,
260 workload semantic checks, topology probes, ten fixed, forty dynamic-end
and forty dynamic-first/end multiple-byte rows. Eighteen of the twenty
workload rows have native root-exit witnesses. All twelve FFI index rows
match their result and call-count oracles; the direct I64/U64 loop rows
require a selected native root and eight observed exits both before and
after GC. Frozen executables, matching JIT modules, archives and hashes are
retained alongside the logs and `ordinary-matrix.sh`.
The matrix completed successfully and restored ordinary ARM64 assertions,
profile `989b7fa27cbc79d1a765557428f7d18de999824a73e91ce86760e4b0146fcc0b`.
This combined source is newer than the full-gate checkpoint `789c1e229`.

## Negative bounds and raw byte snapshots

Source `4f08d4b13` integrates the reviewed negative-dynamic-first/end multiple
byte proof from `e43f3b1f0`. Each normalized bound is tied to its exact source
parameter and string length; the sign, base, checked width and per-byte
displacement guards are replayed independently. Review added acquire reads
in the allocated pass and explicit rejection of forward width references and
negative displacements. The integrated byte contract passed all four profiles,
restoring ordinary ARM64 with profile
`19b8b38bb9d8bddee08c0b7f23677b6e6c97aa1174e562984554a8092ee8da62`.
Evidence is `negative-start-multiple-integration-source.txt` and
`negative-start-multiple-integration-four-profiles.log` in the wide-byte-FFI
directory above.

Source `0a2ad9886` additionally integrates raw U8 snapshot restoration from
`240e0668c`. A snapshot may restore a byte only into the exact one-result
dynamic-positive-index CALL output slot or its immediate copy. Both validators
reject source writes that clobber it, range writes, jumps bypassing its
definition, unrelated slots and invalid allocated locations. Existing integer
snapshot restoration handles the zero-extended byte; no emitter or restoration
change was needed. The merge preserves both complete new proof pairs exactly.

The combined byte contract passed all four profiles, including native
execution, peer activity, GC, boundary exits and independent source/map/layout
mutations. Evidence is `u8-snapshot-integration-source.txt` and
`u8-snapshot-integration-four-profiles.log`; ordinary ARM64 restoration profile
is `cf33d7e6c672ab0d111d8413b05f30264c2b0307430c9aabcace424da4d64b47`.
The component's final ordinary live-peer probes witness both architectures'
selected XLOAD loop, 84 exact-root exits and unchanged code identity after GC.
This snapshot proof does not yet cover raw bytes from multiple-result CALLs.

The combined ordinary matrix passed at notes/merge revision `e9ab0f3e0`, whose
production and fixtures match `8123b7a3d`, now frozen for the next complete
gate. ARM64 assertions and strict bare-`-Werror` x86_64 assertions each pass
509 source cases, 509 v2-bytecode cases, 260 workload semantic checks,
topology/expanded probes, the existing ninety fixed/dynamic-positive byte
rows, forty dynamic-negative byte rows, the live-peer raw-U8 snapshot witness
and twelve FFI-index rows. The matrix and immutable binaries are in
`/Users/frityet/Scratch/ljmt-byte-snapshots-forl-ordinary-20261030/`.
It restored ordinary ARM64 assertions with profile
`a06917bc816592b6e6fce721e780f8ab7b0a063ea72ee0a5f8ed11878cc157c9`.
The full gate at `8123b7a3d` is separately in progress; focused and ordinary
matrix success does not replace its result.
