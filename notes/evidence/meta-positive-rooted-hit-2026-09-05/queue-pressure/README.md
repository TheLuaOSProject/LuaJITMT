# Positive rooted-hit publication under real queue pressure

2026-09-05. Independent functional probe; no shared repository edits.

**Result: PASS** against the immutable assert archive supplied by root:
 /tmp/lj-rooted-positive-hit-20260905-34e9qs5t/assert/src/libluajit.a

Final run exited 0 in 0.004728 seconds on CPUs 10–11, with a 15-second external
timeout and a 5-second observer deadline. This is functional evidence, not a
performance sample. Root was running an unrelated benchmark on CPU 30.

The final fixture is probe.c and executable is probe. Exact compilation,
archive digest, run command and output are in build.json, run-final.json and
run-final.log. The manifest pins artifacts and relevant frozen runtime sources.
The library was not rebuilt, edited or replaced. No production queue capacity,
phase, allocator cursor, worker token or lifecycle field was forged.

## Schedule and evidence

1. Public Lua APIs allocate the source table, string key, C-function result and
   separate rooted C-function filler. Ordinary distinct-string and C-function
   padding allocations put source, key and result in three separate small
   arenas. The result is removed from its setup stack cell after the public
   table store, leaving the table edge as its semantic source.
2. Automatic GC stops only for deterministic setup. An explicit full collection
   settles public-store handoffs. A real lj_gc2_mark_begin starts the next cycle;
   the fixture requires an unmarked result and an empty grey queue using its
   real 256-entry embedded capacity. LUA_GCRESTART re-enables normal automatic
   GC before the publication schedule.
3. Existing helpers enqueue 256 copies of the real graph-bearing filler in the
   grey queue, then fill and flush one actual 1,024-slot SSB and fill the other.
   There is no free SSB node. These are valid explicit traversal identities;
   functions avoid the table scan-stamp coalescing path.
4. The owning main thread calls lj_tab_gettv_rooted_hit_try with separate
   enumerated stack source, key and output cells. A non-VM observer thread
   waits at the existing LJ_GC2_RECOVERY_TEST_SSB_COMMITTED hook. This hook is
   inside writer-side SSB recycling, after the first filler is committed into
   the grey queue and its temporary marking scope released, before the source
   SSB slot/count is cleared.
5. At that pause, the fixture verifies:
   - Output already contains exactly the expected GC result. Original source
     and key words, stack base/top/address, owner word, anchor count and idle
     root descriptor are unchanged.
   - Distinct source/key/result arena reader counts are exactly **1/1/2**.
     The result retains the rooted-reader lease and publication marker lease.
     Source/result remain exact LIVE, READY starts with block bits set and no
     late intent; the key remains an allocated string start.
   - Global SMR readers are **0**, after ending vector provenance. Worker and
     SSB consumer tokens are each held. The published node still contains all
     1,024 source identities, and the active node is still full.
   - Grey capacity really grew **256 → 512**, moved off embedded storage and
     contains **257** identities. Logical allocated bytes increased by exactly
     **4,096**. The result's mark is already published.
   - No table waiting helper, wrapped blocking SMR entry or wrapped VM call,
     protected call, protected C call or resume entry runs during the watched
     operation. No result/filler C callback runs. Cycle and MARK phase remain
     unchanged.
6. The same call returns success after releasing the pause. All 1,024 published
   filler identities move to grey: final capacity **2,048**, count **1,280**.
   Net allocation increase is exactly **16,384** bytes after intermediate queue
   storage is freed. The result occupies the single entry in the rotated active
   SSB. Source/key/result reader counts return to zero, as do worker, consumer
   and SMR ownership; anchors and root descriptor remain unchanged. Recovery
   counts remain zero.
7. The fixture removes the source edge through the public API, confirms it is
   nil, performs full GC to IDLE, then calls the output function. It returns
   **12345**; its first callback occurs only at this deliberate final call.

The linker wrappers are fixture-only guards. Outside the watched operation they
call the original implementations. The successful operation has no catchable
exception or VM reentry. Source review independently establishes that grey
growth uses lj_mem_new_nothrow and cannot throw an OOM through the held
leases/worker/consumer interval.

## Source path

Line numbers below refer to the frozen source, not a later shared checkout.

- lj_tab.c:4862: retained source/key/result leases, root/owner confirmation;
  cleanup ends vector SMR before stack publication and then releases leases.
- lj_tab.c:5013: the positive-only wrapper uses that same implementation.
- lj_state.c:301, lj_gc.c:4400, lj_gc2.c:15486: stack publication, root barrier
  and phase-aware semantic marking.
- lj_gc2.c:17964: scoped semantic MARK publication retains its marker scope
  while publishing a newly marked graph-bearing result.
- lj_gc2.c:14344 and 14369: full SSB recycling claims the existing worker token
  and converts a bounded published node to obtain an active replacement.
- lj_gc2.c:14643 and 14824: a raw SSB function request is retained and committed
  to grey; its temporary scope is released before the commit hook.
- lj_gc2.c:10914 and lj_gc.c:4700: real nonthrowing queue storage growth and
  allocation accounting.

## Setup failures retained

- build-initial.log/json: the first fixture compile used the wrong atomic
  helper spelling and printf type. Fixed only fixture code to la_add32_rlx and
  the correct cast; final compile still uses -Werror.
- run-1.log/json and probe-setup-v1.c: the initial string key shared the
  source's traversable arena. The distinct-arena assertion failed before the
  queue-pressure operation. Ordinary distinct-string padding now selects the
  intended geometry; final reader assertions remain exact.
- run-2.log/json and probe-setup-v2.c: draining setup public-store handoffs
  after starting MARK marked the result early. The required-unmarked assertion
  failed before queue filling. A real full GC before the new MARK cycle settles
  these unrelated handoffs. No mark bits were cleared by hand.
- run-3.log/json and probe-v3-sbb-only.c/executable: first successful complete
  schedule before explicit VM reentry guards. Retained as earlier fixture
  evidence, not another runtime baseline.
- run-final.log/json: final guarded fixture passes. No runtime failure or
  timeout occurs after establishing valid setup.

## Limits

This is one bounded single-mutator schedule with real objects and capacities
filled through existing enqueue helpers. It does not establish how often
ordinary workloads fill these queues. The observer does not attach another TG,
run a collector worker or mutate the graph. The result is a small C function
and the table vector is small; root's separate Huge-vector and other rooted
fixtures cover other geometries.

Growth succeeds; no OOM or forced recovery failure is injected. No TSan/ASan
build was added because this task targets the supplied assert archive and root
already ran separate assert/ASan fixtures. This is not a forced nested
hard-assist checkpoint test; queue allocations total 28 KiB across the three
grows, with 16 KiB net capacity retained.

The wrappers guard named non-inlined entry points, not every instruction or
unrelated callback site. Source review supplies the no-throw/no-callback path
argument. The paused distinct 1/1/2 counts directly establish the requested
exact lease retention through this publication, after vector SMR has ended.

