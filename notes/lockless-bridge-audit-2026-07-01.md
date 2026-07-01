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

- Verdict: mostly accurate. The implementation uses a global GC2 table
  structural owner token, not a pthread mutex, but it serializes structural
  table operations across unrelated tables and waits with a 1 ms native sleep.
- Readers that observe transient `KEYLOCK` can also park/retry through the
  table wait helper. That is a real gap against the lockless warm-path rule and
  the final cooperative table-resize design.
- Classification: temporary/changeable safety bridge. Do not remove it as a
  small patch. The planned replacement is per-table/per-generation resize
  ownership with `next_gen` publication, bounded copy cursors, writer helping,
  reader hop/retry, and epoch retirement.

## string interning reader pin

- Verdict: partially accurate. Normal string interning currently enters/leaves
  the string-table header by CAS-adjusting `StrTabHdr.resize`, so successful
  lookup pays a shared-header RMW pair.
- The "pure per-bucket CAS" claim is overstated: the plan's linearization point
  is per-bucket CAS, but the same tracked design includes resize state and a
  helper-copy protocol. Current active-user pins are documented as a bridge.
- Classification: temporary/changeable, currently required by incomplete
  resize/sweep migration. Remove it only with the full string-table migration:
  bounded helper-copy resize, old/new dedupe during resize, SMR header lifetime
  without per-intern active counts, and Harris-style string sweep/unlink with
  grace-epoch reclamation.
