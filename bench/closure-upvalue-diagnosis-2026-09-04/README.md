# Closure/upvalue diagnosis evidence

See [the diagnosis note](../../notes/closure-upvalue-performance-diagnosis-2026-09-04.md).
All primary samples keep GC enabled. Measurements use frozen normal binaries
on CPU 30, with other work on the shared host; these are diagnostic pilots.

`metadata.json` pins the binaries, original harness, archive, and relevant
runtime sources, and records post-run hashes. `summary.json` is derived from
the raw stdout; it is not a separate measurement. Each `*-runs.json` records
exact argv/environment, timeout, terminal status, and raw output filenames.

- `run-filtered.py`, `filtered-runs.json`: three alternating fresh-process
  pairs with the original harness, scale 0.1, 45-second child limits.
- `filtered-scale1-runs.json`: one original-harness pair at scale 1,
  90-second limits. Each reported row is still best of five internal loops.
- `sequence.lua`, `sequence-harness.patch`, `sequence-runs.json`: independently
  select prefix workloads, fix closure scale at 0.1, stop after closures.
  Other workloads run at scale 1; child limit 60 seconds.
- `sequence-roots.lua`, `roots-runs.json`: initialize the harness's retained
  key tables with `DIAG_ROOT_KEYS=0` or `8192`; closure code is unchanged.
  These cases only select closures, with 30-second limits.
- `sequence-counters.lua`, `counter-runs.json`: native snapshot immediately
  outside each closure loop, using a separate main-owner diagnostic frontend.
- `sequence-reachability.lua`, `reachability-runs.json`: weakly retain each
  insertion result, check its disappearance after explicit collection, and
  optionally invoke the native counters. Closure scale 0.02 bounds the probe.
- `sequence-profile.lua`, `profile-prefix-run.json`: print a marker after
  insertion and before the first closure loop so perf can attach to closures
  only. The attachment is five seconds at 199 user-cycle samples per second.
- `profile-*-flat.txt`, `profile-*-inclusive.txt`: compact perf reports.
  Inclusive rows overlap. Both profiles have zero reported lost samples.
  Trailing spaces in the perf column-heading lines are removed for repository
  whitespace checks; reported symbols and measurements are unchanged.

The native frontend is diagnostic code, not a production remote list reader.
It asserts main-owner, zero-worker/no-secondary-TG execution. The first counter
pair was collected before adding empty-reclaimed and small-registry output to
`gcdiag.c`; the checked-in version includes that extension used for the
reachability probe. The prior frontend executable was overwritten. Both link
the same unchanged runtime archive; primary timing uses the original CLI.

Reproduce a sequence case by taking its argv and environment from
`sequence-runs.json`. For example, from the repository root, with the frozen
paths still present:

```sh
env -u BENCH_GC_MODE BENCH_SCALE=1 CLOSURE_SCALE=0.1 \
  DIAG_STOP_AFTER=closures_upval \
  LUA_PATH='/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/src/?.lua;/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/src/?/init.lua;;' \
  LUA_CPATH='/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/src/?.so;;' \
  timeout 60s taskset -c 30 \
  /tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/src/luajit \
  -jon -e 'io.stdout:setvbuf("line")' \
  bench/closure-upvalue-diagnosis-2026-09-04/sequence.lua \
  tab_insert_newkey,closures_upval
```

Replace the filter with `closures_upval`, `string_intern,closures_upval`, or
omit it to run the complete prefix. Use the measured stock runtime's own module
paths for stock. `run-filtered.py` reproduces the unchanged filtered pairs.
Scripts inherit the original harness's best-of-five convention and do not
replace the repository's semantic correctness fixtures.

The frontend reproduction compiler command is recorded in `metadata.json`.
Its list and block walks occur outside timing and add observable cache work;
use their counts to diagnose behavior, not their row times as CLI performance.
Raw perf data and unstripped symbols remain at the temporary location in
`local-evidence-path.txt`. The measured binaries were never replaced, and no
global perf build-ID cache or host configuration was changed.
