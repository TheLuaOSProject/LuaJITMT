# Owned 32-bit CData arguments

## Diagnosis and proof scope

The preceding INT-argument checkpoint returned correct C values, call counts
and errno in 100 ordinary-runtime observations of 32-bit CData and enum
arguments, but published no loops. The recorded integer payloads were the
expected `FLOAD IRFL_CDATA_INT` with INT or U32 type. Evidence is in
`/tmp/ljmt-cdata32-red.log` and the `ljmt-cdata32-i32-ir` and
`ljmt-cdata32-u32-ir` dumps. This checkpoint addresses the eight exact
`int32_t`/`uint32_t` shapes: direct SLOAD or rooted table VLOAD, each passed
to a same-type fixed argument in unary or 12-argument compact-stack calls.

The existing owned-I64 proof is generalized to recognize four-byte payloads.
Both semantic and allocated checks independently require the exact guarded
CData owner, CType, payload field and signedness, and the same fixed INTEGER
ABI type and width. The exact CData source must occupy the restorable XSAVE
argument slot. Payload bits may feed only certified native arguments; they
cannot enter any Lua snapshot or unrelated IR operation. The new raw signed
INT payload is subject to that exclusion even though INT itself is a valid
Lua snapshot type. The existing eight-byte CNEW producer grammar is unchanged.

Production changes are confined to `src/lj_asm_arm64_admit.h`. The recorder,
emitter, native-call protocol and x64 production code are unchanged. Enum,
cross-width and cross-sign conversions remain outside this certificate.

## Focused evidence

The native fixture has eight fresh candidate prototypes and eight distinct,
permanently interpreted oracles. Six values per kind cover the signed and
unsigned 32-bit boundaries. It requires 48 normal native runs, eight forced
POSTCALL exits and eight same-trace reuses, each making exactly 24 calls.
The C helper checks every argument's exact bits against the oracle, all 11
stack-prefix sentinels, call counts and errno. The native witness binds the
exact callee, trace and live synchronized frame with a native pin, then checks
the continuation PC and complete frame/XSAVE/callback cleanup. Cold traces
alone may retry, at most eight attempts with exact effect checks each time.

The exact CData object must be in the published frame's GC-scanned root span;
table cases also require the exact table. The span ends before `top`, matching
the GC scan. This is not a per-argument-slot or complete GC-lifetime-under-
mutation test; the admission fixture separately verifies the XSAVE slot.

The admission fixture clones eight authentic traces immediately after each
warmup. Each pass accepts all eight originals and rejects wrong source roots,
non-restorable or truncated XSAVE arguments, payload field/tag/owner, absent
or wrong CType guards, wrong signature width/sign and raw payload snapshots.
Four signed shapes also redirect an existing live INT loop increment to the
raw payload, preserving IR types and SSA ordering while introducing an
unrelated use. Allocated checks additionally reject wrong register banks and
out-of-frame spills. Totals are 8 positive / 92 negative semantic cases and
8 positive / 108 negative allocated cases. Fresh originals are accepted
before each paired mutation.

The focused ARM64 assertion run passes in
`/tmp/ljmt-cdata32-focused-final.log`, with no cold retry. Seven counterfactual
checks also fail for the intended reason:

- The preceding header cannot publish the first required native CData32 trace.
- Removing only the semantic or allocated snapshot exclusion admits the raw
  signed INT payload into an unrelated Lua snapshot.
- Removing only the semantic or allocated use restriction admits that payload
  into the existing Lua loop increment.
- Removing only the semantic or allocated exact-source relationship admits a
  different saved CData owner.

Evidence is `/tmp/ljmt-cdata32-counterfactual.log` and its seven per-case logs.
The candidate header was restored byte-for-byte afterward. These are test
sensitivity checks, not production failures. Both fixtures are registered
in the focused table-call contract and the broader M7 FFI suite.

## Validation status and limits

An ordinary ARM64 assertion runtime without test hooks passes all 100
observations: the 40 exact integer cases require loop publication and
same-trace reuse, while all 60 enum cases require refusal and correct
interpreter effects. Signature and atomic-import checks also pass. Evidence
is `/tmp/ljmt-cdata32-ordinary-development.log`. Publication is separate from
the C fixture's native-execution witness.

The complete matrix froze source at
`dfd8db52f9f68b480ce003a30e465c058e980227` and passed:

- All 26 focused fixtures in ARM64/arm64e assertion/release profiles. Each
  new runtime fixture requires 64 native runs and eight forced exits; each
  new admission fixture reports 8/92 semantic and 8/108 allocated cases.
- The complete experimental gate, including all four broader M7 FFI profiles.
  All eight runs of the new runtime fixture across the focused and M7
  matrices completed with zero cold retries.
- The complete bootstrap gate, including interpreter, GC2, TG, callback,
  coroutine/dead-resume and stock checks.
- An ordinary ARM64 assertion build without test hooks: 698 observations
  comprising 598 preceding cases, 40 new exact CData32 cases and 60 enum
  refusals. Boundary checks, bitops, rooted cells/upvalues and both 509-case
  stock source/import corpora also pass. Ordinary publication evidence remains
  distinct from the C fixtures' native witnesses.
- A signed x64 assertion build passing both 509-case stock corpora.
- Restoration of the ordinary ARM64 assertion profile, with repeated numeric,
  INT, numeric CALLM and CData32 observations, valid signing, no test-helper
  exports and no undefined atomic-runtime imports. The source stayed frozen.

The focused profile IDs, in ARM64 assertion, arm64e assertion, ARM64 release
and arm64e release order, are:

```text
fbeaaddcbd9bf695217e5c742f159a5e0ba6c88628a4697c9b05a913390c7a49
1a9a3ee03602977c61168791f6ad306d6a6bc52dfff68a90a48f306d749116dc
22bc113244d2ecbab58ef5ac133ec4bcd56501dce4ee3fbb4cc74c178a8dbf58
581704a313d74dea0ebb822dbe326d8ec4d70d6359242c1a2765648da086b449
```

Evidence is in `/tmp/ljmt-cdata32-matrix.log`,
`/tmp/ljmt-cdata32-four-profile.log`, `/tmp/ljmt-cdata32-experimental.log`,
`/tmp/ljmt-cdata32-bootstrap.log` and the matching ordinary/stock logs.
The watchdog sampled the legacy-bytecode-cell fixture once in each complete
gate; both completed successfully without intervention.

The final ordinary worktree profile is
`2f55a7d7679b8d533d4e7decf3c321fd43283c77b10bb796fb32e5b25c5da094`.
Runtime SHA-256 is
`14cc6609f61a086b9314e8f5cf7f08951e6666f3c785b29b0f91b00be012e854`.
The clean main checkout was fast-forwarded to the same source. Its independent
ordinary rebuild passes all 698 observations, boundary checks, bitops, rooted
cells/upvalues and both 509-case stock corpora, plus signing and helper/import
checks. Main profile is
`8b24b318abba028c261caeb105ef637d01ac6d58de8a0bdb7bd33f4246c43d3c`;
its runtime hash matches the worktree above. Evidence is
`/tmp/ljmt-cdata32-main.log`.

The preceding complete result is documented in the
[INT-argument evidence](arm64-jit-ffi-int-arguments-2026-10-27.md).

This extends exact fixed 32-bit CData arguments only. It does not certify enum
or general CData conversions, arbitrary table PHIs, aggregate calls, general
CALLM/multires or unrestricted Lua call graphs. ARM64 remains opt-in,
experimental and incomplete.
