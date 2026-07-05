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
