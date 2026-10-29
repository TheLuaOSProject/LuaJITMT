# ARM64 captured fixed two-result child call

The ARM64 FUNCF root recorder now admits one bounded captured Lua call that
requests two fixed results (`CALL B=3`) from a one-argument child and consumes
both values to return one scalar. The ordinary benchmark case is
`captured-two-result-call` in `bench/jit-topology-probe.lua`. The child returns
`x*.5, x+1.25`; the parent adds those values. Dynamic/open result calls,
varargs, and tail-call forms remain outside this proof.

The source walk binds the fixed CALL result window to the child’s exact
`RET D=3`, source PC, captured function, and two requested destinations. The
semantic replay and post-RA replay independently derive both results through
the parent continuation, then validate the final caller return snapshot and
physical locations. The cloned-trace fixture confirms its baseline is accepted
and rejects altered child result production, missing/changed return slots,
wrong arity, source/frame mutations, and allocation mutations. Equal returned
values remain valid; the proof does not require distinct values.

The allocated snapshot liveness walk is a forward read-before-write scan.
For a fixed two-result CALL, marking A+1 written must not clear a read of the
old A+1 value that was already seen earlier in the same resumed bytecode
range. The walk now preserves that requirement and records the B=3 CALL as a
write to both A and A+1. The semantic and allocated unchanged-formal checks
also recognize both CALL destinations.

## JFUNCF entry stack extent

The native JFUNCF prologue computes `RA = BASE + framesize` and checks it
against `L->maxstack` before calling `lj_trace_enter_root`. It also nil-fills
missing fixed parameters before that call. It does not synchronize `L->top` on
Lua-to-Lua entry, so the old generic `top >= base` check rejected a valid
JFUNCF entry when `L->top` still described the caller’s earlier C boundary.

The VM now passes this checked frame end as a seventh AAPCS64 argument only for
JFUNCF; loop-entry callers pass null. The helper requires the JFUNCF source to
be the exact live prototype header, requires the supplied frame end to equal
`base + pt->framesize`, and checks the complete interval against `maxstack` and
stack alignment. Only then may JFUNCF accept a stale top below base. Other
entry kinds retain `top >= base`. The helper does not write `L->top`: after
`jit_base` publication GC2 treats the thread as JIT-owned and scans the full
allocated stack conservatively, including frame-function roots. Before
publication the helper does not allocate or wait, and the GC-entry gate is
rechecked around the lease. This keeps the caller’s old top intact on both
rejection and native-entry paths.

The C entry fixture forces a low top below base and confirms a matching frame
end succeeds; a one-slot-short frame end rejects before publication. The root
entry fixture confirms the same low top is rejected for JLOOP. This behavior
also resolves the earlier post-GC mutable-capture FUNCF no-publication
regression without weakening loop-entry validation.

## Validation

- `tools/ci/arm64_jit_funcf_record_contract.sh` passed ARM64 and ARM64e
  assertion profiles. Its C call fixture covers native entry, post-GC reuse,
  selected-root identity exits, independent semantic/post-RA mutations, and a
  synthetic pre-call read followed by a second-result overwrite.
- `tools/ci/arm64_jit_root_entry_contract.sh` passed ARM64/ARM64e assertion
  checks plus its ARM64 release certificate checks, including the low-top
  JLOOP rejection.
- `tools/ci/arm64_jit_native_forl_contract.sh` passed ARM64 and ARM64e FORL
  entry tests after all non-JFUNCF call sites were updated to pass no frame end.
- A no-helper ordinary ARM64 build ran the full topology probe. The captured
  pair case published return root 1 (`nins=14`, `nexit=6`); phase 2 observed 81
  selected-root exits on the bad argument. Output:
  `/Users/frityet/Scratch/ljmt-funcf-pair-call-20261029/ordinary-topology-final.tsv`.

The recorder and native root remain limited to the fixed one-argument child
shape proved by the semantic and post-RA validators; this does not claim
general multi-result CALL parity.
