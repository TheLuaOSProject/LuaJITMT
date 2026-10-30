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
