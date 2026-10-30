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
The full gate at `8123b7a3d` subsequently passed, including all four M7 FFI
profiles and final VM/recorder safepoint runtime checks. Its outer runner
returned `gate exit=0; restore exit=0`. Evidence is in
`/Users/frityet/Scratch/ljmt-byte-snapshots-forl-complete-20261030/`;
ordinary assertion ARM64 restoration profile is
`f494b9ee6ebf7e0f479566b7ce9fa9bc266097c454f90812f5b903d52dc0fb3a`.

Mixed-sign multiple byte ranges (`77611f594`) are integrated at `7d8370786`.
The semantic header insertion conflict was resolved by preserving the
complete U8 snapshot and mixed-sign proof pairs from their immutable parents;
their bodies were mechanically compared after the merge. The combined byte
contract passes ARM64/ARM64e assertions and release. Its source record and
log are `mixed-sign-multiple-integration-source.txt` and
`mixed-sign-multiple-integration-four-profiles.log` under
`/Users/frityet/Scratch/ljmt-wide-byte-ffi-complete-20261030/`.
Ordinary assertion ARM64 restoration profile is
`7902fce7b6f0088462b593c270b292d8b91cd3e2ea1b260cb329acf75e3e1217`.
This later addition is not part of the completed `8123b7a3d` full gate.

Positive literal substring allocation (`aca64562a`, ordinary evidence
`c8dc88d3f`, fresh-publication coverage `c74c9ee21`) is integrated at
`4b494fcc1`. Both header merges were clean. The component passes all four
profiles, both ordinary 509-case stock corpora, 260 workload checks and
sixteen substring rows per architecture. The test-only publication hook
also positively witnesses newly interned strings from native while-loop
code in every profile, with the new object's READY bit set. See
[`arm64-jit-string-sub-constant-2026-10-30.md`](arm64-jit-string-sub-constant-2026-10-30.md).

A separate read-only allocation review found no remaining blocker: the
existing SNEW emitter prepares the snapshot before `lj_str_new`, the source
formal remains rooted while the JIT/native thread's stack capacity is scanned,
and the allocated pass includes SNEW in live-across-call checks. No ordinary
allocator, emitter or locking behavior changes. The combined source at
`4b494fcc1` is frozen separately for the next complete gate; until that run
passes the promoted complete checkpoint remains `8123b7a3d`.

The first combined gate at `4b494fcc1` stopped at the synthetic malformed
SNEW case in `t-arm64-jit-ir-admission.c`: it still rejected the instruction,
but now reports an operand-proof failure rather than an unsupported opcode.
The fixture expectation is updated to that reason, retaining the rejection.
The failed full log and successful ordinary restoration remain in
`/Users/frityet/Scratch/ljmt-mixed-byte-substring-complete-20261030/`.
This failed attempt is not counted as a complete gate pass.

The retry also includes the reviewed string-input `tostring` identity path
through `9e37151c`. Its independent proof now requires the terminal saved cell
to contain the exact guarded CALL argument, rejects every substituted
admitted builtin and checks cell overwrites and control-flow bypasses. The
component's final four-profile run passes. A preflight merge conflict was
resolved by retaining both `FF_string_sub` and `FF_tostring` synthetic-bytecode
checks. The integration adds `string.sub` to the tostring replacement
mutations and wires its four-profile contract into the complete gate.
Source `503c8ba2f` is frozen for the retry and a matched ordinary matrix.
Retry evidence is in the preceding full-gate directory under
`retry-503c8ba2f/`; the matrix is in
`/Users/frityet/Scratch/ljmt-mixed-byte-substring-tostring-ordinary-20261030/`.
Neither run is recorded as passed until its final result is available.

The ordinary matrix at `503c8ba2f` subsequently passed on both targets.
ARM64 assertions and strict bare-`-Werror` x86_64 assertions each pass both
509-case stock corpora, 260 workload semantic checks, topology/expanded
probes, the preceding fixed/dynamic byte and twelve FFI-index rows, all
eighty mixed-sign byte rows, sixteen substring rows and both string-tostring
forms. The raw-U8 live-peer test still witnesses 84 selected-root exits and
unchanged code after GC. Ordinary tostring observes 167/159 selected while/for
root exits on ARM64; both architectures retain code identity and match the
interpreted oracle with a live peer. The matrix directory retains matching
runtimes, archives, JIT modules, source revision and hashes. Ordinary ARM64
restoration profile is
`ac5a80d62d2f8408e8a33ed3836874d93342da7efb44ae1ce5515064a4a04308`.
The complete gate remains a separate running validation.

