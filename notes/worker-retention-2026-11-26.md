# Memory retained after worker exit

This diagnostic uses `78f117627cdecbe83600d9e1d39c0b0f923459c1`, after the
single-mutator plain-storage reuse and reader-progress fixes. Linux stability
remains the priority. The public workload completes correctly, but repeated
worker exit still accumulates memory while an unrelated worker stays alive.
The small-shrink change reviewed alongside this diagnostic does not fix that
retention.

The public matrix runs one, two or four workers with zero or two GC workers,
under both interpreter and JIT execution. It retains completed handles and
returned buffers during an initial phase, then drops them and checks automatic
collection during further churn. A separate sentinel workload parks a
long-lived worker on a public channel while short workers repeatedly exit.
Every payload, result and join check passes: 22 processes complete 48,460,000
verified buffer cycles. No process exceeds its 120-second or 512 MiB watchdog.

The sentinel checkpoints drop short-worker handles and transient results, then
perform three full collections after waves 6 and 12. Collector cycles advance
and return to IDLE. With four short workers and two GC workers, interpreter RSS
grows from 13,164 to 21,056 KiB between those checkpoints; JIT RSS grows from
10,292 to 15,860 KiB. Logical Lua memory grows too, by about 849 and 848 thousand
bytes respectively. Every sentinel configuration shows growth, including those
without a GC worker pool. These are finite measurements of retention, not a
proof that every retained allocation is unreachable.

A separate embedding census reads registered thread states and atomic plain
arena metadata under the existing SMR admission. It does not inspect foreign
bin bodies. It runs 16 waves of four workers, each doing 5,000 checked 1 KiB
buffer refills. Main joins each worker, validates and explicitly frees its
returned buffer, and drops the handle. Three full collections follow each wave.
An unrelated sentinel remains alive. The finalizer census additionally matches
retired-record addresses to their allocator owners. That traversal relies on
the baseline's terminal-only record lifetime and must change when runtime
record reclamation is implemented.

| After wave 16 and three full collections | Count |
| --- | ---: |
| Live / dead thread states | 2 / 64 |
| Dead-owner plain arenas | 128 |
| Completely empty dead-owner plain arenas | 64 |
| Dead-owner allocated starts | 85 |
| Dead-owner late starts / admitted actors / remote queues | 0 / 0 / 0 |
| Retired finalizer records, global / dead-owner | 128 / 64 |
| Lua-accounted memory | 2,137.51 KiB |

After stopping and dropping the sentinel, disabling the GC pool and performing
five full collections, no dead thread state remains in the registry. The main
allocator now owns 130 plain arenas, 65 empty, and 235 allocated starts. Lua
memory falls to 168.12 KiB. This is allocator transfer and metadata release;
it does not unmap the transferred plain capacity. Twenty-one of the 85 earlier
dead-owner starts are not classified by the finalizer-record match. They must
not be freed solely because their allocating worker exited.

Source review identifies three independent causes:

1. Production sweep does not trim owned PLAIN arenas. Dead-owner checkpoints
   drain explicit frees, but allocator transfer requires global quiescence:
   no other managed worker, no GC pool and no active collector. Empty capacity
   attached to dead owners therefore stays unavailable to active allocators.
2. Each buffer creates a 32-byte userdata finalizer-registration node. Unlinked
   records remain on a retired list until universe teardown. The census finds
   one such record in each dead worker's allocator, enough to pin another arena.
3. Dead thread-state bodies and registry metadata also await globally quiescent
   reclamation. A thread-state body includes two embedded 8 KiB barrier buffers
   and other runtime state. It is allocated by the spawning parent, so it is
   separate from the child's owned-arena census.

The next changes need separate lifetime proofs. Empty PLAIN trimming should
examine bounded candidates under physical reclamation authority, close fresh
arena admission, and recheck all allocation, late, queue and side-owner state
before deleting the registry entry and unmapping. A blocked candidate must not
hide empty successors. Retired finalizer records need leased list traversal,
validated physical unlinking and a bounded consumer; point-in-time registration
checks do not retain a node. Dead thread-state transfer must preserve raw owner
lookups, durable wake routing, registry borrows and shared result storage. Merely
removing the current global-quiescence predicates would be unsafe.

The [finalizer-record proposal](evidence/worker-retention-2026-11-26/ownership/finreg-design-v2.txt)
uses leased active-list traversal, frozen links before unlinking, and a retired
stack containing only fully unlinked records. Independent consumers remove one
record at a time. An earlier private-batch design was rejected because a paused
consumer could retain an entire backlog. The proposal still needs runtime
integration, exact drain-context lifetime checks and the listed pause tests;
it is not an implemented reclamation guarantee.

The [public report](evidence/worker-retention-2026-11-26/public/REPORT.txt),
[exact census](evidence/worker-retention-2026-11-26/census/finreg-baseline.stdout)
and [source audit](evidence/worker-retention-2026-11-26/ownership/review.txt)
preserve commands, source and executable identities, all checkpoints and the
proposed reclamation boundaries. No worker-reclamation runtime change is
included in this diagnostic. Windows and macOS remain deferred until release
readiness.
