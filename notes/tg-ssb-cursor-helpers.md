## TG SSB active cursor helpers

The active SSB cursor triple is shared between mutator publication, assist
drain, and the GC2 fixpoint empty predicate. This slice routes production C
access to `ssb_active`, `ssb_base`, `ssb_next`, and `ssb_end` through
`lj_tg_ssb_*()` helpers in `lj_tg.h`.

The ordering remains the plan/05 section 5.6.2 protocol: stores release-publish
cursor resets and cursor movement after slot writes or clears; readers use
acquire loads before computing SSB occupancy.

The M3 GC2 worker scheduler coverage now requires the helper family and documents why raw
production C access to the active cursor fields.
