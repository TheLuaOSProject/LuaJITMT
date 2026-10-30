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

## Sequential leaf integration

Integrated the corrected sequential-leaf chain checkpoint `d06f48e13` and
removed diagnostic formatting residue at `4c9cc207e`. Its full Lua-call
contract passes all four profiles, including the original one-comparison
cases, two/three/four comparison positives, five-comparison refusal and
independent malformed snapshot-size/final-return-slot controls. Log:
`/Users/frityet/Scratch/ljmt-sequential-strings-integration-20261030/four-profiles.log`.
Ordinary ARM64 restore profile:
`9872504bbad39a0a7970549c9c5de1ef1ecc49fac197ed49fb164dc9d6763b81`.

The exact-source ordinary ARM64/strict-x86_64 matrix passes both stock corpora
and the full preceding workload/byte/FFI/string matrix. Its sequential probe
explicitly disables both the oracle runner and its captured leaf function,
then checks neither was patched to a JIT entry. Both architectures match all
2/3/4-decision results and expected cold resume PCs, retain native root/mcode
identity across full GC, and run with a live peer. The earlier expanded probe
only explicitly disabled the runner; this integrated evidence closes that
oracle-independence limitation. Saved matching binaries/modules, archives,
hashes and logs are in
`/Users/frityet/Scratch/ljmt-sequential-strings-ordinary-20261030/`.

The complete gate running separately at `52efb731e` does not include this
later sequential slice. Main remains at the complete `503c8ba2f` checkpoint.

## Numeric identity integration

Integrated `5837b8a31` and added independent extra-iterator-call rejection
plus the newly allowed `tonumber` wrong-callee case in the existing string
`tostring` fixture at `95b555931`. The shared exact one-argument identity proof
retains separate semantic/post-RA source replay and exact numeric SLOAD type.
Integer floor/ceil and one-argument bit conversions remain valid only for
INT identity; one-argument min/max preserve both INT and NUM identities.

The tonumber, tostring and complete Lua-call contracts all pass four profiles
at this source. Logs are in
`/Users/frityet/Scratch/ljmt-tonumber-sequential-integration-20261030/`.
The ordinary ARM64/strict-x86_64 matrix passes both 509-case corpora and every
prior workload/byte/FFI/string/sequential probe. It additionally witnesses 24
native identity rows per architecture: INT/NUM while/FORL for tonumber,
min/max, and INT while/FORL for floor/ceil/tobit/band/bor/bxor. Every row has
selected-root exits before/after full GC, unchanged code and a live peer;
the optional-base and numeric-edge semantic companion passes too.
Binaries, matching JIT modules, archives, hashes and logs are in
`/Users/frityet/Scratch/ljmt-tonumber-sequential-ordinary-20261030/`.
Ordinary ARM64 was restored with profile
`167f846de67078a70b64e782ed59231b2554674d8093b13b3080454b5086d298`.
This later integration is outside the complete gate currently frozen at
`52efb731e`; the latest completed broad production checkpoint remains
`503c8ba2f`.


## Zero-first substring integration

Integrated the bounded zero-first source normalization at `9cc11ead8`.
The complete string.sub contract passed all four native profiles, including
positive, negative-end and omitted-end zero-first modes. Log:
`/Users/frityet/Scratch/ljmt-zero-substring-identity-integration-20261030/four-profiles.log`.
Its ordinary ARM64 restore profile was
`fad7d2643dbb96f4326c3826f62de2e8cc53f74dfea3808e1bd8e7bc36dfdc22`.

The exact-source ordinary ARM64/strict-x86_64 matrix also passed both 509-case
stock corpora and every preceding workload/byte/FFI/string/sequential/identity
probe. It adds 16 native zero-first substring rows on each architecture;
interpreter oracle results, selected-root exits, code identity across full GC,
and live-peer execution agree. Saved binaries, matching JIT modules, archives,
hashes and logs are in
`/Users/frityet/Scratch/ljmt-zero-substring-identity-ordinary-20261030/`.
Ordinary ARM64 was restored with profile
`f2c9eeabb7de69be21f8f4b6e98d816ffb0d81a0e30bdea030e84b4f0b65cbb6`.

