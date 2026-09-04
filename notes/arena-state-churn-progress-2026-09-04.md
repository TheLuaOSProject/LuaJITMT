# Arena state-churn progress review

The unchanged `tests/t-arena-state.c` completes successfully on the normal
Linux runtime at `abf234ca`. A fresh run took **63.341 wall seconds** on logical
CPU 12, under a 150-second process limit, after an earlier run exceeded a
60-second limit. Its final assertion message and zero exit status cover all
24 universes, 20 churn/collection rounds per universe, huge strings, FFI arrays,
and terminal `lua_close()` calls. Stderr is empty.

This resolves the observed timeout as excessive workload cost for the tested
schedule. It does not establish acceptable latency or rule out other stalled
schedules. The fixture and its workload were not shortened, and no runtime
protocol was changed to make this run complete.

[Run result](../bench/arena-state-progress-2026-09-04/unmodified-result.json),
[stdout](../bench/arena-state-progress-2026-09-04/unmodified.stdout), and
[metadata](../bench/arena-state-progress-2026-09-04/metadata.json) retain the
exact command, process limit, compile command, original timeout, and fixture,
archive, and executable hashes. The frozen normal tree matches all 210 tracked
`src/` files at `abf234ca`; the
[source comparison](../bench/arena-state-progress-2026-09-04/source-comparison.json)
records no missing or changed files. This is the leaf-publication and scalar
statistics runtime, before the separate table-request coalescing and deferred
JIT root-abort changes. Functional work elsewhere on the host was not isolated,
so this one elapsed time is not a benchmark acceptance result.

Two earlier stopped-process stack samples, using the unchanged workload with
symbols, found automatic SWEEP table traversal beneath `string.rep`, including
per-edge lifetime admission and huge-registry lookup. They did not establish
whether Lua was advancing. That diagnostic process was deliberately terminated
after sampling; it was not a completed test.

A separate diagnostic adds a C progress callback every 200 insertions and at
round/collection boundaries. Before its 25-second limit it completes 190
rounds across nine complete universes and ten rounds of the next universe.
Every completed collection reports IDLE and zero recovery items; the final
record reaches the next round's 1,800th insertion with SWEEP work pending.
These observations corroborate progress, but callback instrumentation changes
JIT recording and GC scheduling. They cannot substitute for the unchanged
fixture's successful run or provide a valid speed comparison.

Raw diagnostic C source, CSV, commands, and process records remain in
`/tmp/lj-arena-state-progress-20260904-vs7x1eyd/`; stopped stacks remain in
`/tmp/lj-gc-final-combined-20260904-7jk68bv5/arena-state-stack-{0,1}.log`.
The measured repeated traversal cost remains work for the performance plan.
