# Worker-two SWEEP completion diagnosis, 2026-09-05

The preserved fixed-bound workload exposes a missing scheduler path. After the
workers drain the graph, they can leave SWEEP with its semantic root snapshot
complete but its ownership-list bridge unfinished. Both workers then park even
when all private/published remembered sets, grey/recovery queues, rescan tokens,
reader holds, native trace holds and finalizer holds are empty. The background
loop never schedules `lj_gc2_sweep_prepare_bridge_boundary`; its later
`lj_gc2_sweep_to_idle` correctly refuses an unfinished bridge.

No production runtime change is proposed as validated here. `PROPOSAL.md`
describes the smallest scheduling alternatives and the additional proof needed.
The original bound and all failures remain intact. This diagnosis is distinct
from the constructor-held quarantine deferral investigated by
`/root/gc_scheduler_review`.

## Exact inputs and direct controls

The production input is candidate3: base
`0e2119e4a36a7034f1f6e1115849677a4ce4359e` plus
`automatic-control-candidate3-full.patch`, SHA-256
`5f8a22b6bc1ae8956f63844cc0ad8bd63b093db98070078eeb391ee5e48ed9a1`.
`source-validation.json` checks all 815 archived source inputs, with exactly
the six candidate3 changes, against the normal, debug and `asan-candidate`
trees. All match. No shared source or build was modified.

The normal executable/archive are byte-identical to frozen candidate3:

- `runtime/src/luajit`: `a399c29f009c8deecc04de97cb0310205be2c9d1cc305e0a66c70eeb41757013`.
- `runtime/src/libluajit.a`: `abd9ede84ae1399adfd27c11a5079f69d5060a6fd45e7cc9f80cf01b7438eb97`.
- Unchanged fixture `t-string-retention.c`:
  `2e8e840fb4ba3a3b09168c06d828ff10ebafd41e9ff555b9737f34384fea3cf9`.
- Unchanged Lua workload `peer-control.lua`:
  `519ebf714b0a33b9a436d3452a153a1bc3eea3322ebf0bf74d8e76fea4ab8cb2`.

Commands, argv, environment, compiler output and executable identities are in
the corresponding `*-build.json`, `*-results.json` and `*-inputs.json` files:

```
python3 build.py runtime
python3 build.py debug
python3 run.py runtime 0-0-0 0-0-2 0-1-0 0-1-2
python3 run.py debug 0-0-0 0-0-2 0-1-0 0-1-2
python3 asan-candidate-build.py
python3 candidate-asan-run.py asan-candidate 0-0-0 0-0-2 0-1-0 0-1-2
```

These 12 uninstrumented candidate runs all return normally through cleanup.
The six worker-zero runs pass; the six worker-two runs return the original
`INCOMPLETE_AUTO`/exit2. There is no timeout, assertion or sanitizer report in
these runs. The four exact candidate ASan runs use Clang, assertions/API checks,
`detect_leaks=1:abort_on_error=1`; they are separate from the GCC debug runs.

Each automatic round requests three actual completed cycles, with at most 64
bursts of 4,096 escaping TNEW allocations into a 32-slot live ring. Each burst
has eight existing 2 ms native sleeps and completion-counter checks. The
worker-two failure consumes the unchanged 262,144-table bound. No explicit
collect/step replaces automatic progress. Existing explicit collection only
sets up the baseline and performs the fixture's original cleanup after the
failed gate. It does not satisfy that gate. Canonical anchored strings and the
original exact string/byte accounting assertions remain active.

An initial ASan build accidentally omitted the six-file patch when extracting
the base archive. Its tree and four outputs are preserved under `asan/` and
`asan-*`, explicitly classified by `asan-unpatched-base-classification.json`.
They are unpatched-base observations, not candidate validation: worker-zero
peer1 remains IDLE with a published request, and both peer1 cases fail. The
subsequent fresh `asan-candidate/` build is the corrected exact input; no old
result was overwritten or relabeled as a pass.

## Time-indexed evidence

The fixture-only observer adds reads and print/checkpoint calls, never a
collector, forced phase, work publication or gate. `debug-observed-v2-*`
repeats all four cases with the same completion outcomes. Its individual
atomic loads are explicitly not one coherent global snapshot. Remote private
lists and bodies are examined only by the separate all-thread-stopped GDB
decoder. GDB runs perturb scheduling and are diagnostic controls.

