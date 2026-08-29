# GC2 mark-status and object-lifetime audit (2026-07-11)

Status: audit/backlog. This records the required contract and remaining hazards
found while diagnosing terminal-arena cyclic grey requeue. It is not a proof of
the arena terminal protocol, object validation, weak processing, or reclamation.

## Result contract

The WIP internal result has three meanings:

| Result | Current value | Meaning |
| --- | ---: | --- |
| `GC2_MARK_DEAD` | -1 | The exact allocation could not be retained. The caller must not read its header, direct bodies, or graph payload. |
| `GC2_MARK_LIVE_ALREADY` | 0 | A generation-valid mark/rescue already retains the exact allocation. It is safe to inspect only if that retention is a lifetime certificate through the applicable close/grace boundary. This does not prove that mutable payload was scanned. |
| `GC2_MARK_NEW` | 1 | This call installed the first mark or rescued the allocation. The caller owns the first semantic traversal obligation when the type has graph edges. |

Code must never use an internal status as a C boolean: `DEAD` is true and
`LIVE_ALREADY` is false. Test `status == GC2_MARK_NEW` for discovery and
`status >= GC2_MARK_LIVE_ALREADY` for retained lifetime.

The exported/internal-header `lj_gc2_markmem()`, `lj_gc2_markobj()`,
`lj_gc2_markobj_direct()`, and `lj_gc2_markobj_nogrey()` interfaces must retain
their existing new-mark boolean behaviour: first mark/rescue returns 1; an
already marked live object and a dead object both return 0. Existing tests rely
on that behaviour. New status helpers should sit underneath those wrappers.

`marks_this_round` is discovery/progress accounting, not liveness accounting.
Increment it exactly once for a branch that genuinely returns `NEW`, including
RETIRED rescue and HugeTab returns 1 or 2. Never increment it merely for
`LIVE_ALREADY`. In particular, a read-only post-commit classification is
`LIVE_ALREADY`, not a new mark. Weak key/value "marked" counters likewise count
`NEW`, not all live observations.

Whole-arena saturation and terminal commit do not by themselves settle graph
discovery. They need a generation-valid per-cell deduplication mark: returning
`NEW` on every live probe loops on cycles, while always returning
`LIVE_ALREADY` can leave a newly reached object's children untraced.

## Immediate caller rules

* Ordinary mutator `markobj`/`markobj_direct` and the worker mark path enqueue an
  SSB/grey item, or directly traverse userdata, only for `NEW`. Direct table and
  function representation bodies may be preserved for either live result.
  `DEAD` must return before any such read or preservation.
* Thread-root marking treats `NEW` as already queued, explicitly rescans a
  `LIVE_ALREADY` mutable root, and stops on `DEAD`. Replace the current
  mark-false followed by a second `lj_gc2_ismarked()` query; that is a separate
  race and loses the lifetime result.
* Payload and table-child helpers treat `NEW` as already queued and use their
  NEEDSCAN/table-stamp rescan for `LIVE_ALREADY`. They stop on `DEAD`.
* `gc2_ssb_mark_one()` is different: an SSB slot is already explicit rescan
  work, so it may grey-push for either live result. It must discard `DEAD`.
* `lj_gc2_preserve_sweep_root()` preserves the header and direct representation
  bodies for either live result but does not semantically traverse. Its legacy
  return remains `NEW` only.
* During SWEEP, `lj_gc2_trace_sweep_root()` publishes SSB work (or traverses
  userdata) for either live result because the call represents a fresh semantic
  root snapshot. It must stop on `DEAD`; an `ismarked()` fallback is not an
  admissible substitute. Outside SWEEP it retains ordinary `markobj` behaviour.
* `markobj_nogrey` preserves direct bodies for either live result, never queues
  graph work, and retains its `NEW`-only legacy return.

These rules explain the observed grey explosion: terminal branches returned
mere liveness to `gc2_markobj_worker()`, which interpreted every visit to a
cyclic object as a first mark and requeued it indefinitely.

## Pre-admission object hazards

The raw allocation tri-state is necessary but is not yet a complete object
lifetime API. `gc2_markobj_base_valid()` currently reads `o->gct`, cdata flags,
cdata offset/type metadata, and some huge-object fields before the allocation is
retained. A FREEING owner can win between the bitmap/registry probe and those
reads. All `markobj` variants, the worker, sweep preserve/trace roots, and
`lj_gc2_ismarked()` inherit this problem.

Replace validation-then-mark with one object status/view operation:

1. locate and admit the allocation without reading object bytes;
2. reject block0/dead generation, `late[]`, RETIRED that cannot be rescued, and
   FREEING;
