# Numeric C results feeding fixed integer CALLM arguments

## Diagnosis and implementation

The preceding numeric-argument checkpoint compiled a stored Lua NUM passed
to a fixed C integer parameter, but declined the direct composition
`consume(produce(t.value))`. An ordinary control using `fabs(fabs(...))`
compiled; `abs(fabs(...))` interpreted correctly but did not compile.
The red probe is `/tmp/ljmt-callm-numeric-red.log`, with IR in
`/tmp/ljmt-callm-numeric-red-ir.log`.

Both ARM64 CALLM admission passes now bind the final fixed integer argument
to the completed numeric producer. Double producers use their NUM result;
float producers require the exact FLOAT-to-NUM widening retained by the Lua
frame. Width, signedness, compact Darwin stack placement, adjacent CALL/CALLM
source geometry and the saved caller argument remain independently checked.

U32 results require a separate optimized shape. The recorder retains an
exact NUM-from-U32 result for Lua snapshots, while folding the next C integer
argument back to the raw U32 CALLXS. The independent decoders recognize only
the observed low-word, small-width and zero-extension conversions, including
the U64 carrier whose conversion mode encodes I64-from-U32. They recover the
unique original NUM result, certify every native carrier use as a fixed
integer argument, and reject those carriers in Lua snapshots or unrelated
operations. The raw U32 result remains absent from Lua snapshots. Promoted
variadic suffixes retain their existing Lua-value rules.

The stack fixture also exposed stock constant representation: an unsigned
four-byte argument may use a KINT with the ordinary INT tag. That exact
constant carrier is admitted; its source still has to match the saved Lua
argument. No speculative constant NUM-to-integer equivalence was added.

Two malformed-view checks exposed additional admission gaps. A recognized
numeric argument whose membership proof failed could previously fall through
to the generic U8/U16 conversion rule. Both passes now reject that recognized
but invalid chain. NUM and FLOAT return results also now require their exact
restorable caller slot in the native-leave DONE snapshot. FLOAT results bind
the exact widening, and the allocated pass separately checks that the Lua
result survives native leave.

All production changes remain in `src/lj_asm_arm64_admit.h`. Recorder,
emitter, shared VM/GC2, native-call protocol and x64 production code are
unchanged. The changes add compilation-time proofs, not execution-time locks.

## Focused evidence

The runtime fixture has 48 distinct FORL prototypes: double, float and U32
producers, all eight signed/unsigned integer argument kinds, and unary plus
12-argument stack forms. The stack form has eight distinct integer register
sentinels followed by U8, I16, U32 and the converted final value. Every
sentinel is checked at the C boundary.

Each case runs a base fraction, 15 width-specific values, a forced producer
POSTCALL exit and reuse, then a forced consumer POSTCALL exit and reuse:
960 required runs of 24 calls to each C function, including 96 forced exits.
Any additional cold attempts receive the same effect checks. Values
include negative zero, signed fractions, integer width boundaries, huge
values, NaN and infinities. Separate permanently interpreted prototypes
provide the exact producer-input and consumer-argument bit oracle. A native
finish witness must stay zero during each oracle run, and every tested C
invocation checks the expected bits. Counts, errno, frame/trace/callee identity,
native pins, XSAVE and callback cleanup are also checked.

Cold recording can abort without a Lua error while another runtime activity
changes the recording state. The fixture allows eight cold attempts and
re-arms the hotloop threshold, checking argument bits, exact counts and
cleanup even for unsuccessful recording attempts. Actual native runs and
same-trace reuse still require their native witnesses. Lua errors fail
immediately. The admission fixture mutates each authentic trace immediately
after its positive checks, before warming another case.

The admission fixture accepts 48 semantic and 48 allocated views and rejects
43 semantic and 46 allocated mutations. These cover original source and
producer identity, exact conversion modes/types, FLOAT/U32 widening, ABI
signedness, wrong fixed sentinels, exposed native carriers, result/input
snapshot restoration, source CALL/CALLM pairing, register bank and spill
alignment/extent. It is registered with the runtime fixture in the expanded
22-fixture table-call contract and the broader M7 scalar FFI suite.