The later negative-literal substring extension (`39dceb685`) is merged
separately from that frozen gate. Review found no production blocker: both
passes derive a nonpositive end adjustment from the actual signed literal,
bind the same string length, require checked subtraction of a nonzero start
offset and prove a nonnegative width before STRREF. The integration contract
passes all four profiles, with additional tests for exact zero-length and
adjacent boundaries, invalid physical placement of the dynamic width, and
READY publication of all nonempty negative-range result lengths. The native
negative-while publication counts are 3, 39, 39 and 3 respectively.
Evidence, source-parent and the integration test patch are in
`/Users/frityet/Scratch/ljmt-negative-substring-integration-20261030/`.
Ordinary assertion ARM64 restoration profile is
`c411fa90194ed66fd063c4f1a9ffa72624dc66e295bcea42ad4e4348d6bf0dbd`.
This later extension is not covered by the running `503c8ba2f` full gate.

The full ordinary matrix at `ea0db2046` also passes. Both ordinary ARM64 and
strict bare-`-Werror` x86_64 pass both 509-case stock corpora, the preceding
workload/byte/FFI/tostring matrix, sixteen positive substring rows, twelve
negative substring rows and six additional extreme-literal rows. The latter
combine starts 1/32767 with ends -1/-32768 and use 72,000-byte strings;
selected-root exits, exact empty/adjacent boundaries, retained values and
code identity after GC all pass with a live peer. Evidence and immutable
matching runtimes are in
`/Users/frityet/Scratch/ljmt-negative-substring-ordinary-20261030/`.
Ordinary ARM64 restoration profile is
`e0faf4ac7609028ce632b43148799b24b8f4e0c9319da36d81fd70345deb5ab0`.

## Omitted-end substring integration and repeated guard footers

Integrated `0d4f49c90` at `4c022da0f`; production/test checkpoint `52efb731e`
adds omitted-end substring calls and strengthens every repeated CALL-guard
footer. The new malformed-footer fixture initially exposed that a later
invalid resume PC could be skipped after an earlier valid guard had matched.
Both independent passes now reject that case. Tests mutate the string-copy
resume PC and low footer tag independently in semantic and allocated maps,
with unmodified private-owner controls, for both C3 and C4 arities.

The first fresh-publication extension used identical samples for FORL and
LOOP in one process; FORL had already interned the later samples. Inputs now
include the loop kind, preserving the requirement for fresh native
publication. Both earlier failing logs are retained, without weakening the
assertions, in
`/Users/frityet/Scratch/ljmt-default-substring-integration-20261030/`.
The final four-profile log passes positive, negative and omitted-end ranges;
the latter reports fresh native publication counts 1, 37, 36 and 40.
The ordinary ARM64 restore profile is
`72500d79fa1f97038a6326c91f1a9bc693ca5e25d15741f30a6e545c38815ae4`.

The exact `52efb731e` ordinary matrix passes on ARM64 and strict x86_64,
including both 509-case corpora, 260 semantic checks, the prior byte/FFI/
string/tostring matrix, all four omitted-end cases, six negative literal
extreme cases and six omitted-end extreme cases (first=1, 2, 32767 under
FORL and LOOP). The native probes retain interpreter oracles, live peers,
selected-root exits and code identity across GC. Immutable binaries, matching
JIT modules, archives, hashes and logs are saved in
`/Users/frityet/Scratch/ljmt-default-substring-ordinary-20261030/`.
Ordinary assertion ARM64 was restored with profile
`e095cba669dc8eb8e785e0d09b6908887ba0a8ac0b2e35b1a8f1c56eb81b074b`.
The complete gate is starting separately at this frozen source; the verified
main production checkpoint remains `503c8ba2f` until that gate completes.