Review then corrected the Lua component's cold input boundary calculation to
use `max(1, first)`, matching the normalized source offset. The saved ordinary
zero-first probe already used this correction. The corrected component Lua
probe subsequently passed all ten fixture modes on both saved ordinary
binaries; those logs have the `corrected-boundary` suffix. This test-only edit
does not change the production revision validated above. The complete gate
running at `52efb731e` excludes this later slice, sequential leaves and numeric
identity; the latest completed broad checkpoint remains `503c8ba2f`.


## Fixed-pair raw byte snapshot integration

Integrated `569e7ff5c` and tied the Lua companion's raw-load witnesses to the
intended function at `9ebec6139`. The exact two-result `byte(s,1,2)` source
CALL now independently binds its callee copy to the guarded function SLOAD,
its argument window and two result lanes. Wrong builtin, changed callee copy,
skipped setup, result-copy bypass, lower bound, lane and footer/map mutants
are rejected in both passes. No allocator, emitter or locking change is added.

The full byte contract passes all four native profiles at this integration.
Log: `/Users/frityet/Scratch/ljmt-byte-pair-identity-integration-20261030/four-profiles.log`.
Ordinary ARM64 restore profile:
`84f644c927bb11d9d15923265d24fe9ac60d5e71113994f0cef6cd3dc95619a9`.
The exact-source ordinary ARM64/strict-x86_64 matrix passes both 509-case stock
corpora and every preceding inventory, byte, FFI, substring, sequential-leaf
and numeric-identity probe. Saved evidence is under
`/Users/frityet/Scratch/ljmt-byte-pair-identity-ordinary-20261030/`.
Its fixed-pair probe witnesses selected root 1, two raw loads and 5,249 root
exits per architecture, including post-peer-GC execution. Both architectures
match short, empty, NUL/high-byte, wrong-type and independent branch outcomes.
The probe asserts root/code identity after GC and those edge cases, rather
than only printing it. It starts the peer before compilation; starting a
thread afterward had retired the earlier exploratory trace.
Ordinary ARM64 restore profile:
`7358f693d5166ef42b8040544b5255bf1074b9027ffbf0542ac3a574c72341c0`.

The preceding complete gate at `52efb731e` passed and was promoted to main.
A new complete gate is frozen at `9ebec6139`, including sequential leaves,
numeric identities, zero-first substrings and fixed-pair byte snapshots. It
excludes the in-progress negative-first substring and child-side work.


The captured substring callee was separately audited after the fixed-pair
byte source-lineage repair. Replacing its UGET callee setup with KPRI nil is
already rejected independently by both semantic and allocated validation for
FORL, LOOP and omitted-end forms. `144843efa` retains those counterfactuals
as assertions; no production change was needed. Diagnostic and asserted
fixture logs are under
`/Users/frityet/Scratch/ljmt-substring-call-lineage-audit-20261030/`.
This test-only follow-up is outside the frozen `9ebec6139` complete gate.


## Negative-first suffix and computed math integration

At `f752aa882`, integrated `d87ab9d98` (negative literal first index with
omitted or explicit -1 end) and `20db5e293` (computed math result preservation
through the exact loop PHI). The substring fixture merge preserves normalized
zero-first boundaries and derives negative-first boundaries from `-first`.
The large -32768 result path checks HugeTab TRAVERSABLE and READY state;
named negative-first default-end modes now test their named loop form.
The math exception requires a preloop numeric CALLN, a body numeric CALLN,
exactly one matching numeric PHI use, and the exact loop-snapshot RENAME to a
preserved FP register. Other ordinary consumers keep the existing rule.

