# Exact candidate v2 source verdict

ROOT's four-class selector is a suitable narrow implementation of the contract in CONTRACT.md for correctly claimed callers. The exact v2 candidate is **provisional**: the existing test-only overflow bridge lacks a worker claim and reaches the new assertion. AUTHORITY.md provides the complete direct-caller map and a wrapper-only repair proposal. No unclaimed production bounded-drain path was found. This package contains no implementation and no reviewer-run runtime validation.

Reviewed patch: `/tmp/lj-gc-workclass-fair-repair-20260905-q5riyfsd/candidate-v2.patch`, SHA256 **eb3003c2e9f277487cde6308560fd4ba5aea2a33c3b73e5fa281cf3a778034a7**.

| Exact source | SHA256 |
| --- | --- |
| Base lj_gc2.c | 3571a3f2128114c475a730fcb35a413cffdfd27124ba2dcb9e136d9690edbea9 |
| Candidate lj_gc2.c | 56a6d810e2c402261aa1ca61f21ca2d9367b31afb3610d8118c2db104ee260d1 |
| Candidate lj_obj.h | a8d398e6bbba79a591b2a8592bb77e942adbdc06e5cdf4dbfedaead60b8b935c |
| ROOT setup snapshot | 5f0632b057b16fb8ef385b400552b9f525847eba3c224a833d53560cdadba191 |

All225 baseline inputs and all225 candidate inputs matched ROOT's setup. Only lj_gc2.c and lj_obj.h differ. Reconstructing the unified diff from the copied source bytes produced the exact patch bytes. The 225 local baseline inputs match the committed f9ec identities. Parent baseline manifest36 entries and prior receipt-audit manifest232 entries were all reverified unchanged; copies contain text evidence only, not another runnable fixture generation.

The patch meets these source requirements:

- Four distinct class positions persist across claims; successor publication precedes every attempt. Every selector invocation probes each class no more than once and returns on the first unit or defer.
- Worker passes NULL; assist retains its TG; bounded closure retains G2TG. No root ownership or private SSB authority is introduced.
- The worker's original phase/JIT/native checks, SWEEP recorder/finalizer checks and complete work predicates remain. The bounded closure keeps NEEDSCAN stop. Version2 retains outer deferred_epoch comparisons in addition to the per-selector observation.
- Leaf conversion, traversal, recovery, retry, scope, publication and lifetime code is unchanged. Source units remain legacy attempts/transfers; a nonzero result is not a universal service receipt.
- The reused pad preserves source layout. It is initialized once and no unrelated cursor reset overwrites it.
- The common grey helper owns global-grey accounting; removal of the old worker duplicate is correct. Strict per-source quota geometry and newly counted SWEEP global-grey work are intentional changes requiring validation.

The remaining validation status is explicit. ROOT reported60 focused v2 candidate successes with matched baseline controls, then separate regression failures: recovery and coalescing assumptions, the demonstrated test-wrapper assertion, and unexplained stock/JIT signals. This reviewer independently read only the exact traverse GDB artifact needed for the authority audit; those reports are not reviewer-run tests. The initial provisional assessment before the full test-entry audit found no material omission in the four direct production drain replacements; the assertion revealed why `_owned` naming and production caller checks do not cover the exported test wrapper. This verdict supersedes any broader inference from that earlier assessment.

Preserve the wrapper blocker and original failures. Do not promote v2 as a validated integration on the strength of class-fairness source proof. A later wrapper repair needs a separately identified patch/source generation; runtime and cost conclusions belong to its exact build and fixtures.

ROOT subsequently stopped further fairness runtime changes. A separately delegated stock-lifetime diagnosis received a generic cybersecurity review flag; ROOT reported that the exact rejected operation was not available. This reviewer did not attempt or retry that diagnosis. The fairness runtime remains unlanded, its stock/JIT failure remains unresolved, and this source contract supplies no basis to bypass the block. ROOT plans the baseline test-helper claim correction as a separate change. No quota-fixture correction is proposed for integration with the unaccepted fairness runtime.
