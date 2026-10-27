# Lua INT arguments and INT-result CALLM adaptation

## Diagnosis and implementation

The preceding numeric CALLM checkpoint compiled only 14 of 48 observed Lua
INT call shapes. Its ordinary runtime returned the correct arguments in all
240 observations, but 34 shapes remained interpreted. The matrix covers
table values, direct SLOAD arguments and an INT-returning CALLM producer;
each supplies all eight signed/unsigned C integer kinds in unary and
12-argument compact-stack forms. Evidence: `/tmp/ljmt-intargs-red.log` and
the matching `intargs` IR dumps.

Both ARM64 admission passes now independently recognize the recorder's
actual INT carriers: small register sign/zero extension, low-byte/halfword
stack stores, raw low-word U32 arguments, and signed INT extension into
I64/U64. U64-from-INT uses the recorder's U64 destination mode with SEXT,
preserving the existing interpreter's treatment of negative integers.
The new paths bind the original Lua argument to the exact restorable XSAVE
slot. Derived carriers may feed only fixed INTEGER arguments, and cannot
leak into Lua snapshots or unrelated operations. Recognized but invalid
carriers cannot fall through to a more permissive generic conversion rule.

Direct INT-result CALL/CALLM pairs additionally bind the final argument to
the completed producer. INT native results now require the exact caller
result in the DONE snapshot, with a separate allocated-lifetime check across
native leave. The older CALLXS fixture's wrong-INT-result-slot positive is
therefore a negative in both passes, just as its NUM counterpart already was.

Small register arguments must have the exact conversion or an in-range
canonical constant; a raw INT cannot bypass the required width extension.
Identical small stack carriers and i32/u32 low-word carriers intentionally
remain compatible with either signedness when the CType guard agrees.
The previously admitted direct signed-i32 SLOAD path retains its existing
structural proof; this change does not impose the new rooted source proof
on that unrelated path. The FFI parser supports only 32-bit enums, so the
new INT-derived membership checks retain their fixed INTEGER restriction.

Production changes are confined to `src/lj_asm_arm64_admit.h`. They add
compilation-time proofs and preserve the existing recorder, emitter,
lockless native-call protocol and x64 production code.

## Focused evidence

The native fixture uses 48 distinct candidate prototypes and 48 distinct,
permanently interpreted oracle prototypes. It requires 432 native runs of
24 calls, including 64 forced post-call exits: one consumer exit per shape
and a separate producer exit for all 16 CALLM shapes, followed by same-trace
reuse. Six width-specific INT values include signed/unsigned boundaries and
negative inputs. The C helper checks every argument's exact bits, all 11
stack-prefix sentinels, exact call counts and errno. Producer and consumer
use distinct errno values. The fixture separately checks native trace/callee
identity, native pins, continuation PC and frame/callback/XSAVE cleanup.
Cold recording allows at most eight attempts with exact effect and cleanup
checks on every attempt; established native runs and forced exits cannot
use that retry allowance to waive their witnesses.

The admission fixture clones each authentic trace immediately after warming
it. It accepts 48 original views and 24 compatible signedness controls in
each pass, rejecting 464 semantic and 572 allocated mutations. Coverage
includes original argument/producer identity, encoded conversion mode/type,
fixed sentinels, raw small-width bypass, native carriers in unrelated Lua
snapshots, argument/result snapshot restoration, actual CALLM source bytecode
and argument count, register bank, and spill alignment/extent. Raw-bypass
mutations remove all uses of the hoisted conversion and its definition, so
rejection cannot rely on an orphaned conversion node.

The focused native/admission/legacy CALLXS run passes in
`/tmp/ljmt-intargs-focused3.log`. All 24 registered table-call fixtures pass
the ARM64 assertion profile in
`/tmp/ljmt-intargs-contract-development.log`. Both new fixtures are also
registered in the broader M7 scalar FFI suite.

An ordinary assertion runtime without test hooks passes all 240 observations
across the 48 shapes, requiring published loops and same-trace reuse as well
as exact helper bits/counts/errno. Log:
`/tmp/ljmt-intargs-ordinary-development.log`. Publication is not used as a
substitute for the separate C fixture's native-execution witness.

Counterfactual checks each fail for the intended reason:

- The preceding header cannot provide the first small-register native trace.
- Removing only semantic carrier-snapshot exclusion admits mutation 19.
- Removing only allocated carrier-snapshot exclusion admits mutation 19.
- Restoring permissive raw small-register admission admits mutation 17.

Logs are `/tmp/ljmt-intargs-counterfactual.log` and its four per-case logs.
The candidate header was restored byte-for-byte afterward. These failures
test the safeguards independently; they are not production failures.

## Validation status and limits

At `43149a7e`, all 24 focused fixtures passed all four ARM64/arm64e
assertion/release profiles. The complete experimental gate then exposed a
deterministic regression in the older ABI compact-stack matrix: the new INT
argument rule activated the complete XSAVE argument proof, whose source
matcher did not recognize the existing pointer CData's `FLOAD CDATA_PTR`
carrier. Semantic admission refused the trace before native publication.
Diagnostic branch instrumentation isolated this to the source proof; the
INT result-slot check was not the cause. Original evidence is retained in
`/tmp/ljmt-intargs-experimental-initial-failure.log`,
`/tmp/ljmt-intargs-abi-first-repro.txt` and
`/tmp/ljmt-intargs-abi-locate.log`.

Both passes now bind a fixed pointer carrier to its exact saved CData owner.
Each pass independently derives the preceding guarded CTypeID and requires
a native-width, non-reference pointer after stable typedef/attribute
resolution. Fixed pointer arguments always activate this source proof, even
when no INT conversion or table read is present. Existing GC root and
allocated-lifetime checks still apply. The ABI fixture now checks native
and exact helper-count deltas for each stack-matrix callee, so preceding
CALLM runs cannot satisfy its native witness.

The complete ABI fixture passes with the correction in
`/tmp/ljmt-intargs-abi-certified.log`. Four new authentic-clone mutations
independently reject the wrong saved owner, wrong payload owner, scalar
CType and missing CType guard in both passes. Removing only the exact-owner
mapping or only the pointer-type restriction from either pass makes the
corresponding negative fail, recorded in
`/tmp/ljmt-intargs-pointer-counterfactual.log` and its four per-case logs.
Candidate source was restored byte-for-byte after these counterfactuals.

Renewed four-profile, complete experimental/bootstrap and ARM64/x64 stock
validation is pending for the corrected checkpoint. The preceding complete result
remains `51c92374`, documented in the
[numeric CALLM evidence](arm64-jit-callm-numeric-2026-10-27.md).

This adds the observed INT argument grammar and direct INT-result CALLM
adaptations. It does not certify arbitrary enum/bool conversions, general
CALLM/multires, aggregate JIT calls or unrestricted Lua call graphs. Existing
unexplained timeout and profiler limitations remain in the status index.
The ARM64 port remains opt-in, experimental and incomplete.
