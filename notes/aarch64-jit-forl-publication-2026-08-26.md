# AArch64 integer FORL publication proof (2026-08-26)

## Scope and safety boundary

This checkpoint admits recording, assembly, and publication of a narrowly
proved integer `BC_FORL` root on macOS AArch64. It does **not** admit native
`BC_JFORL` entry. A published JFORL still performs the interpreter's numeric
loop update and comparison, stores `FORL_IDX` and `FORL_EXT`, then uses the
existing branch-only stale-startins recovery in the `BC_JLOOP` tail.

The split is explicit:

- `LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED=0` permits the recorder surface;
- `LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED=1` keeps native entry closed;
- `TRACE_ARM64_INT_FORL_ADMITTED` certifies a FORL body independently of
  `TRACE_ARM64_INT_LOOP_ADMITTED`; and
- the generic C root-entry gate remains JLOOP/JFUNCF-only and accepts only the
  LOOP certificate.

This boundary lets the recorder and assembler proof land without risking a
double increment: redispatching FORL after JFORL has already advanced the
index would be incorrect.

## Bytecode generation proof

Before recorder setup, and again at both START and RECORD ingress, the root
must be an exact acquire-loaded FORI/FORL pair:

- FORL has a negative displacement to a non-empty body;
- the instruction immediately before the body is unpatched `BC_FORI`;
- FORI and FORL have the same A field;
- FORI has a positive displacement ending exactly one instruction after FORL;
- `A+FORL_EXT` is inside the prototype frame; and
- a second acquire load of both words matches the captured generation.

The assembler repeats the prototype-range and tuple checks before accepting
the semantic IR. Publication commits and synchronizes mcode, saves the trace,
publishes the immutable original-startins sidecar, and only then performs one
exact full-word FORL-to-JFORL CAS. FORI deliberately remains unpatched.

## Admitted semantic grammar

The admitted root is integer-only and has a non-zero constant KINT step. It
contains exactly two unchecked induction `IR_ADD`s:

1. the pre-loop add consumes the guarded IDX SLOAD and the constant step;
2. the body add consumes the first add and the identical step;
3. the following guards are `LE` for a positive step or `GE` for a negative
   step and compare both results with the identical STOP value; and
4. exactly one integer PHI maps the first index result to the second.

The hidden IDX SLOAD is exactly guarded `TYPECHECK|INHERIT`. A dynamic STOP is
an unguarded `READONLY|INHERIT` integer SLOAD and must be followed by the exact
signed overflow proof:

- positive step: `STOP <= INT32_MAX-step`;
- negative step: `STOP >= INT32_MIN-step`.

A constant STOP is admitted only when `STOP+step` is representable in int32.
Dynamic and zero steps remain rejected. The rest of the body stays inside the
previous scalar integer allowlist: no calls, heap IR, NUM/CONV IR, sides, or
stitches.

Post-RA validation independently requires two `IR_ADD`s for FORL and zero for
LOOP, revalidates source-specific SLOAD layouts, and retains the canonical
integer spill-frame checks. The current measured FORL roots use no spills and
`spadjust=0`.

## Snapshot proof

FORL recording legitimately emits `SNAP_NORESTORE` entries for VM-owned
numeric-loop state. The new exception is restricted to a FORL root, an earlier
integer SLOAD, the identical source/snapshot slot, and only the hidden IDX or
STOP slot. Constants, computed values, ordinary locals, converted loads, and
parent loads cannot use it. The post-RA validator repeats this restriction.

For `local s=0; for i=1,n do s=s+i end`, the fixture pins six snapshots with:

- refs: `1, 5, 7, 8, 10, 12` relative to `REF_BASE`;
- map offsets: `0, 2, 10, 13, 20, 29`;
- entry counts: `0, 6, 1, 5, 7, 1`;
- slot counts: `2, 10, 4, 8, 10, 4`; and
- 32 total snapshot-map entries.

Packed snapshot PCs are decoded directly from the map and checked inside the
held prototype. `jit.util.tracesnap(..., true)` is not used for this proof: its
backward function-header scan currently misreports PCs in prototypes containing
the newer CGET/CSET bytecodes.

## Executable evidence

`tests/t-arm64-jit-forl-record.c` proves:

- positive dynamic-stop step `+1` publishes trace 1 and returns 5050;
- negative constant step `-3` publishes trace 1 and returns 1717;
- negative step `-3` with a dynamic stop publishes only with the exact
  `STOP >= INT32_MIN+3` proof and returns 1717;
- exact IR, PHIs, register allocation, snapshots, self-link, mcode bounds,
  original sidecar, and the distinct FORL certificate;
- a second positive call has exactly 36 branch-only stale-startins recoveries;
- a second negative call has exactly 13 recoveries;
- calling the integer-published positive loop with numeric stop `3.5` returns
  `6.0` through exactly two FP branch-only recoveries and zero native entries;
- flush restores the exact original FORL word, leaves FORI untouched, and a
  subsequent call republishes the same trace-1 certificate and fingerprints;
- root-entry publications, root-entry cleanups, and native exits stay zero;
- dynamic step `+2` returns 400 but publishes no trace;
- a bounded zero-step loop reaches its break at 10 but publishes no trace; and
- no side trace or second runnable trace appears.

`tools/ci/arm64_jit_forl_record_contract.sh` statically pins the generation,
semantic, post-RA, publication-order, and branch-only VM contracts. It builds
and repeatedly executes the fixture on ordinary ARM64 and ARM64e+BTI, includes
randomized mcode placement, runs the synthetic IR admission suite, and restores
the ordinary experimental build. The complete driver passed on 2026-08-26.

The updated fail-closed umbrella also passed, as did the strict root-entry,
native-loop, scalar-loop, integer-spill, phase-gate, live flush/reuse,
mcode-retirement, and exit contracts. These preserve the previously admitted
LOOP execution surface while the shared semantic/post-RA validator now carries
root-specific FORL rules. The only build diagnostics were the two pre-existing
unused-code warnings for `szmcode` and `ccall_rawchild_wait`.

## Remaining work

This is publication proof, not native numeric-for execution. Before opening
`LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED`, the VM/C gate must consume the
exact full JFORL word (including A and trace D), enter only after the
interpreter's one required update/test, and reject without redispatching that
semantic operation. Tests must cover first-iteration and double-increment
errors, positive and negative boundaries, overflow/NUM fallback, snapshot
restoration, PROFILE and STOPREQ windows, flush/retire/reuse, ARM64e/BTI,
random/far mcode, and concurrent lifecycle transitions.
