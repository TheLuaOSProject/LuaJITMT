# Worker SWEEP preparation and native teardown

GC workers now invoke the existing SWEEP preparation boundary after releasing
their drain claim. Previously, the automatic allocation fixture could reach
an empty graph frontier with the ownership-spine cursor still at its head;
workers never scheduled the missing preparation/pruning work. The repaired
worker continues after a certified cursor, preparation, snapshot, EOF or READY
advance. A refused or unchanged boundary retains the existing interruptible
backoff instead of reporting progress and repeatedly waking its peers.

Workers also close their startup native scope before detaching. A consumed
root/allocator action must finish its existing poll hold before private
teardown, while a later request cannot borrow that TG as a parked native owner.
The first worker candidate omitted this boundary; its real private-state
overlap is preserved as a rejected result. No acknowledgement, root, finalizer,
physical-reclamation or IDLE-close guard was removed.

The combined runtime includes the accepted constructor deferral and between-TG
fairness change. It passes 129 isolated runtime processes across default,
assertion/APICHECK and target-only ASan builds, plus 58 registered components:
18 worker preparation/stop/lifetime cases, 37 automatic-control cases and the
three scheduler components. Stock coverage remains 387 tests with JIT off and
509 with JIT on in each isolated build. The separate original worker package
has 88 accepted final processes on its earlier source generation. Exact
source, macro, archive and executable identities distinguish these generations.

The first registered scheduler C run aborted; its two later Lua components
were unrun. Fresh diagnosis found the fixture clearing irreversible READY
and republishing it without owning the worker token, reopening JIT entry
during a real close. The correction moves the manual READY refusal/success
pair before worker startup, with demonstrably uncontended close admission,
and publishes READY under an actual claim. Its asynchronous case establishes
the intended prepared owners before release and permits workers to finish the
phase. All physical arena, progress and stop checks remain. Thirty-six matched
default/assertion/ASan correction runs pass, including boundary observers;
the fresh registered C and Lua
off/on run also passes. Original aborts, the strict invalid-gate assertion,
diagnostic failures and return-only negative controls remain recorded.

The original automatic fixture bounds are unchanged: six rounds, three actual
completed cycles per round, a 32-table live ring, at most 262,144 escaping
allocations per round, and its original native waits and cleanup. Its
interpreted worker cases now finish. Enabling JIT separately passes 9/12
combined cases; the sole-main, zero-worker case still misses round four's
bound in all three builds. The exact earlier runtime reproduces that failure
with byte-identical normal stdout. All four failures exit normally through
the failure oracle and successfully perform the original cleanup. This is an
open automatic scheduling problem, not a repaired case. Those failures reach
50 compiled allocation hard checks; read-only diagnosis observes just 64 SSB
entries serviced per handoff before renewing a native lease, while recovery
grows to 258,143 independently verified identities. The other engine-enabled
cases report zero hard checks and do not prove that compiled-path coverage.

Seven alternating cost pairs per workload retain all 42 processes and 210
samples. Median paired changes are -0.09% interpreted TNEW, -0.30% interpreted
TDUP and +0.20% JIT-enabled TNEW, with overlapping ranges. These limited
single-thread, zero-worker measurements show no material change; they do not
establish worker scalability, stock parity or a general speedup.

Synchronous handshakes and borrowed-root completion remain. The ordinary
pruning budget still has an unbounded pending-chain EOF tail, and a held arena
can still delay another arena in the same TG. Existing string-retention policy
also remains. This worker scheduling repair is not a claim of fully nonblocking
or parallel collection.

See the [combined review](evidence/gc-worker-sweep-2026-09-05/root/review.md),
[worker handoff](evidence/gc-worker-sweep-2026-09-05/owner/HANDOFF.md),
[original diagnosis](evidence/gc-worker-sweep-2026-09-05/diagnosis/HANDOFF.md),
[scheduler diagnosis](evidence/gc-worker-sweep-2026-09-05/scheduler-diagnosis/HANDOFF.md),
[scheduler correction](evidence/gc-worker-sweep-2026-09-05/scheduler-correction/HANDOFF.md),
[JIT-enabled matrix](evidence/gc-worker-sweep-2026-09-05/jit-matrix/HANDOFF.md),
[JIT scheduling diagnosis](evidence/gc-worker-sweep-2026-09-05/jit-diagnosis/HANDOFF.md),
[cost results](evidence/gc-worker-sweep-2026-09-05/root/perf/summary.json),
and [archive manifest](evidence/gc-worker-sweep-2026-09-05/manifest.json).
