# ARM64 native JIT phase-gate checkpoint (2026-08-26)

## Scope

This checkpoint proves that the first admitted macOS ARM64 JIT shape obeys the
real GC2 native-entry gates in IDLE, MARK, and SWEEP. It deliberately remains
limited to one scalar integer `BC_LOOP` root:

```lua
local i, x = 0, 0
while i < n do i = i + 1; x = x + i end
return x
```

Side traces, stitching, function-header entry, allocating IR, FFI IR, spills,
and every other still-closed ARM64 JIT surface are outside this checkpoint.
The runtime fixture verifies the exact admitted IR/snapshot shape, including
`IR_XPOLL`, the two allocator `RENAME`s, and the absence of spills. Each phase
subtest creates a fresh Lua universe so a preceding collection cannot satisfy
a later assertion.

## Files

- `tests/t-arm64-jit-phase-gates.c` is the deterministic runtime fixture.
- `tools/ci/arm64_jit_phase_gate_contract.sh` builds and runs it under ordinary
  ARM64 and ARM64e/BTI, including a randomized mcode-placement ARM64e run.
- No production source changed for this checkpoint; it uses the existing
  root-entry, exit-stat, and IDLE-reclaimer test boundaries.

## IDLE: entry veto during real retired-body reclaim

The fixture first preflights the production IDLE reclaim enter/leave pair. It
then preloads the traced function on the Lua stack before arming the exclusive
writer; a global lookup after the freeze could correctly wait for metadata SMR
and would test the wrong thing.

A foreign pthread calls `lj_gc2_reclaim_retired()` and pauses immediately after
the reclaimer has:

1. acquired `LJ_GC2_SMR_META_EXCLUSIVE`,
2. changed `jit_phase_gate` from one to zero,
3. executed the matching fence, and
4. sampled all TGs as having no active `jit_base`.

Calling the cached `BC_JLOOP` function in that window returns exactly `210`.
The exact observations are zero entry publications, zero publication cleanups,
zero native exits, at least one immutable `startins` recovery call, and
`jit_sweep_displaced == 1`. Thus a late ARM64 entrant cannot appear behind the
reclaimer's zero-active sample, while the interpreter can safely recover the
original `BC_LOOP` without entering metadata SMR. Releasing the reclaimer
returns the universe to IDLE, SMR-open, gate-one state.

## MARK: close after admission, XPOLL, and legal regrant

The MARK subtest calls the real `lj_gc2_mark_begin()` and verifies the activation
handshake, cycle-matched `jit_mark_resume`, active allocation barriers, and an
open JIT gate. The GC threshold is pinned at `LJ_MAX_MEM` before the phase starts.

The root-entry helper pauses after its final admission recheck with `jit_base`
already published. A raw closer calls `lj_gc2_jit_mark_request_exit()`. A
separate watchdog releases the root-entry pause only after it observes that the
closer returned; this turns nonblocking close into a deterministic assertion and
also prevents the historical synchronous-wait failure mode from hanging CI.

At the instant the closer returns it observes all of the following:

- the phase is still MARK and the JIT gate is closed;
- `jit_base` was non-null before and after the close call;
- handshake epoch/ack and TG poll, reqmask, and profile words are unchanged;
- the close completed below the fixture's 50 ms regression ceiling.

Native `IR_XPOLL` then exits root 1 at snapshot 5. In this deliberately tiny
fresh universe, `vm_exit_interp` performs enough real collector work to finish
MARK, reaches IDLE, reopens the gate, and legally re-enters the same still-live
trace. The measured tuple is therefore exactly two entry publications and two
native exits: first `1/5` (the gate-close XPOLL) and last `1/8` (the ordinary
loop-condition exit after IDLE regrant). There is no stale-startins recovery in
this path because the gate has legitimately reopened before redispatch.

This outcome is intentionally recorded instead of forcing an artificial
"interpreted tail" invariant: the production protocol promises asynchronous
quiescence and safe bounded turns, not that a completed phase can never grant a
new native turn.

## SWEEP: close after admission and interpreted tail

The SWEEP subtest creates unreachable tables through the C API while collection
is stopped. This gives the real collector physical work without recording an
unsupported allocating Lua trace. Explicit GC steps then reach
`SWEEP + sweep_bridge_ready + gate-one`; collection is stopped in place without
changing phase.

The same post-admission/watchdog choreography calls
`lj_gc2_jit_sweep_request_exit()`. At closer return, the phase is still SWEEP,
the gate is zero, `jit_base` remains live, counted-request words are unchanged,
and `jit_sweep_displaced == 1`. The call returns exactly `210` with one entry
publication and one native exit, first and last root 1/snapshot 5. The remaining
iterations use immutable `startins` recovery under the closed gate, so the
stale-startins counter is nonzero and no second native entry occurs.

Bounded explicit steps then complete SWEEP to IDLE, reopen native entry, and
leave the exact trace runnable. `sweep_bridge_ready` is phase-qualified and may
retain the completed generation's certificate while IDLE; the next
WEAK-to-SWEEP transition clears it before publishing a new certificate, so it
is not used as an IDLE cleanup predicate.

## Validation

The ordinary native ARM64 fixture passed with warnings-as-errors when compiled
against the experimental archive:

```text
t-arm64-jit-phase-gates OK: IDLE veto and MARK/SWEEP XPOLL exits
```

`tools/ci/arm64_jit_phase_gate_contract.sh` passed end to end. It rebuilt and
ran the same three fresh states once under ordinary ARM64, once under ARM64e
with BTI, and once under ARM64e with randomized mcode placement to exercise the
authenticated far-exit path. The script then restored the shared checkout to
the ordinary experimental ARM64 build. The only compiler diagnostics were the
pre-existing unused `szmcode` and `ccall_rawchild_wait` warnings from production
translation units; the new fixture itself compiles with warnings as errors.
The ordinary ARM64 fixture also completed 25 consecutive direct stabilization
runs without changing any measured counter tuple.

## Limits and next gate

This is a phase-admission and XPOLL lifecycle proof, not broad JIT support. It
does not make ARM64 side/stitch recorders, `BC_JFUNCF`, spills, allocating IR,
FFI traces, or arbitrary scalar IR runnable. It also does not add new production
telemetry: exact entry/exit counts come from test-helper builds.

This complements the immediately preceding live FLUSHJ retirement, real grace,
and public trace-slot reuse checkpoint (`2ee3233d`). The next gate is scalar
widening for the `SUBOV`, `MULOV`, and signed-comparison family while retaining
the same root-only, zero-spill admission boundary.