`gdb-v6-*` and `gdb-v7-*` capture the original last burst, several callback
checks, and actual entry to `lj_native_leave` after native sleeps. Every
snapshot records all stopped threads, backtraces, TG/native/reader state,
private SSB slots, detached/published SSB nodes, grey entries, arena lists and
headers, exact queued-object publication planes and table stamps.

1. During allocation, the dominant rescan identity is the ring table
   `0x7ffff7a5b9d0`, an array of size33, no hash entries, READY1/LIVE1 and no
   structural owner. It is repeatedly modified by the workload. Thousands of
   allocation-recovery identities also remain. This early graph work is real;
   it must not be mistaken for an already empty collector.
2. During the last sleeps, recovery and the ring rescan clear. Callback-time
   observations show a new seven-entry main suffix: global environment
   `0x7ffff7a52680` three times and the counter callback C function
   `0x7ffff7a588e0` four times. The latter's actual function address resolves to
   `automatic_done`; its environment is the former. This suffix is produced by
   ordinary return/call root publication, not a stuck construction token.
3. In both peer0/1 worker2 controls, native-return snapshots at ticks571 and575
   show every TG native and every private SSB empty, global SSB/grey/recovery
   empty, table/thread rescan pending0, worker_active0, hs_pending0, marks0,
   cycle_leader0, finalizer0, JIT gate0/no active trace, and zero reader depths.
   Both worker stacks are parked in `gc2_worker_main`. SWEEPcycle6 remains at
   five completed cycles, root_scanned1, root_done0, bridge_ready0. The root
   cursor remains `0x7ffff7c90190`, the global root-head slot. Debt is still
   tens of MB against hard786432. This establishes an unfinished ownership
   frontier after the graph drained; it is not lack of allocation debt.
4. The later `automatic_progress_missing` sample runs after the fixture checks
   anchored strings and pings the peer. It can therefore contain a fresh
   rescan of the anchored-string table. Those later objects are preserved in
   evidence, but are not claimed as the cause of the preceding missing cycles.

The v7 tick575 all-stop walk identifies the retained ownership work exactly:

| Control | Global ownership nodes | Main pending table nodes | Main old needsweep arenas | Main post-reset owned arenas |
|---|---:|---:|---:|---:|
| peer0, workers2 | 19,241 | 241,664 | 51 | 487 |
| peer1, workers2 | 4,462 | 257,725 | 18 | 519 |

Both pending chains end at NULL without a cycle or exhausted diagnostic bound.
Every pending table is block1/mark1/READY1/rootMEMBER/lifetimeLIVE; the ordinary
global objects are also READY/LIVE/MEMBER. No table LINKING/CONSTRUCT token was
found. The decoder deliberately skips THREAD body planes; its historical
`embedded_thread` label means “THREAD not decoded”, not that the peer thread is
embedded. The full chain hashes, first/last identities, per-arena counts and
publication-state histograms are in `gdb-v7-*/10-native_return-575.json`.
No TG has a quarantine arena. Old arena remote words retain CLOSED and, for a
few arenas, PENDING; their counted-reader bits are zero and SEALED is clear.
These are retained sweep states, not an active body writer.

The final explicit cleanup disables the workers and joins the peer, then
reclaims the churn strings and closes the real cycles. This supports retained
work and canonical identity; it does not repair the automatic failure. RSS is
not used as reclamation evidence.

## Diagnostic limits and preservation

The first observer compile failure is retained. `observed-v2-source/` preserves
the exact successful v2 source before adding a diagnostic invalid-cursor guard.
GDB v3 tried to inspect other threads inside `Breakpoint.stop()` before GDB had
completed all-stop; its errors and partial output remain. v4 moved inspection
after the stop and recorded the correct queue/table objects but used a wrong
sidecar member spelling, retained as per-object errors. v5 corrected that read;
v6 added native-return stops and correct C-function decoding; v7 added the
bounded ownership-chain census. v8 also follows the actual automatic Lua
function retained on the main stack to its closed upvalues: prototype
`peer-control.lua:46`, function `0x7ffff7a5cad0`, first upvalue
`0x7ffff7a5cb10` containing table `0x7ffff7a5b9d0`. This independently identifies
the earlier ring rescan. v8 repeats the empty-graph/unfinished-bridge condition
and uses the clearer `thread_not_decoded` census label. Every version's exact
scripts are preserved.

This is one real workload and bounded Linux x64 controls. It does not prove a
general absence of other SWEEP blockers, wait-free progress, or arbitrary
concurrent string reclamation. The separate constructor/rootbusy failure and
its defer repair remain independent inputs. No runtime patch is present in
this package beyond the already accepted exact candidate3 source.
