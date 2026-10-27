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

The renewed matrix froze corrected source at
`a029884dfff3e0c68f0f42b4795d5d3734ab6559` and passed:

- All 24 focused fixtures in all four profiles. Each new runtime fixture
  requires 432 native runs and 64 forced exits; admission reports 72 positive
  controls, 464 semantic rejections and 572 allocated rejections per profile.
- The complete experimental gate, including all four full M7 scalar FFI
  profiles and their strengthened mixed ABI matrix.
- The complete bootstrap gate, including interpreter, GC2, stock, TG,
  coroutine/dead-resume and FFI callback checks.
- An ordinary ARM64 assertion build without test hooks: 598 smoke
  observations (240 new INT, 256 numeric CALLM and 102 preceding cases),
  boundary/refusal checks, bitops, rooted cells/upvalues and both 509-case
  stock source/import corpora. These ordinary smoke checks require exact
  values/effects and trace publication; the separate C fixtures supply
  native-execution evidence.
- A signed x64 assertion build passing both 509-case stock corpora.
- Ordinary ARM64 restoration, repeated 53 numeric, 256 numeric CALLM and
  240 INT observations, signature validation, no test-helper exports and
  no undefined atomic-runtime imports. The source remained frozen.

The focused profile IDs, in ARM64 assertion, arm64e assertion, ARM64 release
and arm64e release order, are:

```text
2d01377756450ab377268edb2b394fa7d2cec550d2af07a54d78734f4061f770
8c806f8e26709404fccbe1e047a17b510011904f8b37354855e71ff40a9457d1
dc8ec20992b34bf1a1d9f985450775a39bab6a1a69fa96cf139dc90e26f51df2
e67f97012a081346b9003cef92a5935a2da63d85b822fd985fb597b2a52dcb28
```

Evidence is in `/tmp/ljmt-intargs-matrix.log`,
`/tmp/ljmt-intargs-four-profile.log`, `/tmp/ljmt-intargs-experimental.log`,
`/tmp/ljmt-intargs-bootstrap.log` and the matching ordinary/stock logs.
The watchdog sampled the long-running legacy-bytecode-cell fixture once in
each complete gate; both completed successfully. It did not terminate them.

The clean main checkout was fast-forwarded to the same source and rebuilt
as an ordinary ARM64 assertion runtime. It independently passes all 598
smoke observations, boundary checks, bitops/cells/upvalues and both 509-case
stock corpora, with signing and helper/import checks. Evidence is
`/tmp/ljmt-intargs-main.log`; main profile is
`acc8c3a9ae78c0e9c7e4015015f76ae70f2a165c73886cf1f90e776b803499c6`.
Both main and worktree runtime SHA-256 are
`812ad93befb8091d25247a5eba71b407bc5481ae2c93d11ee503238fc9958bef`.
The preceding complete result is documented in the
[numeric CALLM evidence](arm64-jit-callm-numeric-2026-10-27.md).

This adds the observed INT argument grammar and direct INT-result CALLM
adaptations. It does not certify arbitrary enum/bool conversions, general
CALLM/multires, aggregate JIT calls or unrestricted Lua call graphs. Existing
unexplained timeout and profiler limitations remain in the status index.
The ARM64 port remains opt-in, experimental and incomplete.
