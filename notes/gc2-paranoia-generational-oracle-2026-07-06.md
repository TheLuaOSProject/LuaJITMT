# GC2 paranoia oracle and generational marks

`lj_gc2_test_paranoia_root_diff()` is a reverse mark oracle: it scans
traversable arena cells with both `block` and `mark` set, then checks whether
the cell base appears as a non-white or fixed object on the repaired legacy root
spine. It is useful after non-preserving sweeps, but it is not a valid zero-diff
assertion while generational mode intentionally preserves old-generation arena
marks.

`test_minor_major_paranoia()` now disables generational mode before its final
`lj_gc_fullgc()` and zero-diff assertion. That keeps the true-minor coverage but
asks the oracle only after a sweep that can clear preserved old-generation marks.

During the same investigation, deferred arena body free was tightened:
`lj_mem_freegco_defer()` now clears the cell's mark bit when it stamps `gct=0`.
The destructor has already run, and the block bit keeps the cell unavailable
until arena sweep owns reuse; clearing the mark prevents preserving generational
sweeps from treating a tombstone as an old-generation live body.
