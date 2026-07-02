## TG SSB free-list CAS head

The per-TG SSB free-list head is a cross-thread handoff: the owning mutator pops
a fresh node during `gc2_flush_ssb()`, while a GC2 worker or assisting drain can
return a drained node through `gc2_ssb_recycle_node()`.

This slice routes the head through `lj_tg_ssb_free_*()` helpers in `lj_tg.h` and
uses a CAS-backed push/pop stack for the handoff. The owner TG is the only popper;
GC2 workers and assisting drains only push recycled nodes back to the owner. SSB
node `next` publication continues to use `lj_gc2_ssb_next_*()`, so the node link
and the head publish use matching acquire/release ordering.

The M3 GC2 worker scheduler guard now requires the helper family and documents why raw
production C access to `ssb_free`.
