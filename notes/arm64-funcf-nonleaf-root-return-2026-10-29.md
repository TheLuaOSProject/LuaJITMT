# ARM64 nonleaf FUNCF return root

The ARM64 fixed-function root recognizer now admits the bounded nonleaf case
already published by the x86-64 topology probe: a fixed-argument function
loads one captured Lua callee, makes one fixed one-argument/one-result call,
continues with the returned scalar, and returns one result. Branches, varargs,
multiple calls, heap effects, and open results remain outside this source
shape.

The semantic snapshot walk replays the root bytecode and the admitted leaf
body independently at each snapshot PC. It binds the call's callee and
argument to guarded IR, checks source-live locals against each root map, and
requires the exact returned value in the terminal `RET1` snapshot. The
post-RA walk repeats source and result reconstruction through its own
allocated-IR and snapshot view before publication. The root still uses the
existing rooted upvalue read and the ordinary scalar/leaf admission checks.

The first arm64e topology run exposed a lower-bound error in the rooted UGET
source scan. FUNCF has `J->loopref == 0`, but the old fallback selected ref 0
in the biased trace IR vector. LLDB confirmed `before=32769`, `end=T->nins=32781`,
and `J->loopref=0`; the scan then read outside valid IR references. The scan
now uses the loop reference only when it is in `[REF_FIRST, before)`, and
otherwise starts at `REF_FIRST`.

## Validation

`JOBS=2 tools/ci/arm64_jit_funcf_record_contract.sh` passed its arm64 and
arm64e assertion builds. The contract runs the topology probe in both target
profiles. The probe's `nonleaf-function-root-return` rows show a selected
root with a return trace (`nins=13`) and 81 selected-root native exits on the
bad-argument phase, after collection with a live MT peer. Standalone output
was also recorded at `/tmp/funf-arm64-topology.tsv` and
`/tmp/funf-arm64e-fixed.tsv`.

This validates the ordinary captured-call shape only. Tail calls, recursion,
open results, protected calls, and arbitrary nonleaf effects were not covered
by this initial checkpoint; the narrow tail-return extension is documented
below.

## Follow-up scope repair

The first integration attempt let the new one-result inline-frame handling
fall through the generic FORL child-exit validators. A private `C_BASE_ZERO`
mutation then passed post-RA admission. The one-result continuation replay is
now restricted to FUNCF roots; generic roots retain their prior one-result
early-return rule, and inline child frames outside FUNCF still require the
previous two-result shape. The semantic root-snapshot replay also checks that
the next map offset does not precede the current offset before subtracting.

A dedicated C fixture clones the actual FUNCF trace, prototype, IR, and
snapshots. It confirms semantic and post-RA baseline admission, then rejects
seven private mutations in each pass: returned-value binding, terminal PC,
callee-guard PC, CALL argument/result counts, captured C-function identity,
and a return-slot frame tag. It also verifies that an invalid low `loopref`
fails closed. Runtime coverage includes both a mutable captured callee and a
read-only local-function capture. Both execute the whole-body return natively
before and after collection with a live MT peer; the mutable case also changes
the captured callee and observes the exact identity-guard exit before
restoring and reusing the root. The contract's strict preflight runs in both
arm64 and arm64e assertion profiles at
`/tmp/funcf-readonly-final-contract.log`. Each profile reports five
exit-free native returns, one identity exit, 14 semantic and 14 post-RA
mutation rejections, and two low-loopref rejections across the two captures.

The ordinary no-test-helper build initially refused the read-only FUNCF case.
Inspection found helper-only conditionals that skipped root-snapshot,
prototype-PC, and frame-proof failures for non-vararg functions. These
conditionals are removed, so helper builds enforce the same checks. A narrow
FUNCF path accepts the exact completed-call continuation only when
`trace_arm64_lua_leaf_returned_call` binds it to the source CALL; while a child
frame is active, the existing frame walk also requires the recorded FUNCF
source shape. The ordinary ARM64 topology probe then passed without test
helpers: the read-only `nonleaf-function-root-return` row published a return
trace (`nins=13`, six exits) and recorded 81 selected-root exits on its bad
argument phase. Evidence is at `/tmp/funcf-nonleaf-fix-topology.tsv`.

## Direct two-result FUNCF root

The direct pure-scalar root case from the x86-64 topology probe is now admitted:
`return x*.5, x+1.25`. Its terminal `RET D=3` must name two initialized numeric
locals inside the source frame. The semantic bytecode-to-snapshot walk and the
independent allocated-source walk both derive the two result slots from the
terminal opcode and require both exact values in the final root snapshot.
The existing one-result captured-call source remains on its separate replay
path; this does not open multiple-result child calls, open-result calls, or
tail calls.

The C contract keeps a live MT peer and checks both returned values against an
interpreter oracle, native entry before and after full collection, a native
type-guard exit with interpreter error propagation, and native reuse after the
error. Its private trace clone is accepted by both proofs, while replacing the
second returned value with the input and changing the RET D field from 3 to 2
are each rejected independently in semantic and post-RA admission. A separate
post-RA mutation aliases the two simultaneously live result registers and is
rejected, while the unmodified allocation remains accepted. Across the existing
one-result and new two-result cases the fixture reports eight exit-free native
returns, one captured-callee identity exit, 16 semantic mutation rejections,
and 17 post-RA mutation rejections in each profile.

