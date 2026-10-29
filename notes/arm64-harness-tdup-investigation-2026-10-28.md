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

## Symbolization qualification

The original crash report names image UUID
`7DAA8AA3-C387-39CF-A110-C7B352529FA3`, with image base `0x100590000` and
fault PC `0x1005e4354` (offset 344,916). The faulting instruction bytes and
offset match the source-array acquire load in `lj_tab_dup` (`ldapr x15,[x13]`),
and the reported source cursor is the fault address `0x100a30000`. However,
the retained unstripped executable was relinked and has a different UUID.
Its deeper symbolic stack labels must not be treated as authoritative frames
from the crashed image. In particular, they do not establish a path through
the symbolized GC drain/`ldexp` frames. The source-array-load attribution is
supported by the matching instruction bytes; the cause remains unknown.

A proposed stale array-generation explanation is conditional: table-array
snapshot acquisition does not pin a generation, but these prototype template
tables are intended to be immutable and rooted. No observation establishes
concurrent mutation or retirement of the failing template. No runtime change
was made from that hypothesis. A ten-million-iteration stress attempt was
killed before completion and is not a passing result.

## Follow-up: attach/recorder overlap audit (2026-10-29)

The proposed recorder-private-to-attached-thread overlap is not a legal
concurrent TDUP schedule in the audited source. `rec_idx_bump()` checks
`lj_record_mt_runtime_shared()` before touching a `BC_TDUP` template
(`src/lj_record.c:1913-1918`). A new external attachment publishes
`mt_entering`, but `threading_gc_enter_counted()` calls
`threading_mt_active_prepare_traces()` before publishing `mt_live` or allowing
the attached state to execute (`src/lib_threading.c:828-855`). That helper
acquires the global JIT token and flushes traces before completing. A recorder
which passed the earlier predicate still owns that token until its recording
stack returns; an attach begun afterward cannot reach Lua bytecode and execute
TDUP during that recorder's template mutation. Already-live/entering peers
make the recorder predicate refuse the mutation. Therefore the earlier
hypothesis that a legal thread attach races `lj_tab_dup()` against this private
template resize is withdrawn.

GC2 table traversal also does not establish such a race: it retains its body
and SMR scopes and uses `lj_tab_array_snapshot_gc_held()` /
`lj_tab_node_snapshot_gc_held()` for generation access. The raw snapshot in
`lj_tab_dup()` is not itself proof of a bug when the source is the current
immutable template generation and the runtime has one mutator. The mismatch
between first and second snapshots followed by an `lj_assertL` remains a
conditional bounds concern only if a concurrent template resize can occur;
no permitted schedule demonstrating that overlap was found.

No deterministic reproducer or runtime fix was made. The October crash remains
unresolved, and the preserved crash report still does not identify the source
template or generation. A clean baseline was built from commit `c5d8b87ab` with
macOS arm64 assertion, bootstrap/experimental JIT, `LJ_TRACE_TEST_HELPERS`, and
`LJ_TAB_TEST_HELPERS`; its runtime, archive, and source-file SHA-256 manifest
are preserved in `/Users/frityet/Scratch/ljmt-tdup-race-20261029/baseline/`.
