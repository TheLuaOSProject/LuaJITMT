# Closure/upvalue performance diagnosis

Measurements started on 2026-09-04 and finished on 2026-09-05. Linux x64,
GC enabled, JIT enabled. This is a diagnosis of the frozen leaf-publication
runtime, with no runtime source changes and no performance acceptance claim.

The complete pilot's 59.62-fold closure gap has two reproducible components.
Frequent automatic collections repeatedly trace the harness's live key tables.
A preceding large insertion workload then more than doubles closure time by
leaving hundreds of mostly empty arenas in the allocator's reclaimed list;
those arenas repeat preparation, sweep, grace, and quarantine completion every
cycle. The insertion tables and almost all their keys have become unreachable.
The extra work is retained capacity, not retention of that logical object graph.

## Frozen inputs and measurement limits

The [prior leaf review](gc-sweep-leaf-performance-2026-09-04.md) records the
complete JIT pilot: 4,381.87 ns/op for this fork versus 73.50 for pinned stock.
This diagnosis uses those exact normal, unsanitized binaries:

- Fork: `/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/src/luajit`,
  SHA-256 `c6e24874cefde9b2033b0e138cc3564d2e27dfc72ce04597b45ab70ad98fc8cf`.
  This is `09cef065` plus the leaf publication guard, before allocator
  statistics publication, table-request coalescing, or JIT root-abort changes.
- Stock: `/tmp/lj-runtime-performance-review-2026-09-04/stock/src/luajit`,
  revision `b925b3e3fc6771171602323b45fbe9fb8fc90369`, SHA-256
  `d399449cc8cee4b0c600104a4a66fd44eeeac276c0f8571ce8204744041b5e34`.
- Original harness: `plan/aux/bench/bench.lua` from the frozen fork, SHA-256
  `ebd0b8d53b6e7a340c90c45ad33d9bdd47acbd5418890d593d6aae127ef926a9`.

Children ran on CPU 30, an Intel Core i9-14900K core without an SMT sibling,
using the performance governor. Frequency was not fixed. Other agents did
functional work on the shared machine, with a request to use CPUs 0–15.
This was affinity isolation, not system-wide isolation. Small differences near
parity should not be interpreted as improvements.

Every reported harness row is the minimum of five timed iterations inside
one process. Each iteration has an explicit full collection before its timed
loop, and automatic GC stays enabled inside the loop. Only the three filtered
pairs below are repeated fresh-process samples. The remaining controls are
single-process diagnostic pilots. All diagnosis processes completed within
their recorded limits; there are no substituted timeout values. Limits are
30–90 seconds according to the process record. The earlier full interpreter
pilot's 180-second timeout remains unchanged and is not replaced by this work.

[Metadata](../bench/closure-upvalue-diagnosis-2026-09-04/metadata.json) includes
binary, archive, source, and harness hashes. Post-run executable and harness
hashes match their initial values. [Raw evidence and reproduction instructions](../bench/closure-upvalue-diagnosis-2026-09-04/README.md)
include argv, environment, timings, exit status, stdout, stderr, and compact
profile reports. Large perf data and symbol binaries remain in `/tmp`.

## Filter and sequence effects

The unchanged filtered harness at scale 0.1 creates 500,000 closure/upvalue
pairs per timed iteration. Three alternating fresh-process stock/fork pairs
measured fork 1,954.39, 1,956.49, and 1,952.97 ns/op; stock 69.73, 70.02,
and 70.78 ns/op. Median fork/stock is **27.91**. At the full five-million
iteration scale, one fresh pair measured 1,972.49 versus 69.90 ns/op.
Iteration count therefore does not explain the full-sequence result.

`sequence.lua` changes only selection of multiple workload names, a separate
closure scale, and stopping after a chosen row. Other workloads run at scale 1;
closures run at scale 0.1. Thus the insertion prefix still builds 200,000-key
tables in each of its five iterations.