Validation passed `tools/ci/arm64_jit_funcf_record_contract.sh` with helper
instrumentation in arm64 and arm64e assertion profiles. It also passed with
`LJ_ARM64_FUNCF_XCFLAGS` set to the MCODE_TEST-only flags (no trace test
helpers); the resulting topology rows show the two-result root at `nins=5`
and 81 selected-root exits on the bad-type phase. Evidence is in
`/tmp/funcf-two-results-contract.log`,
`/tmp/funcf-two-results-mcode-only-contract.log`, and
`/tmp/funcf-two-results-mcode-topology.tsv`. The profile split was limited to
assertion builds; no release-profile claim is made here.

A fully ordinary assertion build with neither `LJ_TRACE_TEST_HELPERS` nor
`LUAJIT_MCODE_TEST` also passed the topology probe. Its runtime, archive,
generated Lua modules, relevant source files, source diff, and probe output are
preserved under
`/Users/frityet/Scratch/funcf-two-results-ordinary-20261029`. The ordinary rows
match the helper build: `nins=5` and 81 selected-root exits for the direct pair,
and `nins=13` and 81 selected-root exits for the captured one-result root.

## Terminal result-map binding

The two-result snapshot proof now binds both returned slots specifically to
the final terminal snapshot. Earlier snapshots can share the final poll IR
reference while carrying different source PCs, so counting result slots across
all maps could incorrectly combine partial evidence. The terminal map must
contain each result slot exactly once and map it to the value derived from the
terminal `RET` source; two slots may legitimately map to the same IR value.

The C contract removes the second terminal map entry while preserving a valid
map extent and its original footer, and both semantic and post-RA admission
reject that clone. It also runs `local y=x*.5; return y,y` as a separate
two-result candidate: the candidate matches an interpreter oracle and enters
the selected root natively before and after collection with a live peer, and
its cloned trace passes both admission proofs. The focused assertion contract
passed for arm64 and arm64e. Each profile reports 10 exit-free native returns,
one captured-callee identity exit, 20 semantic mutation rejections, 21 post-RA
mutation rejections, and two low-loopref rejections. The log is
`/tmp/funcf-terminal-result-contract.log`.

## Captured one-result tail return

The x86-64 topology probe also publishes the outer `FUNCF` root for a captured
Lua leaf invoked by `CALLT`. ARM64 now admits one fixed shape: the root has one
numeric formal, loads one captured leaf, copies the formal into the fixed
one-argument call slot, tail-calls with one requested result, and returns the
child's scalar result. The semantic and post-RA proofs bind the exact root
`CALLT` identity guard and argument snapshot, then independently bind the
terminal child `RET1` frame marker and returned scalar to the root result slot.
The recorder preflight confirms the current child's physical frame and exact
external Lua caller/return-PC/argument geometry after the root frame has been
replaced. Generic tail calls, variable argument counts, multi-result tail
calls, and arbitrary child effects remain refused.

The native fixture keeps a live MT peer, compares against an interpreter
oracle, enters the selected root before and after collection, checks the
non-numeric type-guard exit and continuation, then re-enters the root. Its
private cloned-trace test accepts the unmodified trace in both independent
passes and rejects four mutations in each: substituted child result,
misplaced result slot, missing terminal result map, and substituted child
function identity. Each ARM64/ARM64e assertion run reports four semantic and
four post-RA tail-mutation rejections; the combined fixture reports 13
exit-free native returns and two captured-callee identity exits. The complete
`tools/ci/arm64_jit_funcf_record_contract.sh` passed for both profiles with
the final source. Preserved profile archives, runtimes, fixture executables,
and topology output are under
`/Users/frityet/Scratch/runtime-audit-tail-20261029/final-contract-final/lj-arm64-funcf-record.jpxsvn`.
The fully ordinary ARM64 assertion build, with neither test-helper define,
also published this `captured-tail-return` root (`nins=12`, `nexit=6`) and
recorded 81 selected-root exits in its bad-argument phase. Its topology output
is `/Users/frityet/Scratch/runtime-audit-tail-20261029/ordinary-topology-final.tsv`.

While preparing the copied-trace proof, two test-only clone defects were
found and corrected. Snapshot footer rebasing now preserves the original raw
low metadata byte instead of taking the low byte of the PC address. IR copy
uses the valid biased range `[nk, nins)` rather than copying from the biased
pointer itself; the previous form read before the IR allocation and produced
matching `calls-final` crash reports. Ten direct runs of the corrected ARM64
calls fixture passed. The earlier randomized recorder-fixture segfault is a
separate observation: its saved ARM64 executable UUID has no matching
DiagnosticReport, the similarly timed ARM64e report has a different UUID,
and a same-seed comparison passed on both the parent checkpoint and
candidate. The final full contract also passed, so that earlier incident
remains unreproduced and is not attributed to the tail implementation.
