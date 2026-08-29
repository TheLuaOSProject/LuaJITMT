# ARM64 integer FORL native entry (2026-08-26)

## Scope

This tranche opens native entry for the constant-step integer `BC_FORL` roots
certified by the preceding FORL publication tranche. It does not widen the IR
grammar: dynamic/zero steps, FP traces, calls, side traces and stitches remain
outside the admitted surface.

`LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED` is now `0` for the experimental
macOS ARM64 build. The independent LOOP gate remains open; JFUNCF, side and
stitch gates remain closed.

## VM boundary

The generated `BC_JFORL` handler has a dedicated integer-success path:

1. Decode and type-check IDX.
2. Execute the signed 32-bit `adds IDX, IDX, STEP`.
3. On signed overflow, continue in the interpreter without storing or calling
   the entry helper.
4. Compare the incremented IDX against STOP for the sign of STEP.
5. Store the incremented tagged integer to both IDX and EXT.
6. Only on the taken edge, preserve the exact full `JFORL` instruction consumed
   by dispatch in callee-save x27 and call `lj_trace_enter_root()`.
7. On success, reserve the fixed 16-byte interpreter spill area and branch to
   `T->mcode`. ARM64e authenticates the target with the exact `GCtrace *` as the
   discriminator.

The FP `JFORL` paths do not call the entry helper. They retain the existing
branch-only stale-startins recovery after completing their numeric update and
comparison.

On helper rejection, the VM restores the consumed `JFORL` word from x27 before
tailing to the shared branch-only recovery. It deliberately does not classify
a newly loaded opcode at this point: the numeric-for edge has already executed,
so redispatching it as a fresh `JLOOP`/`FORL` could skip or double the induction
update. The stale-startins path computes the already-selected FORL branch and
polls without re-executing the update.

## C admission proof

The last helper argument is now the full consumed `BCIns`, not just an opcode.
The helper derives the source opcode and requires all of the following before
returning a target:

- consumed D equals the supplied trace number;
- source is an independently open `JLOOP` or `JFORL` surface;
- `JFORL` metadata starts at immutable `BC_FORL` and has the exact unpatched
  positive `FORI` / negative `FORL` pair, matching A and exit geometry;
- the trace carries exactly the root-specific admission bit (`FORL`, not a
  substituted `LOOP` certificate), is a self-linked root and is not gated;
- semantic/post-RA validation succeeds on both acquired metadata views;
- the consumed word equals `BCINS_AD(sourceop, bc_a(startins), traceno)` and
  the current acquire-loaded word equals that same consumed generation on all
  three bytecode checks;
- the mcode range and, on ARM64e, signed target are consistent with `T->mcode`.

The existing TG-local `jit_base` publication remains the lifetime lease across
metadata acquisition and the VM branch. Every rejection after publication
clears that lease on the single cleanup path.

## Validation

The focused native-FORL contract passed on ordinary ARM64 and ARM64e/BTI,
including randomized mcode placement. It covers:

- positive and negative constant-step native entry;
- a false integer edge with no helper call;
- FP taken edges staying branch-only;
- positive `INT32_MAX + 1` and negative `INT32_MIN - 1` VM induction overflow
  bypassing stores and admission;
- a post-metadata profile-request rejection returning the exact numeric result
  without a double increment;
- a full-word A-only mutation after both metadata views causing cleanup;
- native preheader/body exits and exact recovery results;
- flush, bytecode restoration, trace-slot reuse and re-entry.

The ARM64e negative contract additionally ran control, raw, IA/zero-signed and
wrong-trace-signed targets through both `JLOOP` and `JFORL`. Each malformed
`JFORL` signature reached the new authenticated branch and faulted with
Darwin `SIGBUS`; the valid control entered normally.

The recorder/publication contract, strict root-entry contract and IR admission
contract also pass after this boundary change. The affected scripts now select
the macOS SDK and Xcode Clang by default so ARM64e preprocessing does not depend
on a Homebrew Clang configuration.

The full ARM64 fail-closed umbrella gate also passes. The post-change regression
sweep re-ran spill-free scalar roots, bounded integer spills, IDLE/MARK/SWEEP
entry gates, live flush and trace-slot reuse, retired-mcode lease races, and
ordinary/ARM64e exit recovery. All passed; the only compiler diagnostics were
the pre-existing unused `szmcode` and `ccall_rawchild_wait` warnings.

## Remaining boundary

This is still a deliberately narrow JIT surface. The next work should extend
root shapes/functions or side/stitch entry only with their own immutable
metadata certificate, VM entry point and concurrent retirement tests. Dynamic
numeric-for steps and FP FORL compilation remain rejected rather than silently
borrowing the integer certificate.
