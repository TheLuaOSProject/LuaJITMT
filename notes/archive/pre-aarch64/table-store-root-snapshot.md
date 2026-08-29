# Table store root snapshots

Helper-backed table stores now use writer-side array/node root snapshots before
validating current slots. These snapshots pair `GCtab.array` with the
separated-array header size and `GCtab.node` with the node-header mask, then
recheck that the root pointer stayed stable.

This is intentionally separate from `lj_tab_array_snapshot_acq()`: ordinary
readers wait when they select a retiring separated array, while store helpers
must be able to consume retiring/FORWARD roots and route the write to the
successor generation.

The guard lives in `m5_tab_cas_store`; it poisons the legacy `asize`/`hmask`
mirrors and verifies keyed CAS plus helper-backed array/hash stores still use
the AHdr/NHdr-lite metadata.
