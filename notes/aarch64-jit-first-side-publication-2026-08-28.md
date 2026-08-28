# AArch64 first-side publication and native execution (2026-08-28)

## Scope

This checkpoint publishes and executes one exact first-level AArch64 side
trace through a dedicated one-shot test seam. It is the first end-to-end proof
that the ARM64 recorder, assembler, compact body, GC root, trace slot, parent
topology and authenticated exit target can be committed as one finite
transaction.

This is not yet the production side-trace gate. The ordinary build retains
`LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED == 1`, and the one-shot path requires
both `LJ_TRACE_TEST_HELPERS` and `LJ_ARM64_FIRST_SIDE_PUBLISH_TEST`. The seam
also excludes GDBJIT and PERFTOOLS registration. Only the exact parent-1,
exit-2 fixture shape can reach the transaction or enter a root which owns the
published child.

## Resumed local-cell dispatch correction

The first successful publication initially returned `3` where the fixture
expected `4`. The child was present and native, but its first recorded value
was stale.

The side exit resumes at a `CGET`. ARM64 `vm_exit_interp` handled the local-cell
resume by loading the opcode target from global `GG_G2DISP`. That bypassed the
current TG's recorder dispatch overlay, so the resumed `CGET` executed in the
interpreter and recording began at the following `ADD`. The generated child
then consumed the stale value in its temporary slot.

The local-cell resume now indexes the current `DISPATCH` table with the opcode
byte:

```text
add TMP0, DISPATCH, INS, uxtb #3
ldr RB, [TMP0]
```

This is the same dispatch authority used by the active TG and therefore sees
its recording overlay. It is an architecture fix, not a fixture workaround.

## Exact first-child grammar

With the resumed `CGET` correctly observed, the admitted semantic trace is:

- `REF_BASE+1`: parent `SLOAD` of slot 4 with `PARENT|INHERIT`;
- `REF_BASE+2`: internal `CGET` represented as exact `NOP`;
- `REF_BASE+3`: guarded `ADDOV` of the parent value and integer constant 1;
- `REF_BASE+4`: guarded limit `SLOAD` of slot 2;
- `REF_BASE+5`: guarded `GT`;
- `REF_BASE+6`: guarded `XPOLL`; and
- semantic `nins == REF_BASE+7`.

The trace has five snapshots and seventeen map entries. Snapshot refs are
`CGET`, `ADD`, `LIMIT`, `GT`, and `XPOLL`; map offsets are `0, 3, 7, 11, 14`;
entry counts are `1, 2, 2, 1, 1`; slot counts are `5, 6, 6, 5, 5`; and PCs are
`13, 14, 3, 17, 7`. The first two snapshots retain the inherited parent value;
later snapshots retain the computed add result.

The exact post-register-allocation certificate requires parent x27, CGET with
no register, ADD x28, and limit x27. The child head optionally begins with
BTI-J, then moves inherited x28 to x27, materializes the child trace number in
w30, executes `DMB ISH`, and stores the trace vmstate through the TG-local
dispatch base. The parent map remains x28.

## Publication transaction

The one-shot probe has a release/acquire state machine:
`IDLE -> CONFIGURING -> ARMED -> ACTIVE -> DONE`, with `ABORTED` as the terminal
failure state. The recorder claims an exact `(jit_State, parent, exit)` only
after acquiring the ordinary JIT token and revalidating the parent generation.
Every recorder dispatch and both assembler boundaries revalidate that claim.
Probe reads do not touch configuring payload or the child field before a
`DONE` acquire, so overlapping observation is data-race-free.

All fallible or refusing work occurs before the exact `ASM -> PUBLISH` CAS:

1. capture an absent TRACE vmevent snapshot;
2. validate the parent certificate and private assembled child;
3. prepare mcode commit and synchronize instruction caches;
4. plan and initialize the compact child body;
5. prepare sealed GC-root publication;
6. reserve the exact vmevent absence generation;
7. acquire one nonwaiting SMR reader and revalidate every source and
   destination; and
8. transition to the non-abortable PUBLISH state.

Rollback releases the vmevent reservation, SMR reader and GC-root plan as
applicable, resets an initialized compact body to constructor form, and raises
the ordinary retry error. No shared child edge is visible on that path.

After PUBLISH, the suffix is finite and fail-stop. In order it:

1. publishes the committed mcode;
2. publishes sealed GC membership for the child body;
3. clears recorder-scratch ownership of trace number, exit table, exit stub and
   compact body;
4. changes the reserved child slot from `LJ_TRACE_PENDING` to the child body;
5. performs the no-drain GC trace-publication checkpoint;
6. raises the selected parent snapshot topslot if necessary;
7. changes parent child count from zero to one;
8. changes parent `nextside` from zero to the child number;
9. changes the selected snapshot count to `SNAPCOUNT_DONE`; and
10. compare/exchanges the exact raw parent fallback encoding to the exact raw
    child encoding.

The last CAS is the native-reachability linearization point. On arm64e both
expected and desired words use the owning global state's exit-target
discriminator; stripped-address equality is never accepted as authority.

## Entry validation and observed execution

Root entry remains childless in the ordinary fast case. For the sole admitted
one-child topology it now validates the parent and child twice around target
loading. The view includes exact trace-vector slots, topology, terminal parent
snapshot, raw parent exit word, body geometry, arena live/root membership,
fallback-filled child exit table, semantic/post-RA grammar and arm64e
body-discriminated entry authentication. A changed generation rejects native
root entry rather than repairing shared state.

The native fixture proves:

- one hot side attempt publishes exactly child trace 2;
- the parent exit changes from its raw fallback to the child target only after
  all child metadata is runnable;
- the parent takes exit 2 to the child;
- the child computes the expected result and its guarded `GT` exits through
  snapshot 3;
- the trace remains runnable on a second call; and
- a second arm is rejected because the seam is deliberately one-shot.

The same executable contract ran twice as ordinary arm64 and twice as arm64e
with BTI/PAUTH. The runner then restored the ordinary experimental helper build
and verified that the publication-only APIs were absent.

## Validation

Focused and adjacent checks passed on this Apple Silicon host:

- `tools/ci/arm64_jit_first_side_publish_contract.sh` with two arm64 and two
  arm64e executions;
- `tools/ci/arm64_jit_side_ir_admission_contract.sh`;
- `tools/ci/arm64_jit_side_asm_consumption_contract.sh`;
- `tools/ci/arm64_jit_side_ingress_metadata_contract.sh`;
- `tools/ci/arm64_jit_root_entry_contract.sh`;
- `tools/ci/arm64_jit_exit_contract.sh`;
- `tools/ci/arm64_jit_forl_record_contract.sh`;
- `tools/ci/arm64_jit_funcf_record_contract.sh`; and
- `tools/ci/arm64_jit_gdbjit_prepare_contract.sh`.

The only compiler diagnostic was the pre-existing unused
`ccall_rawchild_wait` warning.

## Remaining blocker

The production side gate must remain closed until retirement has an exact
inverse for this publication. Retirement must first compare/exchange the raw
authenticated parent edge from the child target back to the captured fallback,
making the child unreachable from already-running parent traces. Only then may
it invert exact `nextside` and `nchild` generations and retire the child slot
and body. The selected snapshot's terminal count and conservative raised
topslot can remain sticky.

After that inverse is implemented and race-tested on arm64 and arm64e, the
hard-coded fixture-shape admission can be generalized and the ordinary
first-level side gate considered for opening. GDBJIT/PERFTOOLS integration and
side-of-side local-cell recording remain later work.