| Work before the closure row | Fork ns/op | Stock ns/op |
| --- | ---: | ---: |
| None, filtered | 1,957.37 | 70.43 |
| String interning only | 1,944.70 | 73.88 |
| New-key insertion only | 4,311.25 | 74.30 |
| All ten preceding harness workloads | 4,346.32 | 76.10 |

New-key insertion alone reproduces the extra approximately 2.2-fold fork cost.
Explicit full collection before each closure iteration does not remove it.

The harness initializes and retains 8,192 string keys, an array holding those
keys, and a populated hash table even when only closures are selected. A
separate GC-enabled control changes only that initialization count:

| Retained initialization keys | Fork ns/op | Stock ns/op |
| --- | ---: | ---: |
| 0 | 504.24 | 58.30 |
| 8,192 | 1,952.43 | 69.59 |

Most of the filtered fork result therefore depends on repeatedly processing
that live graph. A real approximately 8.6-fold closure gap remains in the
zero-key pilot. These controls do not establish its complete attribution.

## Where the CPU cycles go

User-cycle perf samples use 199 Hz and DWARF stacks. The unchanged filtered
process yielded 994 samples; a five-second attachment started only after the
insertion prefix had completed yielded 999. Neither reported lost samples.
The latter includes a small amount of explicit collection between closure
rounds (0.76% inclusive), but no insertion execution. Symbols come from
relinking the frozen objects without stripping; the resulting build ID matches
the measured executable. The measured executable was never replaced.

| Symbol or stack, percentage of sampled cycles | Filtered | After insertion |
| --- | ---: | ---: |
| Automatic GC, inclusive `gc2_step_auto` | 92.72% | 94.51% |
| Table traversal, inclusive `gc2_traverse_tab_rec` | 67.35% | 31.67% |
| Owner sweep, inclusive `lj_gc2_sweep_owner_progress` | 17.96% | 40.65% |
| `lj_arena_alloc_quarantine_finish`, self | 5.25% | 28.41% |
| `lj_arena_alloc_prepare_sweep_kind`, self | 1.92% | 12.35% |
| Pair allocator `func_newL_gc1tv_bump`, inclusive | 4.24% | 1.91% |

Inclusive rows overlap and must not be added. Both profiles put almost all
expensive execution under interpreter `lj_BC_FNEW` / `lj_func_newL_gc`
automatic checkpoints, despite JIT being enabled. This identifies where the
expensive work executes; it does not prove that no part of the loop runs as
machine code. No stopped-GC result is used here.

Filtered table traversal spends much of its time in retained admission and
marking: huge-table reader entry/search/lease release, small candidate
admission, and marking an admitted cell. The prefix profile's table percentage
is roughly halved while elapsed closure time roughly doubles, consistent with
similar absolute live-graph work plus substantial allocator work. Retained
string bucket capacity also grows, but it is not a major named profile hotspot;
the evidence does not assign every extra cycle solely to arenas.

## Object reachability versus allocator capacity

`gcdiag.c` is a separate diagnostic frontend linked to the unchanged fork
archive. It records the existing snapshot counters into a C stack object and
walks allocator lists only on their main owner, with assertions requiring zero
workers and no active/entering secondary TG. This is not the primary timing
binary. Counters are sampled immediately outside each closure timed loop.

The first 500,000-iteration round shows:

| Counter delta | Filtered | After insertion |
| --- | ---: | ---: |
| Allocated bytes | 47,980,444 | 47,972,476 |
| Cycles started | 122 | 123 |
| Major root scans | 366 | 369 |
| Worker runs | 11,968 | 102,056 |
| Grey objects drained | 31,380 | 31,354 |
| SSB entries converted | 1,792 | 1,708 |
| Completed arena sweeps | 9,810 | 90,048 |
| Reported live cells in those completions | 3,318,888 | 3,352,847 |
| Pending ownership roots flushed | 1 | 1 |

