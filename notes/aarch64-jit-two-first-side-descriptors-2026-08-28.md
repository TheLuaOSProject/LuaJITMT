# AArch64 JIT two-descriptor first-side admission (2026-08-28)

## Scope

The production AArch64 first-side path now admits two independently observed
and executed first-level side-trace generations. This is a bounded expansion,
not a general side-trace gate: `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains
`1`, so side-of-side recording and every first side outside these two complete
descriptors still fail closed.

The policy is represented by one immutable descriptor selected by parent exit.
Geometry and register allocation are coupled in the same row; callers cannot
mix the bytecode/snapshot shape of one row with the allocator result of the
other.

## Exact descriptors

The two admitted rows are:

| Parent exit | Parent snapshots | Continuation PC | Child snapshot PCs | Inherited register | SLOAD register | Child-head move |
| --- | ---: | ---: | --- | --- | --- | --- |
| 2 | 8 | 13 | 13, 14, 3, 17, 7 | x28 | x27 | x28 to x27 |
| 6 | 9 | 10 | 10, 11, 3, 17, 7 | x27 | x28 | x27 to x28 |

Both children retain the already admitted five-snapshot, seventeen-map-entry
semantic grammar: inherited slot-4 integer, resumed `CGET`, guarded `ADDOV +1`,
limit load, `GT`, and `XPOLL`. The second row is not a new IR language; it is a
second exact parent edge, bytecode geometry, and mirrored allocator result for
that same semantic child.

The pure semantic/pre-head/post-RA fixture proves both positive rows, rejects
nearby exits, and rejects both geometry/register cross-products. The emitted
head and the publication-time reconstructed parent map are derived from the
selected row rather than from global x28/x27 constants.

## Lifetime and indexing order

Every production consumer selects the descriptor before using descriptor-owned
geometry:

- recorder metadata rejects an unknown exit before selected parent-view access,
  double-captures the parent generation, and compares the already range-checked
  continuation position with the descriptor offset using `proto_bcpos`;
- publication first proves certificate/J/parent-view agreement, then selects
  the descriptor, couples the parent snapshot count, and reconstructs the
  post-RA parent map from the descriptor's inherited register;
- root entry validates the compact child geometry and references before reading
  its `IR_BASE`, derives the selected parent exit from that immutable IR, and
  bounds it against both the parent snapshot array and exit table before
  forming either selected pointer;
- the root-entry double capture includes the derived parent exit and its final
  reread reloads that exact dynamic edge; and
- retirement selects the same descriptor from the child's `IR_BASE`, checks
  parent snapshot/exit-table bounds and the exact prototype continuation, then
  forms the selected snapshot and edge used by the transactional inverse.

No new persistent field was added to `GCtrace` or `jit_State`; the selected
exit remains derivable from the child's admitted immutable IR.

## Ordinary execution proof

The production fixture records two distinct root/child pairs without either
side publication test seam:

- exit 2 retains its original `n=3, bias=1` recording and native execution;
- exit 6 records with `n=3, bias=0`, then executes the published child with
  `n=2, bias=0`.

The separate exit-6 native input matters. `n=3` completes through parent exit
1 on a later iteration, while `n=2` takes parent exit 6 directly through the
published child edge. The fixture observes exactly one VM exit from that child,
at child exit 3, on repeated native calls.

An exit-6 probe with the same parent geometry but `ADDOV +2` remains rejected
with `LJ_TRERR_NYIIR`. It publishes no child, does not retarget the parent edge,
and leaves the broad gate closed.

Both admitted pairs also pass the post-token stop-request cleanup checkpoint
and the complete GC-claim, scoped-flush, and full-flush retirement inverses.

## Validation

The following focused contracts passed at their default run counts on this
Apple Silicon host:

- `tools/ci/arm64_jit_side_ir_admission_contract.sh` on arm64 and arm64e;
- `tools/ci/arm64_jit_side_ingress_metadata_contract.sh`;
- `tools/ci/arm64_jit_root_entry_contract.sh` on arm64 and arm64e;
- `tools/ci/arm64_jit_first_side_production_contract.sh`, including the
  ordinary no-helper smoke test and two executions of GC claim, scoped flush,
  and full flush on both arm64 and arm64e; and
- `tools/ci/arm64_jit_first_side_publish_contract.sh`, including two executions
  of every retirement mode on both arm64 and arm64e.

All five scripts passed shell syntax checks and source-contract checks. The
runner restored the ordinary experimental ARM64 helper archive and released
its test lock.

## Remaining work

This checkpoint establishes a second real production generation without
turning observations into a permissive relational rule. The next expansion
should find another naturally occurring side shape, add its complete immutable
geometry/allocator descriptor, and repeat the same ordinary arm64/arm64e
publication, native-entry, race, and retirement proof. Generic first sides,
side-of-side recording, and unsupported FFI/JIT surfaces remain closed.
