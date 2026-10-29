# Stack-passed two-float HFA CALLXS arguments (2026-10-29)

This slice extends Darwin ARM64 CALLXS admission to one exact stack shape: a
fixed, by-value two-`float` HFA (size 8, offsets 0 and 4) after all eight
floating-point argument registers are consumed. The fixture passes eight scalar
doubles first, the HFA next, and a trailing scalar double after it. Its expected
stack plan is HFA lane offsets 0 and 4, then trailing double offset 8, with a
16-byte total stack argument area. Partial-register-bank overflow remains
closed. This patch also keeps other stack-passed HFA widths/counts closed; the
positive proof is only for this two-float shape.

The interpreter's Darwin AAPCS64 path marks an HFA as an FPR aggregate,
`CCALL_HANDLE_REGARG` saturates `nfpr` when all-or-stack allocation cannot fit,
and the stack branch aligns C structs to at least pointer width before copying
the value (`src/lj_ccall.c`, `CCALL_HANDLE_REGARG` and `CCALL_ALIGN_STACKARG`).
The JIT signature classifier mirrors that placement only when `nfpr` is already
8. The generic ARM64 emitter has already consumed all FPRs at that point, so its
ordinary per-carrier stack stores produce the certified 0/4/8 offsets. An
unaligned stack prefix is rejected because the current emitter has no HFA group
alignment marker. No integer aggregate stack argument is opened here.

Semantic and post-RA CALLXS validation remain separate. Each pass independently
rebuilds the CType plan and stack offsets, then checks the HFA XLOAD lanes and
the trailing scalar carrier against that plan. The cdata argument stays rooted
in its Lua frame slot through XSAVE and native enter/leave. The runtime fixture
keeps a live MT peer attached; it compares against an interpreter-only oracle,
requires native finish and selected-root exit, checks exact C effect counts,
forces native re-entry/exit after full GC, and changes to an alternate callee
to prove the guarded exit. The C fixture checks the standalone signature plan
and runs private semantic/post-RA malformed-view checks on the published stack
trace. Its candidate and oracle closures are declared immediately before this
probe so the added closures do not change the setup of the existing HFA roots.

## Ordinary cross-architecture comparison

A pre-change ordinary-binary probe demonstrates the exact gap without relying
on helpers or a profile-only fixture. Source:
`/Users/frityet/Scratch/ljmt-iterator-review-20261029/stack_hfa.lua`.
It calls `lj_callxs_sysv_spill_sse` from the universal
`small.dylib`, passing eight doubles, a two-float struct, then a double. The
saved ordinary x64 and ARM64 binaries were built at `8c32bce2d1c7bfbafadd6dc0d2cc8b391d3086ac`:

- x64 (Rosetta): 1 selected loop root with 2 CALLXS operations, native-root
  exits observed, 1,120 foreign calls, 20 candidate/oracle comparisons.
- ARM64: exact values and 1,120 foreign calls, but no selected native root and
  no native exits.

Logs are `/Users/frityet/Scratch/ljmt-hfa-stack-probe-20261029-x64.log` and
`/Users/frityet/Scratch/ljmt-hfa-stack-probe-20261029-arm64.log`. Those ordinary
binaries predate this implementation; they are the matching control evidence
for the missing capability, not evidence for the new ARM implementation.

## Validation

`tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passes ARM64 and arm64e
with assertions and release flags. This includes native stack-HFA publication,
entry, exit, GC re-entry, alternate-call fallback, and independent semantic and
post-RA negative checks. Strict Darwin x86_64 compilation with
`-Wall -Wextra -Werror` passes for `lj_crecord.c`, `lj_asm.c`, `lj_ccall.c`,
`lj_ctype.c`, and the touched aggregate fixture/library objects. This is a
compile-compatibility check, not an x64 behavior change.