Both integrated scalar and substring contracts passed all four ARM64/arm64e
assertion/release profiles. Logs are in
`/Users/frityet/Scratch/ljmt-negative-first-math-integration-20261030/`.
Ordinary restores: scalar
`1f7216335ceed820730f4fd129ccca431915c3f60adb4625ef5fcfca87ca848c`,
substring
`879b1a55d2e5ca9b8dffbf476197012601e938eb3aa8dfd33c7294be38911310`.

The exact-source ordinary ARM64/strict-x86_64 matrix passes both stock corpora
and all previous probes, plus 12 negative-first index/arity/loop combinations
and computed-sin results in both loop forms. Saved binaries, JIT modules,
archives and hashes are in
`/Users/frityet/Scratch/ljmt-negative-first-math-ordinary-20261030/`.
The stricter `computed-math-required.lua` probe additionally requires an actual
loop root, nonempty mcode and increasing selected-root exits in each phase,
including after GC. ARM64 FORL/while observed 83/84 exits in each phase; x64
observed 5/5. Both retain identical code and match interpreted outputs. These
counts prove selected native execution, not equal compilation coverage or
performance for every input type. Negative-first probes assert SNEW, selected
root exits after peer collection and code identity for all 12 combinations.
Ordinary ARM64 restore profile:
`514842a131ac00670c11e88d40d45d87acf4c70fbc065f6dc8024a2516f7fdb4`.
The complete gate remains frozen at `9ebec6139`, excluding these later slices.


The computed-sin exit-count difference was isolated to number mode rather
than a new port defect. `lj_arch.h` defaults x64 to single-number mode and
ARM64 to dual-number mode. In the input-resolved probe, integer zero causes
79/80 guard-0 exits on ARM64 FORL/LOOP, while default x64 has one loop-end exit.
An additional strict ordinary x64 build of exact `f752aa882` with
`LUAJIT_NUMMODE=2` reproduces the ARM64 79/80 guard-0 exits and 83/84 phase
totals, with matching results and retained code across GC. That isolated
configuration comparison is saved in the ordinary evidence directory's
`x86_64-dualnum/` bundle and `*-computed-math-exit-inputs.log` files. It does
not replace the default x64 matrix or establish performance parity.


## Both-negative substring endpoints

Production source `0a04bdf1f` merges `278cabab1`, extending the negative-first
range proof to fixed negative end indices. The same string length must feed
both normalized indices, and the guarded difference must feed SNEW. The
zero-first cold-boundary fix remains intact. The integrated substring contract
passes all four profiles, including independent proof corruptions, fresh
publication readiness and HugeTab header-inclusive size boundaries. Evidence:
`/Users/frityet/Scratch/ljmt-negative-end-integrated-20261030/`.
Ordinary ARM64 restore profile:
`b8a15288a00109eaf801b23b80e6c2be48e533df54b162ae6e9488f4e69adc99`.

The exact-source ordinary ARM64/strict-x86_64 matrix also passes both 509-case
stock corpora and every preceding probe, with the stricter computed-math
native assertions now part of the matrix itself. Fourteen both-negative
endpoint/loop rows assert SNEW, selected-root execution after peer GC,
boundary/type behavior and unchanged native code. An additional empty-result
live-peer probe requires a loop root, nonempty machine code, selected exits
before and after GC and unchanged code on both architectures. Its no-peer
companion leaves JIT enabled and reproduces the distinct ARM64 UREFC refusal;
results still match the interpreted oracle. Evidence and matching binaries:
`/Users/frityet/Scratch/ljmt-negative-end-ordinary-20261030/`.
Ordinary ARM64 restore profile:
`ccee6637dd078338155ee3993cb3aa869a2b57b7c494146cba286f052cb63420`.

The full gate at `9ebec6139` has passed and its promoted main branch has also
passed an independent ordinary rebuild. The next full gate is frozen at
`f752aa882`, which excludes the both-negative endpoint slice above and the
ongoing numeric-concat, child-side and no-peer upvalue work. Full parity is
still incomplete.