Both runs use trigger 393,216 bytes and hard threshold 786,432 bytes. Before
the first closure round, the filtered case has 8 traversable reclaimed arenas;
the insertion prefix has 735. Interned strings are 8,481 versus 8,482, and root
spine objects are 263 versus 264. String bucket masks are 16,383 versus 262,143;
the huge registry capacity is 65,536 in both. The prefix initially retains a
stale larger live estimate; later samples settle near 436 KiB while the cap and
extra work persist. Cycle frequency and logical live-cell work are nearly the
same, while completed arena sweeps increase over ninefold.

A separate weak-value watcher stores each of the five insertion result tables,
then releases the ordinary local result. All five weak values are absent before
every closure round in both fork and stock. The fork probe's extra helper
strings/objects raise its counts slightly, to about 8,549 strings and 267–268
root-spine entries, not to 200,000 keys.

That probe also counts reclaimed arenas whose entire `block[]` bitmap is zero.
Before its first 100,000-iteration closure round, 553 of 734 reclaimed arenas
are block-zero; after that round, 723 of 732 are block-zero. Later pre-round
samples show 724 of 734. The first watch-report print starts a new MARK cycle,
so its following snapshot has phase MARK; later pre-round snapshots are IDLE.
The weak-value observation itself follows the explicit collection. Block-zero
counts are observational diagnostics, not an authority for skipping production
reclamation predicates.

## Source implications and the next bounded change

In the frozen source, `lj_arena_alloc_prepare_sweep_kind` at `lj_arena.c:6983`
detaches both owned and reclaimed lists. Every reclaimed arena, including an
empty one, becomes PREPSWEEP and then NEEDSWEEP. `arena_sweep_state_prepare`
walks the complete sweep plane. The owner performs sweep and grace, and
`lj_arena_alloc_quarantine_finish` at `lj_arena.c:7325` checks/applies complete
bitmap planes, resets sweep state, and pushes the arena back to reclaimed.
Nothing in that cycle removes empty spares from the next preparation pass.
The profile and counters identify this repeated work as the first allocator
target for the history penalty.

The follow-up design should preserve empty arenas for allocator reuse while
excluding an exactly certified empty CLOSED/reclaimed generation from repeated
full-plane GC work. Quarantine's post-commit terminal-word proof is the natural
place to publish such evidence. Adoption already treats `live_cells == 0` only
as a hint and rechecks all authoritative root, recovery, destructor, and
lifetime planes before publishing a reusable run (`lj_arena.c:6719`). A new
skip must retain that standard: the count, a block-zero observation, or a
cached flag without generation and invalidation proof is insufficient.

The design audit must cover adoption and failed OPEN rollback, late/remote
intent, recovery and destructor ownership, held readers, arena transfer, and
terminal teardown. Required controls include a retained live block, every
certificate invalidation class, grace/reader deferral, and eventual successful
allocation from the retained spare. This note does not claim that the skip is
implemented or proved safe.

Separately, `lj_gc2_update_pacing` at `lj_gc2.c:3310` caps the allocation trigger
at 384 KiB to bound fresh pending-root discovery. The current specialized
closure/upvalue pair publishes typed destructor ownership with root state NONE
(`lj_func.c:728` and its publication helpers). The measured loop allocates
about 96 bytes per iteration but adds only one pending root in 500,000
iterations, while the allocation cap still causes 122–123 cycles. This suggests
auditing a separately bounded pending-root work budget instead of charging all
rootless allocation to that bridge constraint. It does not justify raising the
threshold without proof: the [July closure review](gc2-closure-churn-composed-2026-07-12.md)
records a larger-cap experiment that worsened quarantine behavior on its then
different ownership model. Automatic progress, retained memory, and bounded
root work must be validated together.

The safe priority is to remove repeat work with unchanged lifetime and edge
authority, then measure the remaining closure cost. These samples support an
allocator capacity investigation and a pacing audit; they do not support
weakening publication/admission or declaring the lockless/performance goal met.