Both new fixtures pass the ARM64 assertion profile in
`/tmp/ljmt-callm-numeric-focused7.log`. The ordinary runtime separately passes
256 compiled smoke runs without test hooks: five values across all 48
CALLM prototypes, plus 16 stored-U32-result controls. These require published
loops, exact argument bits/counts/errno and same-trace reuse. Development log:
`/tmp/ljmt-callm-numeric-ordinary-development.log`.

The narrowed-carrier snapshot negative was checked against both old fallback
paths separately. Removing the semantic rejection admits mutation 16; keeping
that fix but removing the allocated rejection admits the same mutation in
the allocated pass. Both counterfactual runs fail as intended. Logs:
`/tmp/ljmt-callm-numeric-snapshot-counterfactual.log` and
`/tmp/ljmt-callm-numeric-postra-counterfactual.log`. The complete candidate
header was restored byte-for-byte afterward.

The first complete contract run at `74fddbe6` reached an obsolete positive
in the older CALLXS admission fixture: it replaced the double C result in
the completed-call snapshot with the loop index and expected acceptance.
That now correctly fails the exact result proof. The fixture requires
rejection for the double profile and independently checks the semantic
pass; its existing INT behavior remains unchanged. The corrected older
fixture and both new fixtures pass together in the ARM64 assertion profile
(`/tmp/ljmt-callm-numeric-legacy-snapshot.log`). No production change was
needed for this correction.

## Full-gate failures and scalar fixture recovery

At `56befe28`, all 22 focused fixtures passed all four ARM64/arm64e
assertion/release profiles. The first complete experimental run then timed
out after 60 seconds in the older `t-arm64-jit-table-i64-args` fixture, with
no assertion or thread sample. Thirty isolated repeats passed, and that
fixture passed all four profiles on the next full-gate attempt. The original
timeout remains unexplained; later passes do not identify or repair its cause.
Logs: `/tmp/ljmt-callm-numeric-experimental-i64-timeout.log` and
`/tmp/ljmt-i64-timeout-repro.log`.

The second complete experimental attempt reached its final arm64e release
M7 profile, where the older scalar-table fixture returned the correct Lua
result and errno but saw no native finish during its first cold call to
`__table_scalar_run_v_double`. Fifty isolated runs passed with additional
recording diagnostics, so the original abort reason was not recovered.
Logs: `/tmp/ljmt-callm-numeric-experimental-scalar-cold.log` and
`/tmp/ljmt-scalar-cold-repro.log`.

That scalar fixture now permits at most eight attempts for a fresh,
non-forced runner. It never flushes traces, checks the exact result, errno,
total and selected-helper counts plus native-state cleanup on every attempt,
and still requires an authentic published trace and its native witness.
Pre-existing trace reuse and forced post-call exits remain single-attempt
checks. A deterministic test interrupts cold recording at the target
prototype's TGETV using RECERR, requires an interpreted-only first attempt,
then requires native execution on recovery. This synthetic interruption is
not evidence of the cause of the original sporadic failure.

The corrected fixture passes the arm64e release profile with one injected
abort and one cold retry. A counterfactual that disallows the retry fails
at the missing native witness as intended. Logs:
`/tmp/ljmt-scalar-cold-fixed.log` and
`/tmp/ljmt-scalar-cold-counterfactual.log`. This changes only the test.

## Frozen checkpoint validation

Source checkpoint `51c923747ff0ca4bff4b801ef775c553ea918024` passes all 22
focused fixtures in all four profiles. Each includes the 960 required native
runs and 96 forced exits, plus 48 semantic and 48 allocated positives and
43 semantic and 46 allocated negatives in the new CALLM fixtures. The scalar
fixture separately reports one injected abort and one cold retry in each
profile.

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `1eabb488c3a5a50b73b50acc174aee8c58ec41ca22a73d9596d49906369cb87d` |
| arm64e/BTI assertion | `9db62191bafc25558c47ed5a8a5cddb058c1640185f62ef6f38066199b368b91` |
| ARM64 release | `26fb0ddc5c7bbb41a7ec0111a8dacb34fed557fe3f9551d0f4ada9fe3282e739` |
| arm64e/BTI release | `7127e1fa3471a31df0a371f47fc4cfe3e0971bd9785280ee14a115c111791fba` |

