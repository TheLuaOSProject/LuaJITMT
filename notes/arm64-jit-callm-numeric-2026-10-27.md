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
960 runs of 24 calls to each C function, including 96 forced exits. Values
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

## Validation still to complete

The renewed frozen four-profile contract, complete experimental/bootstrap
gates, ordinary compatibility matrix and main-checkout integration have not
yet been completed for the corrected fixture checkpoint.

## Limits

This certifies numeric producers feeding the fixed integer argument grammar.
It does not establish arbitrary CALLM adaptation, enum/bool conversions,
aggregate JIT calls or general Lua/metamethod-call coverage. NaN and
out-of-range results are compared with this target's existing interpreter
conversion rules. The ARM64 port remains opt-in, experimental and incomplete.
