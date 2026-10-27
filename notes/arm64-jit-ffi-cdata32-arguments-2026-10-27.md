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

The complete frozen four-profile, experimental, bootstrap and stock-
compatibility matrix is still pending. The main checkout
remains on the fully validated [INT-argument checkpoint](arm64-jit-ffi-int-arguments-2026-10-27.md).

This extends exact fixed 32-bit CData arguments only. It does not certify enum
or general CData conversions, arbitrary table PHIs, aggregate calls, general
CALLM/multires or unrestricted Lua call graphs. ARM64 remains opt-in,
experimental and incomplete.
