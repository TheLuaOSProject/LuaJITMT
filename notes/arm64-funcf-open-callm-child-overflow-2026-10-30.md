# ARM64 FUNCF Open-CALLM Child Overflow

This checkpoint admits at most one inlined producer frame in each child-footed
snapshot for the existing fixed FUNCF/open-CALLM source shape. Such a snapshot
is accepted only when the source contains the exact open producer `CALL` and
the child validator independently proves that frame, its function, arguments,
results, and snapshot. The frame link must return to the instruction after the
source producer call. Parent values are replayed at the pre-call state; live
values inside the child extent must be proved by the child path.

The producer-function slot may differ between its parent CGET source value and
the saved child-frame function value only at the exact source CALL slot, when
both resolve to the same function and one validated identity-guard snapshot
binds the transition. Semantic and allocated post-RA passes check this
independently. The change does not alter snapshot restoration or general stack
limits, and it does not admit multiple child frames or broader open-call
shapes.

The regression producer returns `x+1,.25`, with the open vector consumed by a
fixed two-argument Lua function. The native trace contains a child `IR_ADDOV`
snapshot at the producer's `BC_ADDVN`. `INT32_MAX-1` returns without an exit;
`INT32_MAX` takes exactly that child snapshot, resumes with the interpreter
result matching the separately loaded oracle, and re-enters the same root and
mcode after a full collection. The existing live threading peer remains active
during these checks. Copied semantic and post-RA traces each reject a changed
child return PC, substituted child callee/function slot, and changed
root-derived child argument map.

## Validation

- `tools/ci/arm64_jit_funcf_record_contract.sh` passed with its default
  repetitions on ARM64 and arm64e, including the child-overflow witness,
  randomized FUNCF fixture, and topology probe.
- `tools/ci/arm64_jit_open_callm_exits_contract.sh` passed on its ARM64 and
  arm64e assertion/release profiles.
- Logs: `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/funcf-record-final.log`
  and `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/open-callm-final.log`.
- The matched x64 dump for this mixed numeric producer is in
  `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029/mixed-callm-addint-x64-dump.log`;
  that evidence predates this ARM64 admission change.
