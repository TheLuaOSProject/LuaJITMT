# Harness engine state and intermittent table duplication failure

The combined experimental gate on production source `8dc0a6cfe` did not pass.
Its first combined M7 ARM64 assertion run stopped in
`suite_utils.luajit_fixture_libs` with `table overflow`. An instrumented rerun
instead crashed after the CALLXS lifecycle child, while the host copied a table
template in `lj_tab_dup`. Symbolization and matching instruction bytes place
the fault at the source-array acquire load; the apparent array size was
10,209,760 and the source cursor reached unmapped memory. No cause is established.

The runner was incorrectly described as wholly interpreted. In `lib_jit.c`,
`jit.off(true, true)` selects the calling function and its subfunctions, whereas
`jit.off()` selects the engine. Modules loaded afterward could still record
and execute traces. `tools/test.lua` now uses the latter, matching its existing
control-plane intent. Loading all suites with `--list` and asserting that
`jit.status()` is false passes. This harness correction is not a runtime crash
fix, and a clean aggregate result will not by itself resolve the original
failure.

A third run with temporary table-template diagnostics passed the exact M7
selector. A separately built `49523abd` checkout also passed that selector.
The table, VM, suite, and fixture-library helper sources are unchanged between
`49523abd` and `8dc0a6cfe`; JIT admission and recording changes remain relevant
because the host engine was enabled. Small literal append probes with repeated
full collections passed old and new binaries, so they do not yet reproduce
the failing workload. Temporary table diagnostics have been removed from the
working source; their patch and logs are retained outside the repository.

Evidence: `/Users/frityet/Scratch/ljmt-math-dynamic-20261028/` contains
`experimental.log`, `m7-diagnostic.log`, `m7-tdup-diagnostic.log`, the diagnostic
patch, symbolized executable/map, and `harness-engine-off.log`. The crash report
is `luajit-2026-10-28-205421.ips` in the local DiagnosticReports directory.
The old-checkpoint comparison is under
`/Users/frityet/Scratch/ljmt-m7-oldmain-20261028/`.

Next evidence must distinguish actual host trace execution, recorder effects,
and GC/template lifetime. The port checkpoint is not promoted to `v2.1` on
these results, and full x86_64 parity remains incomplete.

## Follow-up reproduction probes

An exact `8dc0a6cfe` experimental/assert host was copied outside its build tree
and checked with `jit.status() == true`. This matters because the M7 selector
restores `src/luajit` to an interpreter bootstrap build at the end: reusing that
path for the next run silently changes the host profile. A subsequent M7 run
from the immutable host, with `MallocScribble=1` and a private temporary
directory, passed. PID-filtered events showed starts and aborts, no trace-stop
publication, and no event sourced from `suite_utils.lua`. Other instrumented
old/new host runs also passed. They do not establish the execution history of
either original failure.

A closer control-plane probe uses the real `Test:path`, `Test:cc`, `Test:run`
and fixture-library helper, substitutes successful command execution, allocates
large strings, and collects fully every 137 iterations. Its 30,000 iterations
passed all library-value and 90,000-command checks. The observed variant had
4,758 trace starts, zero stops and 24 abort events; an unobserved variant with
`hotloop=1` also passed. These are recorder-pressure probes, not native-code
execution evidence. A separate one-million-iteration fresh-table helper probe
also passed. No reproducer or runtime fix has been established.

The immutable host and preserved allocator-stress trace are under
`/Users/frityet/Scratch/ljmt-m7-hosttrace-20261028/`; the control-plane probe and
its `-trace.log`/`-hot.log` results are in the original evidence directory.