## No-peer captured substring upvalue

Production source `6ccf3a269` includes the bounded no-peer normalization from
`67da4e268`: a closed, root-context read of captured `string.sub` uses the
existing rooted helper and exact fast-function/source admission. The x64 raw
upvalue path remains unchanged; ARM64 pays an additional helper read, whose
overhead has not been benchmarked. Raw UREFC admission stays closed.

The integrated rooted-upvalue contract passes all four profiles, including
private proof mutations and the strengthened peer-transition test. That
legacy contract does not restore a build itself; a separate ordinary ARM64
rebuild and transition run passed afterward, with profile
`28bc95e0783f4945c5c2824a3ee7071970851632410b77c53ee355bc42b64b79`.
Evidence: `/Users/frityet/Scratch/ljmt-no-peer-upvalue-integrated-20261030/`.

The exact-source ordinary ARM64/strict-x64 matrix passes both 509-case stock
corpora and every preceding comparison. The four previously missing no-peer
empty-substring rows now require actual loop roots, nonempty native code,
selected exits before/after GC and unchanged code on both architectures.
These rows establish selected-root execution, not full-body execution for
every guard/input path. The separate ARM64 transition test observes 40 exits
in each phase and checks a live current root before first peer activation,
its retirement, and native re-entry after activation. Binaries, modules,
hashes and logs: `/Users/frityet/Scratch/ljmt-no-peer-upvalue-ordinary-20261030/`.
Ordinary ARM64 restore profile:
`3254e2ae7826da1851590cbb114854c5132dc5c83da7b7a62d79e64d18743c5c`.
This slice is outside the still-running complete gate at `f752aa882`.

## Closed root-context upvalues on the refreshed SDK

At `74a48b1b8`, the no-peer recorder normalization is generic for closed
root-context UGETs, reusing the existing rooted helper and independent typed
admission. The root merge retains the non-vacuous peer-retirement checks,
adds native code identity through GC for both captured-function and numeric
cell roots, disables the test harness/oracles recursively before enabling
candidates, and makes byte/sub replacement functions return visibly different
results. No raw UREFC/ULOAD or unsupported operation admission was opened.

On Xcode 27.2/macOS SDK 27.2, the integrated rooted-upvalue contract passes
all four ARM64/arm64e assertion/release profiles. The ordinary ARM64 rebuild
also passed, profile
`149b8ff00a04a3d744241000c7183bb3ae13322ca7d4436f7fca510e9382d21a`.
Evidence: `/Users/frityet/Scratch/ljmt-closed-upvalue-integrated-sdk272-20261030/`.

The exact-source ordinary ARM64/strict-x64 matrix passes both 509-case stock
corpora, all preceding comparisons, nine closed-cell value/type cases and
three captured builtin cases. ARM64 witnesses use rooted helpers with no
UREFC; x64 keeps its existing raw representation. The scalar rows observe
31 selected exits after collection, builtin rows 32; these counts are
execution witnesses, not performance comparisons. The strengthened peer
transition also passes. Evidence and matching binaries/modules:
`/Users/frityet/Scratch/ljmt-closed-upvalue-ordinary-sdk272-20261030/`.
Ordinary restore profile:
`af033c43c939492cf6efae86bdb9693082562ba30e3be661efee19c9440f9f25`.

The prior full gate at `f752aa882` stopped when its SDK path disappeared;
see `arm64-sdk272-gate-restart-2026-10-30.md`. A new full gate is frozen at
`74a48b1b8`. The last promoted complete checkpoint is still `9ebec6139`.

## Numeric concatenation integration

