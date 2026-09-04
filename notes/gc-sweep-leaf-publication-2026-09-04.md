# Preserve SWEEP leaves without queueing graph traversal

Date: 2026-09-04. Linux x86_64; follows `09d09e63` and `09cef065`.

The final publication branch of `gc2_trace_sweep_edge` used allocator
classification to decide whether an object had children. Strings and cdata can
occupy storage carrying `LJ_AF_TRAVERSABLE` or `LJ_HUGEF_TRAVERSABLE`. A successful
barrier therefore marked these leaves and also placed them on an SSB. Repeated
parent scans or root publications could fill that buffer and overflow into the
allocation recovery lanes, although the leaf had no child graph to discover.
The recovery-backlog diagnostic after the public-table repair found live,
already marked string keys among those queued objects.

The branch now additionally requires `gc2_gct_may_traverse(gct)`. This matches
the ordinary semantic marker's classification. The guard comes after exact
admission, type/layout validation, mark/rescue, and direct-body preservation.
Successful STR and CDATA edges finish at that point. Transient admission or
late-free failures still take the earlier recovery or reclaim-veto path.
UDATA keeps its explicit exception to allocator traversal classification, and
tables, functions, prototypes, upvalues, threads, and traces remain graph work.

The semantic check is supported by `gc2_traverse_obj`: its STR/CDATA branch has
no child traversal. String payload is immutable bytes; cdata contains its C
type identity and C payload. FINREG generation tables and ordered raw records,
metatype roots, and callback roots have separate discovery paths. Skipping an
already admitted cdata leaf does not skip the FINREG graph or dispatch a
finalizer. The fixture includes a registered cdata and verifies one finalizer
call at close. A successfully preserved arena leaf returns zero from
`lj_gc2_trace_sweep_root` because it added no traversal work; that return is not
a liveness flag. The immortal embedded empty string keeps its existing special
return path.

`tests/t-gc2-sweep-leaf-publication.c` exercises real allocator storage:

- A small string, a huge string, fixed cdata, small variable cdata, huge
  variable cdata, overaligned cdata, and registered cdata. It validates actual
  physical marks and reader counts, and checks that their storage carries the
  traversal classification which previously caused unnecessary publication.
- Public object roots, TValue barriers, root-publication barriers, and semantic
  object leases. Each route repeats beyond the active SSB's capacity, covering
  first marks and already marked leaves while checking unchanged SSB/grey
  publication and zero recovery work.
- A cyclic table graph plus small and huge userdata roots. All leaf values and
  the table child graph must become marked, and queued graph work must drain.
- A real small-leaf MUTATING admission denial. It must still set the durable
  reclaim veto, leave the unadmitted leaf white, and release its admissions.

The original-path control differs only by removal of the new type guard. Its
first leaf case reports an actual mark followed by `queued=1 ssb_changed=1` and
fails the no-graph-work assertion. Its graph and transient cases both pass,
confirming those obligations already existed. No runtime hook or linker
wrapper is needed by this fixture. The canonical M3 registration appears in
both the scaffold dependency list and its executed cases.

Artifacts are in `/tmp/lj-sweep-leaf-validation-20260904-c6pzgeam`. The strict
tree starts from the stability-only `90db531b` archive used by the previous
regression and overlays the retained-admission GC source plus this leaf guard.
The source overlay, control, build commands, and results are recorded there.
It excludes the concurrent arena-statistics changes and the unmerged dirty
coalescing experiment.

GCC assertion/helper validation passes 20 processes of the new fixture, the
complete GC traversal and recovery fixtures, the public table/FINREG fixture, and the
retained-edge fixture. The latter retains its small/huge late-free, exact type,
cdata geometry, reader saturation, and retry checks. The canonical
`tools/test.lua m3_gc2_sweep_leaf_publication` run passes and restores the
default build. A Clang ASan assertion/helper build also passes the new fixture
with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`.

The normal Linux runtime also passes all 387 interpreter and 509 JIT stock
tests, both without and with the concurrent allocator-statistics correction.
The latter combined build passes traversal, recovery, both earlier SWEEP
regressions, allocator statistics, and threading detach/close lifecycle tests.
Its source hashes and results are recorded in
`/tmp/lj-gc-final-combined-20260904-7jk68bv5`. The larger arena-state churn test
still exceeds 60 seconds, with samples in automatic SWEEP table traversal.
That unresolved workload cost is not counted as a pass.

This repair preserves every public table-rescan request introduced by
`09d09e63`. It does not use the table scan stamp to coalesce publications and
does not change the remaining owner-dependent GC protocols. Performance
comparisons and remaining cliffs must be assessed separately from these
lifetime and graph-preservation checks.
