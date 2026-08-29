# JIT pre-MT NEWREF hash stores, 2026-07-03

Pre-MT nonnumeric `NEWREF` `HSTORE` now uses the returned hash value slot
directly for primitive/non-GC values. `lj_tab_newkey()` still owns insertion,
claiming, resize, key publication, and the weak-key barrier before returning
the slot; the traced store then writes the primitive value without the extra
`lj_tab_storetv_forjit_hash()` helper call.

The direct path is intentionally narrow. Numeric `NEWREF` stays helper-routed
because it can resolve to an array slot. GC-object values stay helper-routed
because the value edge and weak-write/parent-barrier work cannot be skipped.
After `mt_entering` or `mt_active`, published table stores still use the
CAS/helper route so concurrent resize, forwarding, and retired generation
checks remain outside raw trace stores.

This changes code shape, not the current `tab_hash_write` result: steady-state
prefilled hash writes are already near stock, and the remaining benchmark gap is
mostly in string construction, GC pacing, and lockless new-key/resize work.
