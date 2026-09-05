# Scheduler abort diagnosis — frozen 2026-09-05

The worker+fair default runtime has a concrete scheduler-fixture race: the final synthetic READY publication reopens the JIT entry gate while a real worker already owns SWEEP close. The worker then reaches the mandatory IDLE handshake with the gate open and aborts. The newly scheduled boundary also actually performs the RESET_ALLOC handshake that the fixture implicitly assumed would not occur.

This package contains diagnosis only. No runtime or fixture repair, assertion weakening, timeout increase, rotation implementation, shared edit, or runtime rebuild occurred. The quarantined-arena rotation source generation remains separately frozen and functionally unvalidated.

## Exact identities and scope

- Worker+fair input: `/tmp/lj-worker-bridge-combined-20260905-bz9wysjp/candidate`; all 225 recorded source inputs match both that package and this package's `source/combined/` copy.
- Matched default baseline: `/tmp/lj-reclaim-fair-combined-20260905-yws2eaap/candidate` (eb8 runtime / accepted between-TG fair source, without worker-boundary scheduling). All 225 inputs match `source/baseline/`.
- Both original runtime archives were built with their default flags, without assertion/helper macros. The fixture uses the canonical `-DLJ_GC2_TEST_HELPERS` compile macro alone.
- Combined archive: `0ff71ee36552489234ad9d48455b425bc26f2aeb3c46cc95ef1ae8f88ba78e75`.
- Baseline archive: `e7e3beb9ff9f85ec932837f4162382f6c7a6701325b41b83f9642a761fd5a157`.
- Untouched final scheduler fixture: `e9d2173e1279088d500b7bff816dae54e260b039ad5c5d6034f738d5ad2a1a27`. Publication retry and synthetic shutdown cleanup are already present.
- ROOT's original canonical failing ELF: `9c76bd3021401467822751d2505ece821a5ca2687b5894535b83c708e4e4fd91`, copied to `prior/original-canonical-elf`. Its original shared archive was `8ccf78b4109544b92898d47453722dc3ceba1088bcc4a25f5c89aac96d645fdb`, not the frozen combined default archive above. Source identity matches; archive/ELF identity is explicitly different.

The original canonical abort's internal state was not captured. These new matched-source/default-configuration failures establish the concrete failure mechanism; they must not be described as recovered observations from that original failure. ROOT's previously passing strict/ASan runs are separate configurations, not negative evidence against these default failures.

`setup.json`, copied prior records, and `final-input-validation.json` preserve provenance. Exact commands, working directories, LUA_PATH, 60-second runtime bounds, exit codes, timings, and hashes are in each generation's JSON. Default canonical C flags are unchanged; later diagnostic-only links add `-g` and explicit observer objects/wrap flags. ELF/archive artifacts are hash-only in the manifest.

## Preserved generations

| Generation | Combined | Baseline | What it establishes |
| --- | --- | --- | --- |
| `results/` | untouched pass; abort-only backtrace wrapper abort | untouched pass; wrapper pass | Actual abort stack: `gc2_idle_transition_handshake` -> `lj_gc2_sweep_to_idle` -> `gc2_worker_main`. |
| `gdb1/` | debugger SIGTRAP termination after temporary main breakpoint | unrun | Diagnostic failure only; no fixture defect state and no successful fixture result. |
| `gdb2/` | first run abort, all-stop snapshot | 8 passes | Exact failed gate state and main fixture location after actual abort decision. |
| `gdb3/` | one pass, then abort | 8 passes | Main's second external READY call races an existing close token and opens its JIT gate. |
| `gdb4/` | first run abort | one pass | Actual worker boundary RESET_ALLOC repairs missing main/worker prepare epochs during the synthetic phase; the same READY/close race follows. |

There are 25 completed new fixture runtimes: 4 combined aborts, 2 combined passes, and 19 baseline passes. This count excludes the debugger-induced startup SIGTRAP failure and ROOT's preserved original canonical failure. The diagnosis rests on captured state and source authority, not on pass ratios. No new broad or sanitizer validation was attempted.

## Concrete observations

`gdb2/combined-0-gdb.stdout`, `gdb3/combined-1-gdb.stdout`, and `gdb4/combined-0-gdb.stdout` all stop only after the runtime has called abort. The abort-only observer records the ABI's incoming RDI, enables attach for that failed process, and raises SIGSTOP. ROOT's earlier GDB signal outputs are preserved under `prior/` and were not overwritten.

The all-stop observations show phase=IDLE, cycle=4, cycle_leader=GCSCAN, jit_phase_gate=1, worker_active=0, and a NULL `idle_transition_gate_g` in the aborting worker's TLS. Its stack is the explicit guard at `lj_gc2.c:4343` through `lj_gc2_sweep_to_idle`. Main is sleeping at unchanged `test_async_sweep_and_stop:1442`, after the second direct READY call. The TLS offset is established by the exact default disassembly (FS-120), not an assumed layout.

In `gdb3` and `gdb4`, the second external READY call's before/after observations are:

| Field | Before | After |
| --- | --- | --- |
| phase / cycle | SWEEP / 4 | SWEEP / 4 |
| READY | 0 | 1 |
| root_scanned | 1 | 1 |
| jit_phase_gate | 0 | 1 |
| cycle_leader | GCSCAN | GCSCAN |
| worker_active | 1 | 1 |

Its return address identifies unchanged fixture line 1438 (`test_async_sweep_and_stop+1198` in these links). Main did not own the worker token. The observations are ordered loads around the real helper, not a new atomic protocol snapshot. They show the main-thread helper opening the gate within a close owner's authority interval, and the subsequent abort sees that gate still open.

`gdb4` also captures a real `gc2_sweep_prepare_bridge_result` RESET_ALLOC (actions=64) in synthetic SWEEP cycle 4, READY=1, root_scanned=1, worker_active=1, cycle_leader=0. Main prepare changes 1 -> 4; worker prepare changes {0,0} -> {4,4}. The corresponding baseline run records only its earlier real cycle-1 preparation and no synthetic cycle-4 RESET_ALLOC. This directly confirms the source lead; the captured reset is not inferred from final epochs the fixture later overwrites.

## Source explanation and required correction boundary

See `SOURCE-CAUSE.md`. The actual abort guard must remain. This fixture cannot treat a successful arena counter observation as a lease preventing the worker from closing the phase. It cannot revoke irreversible READY while a worker may hold close authority, or treat `sweep_to_idle()==0` with a busy token as proof of READY refusal. The smallest correction should separate uncontended manual READY-veto setup from asynchronous physical progress, publish READY under the caller's actual token/phase authority, and retain all physical-arena and worker-stop checks. A repair belongs in a separate package and requires fresh validation; none is included here.