3. install or observe a generation-valid mark/rescue;
4. only for a live result, read and validate the type/header while admission or
   the durable mark still supplies the lifetime certificate;
5. expose the validated base/type to direct-body and traversal callers.

For ordinary aligned objects, the candidate object address is the allocation
start and can be marked before reading `gct`. Cdata needs separate treatment:

* Variable/over-aligned small cdata may place `GCcdata` in an extent cell whose
  block bit is zero. `lj_cdata_validate()` cannot safely recover its base before
  a pin because it first reads the interior header. Use allocation-owned cdata
  header-to-base metadata, or an admitted bounded allocation-start lookup, then
  validate the stored offset after retention.
* Huge cdata may also have an interior header. `hugetab_range_lookup()` followed
  by an exact `hugetab_mark()` races unmap. Add an atomic range-mark operation
  which finds the containing entry, rejects FREEING, CASes MARK/rescue on that
  same entry, and returns its exact base before header access.
* HugeTab mark results map as `-1 -> DEAD`, `0 -> LIVE_ALREADY`, and both `1`
  (first mark) and `2` (RETIRED rescue) to `NEW`.

A live result must remain a real retention certificate after the short rescue
admission is released. A post-commit read may report `LIVE_ALREADY` without
writing `mark[]` only when the exact terminal LP proves that the stable block
survives; a fresh semantic sweep root must still publish traversal work. Any
case without that terminal proof must publish a counted next-generation rescue
token or report `DEAD`; an unvalidated block bit alone is insufficient.

## `ismarked`, weak processing, and raw dereferences

`lj_gc2_ismarked()` is an observational liveness query, not permission to
dereference. Its small-arena decoder must share the same terminal rules as the
mark operation. In particular, a block1/mark1 cell with `late[]` set is dead for
the old allocation even when ordinary sweep flags have already been cleared.
Marked (`> 0`) can serve as a lifetime certificate only after the mark protocol
proves reclamation honours it; zero or negative never authorizes a body read.

Weak clearing must not create a mark merely to query reachability. It needs a
late-aware expected-TValue-type marked query, or a retained object view which
reports unmarked versus marked without mutation. In `gc2_weak_mayclear()`, a
post-validation query result of -1 must be handled like unmarked (`<= 0`), not
retained by an `== 0` test and then followed by a FINALIZED flag read. Weak
string handling remains strong-marking, and weak key/value telemetry increments
only when that strong/barrier mark is `NEW`.

Specific remaining read sites include:

* `gc2_preserve_direct_bodies()` is called unconditionally by current object
  mark variants. A dead table/function therefore leads to raw pointer reads.
* `gc2_finreg_markobj()` calls a void root marker and then reads `o->gct` and can
  traverse a table regardless of the mark outcome. It must receive and gate on
  the status. FINREG side-list ownership must be documented as an independent
  lease wherever it is relied upon.
* `gc2_tab_weak_mode()` reads a metatable and mode string before retaining them.
  During SWEEP, retain the metatable and mode string first and parse only a live
  result.
* `gc2_traverse_tab_rec()` marks array/node bodies but ignores a dead result and
  then scans them. A dead body must make the snapshot abort/retry unless a table
  generation pin independently proves its lifetime.
* `gc2_ssb_mark_one()`, payload/table-child helpers, weak snapshot candidate
  decoding, and rescan-pending clear helpers all read type/header state through
  `gc2_markobj_base_valid()` before receiving a lifetime result.
* Raw `markmem`-then-dereference paths for trace bodies, retired-list records,
  table vectors, and root buffers must either check for a live status or state
  the independent SMR read section, JIT token, owner claim, or generation pin
  that keeps the body valid. A failed mark is not such a pin.

## Completion gates

This area is not complete until all of the following hold:

1. Every terminal admission mode returns the tri-state contract and passes a
   two-node cyclic graph test with one `NEW` per generation and bounded grey
   growth.
2. All object/direct-body/traversal callers follow the matrix above; DEAD table
   and function tests prove no body read under ASan/UBSan.
3. Object type validation occurs after allocation admission/retention, including
   over-aligned small cdata and interior huge cdata.
4. `ismarked` is late-aware in OPEN, RECLAIMED, committed, and restored states;
   weak invalid/freeing races clear or stop without reading object flags.
5. Sweep-root `LIVE_ALREADY` still publishes semantic rescan work, while an
   ordinary worker only queues `NEW`; weak and mark-round counters remain exact.
6. Table body snapshot races either retain the selected generation or retry,
   and FINREG/weak-list users have an explicit lifetime lease.
7. Focused terminal/model tests, stock GC/FFI/JIT suites, assertion/paranoia
   builds, repeated stress, and sanitizers pass without grey-capacity growth,
   stale body reads, or conservative permanent retention.