At `68c122f2a`, bounded NUM-input TOSTR and the exact unused-result per-thread
buffer reset are integrated. The numeric CALLN loop-RENAME helper was restored
byte-for-byte to its preceding implementation, removing unreachable STR
branches left after the speculative STR-specific exception was disabled.
TOSTR uses the existing generic preserved-register/spill proof. The future-use
opcode bounds check remains. No formatter or emitter was changed.

The integrated numeric-concat contract passes all four ARM64/arm64e
assertion/release profiles on SDK27.2. Every fresh-publication run observes
79 strings under published `jit_base`, including the exact new final string
once, READY/traversable arena state, and a normal loop continuation exit.
The separate `in_native` depth counter can be zero in generated trace helper
calls and is not used as the trace-lease witness. Ordinary ARM64 restore:
`39ff8a1619fa90ff13766f1731e4ca21e919a242c8d0294124a432424e9ed938`.
Evidence: `/Users/frityet/Scratch/ljmt-numeric-concat-integrated-sdk272-20261030/`.

The exact-source ordinary ARM64/strict-x64 matrix passes both 509-case stock
corpora and all preceding probes. The numeric-concat probe now requires
self-linked roots, nonempty native code, TOSTR presence, selected exits in
both phases, code identity after GC and exact output against the interpreted
oracle, including infinities/NaN and cold wrong-type calls. Each architecture
observes 18 selected exits in both FORL/while forms. Evidence and matching
binary/module bundles:
`/Users/frityet/Scratch/ljmt-numeric-concat-ordinary-sdk272-20261030/`.
Ordinary ARM64 restore:
`e26d7ace1aa5097ec0bbfac56d3749e203a1f532841eec54268fd4488a7b3b06`.

A second complete gate is frozen at `68c122f2a` in the previously idle clean
`arm64-strings` checkout, while the earlier `74a48b1b8` SDK27.2 gate continues
in `arm64-branches`. Its evidence is under
`/Users/frityet/Scratch/ljmt-numeric-concat-complete-sdk272-20261030/`.
Both complete gates remain pending; the promoted main checkpoint is still
`9ebec6139`. Integer formatting, unary Lua leaves and child-side publication
work remain outside these frozen sources.

## Integer formatting and child-return recording integration

`ad7cc73aa` integrates the INT TOSTR admission from `beba66068`, followed by
`43bd1ca75` integrating child-return recording through `f1784218f`. These
changes are committed and pushed. The child work records and validates a
specific child-to-caller boundary, including parent generation, side trace
number, source CALL/RET, and exact frame geometry. It does not authorize a
cross-prototype side publication; that remains pending independent admission.

At frozen `43bd1ca75`, both the numeric/integer TOSTR contract and Lua leaf
contract pass all four ARM64/arm64e assertion/release profiles. The former
observes exact fresh NUM and INT strings published under the native trace
lease; the latter retains native/child-exit/certificate and mutation checks.
Evidence: `/Users/frityet/Scratch/ljmt-int-concat-child-witness-integrated-20261030/`.
The TOSTR contract restores ordinary ARM64 at profile
`660b748b951dd250151b3ff3fcd93316d3f9cad103385fda7bfa09b9e50e4e2f`; the leaf
contract restores ordinary ARM64 at
`f0120a721f657907a011148c7166bd5b96553e80e21d2673c9738751ec764d6a`.
An additional explicit ordinary restoration is recorded in the same directory.

The follow-up `351c125ad` extracts the exact frame-marker consumer and adds
valid-marker, payload-forged-as-KNUM, and wrong-slot controls. It passes the
four-profile leaf contract in the agent checkout. `1aee08aa7` removes unrelated
formatting churn without changing behavior. These later checkpoints do not
extend the frozen `43bd1ca75` combined evidence to an unrun complete gate.

## Combined integer modulo, formatting and child-witness validation

`f4ee0176d` integrates INT modulo `29eb99e92` with the integer-formatting and
child-return recording source. The merge preserves both test families in the
shared scalar fixture; its only conflicts were test-helper insertion points
and case-name selection. The combined scalar/NUM+INT publication contract
passes all four ARM64/arm64e assertion/release profiles, restoring ordinary
ARM64 at `b85955d8af84e9eb61d90978c0721661cec7cfe9b61e97b034f274f4503b7977`.

