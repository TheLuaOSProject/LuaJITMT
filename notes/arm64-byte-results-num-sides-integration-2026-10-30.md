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

The main parity branch remains at its full-gate-tested `ff86407a1` production
checkpoint. Later focused results are not a full x86_64 parity claim.
