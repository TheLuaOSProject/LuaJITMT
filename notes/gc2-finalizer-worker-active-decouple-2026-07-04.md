# GC2 idle finalizer drain no longer claims worker_active

`gc2_worker_finalizer_drain()` now drains the idle finalizer MPSC stack under
the finalizer owner token (`finalizer_active` plus `finalizer_owner`) only. The
worker path first requires a real TG owner id and refuses the TLS-less `~0u`
pseudo-owner, because multiple helper threads without TLS would otherwise look
like a reentrant finalizer owner. This path just splices finalizer nodes from the
producer stack into the single-owner finalizer ring; it does not traverse the
GC2 grey deque, drain weak tables, advance sweep batches, or close a GC2 phase.

`worker_active` remains required around those grey/weak/sweep/close paths because
they still share single-owner bridge state. Keeping idle finalizer draining out
of that token prevents a held grey/sweep owner from also blocking unrelated
finalizer queue publication work while the collector is idle.

The scheduler regression in `tests/t-gc2-worker-scheduler.c` holds
`worker_active` while enqueuing finalizers and requires the worker to drain the
MPSC stack without clearing that token. That checks the intended ownership split
without relying on a source-text guard. The same fixture stops the parked worker
pool and verifies a plain pthread with no TG cannot drain the MPSC stack through
the TLS-less pseudo-owner.
