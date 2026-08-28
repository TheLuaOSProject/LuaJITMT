# AArch64 first-side dry publication seal (2026-08-27)

## Boundary

This checkpoint proves the last recoverable boundary before the first ARM64
side-child publication transaction. It does not publish a child, retarget a
parent exit, update root topology, register debugger metadata or make side code
enterable. `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`; only the
dedicated test build reaches the seal, and it always aborts before
`trace_stop()`.

## Same-word abort ordering

`LJ_TRACE_PUBLISH` is a non-abortable active state in the existing atomic
`jit_State.state` word. `lj_trace_state_publish_try()` performs one exact
`ASM -> PUBLISH` compare/exchange:

- if asynchronous abort clears the ACTIVE bit first, the exact publish CAS
  fails and ordinary recorder rollback remains possible;
- if the publish CAS wins first, `lj_trace_state_abort()` observes PUBLISH and
  leaves it unchanged, so the token owner must finish a future bounded,
  infallible suffix.

This removes the invalid validation-then-publication window: parent retirement
cannot win between a separate final validation and a later unrelated state
publication. Production code cannot currently enter PUBLISH.

## Exact source and destination generation

The token-private parent certificate now also stores the exact `TraceVec *` and
reserved child number. Capture and revalidation require that this same vector
generation remains current and that its exact child slot still equals
`LJ_TRACE_PENDING`. A byte-identical replacement vector is rejected as an ABA
change. The already certified parent body, raw mcode, continuation instruction,
parent number and exit number remain authoritative.

The seal takes one nonwaiting trace-body SMR reader and retains it on success.
While it is held, it revalidates the certificate and constructs a direct-pointer
publication plan. A failure releases the reader before returning; the eventual
infallible suffix must keep a successful reader through its last parent-exit
CAS. Before attempting the state CAS the seal proves:

- the exact ASM token owner and canonical current first-side scratch;
- the still-private compact `J->curfinal` body and compact IR extent;
- exact scratch ownership of snapshots, exit table and synchronized mcode;
- canonical four-snapshot/thirteen-map and five-exit layout;
- every child exit slot contains the exact owning-`global_State` encoding of
  the shared fallback, compared as raw bits rather than a PAUTH-stripped
  address;
- the complete immutable semantic/post-RA certificate, including the x28 to
  x27 head shuffle and optional BTI prefix; and
- the parent exit slot still contains the exact captured raw fallback encoding.

The last item matters on arm64e: an unsigned or wrong-discriminator pointer can
strip to the same address but must not authorize publication or a later CAS.

## Native dry transaction

The dedicated assembler probe runs at the real post-assembly seam after mcode
fixup and cache synchronization. A successful dry seal enters PUBLISH, checks
all captured plan pointers and raw encodings under the retained reader, calls
the asynchronous-abort path and proves the state remains PUBLISH. Because this
checkpoint has performed no irreversible action, the test alone restores
`PUBLISH -> ASM` with an exact CAS, releases the reader, and then raises the
mandatory unpublished abort.

The fixture proves the reserved child slot remains PENDING, root child count
and side link remain zero, the selected snapshot is not DONE, the parent exit
still targets its fallback, no SMR reader leaks, and recorder/token/VM state
returns cleanly. It forces an `LJ_TRERR_MCODELM` retry and executes twice as
ordinary arm64 and twice as arm64e with BTI/PAUTH. Ordinary builds are checked
to contain neither dry-seal test API.

Focused validation on this Apple Silicon host:

- `tools/ci/arm64_jit_side_asm_consumption_contract.sh`;
- `tools/ci/arm64_jit_side_ingress_metadata_contract.sh`;
- `tools/ci/arm64_jit_side_ir_admission_contract.sh`; and
- `tools/ci/arm64_jit_b26_contract.sh`.

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella then passed,
including its ARM64/arm64e entry, exit, retirement, live-reuse and safepoint
runtime coverage.

## Remaining production blockers

PUBLISH deliberately has no production consumer yet. Before the side recorder
gate can open, the post-seal suffix still needs all of the following as one
audited transaction:

1. prepare all fallible GDBJIT state before the seal and bound long chunk names;
2. provide a no-fail, no-drain GC publication barrier covering an IDLE-to-MARK
   phase transition;
3. transfer compact-body, snapshot, exit-table and mcode ownership without an
   unwind path;
4. publish the GC root and exact PENDING child slot before topology;
5. update selected topslot, root child count, side chain and snapshot DONE with
   exact expected-value operations;
6. compare/exchange the captured raw authenticated parent fallback to the child
   target last;
7. move STOP callbacks and any PERFTOOLS I/O outside the token-held infallible
   suffix; and
8. prove exact one-child publication, flush/retirement, arm64e authentication,
   race, sanitizer and end-to-end execution behavior.

Until those are complete, this is a validated dry seal rather than an
implemented side-publication feature.
