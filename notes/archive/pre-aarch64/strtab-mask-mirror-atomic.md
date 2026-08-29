# String table mask mirror atomics

`StrInternState.mask` is only a compatibility mirror of `StrTabHdr.mask`.
Lockless string-table readers should acquire the current `StrTabHdr` and use the
header as the authoritative bucket snapshot. The mirror remains for legacy fast
paths, diagnostics, and focused fixtures, so it must not be read or written as a
plain shared field while other TGs can resize the string table.

`lj_str_mask_acq()`, `lj_str_mask_store_rlx()`, and `lj_str_mask_rel()` make that
boundary explicit. Bootstrap stores the unavailable sentinel with relaxed
ordering before the state is shared. Resize publication release-stores the
mirror before publishing the replacement header, and tests acquire-load the
mirror when checking compatibility state.

The same helper boundary now covers the other mutable `StrInternState` scalars:
`lj_str_num_*()` owns conservative live-string accounting, `lj_str_id_add_rlx()`
owns range reservation for `GCstr.sid`, and `lj_str_second_*()` owns the
secondary-hash flag. The string seed remains a plain field because it is written
before the state is shared and then immutable.
