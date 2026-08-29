# Bounded JIT trace pressure for automatic GC2 cycles

## Release blocker

The mandatory stock `misc/gc_trace.lua` oracle failed before its explicit
collection: 100 short-lived compiled chunks left trace slots 1 through 102
published, so slot 90 was still visible. The failure was deterministic and was
not a trace-retirement race. A following explicit GC2 collection retired the
dead prototypes and trace bodies and released/reused the old slots correctly.

The missing edge was automatic cycle initiation. Immediately after the
fixture's first explicit collection, a representative snapshot was:

```text
total_bytes          147313
live_estimate         30512
trigger_bytes        294626
burst allocation     133524
```

`g->gc.estimate` has no GC2 cycle-close publication, so the compatibility
fallback in `lj_gc2_update_pacing()` uses total bytes. GC2's fixed state and
first worker are substantially larger than stock's fixed heap, and the short
trace/prototype burst never reaches that global trigger. Stock performs one
mid-burst cycle and naturally keeps the highest live slot below 90.

Lowering `LJ_GC2_PENDING_ROOT_TRIGGER_MAX` globally would also make the oracle
pass, but would undo the closure-throughput reason it was raised. Switching
global pacing directly to the traversable-only `gc2.live_estimate` would omit
plain/raw backing allocations. Correct raw/plain live aggregation and exact
stock pause-to-allocation-debt normalization remain separate pacing work.

## Nonblocking pressure edge

The JIT state now counts only completely published traces under the existing
recorder token. Every 64 publications, `trace_stop()` checks whether the
token-owned free cursor also proves that the live/reserved trace namespace has
crossed 64 slots. Repeated recompilation which reuses a small namespace is not
treated as heap pressure. A crossed batch returns a pressure hint.
The protected recorder state then:

1. restores interpreter state;
2. releases the recorder token;
3. calls `lj_gc2_request_cycle_pressure()`; and
4. enters the already-existing bounded `lj_gc_step()` path because a successful
   request made the normal threshold due.

The pressure call is only a request. It neither traces roots, decides liveness,
flushes JIT state, waits for a token, nor reclaims a body. It reuses the ordinary
automatic-GC IDLE leader CAS and threshold publication, and therefore honors
`collectgarbage("stop")`. If a cycle is already active, the ordinary request
path merely wakes its worker. Full `jit.flush()` resets the publication count
along with the trace namespace.

Combining a publication cadence with the current slot high-water gives bounded
hysteresis across slot reuse. Long-lived or high-namespace workloads publish at
most one pressure request per 64 completed compilations, while churn which stays
below the boundary pays one token-private increment and no collector request.
GC2 remains the only authority which can prove a prototype unreachable and
retire its trace graph.

## Coverage

`tests/t-jit-trace-gc-pressure.lua` verifies both sides of the public policy:

- 80 throwaway compiled chunks increase `threading.gcstats().cycle_requests`
  even when the larger compatibility trigger is not reached; and
- the same publication pressure cannot request a cycle while
  `collectgarbage("stop")` is active.

The `m6_jit_trace_proto_gc` case runs that fixture and the unchanged stock
`misc/gc_trace.lua` lifetime/record-callback oracle. The combined case passed
20 consecutive runs before the slot-high-water refinement and 10 consecutive
runs after it, with no retries.

The final source also passed the recursive trace-retention, GC2 JIT readiness,
active-stack flush/GC, concurrent `jit.util` flush, MT-activation flush,
GC-worker-activation flush, and VM-event flush cases. The active-stack closure
control reported a best 314.42 ns/allocation, inside the range measured before
this change. A pinned seven-process comparison against an isolated `af2e03dd`
parent measured 320.37 versus 319.40 ns/allocation with GC active (+0.30%) and
98.45 versus 98.73 ns/allocation with GC stopped (-0.28%); both differences are
noise. An 80-unique-trace burst deliberately costs more once it initiates the
missing cycle (1.59 ms versus 0.46 ms immediate CPU time), but including the
explicit collection the parent would otherwise defer narrows that comparison
to 2.26 versus 1.72 ms. This is bounded reclamation work for pathological trace
churn, not overhead on ordinary trace execution or closure allocation.

The full recovery case, paranoia matrix (four C oracles, 509 JIT and 387 no-JIT
stock tests), and GC-worker scheduler case all passed on their first run. The
pre-existing `m6_jit_token` assertion that recording resumes after releasing a
foreign recorder token still fails: the exact pushed parent `af2e03dd`
reproduced that failure three consecutive times in a clean worktree, so it is
tracked as a separate correctness tranche rather than attributed to this
pressure edge.
