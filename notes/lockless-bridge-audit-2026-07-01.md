# lockless bridge audit, 2026-07-01

Subagent audit of three reported warm-path bottlenecks:

## `lj_gc_linkobj` and legacy `gc.root`

- Verdict: partially accurate. Common GC object constructors still publish
  through `lj_gc_linkobj()` and CAS-prepend to the global `g->gc.root` list,
  and GC2 still bridges through legacy root-list sweep before arena sweep can
  fully close.
- Not literally every object uses the root-head CAS: strings publish through
  the string table, and some userdata/thread paths use `lj_gc_linkobj_after()`.
- Arena mark/block bitmaps are real and used, so the bitmap work is not lost;
  the missing endpoint is deleting `nextgc`/legacy sweep as an authoritative
  discovery path.
- Classification: temporary/changeable bridge. It should be removed as part of
  ADR-4 completion by making GC2 root/domain registries and arena sweep
  authoritative, moving finalizer/unlink discovery off the root list, then
  deleting `GCHeader.nextgc` and legacy `gc_sweep()`.

## `tab_struct_owner` and `KEYLOCK` waits

- Verdict: mostly accurate for the pre-follow-up bridge. The implementation
  used a global GC2 table structural owner token, not a pthread mutex, but it
  serialized structural table operations across unrelated tables and waited
  with a 1 ms native sleep.
- Readers that observe transient `KEYLOCK` can also park/retry through the
  table wait helper. That is a real gap against the lockless warm-path rule and
  the final cooperative table-resize design.
- Classification: temporary/changeable safety bridge. Current follow-up moves
  the structural owner token into `GCtab`, so unrelated tables resize/clear/
  compound-shift independently while same-table structural mutations remain
  serialized. Remaining work is per-generation resize ownership with bounded
  copy cursors, writer helping, reader hop/retry, and removal of the 1 ms
  `KEYLOCK` wait path.

Follow-up slice:

- `GCtab.struct_owner` now replaces `global_State.gc2.tab_struct_owner`.
  `lj_tab_struct_enter(L, t)` CAS-claims only the table being structurally
  mutated, and `m5_tab_struct_owner` verifies that one thread holding table A's
  structural owner does not block another thread from entering table B while a
  same-table entrant still waits for release.

## string interning reader pin

- Verdict: accurate for the pre-follow-up bridge; narrowed by the follow-up
  slice below. Normal string interning used to enter/leave the string-table
  header by CAS-adjusting `StrTabHdr.resize`, so successful lookup paid a
  shared-header RMW pair. Current HEAD moves that active marker to the owning
  `TGState`, so ordinary interning no longer mutates the shared header on every
  lookup/insert.
- The "pure per-bucket CAS" claim is overstated: the plan's linearization point
  is per-bucket CAS, but the same tracked design includes resize state and a
  helper-copy protocol. Current active-user markers are documented as a bridge.
- Classification: temporary/changeable, currently required by incomplete
  resize/sweep migration. Remove it only with the full string-table migration:
  bounded helper-copy resize, old/new dedupe during resize, SMR header lifetime
  without per-intern active counts, and Harris-style string sweep/unlink with
  grace-epoch reclamation.

Follow-up slice:

- `TGState.strtab_active_hdr/depth` now carry the active lookup/insert marker.
  `strtab_enter()` publishes the TG-local marker and rechecks the current header
  plus resize bit. Depth transitions use release stores read by resizer acquire
  scans, so the resizer has a stable ordering point without putting an RMW on
  either the shared string-table header or the owner TG's active-depth word.
  `strtab_claim()` still sets `StrTabHdr.resize` but waits by scanning live TG
  markers for the claimed header. This preserves the current destructive
  resize/secondary-rehash exclusion rule while removing the shared-header RMW
  pair from the normal intern path.
