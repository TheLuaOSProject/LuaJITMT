## GC2 published SSB stack helpers

The global `gc2.ssb_head` field is the MPSC stack where mutators publish full or
partial SSB nodes and GC2 workers/assists atomically detach work. This slice
routes initialization, acquire reads, CAS publication, partial-list republish,
and detach-by-xchg through `gc2_ssb_head_*()` helpers in `lj_obj.h`.

The ordering stays the plan/05 section 5.6.2 protocol: mutator publication uses
the node `next` helper, then CAS-publishes the head with acquire/release order;
drainers acquire/release-xchg the head before consuming published slots.

`m3_gc2_worker_scheduler` owns the observable SSB-drain behavior. Production C
access to `gc2.ssb_head` must stay behind the documented helper family instead
of source-text matching.
