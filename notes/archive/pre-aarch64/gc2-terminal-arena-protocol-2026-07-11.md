# GC2 terminal arena protocol checkpoint (2026-07-11)

Status: implemented checkpoint, not a completion claim. The focused allocator
and GC-sweep suites and deterministic interleaving model pass. The cdata/object-
view and broader caller-lifetime items listed below remain P0 work.

## Packed gate and linearization points

`GCAhdr.remote_active` is one naturally aligned 64-bit atomic at header offset
56. Its high bits are `CLOSED`, `SEALED`, and `PENDING`; the low 61 bits are the
publisher count. `remote_free` remains at offset 64 and `GCAhdr` remains 128
bytes. Supported x64 processes cannot create 2^61 simultaneously live
publisher contexts because every admission owns nonzero address-space-backed
execution state. Violating that platform invariant aborts instead of dropping a
lifetime intent.

An owner gets header/bitmap mutation ownership only through exact
`CLOSED|SEALED` with count zero. A pre-commit bit/status producer atomically
adds count plus `PENDING` before publishing its bit. The owner may clear pending
only from exact `CLOSED|SEALED|PENDING`, then terminal commit is exact
`CLOSED|SEALED -> SEALED`. Adoption and abort restore rebuild allocator bins in
private staging and publish OPEN only through exact `SEALED -> 0`; losing this
CAS rolls owner-visible metadata back and unseals to CLOSED without waiting.

After terminal commit, rescue is read-only. Terminal bitmap words are release
published before sweep sidecar words are reset to WHITE. A committed reader
loads state first, accepts only WHITE, acquire-loads and rechecks block, then
rejects `late`. Thus a dead cell is observed either as pre-reset FREEING or as
post-reset WHITE plus block0; it cannot be synthesized as live across the two
generations. A committed reader never writes mark state that terminal apply can
overwrite.

## Late frees and abort restore

Terminal/grace-late frees publish only a sidecar bit. They do not overwrite the
still-SMR-visible body with an intrusive queue node. Admission publishes
`PENDING` first, and the release leave edge wakes sweep progress. A current
generation late bit pins block1 and is consumed only by a later PREPSWEEP,
which changes that start to FREEING before a fresh grace.

Abort restore takes its exact clean LP before irreversible sidecar/bitmap work.
PREPSWEEP already exchanged late bits before setting FREEING, so restore cannot
infer provenance from `late == 0`: it re-pins every block1 FREEING start,
normalizes it to WHITE, and does not make it reusable. This conservatively
delays ordinary remote frees by one complete cycle. A failed owner restore
leaves SWEEP active; forced close checks every ready TG for nonzero
`prepare_epoch` or a remaining NEEDSWEEP list before publishing IDLE.

## Mark results and object reads

The internal small-object mark result is tri-state:

* `DEAD`: no header, direct body, or payload dereference is permitted;
* `LIVE_ALREADY`: lifetime was retained but this is not first discovery;
* `NEW`: this call installed the first mark or won RETIRED-to-LIVE rescue.

Public `lj_gc2_markmem`/`markobj` interfaces keep their old boolean meaning and
return true only for `NEW`. Ordinary edge discovery queues semantic traversal
only for `NEW`. Explicit SSB work and sweep semantic roots process both live
states. Direct table/function bodies are preserved only after a live result.
Small object marking now joins the arena gate even in OPEN and holds the counted
scope across type/base/direct-body reads; this closes RESET/adoption reuse across
ordinary validation. Expected-type retained child scopes are used for function
proto and upvalue direct bodies to prevent stale pointers from recursively
driving arbitrary object types.

## Deallocation-ticket precondition

For one allocation generation there is one exact-size deallocation right. Its
sole first/only free publication is synchronous with the logical retirement LP;
collector, destructor, and deferred consumers publish that one free only after
owning retirement. A same-address publication which starts after that cell has
been adopted and reallocated is an invalid double-free, not a legal delayed
first producer. Internal code must preserve this invariant. If a real producer
cannot, its caller must carry an allocation generation/ticket; pointer plus size
alone cannot distinguish the old and new allocation while preserving ABI.

## Remaining P0 work

* Variable/over-aligned small cdata can have an interior header on a block0
  extent, and huge cdata validation currently precedes an exact containing-slot
  mark. They need a retained object-view operation (bounded small start lookup
  and atomic huge mark-containing CAS) before header validation.
* Variable/aligned cdata is presently allocated as PLAIN while only
  TRAVERSABLE arenas enter the GC sweep. It needs a sweepable non-graph
  allocation kind/flag; generic PLAIN raw storage must not be swept.
* The broader mark-lifetime caller audit remains tracked in
  `gc2-mark-status-lifetime-audit-2026-07-11.md`, including weak/FINREG,
  table snapshot, and raw mark-then-dereference sites.
* Ignoring custom `lua_Alloc` remains a temporary, explicitly accepted project
  restriction. It must be replaced with a compatible retained-allocation
  protocol before claiming full LuaJIT API/ABI allocator compatibility.

## Evidence at this checkpoint

* Full x64 Linux build passes.
* `m2_arena_all` passes, including `m2_arena_sweep` and
  `m2_arena_gcsweep`.
* The split-table reclamation regression is fixed: `GCtab.colo`'s high bit is
  resize state, not part of the colocated allocation size. Physical-size and
  GC validation now use the masked canonical accessor, and
  `m5_tab_colocated_resize` passes.
* The standalone C11 model uses a lock-free 64-bit atomic gate and exhaustively
  covers exact commit, pending reconciliation, block-before-sidecar-reset
  committed reads, late publication, adoption rollback, and OPEN arbitration.
  It passes 208 rescue schedules, 41 late-free schedules, and 174 adoption
  schedules; every deliberate protocol mutant produces a counterexample under
  GCC, Clang, ASan, and UBSan builds.
* `git diff --check` passes.
