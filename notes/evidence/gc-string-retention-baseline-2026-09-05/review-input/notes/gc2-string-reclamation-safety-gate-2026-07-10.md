# GC2 runtime string reclamation safety gate (2026-07-10)

## Status

The tagged-link, independent sweep-owner, bounded TAG/grace/UNLINK/grace,
side-retirement, header-pin, and shutdown-drain machinery is present, but
production entry is intentionally disabled by
`LJ_GC2_STRING_BODY_RECLAIM == 0`.

This is a correctness gate, not a return to the legacy collector. GC2 remains
the only runtime/shutdown collector. Until the gate is removed, published
interned strings retain the historical table-owned lifetime and are reclaimed
by the terminal GC2 shutdown drain.

A focused development build with the gate enabled reclaimed all 8,192 seeded
unreachable strings before `lua_close`, while retaining a registry-rooted
canonical string. That result proved the driver and physical lifetime path, but
the concurrency audit found publication contracts which the test did not cover.

## Why production unlink is not enabled yet

### Late publication must defeat retirement atomically

An incoming `DEAD` rescue by an interner is an exact CAS and is safe. A generic
sweep-time publication (stack/TValue/table/JIT/FFI side root), however, currently
sets the arena mark without owning the incoming string-table edge. The unlink
owner can read mark-zero, then the publisher can set the mark, then the owner can
successfully CAS-unlink the still-tagged edge.

Merely checking the mark a second time narrows this window but cannot close it.
The required completion is an explicit per-string retirement state in which:

1. TAG publishes a recoverable candidate state before publishing `DEAD`.
2. Every semantic string publication atomically changes that state back to
   live (and clears the exact `DEAD` edge when it is still linked).
3. The unlink/free owner must win a state transition which no publisher can
   miss, not just sample a bitmap.
4. If unlink already won, a canonical reservation remains discoverable to
   interners until either the same body is re-linked or the publishing slot is
   safely canonicalized. Two equal live `GCstr *` values are not allowed.
5. The final `FREEING` transition participates in the same stale-value
   validation protocol as traversable-arena GC objects.

This protocol must cover racy Lua publications as required by the project, not
only `lj_str_new()` matches.

### Native raw-byte borrows need an explicit lifetime contract

Stock LuaJIT safely uses short unrooted `GCstr`/`strdata` borrows because the
collector cannot concurrently free them. GC2 may remotely acknowledge an
explicit native section. One concrete path is `clib_extname()`: it formats a
temporary string, pops the stack root, and passes the returned bytes to native
`dlopen`/`LoadLibrary` code.

Before runtime string bodies can be reused, all such internal borrows must be
covered by one of:

- a per-TG string hazard/root anchor held through the last byte access;
- an epoch-tagged borrow which the string reclaimer explicitly observes; or
- a proven global native-quiescence gate as a temporary conservative boundary.

The final design should use precise hazards for performance. A global
`any-native` deferral is safe but can retain arbitrary string garbage behind an
unrelated long FFI call, so it is suitable only as a stepping stone.

## Hardened pieces which remain useful with the gate closed

- String headers use distinct `RESIZE` and `SWEEP` owner bits. Ordinary
  interners ignore `SWEEP`; resize and secondary rehash fail fast behind either
  destructive topology owner.
- Rebuilds transfer each target's incoming `DEAD` bit to its new incoming edge,
  while preserving the bucket's independent `SECONDARY` bit.
- Exact matches mark/preserve before final incoming-edge validation and clear
  `DEAD` by exact CAS.
- Nonmatching traversal during an active GC phase preserves the predecessor,
  validates its incoming edge, and clears `DEAD` by exact CAS before following
  `nextgc`. This prevents traversal through a detached predecessor chain.
- New strings are explicitly marked before publication in MARK/WEAK/SWEEP.
- Secondary collision rehash no longer recursively re-enters `lj_str_new()`
  while a sweep topology owner is present.
- The unsafe whole-table fixed-string root scan was removed. `fixstring()` marks
  at promotion, weak processing treats strings as non-clearable, and both TAG
  and UNLINK independently retain `FIXED|SFIXED` strings.
- String-table read pins now record their entry handshake epoch, allowing a
  future grace transition to wait for only pre-tag readers rather than all new
  intern traffic.

## Gate-removal checklist

The default may change to enabled only after all of the following pass in
ordinary, worker, JIT-on, assertion, ASan/UBSan, and race-stress builds:

1. Generic late publication wins both before and after the unlink owner's last
   liveness observation without producing a duplicate canonical string.
2. Exact interner rescue and unlink each win their CAS race safely.
3. A pre-tag pinned reader can remain remotely acknowledged through multiple
   epochs without UAF or stale-chain return.
4. Internal native raw-byte borrowers remain mapped through their last use.
5. Resize/secondary rebuilds preserve DEAD ownership exactly.
6. Metadata OOM retains a discoverable canonical body and later recovers.
7. Major cycles shrink both string count and table capacity; minor cycles do
   not collect outside their generation contract.
8. Worker reconfiguration transfers outstanding retirement metadata safely.
9. Shutdown from every subphase drains linked, pending, retired, and rescued
   ownership exactly once.
10. Collision-heavy and ordinary workloads remain near or above stock LuaJIT
    throughput; retirement metadata is pooled/batched rather than one fresh
    allocation per dead string.
