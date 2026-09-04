# Preserve public SWEEP rescans without FINREG self-publication

Date: 2026-09-04. Linux x86_64. Stability repair against `90db531b`;
`ea23bf0c` has the same starting GC and CType sources.

A public SWEEP parent barrier could lose a new child graph. Once a table had a
current scan stamp, a raw payload store followed by `lj_gc2_trace_sweep_root`
published an ordinary SSB request without changing that stamp or creating a
rescan token. The SSB converter then treated the clean, current table as a
duplicate and consumed the request. The child and its descendants remained
unmarked. Full-buffer recovery concealed this defect in the earlier saturated
cyclic-table fixture because the recovery lane separately recorded REDIRTY.

`gc2_ssb_mark_one` now consumes every published table request during SWEEP as
semantic traversal work. The prepublication private-edge check still suppresses
already scanned tables found through graph discovery, so an unchanged table
cycle can finish. MARK and WEAK retain their existing converter suppression.
This does not introduce a scan cursor or change the table-store protocol.

That correction exposed a second problem: `lj_ctype_fin_istab` used semantic
table admission merely to inspect membership. Each FINREG table scan therefore
published another FINREG table scan. The first SSB correction passed the new
child-graph fixture and recovery, but full traversal timed out after 60 seconds.
A normal combined build also timed out in the interpreter stock suite at 240
seconds. GDB stopped the JIT stock run at this live publication chain:

```
gc2_publish_mutator_
gc2_markobj_expected_scoped_status_mode
lj_gc2_obj_lease_acquire
ctype_fin_gen_tab_acquire_next_smr
lj_ctype_fin_istab
gc2_tab_weak_mode
gc2_traverse_tab_rec
gc2_traverse_obj
gc2_worker_drain_inner
lj_gc2_collect_active
```

The JIT stock run was deliberately terminated after that diagnosis; its result
was SIGTERM, not a timeout. The failure artifacts remain in
`/tmp/lj-sweep-public-table-20260904-p7mjelkx` and
`/tmp/lj-stability-combined-20260904-hawy8qwo`.

Membership now acquires an observational TValue lease with an explicit TAB tag.
It keeps the raw generation admission and exact generation-to-table pointer
recheck, then reads the FINREG enable flag under the retained table lease.
Actual FINREG get/register/claim operations continue using semantic admission.
Raw generation records can still be marked by their existing raw-memory lease;
the predicate does not semantically mark or publish the observed table.

The predicate returns FOUND, MISS, or RETRY. Closed SMR, a transient raw
generation, a transient table, or a changed generation pointer cannot establish
ordinary weak-table policy. The three GC consumers consequently defer before
publishing a scan proof or ordinary weak record: weak-mode classification,
FINREG-specific traversal, and weak-record discovery. Their existing retry
paths retain a queue locator and counted rescan ownership. They resume after
membership can be established.

The regression fixture `t-gc2-sweep-public-table-rescan.c` covers:

- Five public barrier APIs, one or two requests, and acyclic or self-cyclic
  parents: 20 schedules with available SSB space, no recovery fallback, exact
  request consumption, marked child and grandchild, and empty final work state.
- Observational membership in MARK and SWEEP, with a white FINREG table staying
  white and unchanged semantic marks, SSB/grey publication, recovery, and lease
  counts. All raw generation marks are primed before mark-counter comparison.
- A clean, already scanned FINREG table whose membership query must leave no new
  SWEEP request, directly exposing the self-publication defect.
- Real closed-SMR, plain raw-arena writer, and table MUTATING admission denials,
  each returning RETRY and releasing its admissions; disabled membership returns
  MISS, and restored admission returns FOUND.
- A transient predicate result at each of the three production consumers. A
  bounded worker turn retains COUNTED ownership and a concrete retry locator,
  publishes no completed scan or weak record, and leaves the weak value white.
  Removing the transient condition completes the scan with weak-value policy.

The consumer schedule uses GNU linker wrapping for `lj_ctype_fin_istab` only;
the direct membership tests call the actual predicate and real admission
protocols. The canonical M3 case is therefore Linux-only and is included in
both the scaffold dependency list and its executed cases. No runtime test hook
was added. Windows and macOS verification remains deferred until release work.

Validation artifacts are in `/tmp/lj-finreg-observe-20260904-pbbfrkxy`. Its
`fixed` source is an exact `90db531b` archive plus the public SSB condition,
observational CType predicate, and three RETRY consumers. It excludes the
concurrent retained SWEEP lease optimization and JIT side-publication change.
The build uses GCC, GNU C11, `-mcx16`, `LJ_GC2_TEST_HELPERS`, and `LUA_USE_ASSERT`;
C fixtures also use `-O2 -Wall -Wextra -Werror`.

That stability-only build passes the complete GC traversal and recovery
fixtures, the new rescan/membership fixture, public weak-window and weak-resize
fixtures, FINREG free-path and retained-slot fixtures, cdata weak `__newindex`,
and alternating cdata/userdata close-finalizer convergence. The deliberate
child abort in the FINREG free-path test remains its expected assertion.
Exact `90db531b` archive controls independently fail the new lost-edge,
observational-table-mark, self-publication, and RETRY schedules. Reverting each
of the three GC RETRY consumers independently also fails the corresponding
bounded retry schedule against otherwise fixed source. The final fixture
passes 20 repeated processes and the canonical
`tools/test.lua m3_gc2_sweep_public_table_rescan` registration, including its
default-build restoration.

The first canonical run exposed a fixture assumption: priming only the head
generation leaves older raw records white, so an ordinary MISS can legitimately
change the global raw-mark counter. The final fixture primes every generation
under its existing raw lease before comparing that counter. All exact table
mark, queue, admission, and RETRY assertions remain. That initial failure is
preserved in `unprimed-canonical-run.log`.

The restored normal stability-only runtime also passes `t-weak-modes.lua` and
`t-ffi-gc-finreg.lua 6 240` in both JIT modes, and `t-ffi-gc-trace.lua` with JIT.
The FINREG test observes 1,080 worker finalizations per mode; the trace test
observes 800 direct and 800 metatype finalizations with two metatype traces.

The combined normal build additionally passes all 387 interpreter stock tests
and 509 JIT stock tests. Its source hashes, commands, and logs are recorded in
`/tmp/lj-stability-finreg-combined-20260904-j7xg82hs`. The final new fixture also
passes Clang ASan with `detect_leaks=1:abort_on_error=1`, linked against the
combined assertion/helper archive in
`/tmp/lj-gc-sweep-lease-20260904-2p5yleu8/asan`; its command and result are in
`asan-result.json` beside the stability-only artifacts.

A subsequent performance diagnostic remains open. Minimal normal 10,000-key
insertion takes tens of seconds during SWEEP while recovery work accumulates;
the final explicit collection does reach IDLE with zero recovery work. The
ordinary benchmark control timed out after 90 seconds. This is unacceptable
performance and requires follow-up; the evidence does not establish another
FINREG self-publication loop. Coalescing requests safely requires proving that
a completed scan covers the latest publication, including raw-write barriers.

These results establish the specific repairs and convergence regressions; they
do not prove the remaining GC/JIT/FFI plan complete or remove its owner-dependent
protocols.
