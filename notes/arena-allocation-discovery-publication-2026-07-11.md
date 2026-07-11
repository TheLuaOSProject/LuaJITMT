# Arena allocation-discovery publication (2026-07-11)

This change narrows several arena bitmap races without treating bitmap state as
GC2 root-admission authority.

## Implemented ordering

- `mark[]` updates use atomic read/modify/write operations. The x64 interpreter
  and traced one-upvalue FNEW paths use locked `bts`/`btr` to match the C
  primitive.
- An allocation start is release-published through `block[]` only after its
  birth mark is initialized. Positive `block[]` observations use acquire loads.
- `lj_arena_state()` sequences its block acquire before its mark load; C operand
  evaluation order is not used as a synchronization argument.
- Empty-table C and x64 VM bump allocation initializes the complete table header
  before publishing `block[]`.
- Closure/upvalue C bump allocation already initialized each body before bitmap
  publication. The backwards-emitting x64 JIT template was reordered by actual
  machine-code order: both bodies, locked marks, both block bits, then the exact
  pending-root chain. Its regression test inspects executable trace bytes, not
  the misleading source order of `emit_*` calls.

`block[]` remains single-structural-writer state. The owning TG allocator writes
while an arena is owned; RESET_ALLOC detaches it before terminal sweep/rebuild;
remote marker and free paths write mark/late/sweep side state, not block starts;
dead-TG transfer occurs only after source quiescence. This is why block
publication can remain a plain x86 store/RMW instead of adding a locked
instruction to every hot allocation.

## Deliberate limits

This is not a phase-close or reclamation proof.

- A cycle-reset `mark[]` clear can still race a newly admitted writer. The exact
  activation gate and root descriptors must close that admission window.
- `lj_arena_sweep_words()` uses atomic whole-word loads/stores but is only valid
  under its existing exclusive sweep ownership; it must not overlap mark RMWs.
- Generic raw arena allocation reserves/publishes storage before an arbitrary
  caller initializes its object header. A future arena census must not assume
  every observed block bit is immediately decodable until generic allocation is
  converted to reserve/initialize/commit or protected by typed descriptors.
- Dirty epochs and point root barriers remain conservative liveness aids, not
  leases authorizing phase close.

Dormant full-heap overflow epochs, arena-registry visibility flags, and physical
HugeTab enumeration from the discarded sampled-barrier experiment were removed.
They did not provide stable pointer lifetime or bounded recovery.

## Verification

- `m2_arena_bitmap`, `m2_arena_publication`, `m2_arena_sweep`,
  `m2_arena_gcsweep`
- `m5_x64_tnew_empty_inline`, `m6_jit_fnew_bump`, `m5_cell_ops`
- GCC ThreadSanitizer on the 200,000-round latch-controlled publication test
- Fresh executable-mcode inspection in `t-jit-fnew-bump.c`

The optional pre-existing `LJ_TEST_TRACED_FNEW=1` post-sweep counter assertion
fails identically at parent commit `0a21d783`; the default focused suite and the
new emitted-order guard pass.
