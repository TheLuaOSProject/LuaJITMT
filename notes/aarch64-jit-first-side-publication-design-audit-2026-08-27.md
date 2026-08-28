# AArch64 first-side publication design audit (2026-08-27)

## Verdict

The generic `trace_stop()`/`trace_save()` path is not a valid lockless
publication transaction for the first ARM64 side child. It can block entering
SMR, exposes the GC body before topology is complete, allocates GDB metadata
after publication, performs PERFTOOLS I/O, and invokes the TRACE stop callback
while recorder ownership is still live. None of those operations has a sound
rollback after a child becomes visible.

Production side recording must remain closed. The first child needs a dedicated
prepare/seal/publish path whose sealed suffix is allocation-free, nonwaiting,
nonthrowing and callback-free.

## Abortable prepare phase

Before the exact `ASM -> PUBLISH` CAS, complete every operation that can fail,
wait, allocate, retry, write a file, protect memory through a fallible generic
path or invoke user code:

1. finish optimization, assembly, semantic/post-RA and linked-tail validation;
2. synchronize mcode and reserve the exact pending child generation;
3. allocate the compact child, snapshots, exit table and optional debugger
   resources without transferring scratch ownership;
4. capture exact parent, canonical root, trace-vector, pending-slot, selected
   snapshot, topology, raw exit-target and child identities in a private plan;
5. precompute raw authenticated fallback and child-target encodings;
6. acquire one `lj_gc2_smr_read_try()` reader and perform the final complete
   revalidation; and
7. seal with the exact same-word `ASM -> PUBLISH` CAS.

A lost CAS releases the reader and uses normal unpublished cleanup. There is no
normal rollback after a successful seal.

This audit produced the split recorded in
`aarch64-jit-mcode-commit-split-2026-08-27.md`.
`lj_mcode_commit_prepare()` now completes the protection/RUN transition without
advancing `mctop` and captures the exact old/new top generation;
`lj_mcode_commit_publish()` consumes that plan using only a checked top store.
The dedicated first-child path still needs to place those two calls on the
opposite sides of its final seal.

## Sealed suffix order

Retain the successful SMR reader through the last runnable-edge CAS. Every
unexpected value after the seal is an invariant failure, not a Lua error:

1. complete the nonfailing mcode-top publication;
2. initialize the compact child and transfer scratch ownership;
3. link the initialized child into the GC root structure;
4. CAS its exact slot `LJ_TRACE_PENDING -> child`;
5. perform a no-fail/no-drain GC publication catch-up;
6. CAS the selected parent snapshot topslot to the required maximum;
7. CAS root `nchild` exactly `0 -> 1`;
8. CAS root `nextside` exactly `0 -> child`;
9. CAS-loop the current non-DONE snapshot count to `SNAPCOUNT_DONE`, allowing
   concurrent hot-exit increments before that transition;
10. raw-CAS the parent exit from the captured authenticated fallback encoding
    to the precomputed authenticated child target, last;
11. clear scratch ownership and the certificate, then release the SMR reader;
12. publish terminal IDLE and release the recorder token; and
13. only afterward handle optional debugger/perf state, detached events and GC
    pressure/stepping.

The mcode publication plan is one-shot even if rollback and the next reserve
reuse the same pointers: it carries the exact odd reservation generation, and
commit/abort plus area replacement advance that generation without wrapping.

The trace-slot CAS publishes the initialized GC body. The last parent-exit CAS
is the runnable-graph linearization point. Transaction-owned shared fields use
exact expected-value CAS operations, never unchecked stores or a generic
increment loop.

## GC phase race

An SMR reader prevents physical reclamation; it does not freeze the collector's
phase. `gc2_mark_begin()` can race IDLE to MARK while the recorder owns the JIT
token. Sampling IDLE before publication is therefore insufficient.

The child must first be GC-linked and slot-published, then pass a phase-aware
catch-up barrier before topology and the runnable edge become visible. If MARK
already began, the barrier must mark the new graph. If it still observes IDLE,
the already linked body must be found by the next root scan. The suffix helper
must return success unconditionally: any inability to mark immediately must
install a durable retain/veto state rather than allocate, wait, throw or return
to generic trace abort.

## ARM64e raw representation

`trace_exittarget_arm64_acq()` strips PAUTH and is not a publication
certificate. An unsigned or wrong-discriminator slot can strip to the same raw
address. Final validation must load the stored raw bits and compare them with
`trace_exittarget_arm64_encode(g, fallback)`. The last publication operation
must use a raw pointer CAS from that captured encoding to the precomputed child
encoding. ARM64e+BTI negative tests must mutate the signature and discriminator,
not only the stripped target address.

## GDBJIT split

Current GDB registration is unsafe for this transaction:

- `trace_save()` first publishes the trace and only then calls
  `lj_gdbjit_addtrace()`;
- its entry allocation can throw OOM after scratch ownership has been cleared;
- object construction copies the filename twice into a fixed 4096-byte buffer
  before its size assertion, so a long chunk name can overwrite the stack; and
- successful descriptor registration cannot be rolled back reliably because
  runtime descriptor locking is deliberately one-shot.

Split it into private prepare, one-shot commit and abort operations. Preparation
must run before the final seal, read `J->cur`, authenticate an exact side parent,
bound the two filename copies before emission, and preferably treat allocation
failure as omission of optional metadata. Commit runs only after semantic
publication and performs one descriptor-lock attempt without allocation or
freeing. Cleanup of an uncommitted preparation occurs after token release.

PERFTOOLS output has the same placement rule: no file creation, writes or
STOPREQ-capable work inside the sealed suffix.

## Event handoff

The TRACE stop callback currently runs arbitrary Lua while the recorder token
and scratch transaction are live. A first-child publisher must instead freeze
the event inputs, pin any published source required by the callback, complete
terminal token handoff, then deliver the callback using the detached flush-event
pattern. No vmevent, Lua string allocation or protected call belongs between
PUBLISH and the final exit CAS.

## Retirement symmetry and proof gates

Retirement must reverse publication: restore the parent exit fallback, unlink
root `nextside`, then decrement `nchild`, while retaining the child body in its
retired slot until epoch grace, zero readers, token ownership and native-pin
checks permit reuse. Existing retirement leaves the parent snapshot DONE; treat
that as deliberate retry suppression unless a separate token-owned reopening
policy is designed.

The publication test must execute the exact parent-1/exit-2 child under both
arm64 and arm64e+BTI and prove the child slot/body, topology, snapshot state,
raw authenticated target, actual native side execution, last-store ordering and
clean token/SMR/certificate handoff. Inject pre-seal abort, closed SMR, vector or
slot ABA, plan mutation, mcode retry and optional-debug failures. Add active-MARK
and IDLE-to-MARK race cases, then flush/retire/reuse the published child.

A source contract must reject allocation/free, blocking SMR, errors, callbacks,
GDB/perf operations, GC stepping and unbounded retry inside the sealed suffix.
