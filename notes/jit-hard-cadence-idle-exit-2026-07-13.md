# Trace hard-cadence service before GC2 exit

## Failure classification

`m6_jit_gc2_readiness` failed deterministically at the TNEW hard-check
fixture on checkpoint `6cc0c583`. Two independent contract changes had been
conflated:

- `t-gc2-jit-hard-check.c` entered MARK before compiling each allocation loop.
  Since the GC2-only transition, mark start performs `EXIT_TRACES` and the VM
  keeps JLOOP entry closed until IDLE. Those loops therefore ran interpreted;
  they could not prove anything about x64 trace allocation checks or trace-side
  assists.
- `lj_gc_step_jit()` returned on `lj_gc2_jit_needs_exit()` before servicing a
  hard-only check. Since that predicate includes `gc_hard_assist_due_jit()`, an
  already-entered stopped-IDLE trace neither incremented `jit_hard_checks` nor
  advanced `hard_check_bytes`. It repeatedly exited at the same debt instead of
  observing the intended bounded trace cadence.

The first item was a stale fixture expectation. The second was a runtime
pacing and telemetry defect, not a reason to remove the expectation that a
native allocation check services its hard cadence.

## Runtime contract

`lj_gc_step_jit()` now samples phase/threshold exit independently from the
hard-only predicate:

- MARK, WEAK, SWEEP, or a public threshold that was already due still prevents
  trace-owned trigger/state-machine progress and exits immediately after the
  bounded hard-check service, if any.
- A hard-only stopped-IDLE check skips cycle triggering, records the JIT hard
  check, attempts the ordinary bounded assist (which is passive in IDLE), and
  advances `hard_check_bytes`.
- The helper then resamples the combined exit predicate. It can continue the
  trace only when cadence advancement removed the sole exit reason; any active
  phase or threshold transition still forces the interpreter exit.

No wait, lock, extra allocation-path poll, or active-phase trace progress was
added. The extra work occurs only after x64 assembly has already entered the
out-of-line GC helper for a due hard check.

## Regression shape

The focused fixture now compiles and anchors each allocation loop in stopped
IDLE before making a normal (`hard_bytes >= LJ_GC2_ACCT_FLUSH`) cadence check
due. TNEW, CNEW, local-cell CNEW/FNEW, and a non-constant-foldable SNEW loop
must each:

- increment `jit_hard_checks`;
- advance `hard_check_bytes`;
- leave `assist_runs`, cycle requests, and cycle starts unchanged; and
- remain in IDLE.

The dynamic SNEW indices are intentional: the former constant
`string.sub("abcdef", 1, 3)` folded SNEW to a KGC during recording and did not
exercise runtime string allocation lowering. The Lua functions are returned
directly to the C fixture and kept on its stack, avoiding an unrelated global
table publication dependency.

## Validation

- Exact A/B with the revised fixture: the unmodified `6cc0c583` runtime fails
  deterministically at TNEW; the split runtime passes 100 consecutive fresh
  processes.
- `m6_jit_gc2_readiness`: pass.
- `m6_jit_gcstep_pacing`: pass, including the stock GC-step script and JIT/no-JIT
  closure loops.
- `m6_jit_fnew_bump`: pass.
- `m5_gc2_pacing_atomic`: pass, including the 8-thread atomic update fixture
  and 4-thread allocation smoke.
- Five `BENCH_SCALE=.05` `closures_upval` samples were 375.19--383.21 ns/op;
  five same-session unmodified-checkpoint samples were 374.68--391.68 ns/op.
  The cold helper split caused no measurable closure regression.

Two broader checkpoint failures reproduced identically without this patch and
are not attributed to it: `m9_trace_hard_assist_cadence` reports
`worker_delta=283` against its 160 bound, and `m6_jit_alloc_account` reaches an
already-nonempty SSB in `test_public_minor_skips_registry_roots()`.
