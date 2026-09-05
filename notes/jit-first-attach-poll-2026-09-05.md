# First attachment polling in pre-MT traces, 2026-09-05

Every x64 `IR_XPOLL` now checks the current TG's request word. Previously,
literal mode 0 checked only the GC phase gate, so first external attachment's
trace-flush request could wait for a native loop to finish naturally. The
generic flush does not own that phase gate. The added 32-bit memory comparison
exits through the existing snapshot; mode 1 retains its combined 64-bit
poll/profile-request comparison. Phase-gate ownership and all flush/root
completion protocols remain unchanged.

The preceding real-wait diagnostic is in
[native progress boundaries](native-progress-boundaries-2026-09-05.md).
This repair supplies a missing native observation point. It does not make the
whole attachment or handshake protocol nonblocking when a different owner or
collector is suspended.

## Exact change and review

The isolated baseline is `b4e26564`. All 224 tracked `src/` and `dynasm/`
files were compared across baseline, normal candidate, and assertion-enabled
candidate. Only `src/lj_asm_x86.h` differs in production. The matched normal
builds use identical options; only `lj_asm.o` and `lj_asm_dyn.o` differ among
their runtime objects. The assembly patch SHA-256 is
`bc142e4172537dcc3bd62ea6d18417a402af367c3038fe271986dfa0abbbed7c`.

[Independent source review](evidence/jit-first-attach-poll-2026-09-05/independent/review.md)
checks backward emission, the existing guard snapshot, loop inversion,
register use and code-buffer bounds. The phase gate still executes first.
No additional scratch register is needed. The successful pure native interval
does not acquire a new helper or metadata effect; a failed poll uses ordinary
exit/reentry rules. The reviewer did not rerun tests.

The permanent fixture is `tests/t-jit-first-attach.c`, registered as
`m6_jit_first_attach_poll`. The existing token fixture names its IR-literal
count as the wide poll count, since literal 0 also observes TG requests now.

## Native progress and regression checks

The fixture warms a real mode-0 trace, checks its IR and native exit witness,
then starts a physical external pthread that waits for the owner's actual
`jit_base` before calling `lj_threading_attach` on a rooted child state. Only
the owner exit callback changes the loop's stop flag. It records the last
completed iteration first, preserving an exact snapshot/result oracle.

Requiring an exit before the finite billion-iteration limit proves the missing
progress without asserting a wall-clock latency. Both the optimized `IR_LOOP`
backedge and optimizer-disabled terminal poll are exercised:

| Runtime | Optimized loop | Optimizer-disabled loop |
| --- | --- | --- |
| Matched normal baseline | Fails at 1,000,000,000; attachment 2065.764 ms | Fails at 1,000,000,000; attachment 3179.306 ms |
| Normal candidate | Passes at 32,889; attachment 0.051 ms | Passes at 55,776; attachment 0.059 ms |
| Assertion candidate | Passes at 36,802; attachment 0.060 ms | Passes at 21,712; attachment 0.058 ms |

Each process verifies the exact returned iteration, successful attachment,
ordinary detach/join, full collection and close. The timing columns describe
these runs, not latency guarantees. All use CPUs 0–15 with finite process
bounds. The shared default build additionally passes both permanent fixture
modes after integration.

Normal and assertion candidates each pass stock tests (387 with JIT off,
509 with JIT on) and all eight cdata mutation/lifetime guard modes with JIT
off and on. Canonical first-attach, token, XBAR/XPOLL, MT activation and
GC-worker activation cases pass. No sanitizer build was added for this
assembler-only change.

The broader `m6_jit_barrier_xpoll` case fails its first native-trace assertion:
the active-worker `setmetatable` loop reports recorder `NYI: bytecode ISLT`.
Exact baseline, normal candidate and assertion candidate all reproduce the
same standalone failure. The aggregate stops there; its remaining selected
cases were run separately and pass. This is retained as an existing tracing
coverage/performance gap, not reported as a passing full M6 suite or repaired
by the poll change. Raw failure and comparison commands are archived.

## Bounded cost

Seven fresh alternating baseline/candidate pairs per workload ran on CPU 30,
using identical normal builds and five internal CPU-clock samples per process
with GC enabled. All 42 processes pass exact results and native exit checks.
Separate diagnostics verify each root retains literal-0 XPOLL and that the
candidate contains the new TG request comparison. The numeric root grows
from 138 to 152 bytes; its IR is unchanged.

| Workload | Iterations per sample | Baseline median ns/iteration | Candidate median ns/iteration |
| --- | --- | --- | --- |
| Numeric increment | 500,000,000 | 0.684 | 0.684 |
| Cdata field update/read | 50,000,000 | 2.051 | 2.051 |
| Table array reads | 100,000,000 | 1.367 | 1.367 |

Paired median changes are below 0.01% in these samples. The extra comparison
has no measurable throughput cost here; this small set on a shared host is
not a full-suite or stock-parity result. The separate cdata load-coalescing
candidate and callback stack repair are not included in these measurements.

Exact commands, source/object and binary identities, initial and final probe
versions, canonical outcomes and review hashes are in
[the evidence manifest](evidence/jit-first-attach-poll-2026-09-05/artifact-manifest.json).
[Raw measurements](../bench/jit-first-attach-poll-2026-09-05/cost/raw.json),
[paired results](../bench/jit-first-attach-poll-2026-09-05/cost/summary.json)
and the benchmark driver are retained separately. Windows/macOS validation
remains deferred until preparation for the next release.