At that same frozen source, fresh ordinary ARM64 and strict x86_64 builds pass
both 509-case stock corpora and the complete prior ordinary matrix, augmented
with INT concatenation and the four native modulo rows. Logs, scripts, saved
runtime/JIT-module bundles and hashes are in
`/Users/frityet/Scratch/ljmt-modulo-concat-child-integrated-20261030/`.
Final ordinary ARM64 restoration is
`ac110df66682cfc6605041f3b8def2181d41cc7dd77bb06f0365a0f2365dec31`.

The main parity branch now promotes full-gate-verified source `68c122f2a`,
with an independent ordinary rebuild recorded through `70bc26286`. The full
gate frozen at `505069708` (integer concatenation and child-return witnesses,
excluding modulo) is still running. Later unary-leaf, character-string and
modulo-result allocation work is not included in these frozen results.

## Retained modulo results and integrated unary validation

Merged `72f03e07a` after its four-profile and fresh ordinary ARM64/strict-x86_64 checks passed; the detailed evidence is in `arm64-integer-modulo-result-preservation-2026-10-30.md`. The merge retained the INT concat fixtures and existing child-return witness work.

The integrated unary source `de8516eab` passed all four leaf-contract profiles, including the added full-width D-operand mutation that sets bit 8 while preserving the low byte. Evidence: `/Users/frityet/Scratch/ljmt-unary-integrated-20261030/four-profile.log`; ordinary restoration fingerprint `88d42e9d7f8528b2a5197d1147773a8f6ac4d182edd3048bfd3cd8ea6f071b5c`. An independent fresh ordinary unary run remains separate. These are component and integration checks, not full parity.

## Guarded CHAR formatter integration

Merged `cdcfb2551` after its independent four-profile and ordinary ARM64/strict-x86_64 checks. The shared loop-result validator combines exact CALLN/NUM, MOD/INT and CHAR-TOSTR/STR producer/backedge pairs; original numeric register-only restrictions remain. Only the CHAR string case permits the existing bounded spill proof and additionally requires one restorable source in the exact loop snapshot. MOD and CHAR mutation fixtures are both retained. A combined formatting/modulo regression run is required for this merge before promotion.

Fresh ordinary unary checks of `de8516eab` also completed independently: both 509-case stock corpora on both architectures, direct unary companion, and a separate actual-child probe with the selected caller function identity guard and numeric NEG. The actual-child probe covers alternate closure exits, original reentry, live peer, oracle and GC/code identity. Evidence: `/Users/frityet/Scratch/ljmt-unary-ordinary-de8516eab/`.

## Combined CHAR, modulo and unary checkpoint

Source `4f8f43da5` passes the numeric/integer/CHAR formatting contract across all four ARM64/arm64e assertion/release profiles. The shared scalar fixture includes both integer-MOD mutation sets alongside CHAR guard/PHI/location mutations; fresh NUM/INT/CHAR publication fixtures also pass. The restored ordinary runtime passes both modulo companions and the unary scalar companion. Evidence: `/Users/frityet/Scratch/ljmt-char-mod-unary-integrated-20261030/`; restoration fingerprint `50ce20974da2c3ad9a5f0f4d7e75b703de2852bf92586470219b6431fcf1ab32`.

The complete source-`505069708` gate finished successfully with all four M7 profiles, safepoint runtimes and ordinary restoration, and was promoted to `codex/arm64-parity` at `ba4b9932e`. Those notes are merged here. A new full gate is frozen at `4f8f43da5` in the separate `arm64-strings` checkout, with evidence under `/Users/frityet/Scratch/ljmt-char-mod-unary-complete-sdk272-20261030/`; its result is still pending.
