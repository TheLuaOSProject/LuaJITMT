## TG SSB active cursor helpers

The active SSB cursor triple is shared between mutator publication, assist
drain, and the GC2 fixpoint empty predicate. This slice routes production C
access to `ssb_active`, `ssb_base`, `ssb_next`, and `ssb_end` through
`lj_tg_ssb_*()` helpers in `lj_tg.h`.

The ordering remains the plan/05 section 5.6.2 protocol: stores release-publish
cursor resets and cursor movement after slot writes or clears; readers use
acquire loads before computing SSB occupancy.

`m3_gc2_worker_scheduler` owns the observable SSB cursor behavior. Production C
access to the active cursor fields must stay behind the documented helper
family instead of source-text matching.
