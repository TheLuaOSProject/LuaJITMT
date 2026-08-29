# AArch64 first-side transactional retirement (2026-08-28)

## Scope

This checkpoint adds the exact inverse of the one-shot AArch64 first-side
publication transaction. The admitted parent-1/exit-2 child can now be retired
through the production GC claim, scoped-flush and full-flush routes without
leaving a native parent edge to retired machine code.

This does not open the production side recorder. The ordinary build still has
`LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED == 1`; creation of this exact child
still requires `LJ_TRACE_TEST_HELPERS` and
`LJ_ARM64_FIRST_SIDE_PUBLISH_TEST`. The retirement implementation is in the
production paths so the test-only published generation exercises the same
teardown machinery which a later production admission will use.

## Exact inverse grammar

Retirement first reconstructs the same finite generation certificate used for
publication. With the recorder token held and both bodies protected by an SMR
reader, it validates:

- the child trace slot, trace number, parent/root/link fields and exact compact
  IR, snapshot and native-code body shape;
- the parent root generation, single-child topology, trace slot and exact
  loop-admission marker;
- the shared prototype and the selected parent snapshot continuation;
- the terminal `SNAPCOUNT_DONE` state and conservative topslot 5;
- the arm64e body-discriminated child entry authentication; and
- the exact raw parent exit-table word, encoded using the owning global
  state's exit-target discriminator.

The plan accepts only the live unretired endpoint or the listed, pin-closed
retired endpoint. Scoped pending is orthogonal and is accepted at either
endpoint. A candidate which loses this exact grammar after the token and SMR
reader are held is corruption and fails closed; it is not handed to the legacy
side-unlink path.

## Transaction ordering

The inverse is deliberately ordered differently from generic retirement:

1. compare/exchange the authenticated raw parent exit target from the exact
   child encoding to the exact fallback encoding;
2. publish the child's retirement epoch, close native pins and link the intact
   body into the retired list;
3. compare/exchange the parent's `nextside` from the child number to zero;
4. compare/exchange the parent's `nchild` from one to zero; and
5. reset only the retired child's local exit table and routing fields.

The first CAS is the native-unreachability linearization point. No new parent
exit can enter the child after it, while a native user which fetched the old
edge is covered by the later retirement grace period. The body becomes
discoverable on the retired list before GC2 topology stops naming it.

`nextside` is removed before `nchild` because GC2 follows the side chain. Root
entry already rejects both finite intermediate states, so the inverse needs no
lock and no repair transaction. The selected snapshot remains terminal and its
raised topslot remains sticky; clearing either would permit a stale parent exit
to record a replacement against this already-consumed generation.

On arm64e, the acquired encoded `void *` word is compared using the same raw
pointer convention as first-side publication. The existing PAUTH negative
contract substitutes the same stripped address with a different discriminator
and proves that this convention rejects it. Retirement additionally checks the
child body's `mcauth` representation explicitly and uses a raw compare/exchange
for the parent edge. Stripped-address equality is never an idempotence signal.

## Retirement routes

### GC claim

`lj_trace_retire_gc_claim` acquires the recorder token if needed, then repeats
the already-retired/discoverable test under that token. This closes the window
where a peer could finish synchronous retirement after the optimistic entry
snapshot. An exact admitted child takes a nonwaiting SMR reader, proves there
is no terminal trace-link source, removes the parent mcode edge, publishes the
retirement descriptor and disconnects topology in one token transaction.
Temporary token or SMR contention returns retry instead of waiting in a GC
worker.

### Scoped flush

The scoped mark sets the pending entry gate and immediately changes the exact
parent edge to fallback. After the EXIT_TRACES boundary, the token owner
revalidates that same fallback while the graph is SMR-pinned, publishes
retirement, removes topology and retires the slot. A live non-pending admitted
child refuses the recorder-abort unlink route because that route has no
retirement half and would otherwise create a detached but live generation.

### Full flush

The ordinary destructive loop may visit the root before or after its child,
depending on trace-number allocation. A bounded prepass therefore visits exact
admitted children while every parent slot and body is still live. It performs
only edge removal, retirement publication and topology collapse. The existing
reverse pass remains the sole owner of debug registration, bytecode,
prototype, exit-table, trace-slot and mcode-area teardown. It skips repeat
retirement only for an exact candidate already listed by the prepass; generic
traces retain the original `trace_retire` behavior.

## Idempotence and reclamation

The public child slot and complete compact/native body remain intact until the
retired-list grace interval permits reclamation. A repeated GC claim sees the
listed body and returns success without publishing a second descriptor.

Later disconnect/reclaim can run after the former parent has already been
removed. The exact topology helper therefore recognizes the child-local
terminal state—nonzero retirement epoch, listed body, closed pins and cleared
routing fields—before looking up the former parent. This reentry performs no
parent access and no second topology mutation.

## Validation

The focused executable contract creates a fresh root and first child for each
mode and runs every mode twice as arm64 and twice as arm64e with BTI/PAUTH. It
proves:

- GC claim publishes exactly one retirement descriptor, preserves the complete
  child body through grace, is idempotent on a second claim and executes the
  parent fallback twice;
- scoped flush retires only the child, preserves a runnable parent and its
  prototype/bytecode entry, and executes the fallback twice;
- full flush publishes exactly two descriptors, preserves both bodies through
  grace, clears both public trace numbers and interpreter/prototype entry, and
  detaches the shared mcode area;
- all three routes restore the exact authenticated fallback, collapse topology
  to zero/zero, preserve terminal snapshot metadata, and leave no token or SMR
  ownership behind; and
- the source contract pins edge-to-list-to-topology ordering, exact raw-target
  rejection, topology CAS order, prepass placement, generic-path preservation
  and parent-free idempotent reentry.

The only compiler diagnostic in the focused builds was the pre-existing unused
`ccall_rawchild_wait` warning.

Focused and adjacent checks passed on this Apple Silicon host:

- `tools/ci/arm64_jit_first_side_publish_contract.sh`;
- `tools/ci/arm64_jit_exit_contract.sh`;
- `tools/ci/arm64_jit_live_flush_reuse_contract.sh`;
- `tools/ci/arm64_jit_mcode_retire_contract.sh`;
- `tools/ci/arm64_jit_root_entry_contract.sh`;
- `tools/ci/arm64_jit_side_ir_admission_contract.sh`;
- `tools/ci/arm64_jit_side_asm_consumption_contract.sh`; and
- `tools/ci/arm64_jit_side_ingress_metadata_contract.sh`.

The runners restored the ordinary ARM64 experimental-helper build afterward
and confirmed that the first-side publication-only symbols were absent.

## Remaining work

The inverse blocker named by the first-side publication checkpoint is now
closed for the exact one-shot generation. The next step is to replace the
hard-coded parent-1/exit-2 fixture certificate with a production first-level
side-admission grammar while preserving the same publication and retirement
transactions. The ordinary side recorder must remain closed until that
generalization has equivalent arm64 and arm64e execution, race, flush and
reclamation coverage. GDBJIT/PERFTOOLS integration and side-of-side recording
remain later work.
