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

## Follow-up on 2026-11-27

The [new census](evidence/worker-retention-2026-11-27/counts.csv) measures
the worker-control revision `433fe7bf2`, integrated as `bb0dacb44`. Four
workers per wave each refill and read a 1 KiB buffer 2,000 times. The
first wave's four results stay reachable and are checked after every
subsequent wave. A separate worker stays parked through the final sample.
The C observer takes an SMR read admission, reads the plain arena bitmaps
and checks whether the retired-record queue is empty.

| Background collectors | JIT | Peak dead groups after collection, waves 16–96 | Dead groups after releasing results | Remaining plain arenas in dead groups |
| ---: | --- | ---: | ---: | ---: |
| 0 | Off | 9 | 1 | 0 |
| 0 | On | 9 | 3 | 2 |
| 2 | Off | 9 | 1 | 0 |
| 2 | On | 9 | 2 | 1 |

The peak includes groups already unlinked and awaiting a grace period.
Every settled sample had zero empty dead-owner plain arenas, zero unlinked
groups awaiting a grace period, and an empty retired-record queue. These
measurements use explicit full collections.

Build the observation-only executable with the same definitions as the
runtime. Run the four combinations by changing `POOL` and `CENSUS_JIT`.
The CSV records each sample before and after the three collections per wave.

```sh
evidence=notes/evidence/worker-retention-2026-11-27
checks='-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS'
make -C src BUILDMODE=static XCFLAGS="$checks"
cc -std=gnu11 -O2 -mcx16 $checks -Isrc -Itests "$evidence/reap-census.c" \
  src/libluajit.a -Wl,-E -lm -ldl -pthread -o /tmp/lj-worker-census
POOL=2 CENSUS_JIT=on taskset -c 16-23 /tmp/lj-worker-census "$evidence/workload.lua"
```

The [automatic workload](evidence/worker-retention-2026-11-27/automatic-workload.lua)
uses allocation pressure with the same parked sentinel. With explicit
collection omitted through measurement, the baseline retained 384 dead
groups in every configuration. The dispatch checkpoint change in
`de06f6697` left 0, 1, 1 and 2 respectively, in the same table order.
The number already unlinked and awaiting grace fell to zero in all four.
An executing arithmetic sentinel gave a different result. With JIT enabled
the collector stayed in SWEEP and both pool sizes retained 384 dead groups
until the sentinel exited. [Automatic baseline and candidate measurements](evidence/worker-retention-2026-11-27/automatic-counts.csv).

## Automatic collection with an executing native peer

Each 96-wave run started four workers per wave, checked every join, and
verified four retained results until their explicit release. The main thread
only allocated during the measured interval. The sentinel kept computing.
On the baseline it retained 384 dead groups at both pool sizes. Separate
16-wave observations found SWEEP still active after all results were dropped.

With the SWEEP change in `7396408ce`, the collector can finish the sweep
after yielding a turn to native code. The collector keeps priority once the
rescue work from such a turn consumes a whole batch. At the settled
checkpoint the sentinel was still running and the retirement grace queue
was empty in both runs.

| GC workers | Baseline heap (KiB) | Fixed heap (KiB) | Fixed dead groups | Completed sweeps at checkpoint |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 90,356.3 | 1,019.0 | 1 | 272 |
| 2 | 90,356.2 | 1,158.7 | 2 | 284 |

The 96-wave baseline used `de06f6697`. The measured fix was
`ca4c342e6`, integrated as `a4ee560c2`. The CSV includes all wave
observations. Empty fields identify counters added to the observer after
the baseline measurement. To repeat the measurement, use the compiler
command above with `active-census.c` and run the executable with
`CENSUS_JIT=on`, `WAVES=96`, and `active-census.lua`.
[Census data](evidence/worker-retention-2026-11-27/active-counts.csv),
[observer](evidence/worker-retention-2026-11-27/active-census.c),
[Lua workload](evidence/worker-retention-2026-11-27/active-census.lua).

## Draining published work during SWEEP

On 2026-11-27 a single mutator reading 256 reachable tables through
`lua_rawgeti` reproduced an unnecessary handshake. One SWEEP turn advanced
the handshake epoch from 7 to 8 with 269 grey entries already queued. At
`33e16dd21` the collector drains published buffers, grey entries and recovery
work before asking every mutator to flush a private buffer. The existing
checks for completion still cover work arriving during the drain.
[Public table-read regression](../tests/t-gc2-sweep-quantum.c).

Twenty fresh processes per revision and CPU set ran with two background
collectors and JIT enabled. The main thread joined 48 workers, retained the
first worker's buffer and churned allocations with a live sentinel. The first
census followed the fixture's usual 12 sleep-and-allocation intervals. A
process above the limit of 16 detached or pending groups continued the same
2 ms sleep and allocation loop for up to five seconds. Retirement also
required a completed sweep and a published unlink epoch. The two revisions
alternated order. The three CPU sets ran concurrently on a shared host.

| Sentinel and CPUs | Revision | Met at first census | Met within five more seconds | Median handshake epochs | Median completed sweeps |
| --- | --- | ---: | ---: | ---: | ---: |
| Native, 2 CPUs | `a02525340` | 2/20 | 6/20 | 4,414 | 3 |
| Native, 2 CPUs | `33e16dd21` | 6/20 | 17/20 | 1,133 | 9 |
| Native, 6 CPUs | `a02525340` | 5/20 | 10/20 | 2,876 | 8 |
| Native, 6 CPUs | `33e16dd21` | 14/20 | 20/20 | 853 | 11 |
| Parked, 6 CPUs | `a02525340` | 20/20 | 20/20 | 1,762.5 | 14 |
| Parked, 6 CPUs | `33e16dd21` | 20/20 | 20/20 | 1,724.5 | 13 |

The medians use processes with both counter samples. Five baseline processes
missed native entry during setup. One candidate process timed out acquiring
the initial census. Two other candidate processes on two CPUs retained 48
groups at five seconds. All three are included in the denominator. The
production gate continues to use its original sampling interval.
[Per-process observations](evidence/worker-retention-2026-11-27/sweep-flush-trials.csv),
[allocation workload and census](../tests/t-tg-auto-reap.c).