Both complete gates then pass on that unchanged checkpoint:

- `tools/ci/arm64_jit_fail_closed_gate.sh`, including all four broader M7
  scalar FFI profiles, rooted effects, construction, GC lifetime, trace
  publication/entry/exit/flush, callbacks and unwind checks.
- `tools/ci/arm64_bootstrap_gate.sh`, including the 387 applicable stock cases,
  interpreter GC2 hard assist, TG hooks, coroutine/dead-resume,
  aggregate/variadic interpreter FFI and callback/errno unwind checks.

Logs: `/tmp/ljmt-callm-numeric-four-profile.log`,
`/tmp/ljmt-callm-numeric-experimental.log` and
`/tmp/ljmt-callm-numeric-bootstrap.log`. Both previously failing fixtures pass
all four broader M7 profiles. The watchdog sampled only the normally
completing legacy-bytecode fixture. This does not explain the earlier I64
timeout or identify the original scalar recording interruption.

Ordinary ARM64 assertion profile
`27674dac5b5e7412801edad6fdafd157139d16930bdb9357cb658967db332624`
passes 256 new compiled smoke runs without test hooks: five values across
all 48 CALLM prototypes plus 16 stored-U32-result controls. They require
exact argument bits/counts/errno, published loops and same-trace reuse.
All 102 preceding numeric/held-result/scalar-table/nested-read/I64-argument/
callable smoke runs, bit operations and rooted cells/upvalues pass too.
The former `abs(fabs(t.value))` boundary now requires compilation and passes,
together with its NUM identity and stored-result controls.

Both stock source and original-bytecode import corpora pass 509 cases on
ARM64. Signed x64/Rosetta assertion profile
`65a63c0e068781089da2fc7a7e282ccf05375c5905df5c4042b70bd778097931`
passes both 509-case corpora too. This remains bounded compatibility
evidence, not a complete x64 umbrella or native Intel performance result.

The matrix verifies unchanged source and restores ordinary ARM64 assertion
profile `d5c47d619fbc10bc864469aa4e97736449b0922743e2bae1431d67e5f10f178a`.
That runtime repeats the 53 preceding numeric and 256 new CALLM smoke runs.
Code-sign checks pass; selected test-helper symbols and out-of-line atomic
imports are absent. Log: `/tmp/ljmt-callm-numeric-matrix.log` and matching
ordinary/stock/x64 logs.

## Main checkout

The clean main `v2.1` checkout was fast-forwarded to the source checkpoint
and rebuilt with ordinary bootstrap/experimental/assertion flags and no
test hooks. Profile
`bb32d71f4762613b23e5b59f83e8488e3e1051b0420be74cc7ccdf561962bc5b`
passes all 358 smoke runs, the new CALLM and preceding boundary checks,
bit operations, rooted cells/upvalues, and both 509-case stock corpora.
Code-sign and atomic/test-helper symbol checks pass.
Log: `/tmp/ljmt-callm-numeric-main.log`.

- Runtime SHA-256: `1a5da5385696b7dc8eea3926adffb60cbcb85bda5e90ee0707c4a4014d858987`
- Archive SHA-256: `df9a19f8f7c2b1804ffb50119000ef51081d8a6c34ac6ca8ef8f6a854f7265f8`
- Build-signature SHA-256: `6a13a095289312fab80fd08c434e54c6ff4b4741d9b424dae9a358c0b61e5e2d`

## Limits

This certifies numeric producers feeding the fixed integer argument grammar.
It does not establish arbitrary CALLM adaptation, enum/bool conversions,
aggregate JIT calls or general Lua/metamethod-call coverage. NaN and
out-of-range results are compared with this target's existing interpreter
conversion rules. The ARM64 port remains opt-in, experimental and incomplete.
