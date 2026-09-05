# GC work selection and exact service outcomes

A continuing stream of ordinary SSB entries can starve eligible recovery work.
On the current baseline, MARK and controlled pre-READY SWEEP consume 4,096
string entries while one parent recovery identity remains PENDING. A matched
quiet control uses the same ordinary worker drain to retire that identity and
mark its child after the stream ends. Every original run completes graph and
cycle cleanup. Assertion, optimized and ASan builds agree.

The original baseline package contains 40 processes: 20 quiet passes and 20
intended feeder counterexamples, including the final standalone fixture.
The old included-fixture optimized compilation failure is preserved. A later
fixture adds worker WEAK and assist MARK/WEAK, stops feeding after actual early
service, and retains the same cleanup. Across three builds it gives 30 quiet
passes and 30 starvation counterexamples on the baseline.

An isolated four-class candidate rotates published SSB, owner-active SSB,
grey and recovery work across the actual worker claim. All 60 focused
candidate processes pass: the parent recovery retires on call two for quota
one, or call one for quota 64. This proves the selected eligible-recovery
case. It does not prove reverse-class, retained-head, bounded-closure,
per-object or native-service progress. Its stricter quota counts a transfer
and subsequent traversal separately.

**The candidate is unlanded.** Broader strict and ASan matrices each report
22 passes and four failures. These include two quota-sensitive fixture
assumptions, an unclaimed test-only weak bridge, and stock-JIT signals.
The independent [weak helper correction](gc-weak-helper-claim-2026-09-05.md)
addresses only the test bridge on the original baseline. It contains no fair
scheduler code or quota-fixture changes.

The original stock-JIT failures report SIGBUS and SIGTRAP with empty logs.
A later natural-ASLR GDB run captures SIGSEGV at a prototype upvalue-vector
load in `lj_func_newL_gc`. Its relationship to the earlier signals and its
underlying cause are unproved. Passing debugger runs and one matched baseline
pass do not clear that failure. Automatic approval review subsequently
rejected the delegated stock-lifetime investigation with a generic
cybersecurity flag and no specific rejected tool action. That investigation
was not retried or transferred. No candidate performance claim is made.

The separate leaf audit also narrows the foreground accounting design:
recovery completion returns zero for both actual count retirement and
fail-closed failure; traversal DONE includes a thread NEEDSCAN transfer; and
successful publication can mean existing or terminal coverage. A source slot
may retire before later deferral, or a recovery count may retire while its
payload is requeued elsewhere. A positive drain count cannot pay native
allocation credit as if it meant completed graph work.

The proposed internal scalar side result records source commitment, payload
outcome, successor work and refusal independently, while retaining public
returns and ownership actions. That result refactor is source-only and grants
no allocation credit. Fair work-class opportunities and a sustainable native
service policy remain separate requirements; whole-chain EOF, per-object
queue fairness and parallel collector ownership remain open.

See the [baseline witness](evidence/gc-work-service-2026-09-05/baseline/BASELINE.md),
[unlanded candidate review](evidence/gc-work-service-2026-09-05/candidate/REVIEW.md),
[class-selection contract](evidence/gc-work-service-2026-09-05/contract/CONTRACT.md),
[leaf outcome map](evidence/gc-work-service-2026-09-05/receipts/RECEIPT-MAP.md),
[result refactor contract](evidence/gc-work-service-2026-09-05/receipts/REFACTOR.md),
[automatic review stop](evidence/gc-work-service-2026-09-05/candidate/stock-lifetime-review-stop.json),
and [archive manifest](evidence/gc-work-service-2026-09-05/manifest.json).
