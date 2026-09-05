# Constructor deferral and fair TG sweep turns

A nested collector now returns when it encounters an unfinished intrusive
owner, retaining the exact allocation for publication or cancellation. Sweep
also resumes at the next current TG after a work-quota or arena-completion
boundary, so a held constructor no longer consumes every turn while another
TG has eligible work. Physical writer cleanup precedes deferral; lifetime,
READY, cursor and reclaim gates remain intact.

The continuation is a scalar ID resolved through the current list under the
existing worker claim. It holds no TG pointer across calls. Tests require
actual independent completion while one or three constructors stay suspended,
including quota one, background workers, and a naturally saved ID whose TG is
then physically unlinked. The original nested timeout is test-injected;
that observation alone does not establish an ordinary Lua trigger.

The latest-source combination passes 170 functional processes: 39 initial,
82 GC/native regressions and 49 registered tests. Assertion/APICHECK and
target-only ASan runs retain leak checks. The new
`m3_gc2_constructor_defer` case includes 11 constructor and fairness runs and
restores the default build. Original failure controls remain recorded.

Seven paired GC-enabled allocation measurements per workload show overlapping
timings: +0.18% interpreted TNEW, -0.28% interpreted TDUP, and -0.68% JIT-enabled
TNEW. All 42 processes and 210 samples are retained. This is a limited cost
check, without a stock-parity or speedup claim.

Fairness is conditional on reaching the owner pass and the target's existing
guards admitting work. For a stable N-TG list, each target receives a turn
within N such invocations. **A held arena can still delay another arena in the
same TG.** The quota counts existing work quanta, not raw cells or elapsed
time. Synchronous GC ownership, general iterator progress and concurrent
reclamation remain open; the separate worker SWEEP candidate is not included.

See the [root review](evidence/gc-construction-defer-fairness-2026-09-05/root/review.md),
[focused handoff](evidence/gc-construction-defer-fairness-2026-09-05/focused/FOCUSED-HANDOFF.md),
[broader handoff](evidence/gc-construction-defer-fairness-2026-09-05/broad/HANDOFF.md),
[canonical results](evidence/gc-construction-defer-fairness-2026-09-05/root/canonical.json),
and [archive manifest](evidence/gc-construction-defer-fairness-2026-09-05/manifest.json).
The [first rejected candidate](gc-construction-defer-review-2026-09-05.md)
remains a meaningful starvation negative.
