# SWEEP table request coalescing

Public table barriers now invalidate the table's scan stamp under retained
allocation admission before publishing an SSB request. The converter may consume
a duplicate only after a complete scan covers that dirty epoch or a later one.
This preserves the raw-store/public-barrier contract repaired in
`gc-sweep-public-table-rescan-2026-09-04.md`, while avoiding repeated scans of the
same payload for queued duplicates.

The order is payload store, dirty-word CAS, then queue cursor publication. The
scanner samples the dirty word before reading the payload and can publish its
proof only if that word still matches. A scanner preceding a later dirty CAS
cannot cover the later request. A scanner completing after the latest
invalidation can cover earlier queued requests, including a request whose
publisher is paused before exposing its SSB slot. The exact retained table body
scope extends through invalidation and queue/recovery publication.

The converter also requires clear NEEDSCAN and no legacy INSTALLING/COUNTED
membership. Recovery remains independent of this filter: its exact count,
CLAIMED/REDIRTY state, and traversal ownership are unchanged. A dirty value of
UINT32_MAX is never coalescing authority. The existing saturation CAS and sticky
reclaim veto remain; ordinary worker cycle suppression can still use the scan
stamp so saturation does not manufacture endless private graph work.

The dirty counter remains 32-bit. A long-lived hot table can reach saturation
and permanently set the global reclaim veto, so safe rollover and sustained
reclaim progress remain release work. This patch preserves the safe veto; it
does not solve that limit or assume saturation is practically unreachable.

An admitted table's array/hash self-values, hash self-key, and self-metatable are
consumed by its current scan. The scanner already retains and marks that exact
allocation, and the self-edge adds no payload beyond the scan in progress. This
does not suppress a public publication naming that table. It prevents a newly
invalidated table from REDIRTYing its own CLAIMED recovery item merely because
its replacement scan proof has not been published yet. Weak-mode and FINREG
classification still precede the relevant payload-edge handling.

## Publication audit

| Publisher | Authority before a suppressible table request |
| --- | --- |
| Public SWEEP root, table, and TValue barriers | `gc2_trace_sweep_edge` retains the supplied scope or its own new scope; admitted semantic publication invalidates before queueing. Private worker edges keep their separate discovery path. |
| Public MARK table barrier delayed into SWEEP | `gc2_barrier_tab_mark` acquires worker expected-TAB admission and keeps it through dirty/token/queue work. Small RETRY transfers to recovery; terminal wrong type remains untouched. Saturation uses an explicit scoped SSB/recovery request instead of the legacy current-stamp shortcut. |
| Expected-type/table marking and public semantic object leases | `gc2_publish_semantic_scoped` invalidates under the exact small or huge body scope. Invalidation is unconditional with respect to earlier phase samples. |
| Small expected-type post-mark RETRY | The original counted admission still pins the already-validated exact table through its retry witness, invalidation, and publication. A successful API retry is not needed to repair the graph. |
| Huge wrong-expected-type or wrong-candidate mark discharge | The actual validated object and type are used under the huge reader; an actual table is invalidated before its graph request. |
| NEW-only base validation and thread-root marking | The retained scope bridges marking into semantic publication. NEW does not exempt a delayed publisher from invalidation: another owner may finish a scan before its slot is exposed. |
| IDLE remembered publication | Observational admission pins the exact table through unconditional invalidation and the no-drain queue attempt. This also covers delayed publication after activation. |
| Huge reader saturation or registry admission retry | There is no dereferenceable body certificate. Publish allocation-side recovery directly; if its independent admission fails, retain the established sticky reclaim veto. Never place this unresolved request in the suppressible SSB lane. |
| Legacy deferred table traversal | INSTALLING/COUNTED and NEEDSCAN retain the pending obligation through generic publication. A failed traversal does not clear its exact credit. |
| FNEW certificate's direct environment slot | Closure-edge discovery only; it does not write environment payload. Environment mutations retain their own barriers. The MARK certificate and native-entry gates remain unchanged. |
| Other direct generic publishers | Main/thread roots, prototype/trace/function/upvalue/userdata work have no table scan-stamp suppression. Test-only raw queue helpers deliberately expose the queue contract without adding semantic authority. |

The defensive NEW type-mismatch branch in preservation is also routed through
admitted semantic publication. Current small and huge retention already reject
the mismatched expected type earlier; the branch must remain safe if that
internal layering changes.

## Deterministic validation

`tests/t-gc2-sweep-table-coalescing.c` uses actual public publishers, the real SSB
converter, and worker traversal. Its test hook only observes or pauses exact
publication/scan boundaries; it does not replace marking or traversal. Available
SSB space prevents recovery overflow from concealing a dropped ordinary request.

- Thirty-two requests produce one parent scan on small and synthetic valid huge
  tables, across root, TValue, semantic lease, and expected-table APIs. Fixtures
  include every private self-edge location and two-/three-table cycles.
- A publisher pauses both before and after invalidation while an older queued
  request completes a scan. A late payload write while a scanner is paused
  before proof publication must retain and eventually mark its child graph.
- At dirty saturation an old scanner can publish the ambiguous maximum after a
  new write. The public request must still scan and mark the new descendants;
  the reclaim veto remains sticky.
- A real small lifetime transition after mark admission returns RETRY. Restoring
  LIVE and consuming that sole publication must preserve the new graph, without
  calling the successful API again.
- Huge wrong-type mark discharge preserves descendants. Actual huge reader
  saturation and a closed registry are tested with inaccessible body pages to
  reject speculative header/stamp reads.
- MARK-to-SWEEP publication races cover ordinary expected marking and NEW base
  validation. IDLE remembered publication crosses activation and SWEEP. Another
  owner completes a real scan before the delayed publication, then a raw store
  makes that proof obsolete. All scopes and counts balance after completion.
- The public MARK table helper is delayed after its phase gate into SWEEP.
  A DESTRUCT admission denial must retain a recovery identity and discover the
  newly stored child; an unrelated MUTATING denial must preserve the sticky
  veto without table reads. Plain userdata and a real traversable function
  exercise wrong-type rejection, including unchanged sidecar/token state. An
  old maximum-dirty scan completes between invalidation and publication to
  verify the helper cannot drop the request before the SSB converter sees it.

The Linux canonical case `m3_gc2_sweep_table_coalescing` is registered directly
and in the M3 scaffold. Its helper build and default-build restoration pass.
The final fixture passes twenty complete runs each against the protocol-only,
integrated, and ASan archives. The saturated MARK cutover also passes one hundred
integrated repetitions and twenty each against the protocol-only and ASan
archives, with its one-worker-quantum assertion intact.

Protocol-only GCC assertion/helper runs pass the new fixture, public table/
FINREG rescan, leaf publication, full traversal, full recovery, public weak-store
window, and weak-resize fixtures. Clang 19 AddressSanitizer with leak detection
passes the new fixture, full traversal, full recovery, and the updated scalar
table-store guard fixture.

The guard's old SWEEP expectation failed because `begin` already performs two
semantic parent publications: object admission and the by-value root before
ACTIVE. Both now invalidate. The updated assertion requires exactly two begin
increments, no additional increment for failed finish, two more increments for
the next begin, then exactly two successful-finish increments (transaction dirty
and public SWEEP rescue), all before INSTALLING. It retains descriptor, resource,
COUNTED ownership, and failed-store checks. The earlier strict link failure
without `LJ_TAB_TEST_HELPERS` and the old assertion failure remain in the logs.

## Negative controls and evidence

Artifacts are in `/tmp/lj-sweep-coalescing-20260904-lmawpxay`. `manifest.json`
records source hashes and compilers; `scheduled-negative-controls.json` records
the final fourteen results, with a separate source diff and log for each control.
`scheduled-fixture-metadata.json` binds the final fixture, executable and archive
hashes to the compile/run commands and `scheduled-fixture-results.json`.
`canonical-scheduled-result.json` and its log record the final canonical pass.
The earlier `final-negative-controls.json` and failed canonical log remain intact.
The protocol tree
began from archive `0048573073961e56153e72b906eaaa7c777e8c05` with the exact
owner-published arena statistics files and coalescing GC files overlaid. It
excludes the concurrent later JIT change. Source hashes identify that boundary
more precisely than the concurrently advancing shared HEAD.

The final GC source SHA256 is
`61009e510a1de2b591fffe32162a1772a4a24d4a4bb7b84368bbdc0eb8341c25`;
the header SHA256 is
`12a024f5b2de1c760bc56702dc17d112357a7fc56d229f013a299d9b13552f99`.
The final fixture SHA256 is
`a664e380f4688afc0dfb836baa2cb1f859b8f8f3815b694a1797cc199bf562fb`.
The final protocol and ASan records use those exact source files and retain the
previously validated arena/statistics files, excluding later arena edits.

All fourteen targeted negative-control runs fail at the intended assertion:
removing publication invalidation loses the raw-write child in the existing
public-barrier fixture; disabling coalescing scans the parent thirty-two times;
omitting small RETRY or huge wrong-type invalidation loses descendants; routing
huge unresolved work through SSB loses descendants or fails the closed-registry
veto contract; admitting saturated proof loses the late-write child; restoring
private self-publication breaks the unchanged one-pass recovery assertion;
releasing the unscoped SWEEP admission early violates the exact retained-scope
check; and phase-conditional invalidation loses descendants in the expected,
NEW base, and remembered publication schedules. Restoring the previous MARK
helper loses the actual descendant after denied admission, and removing its
explicit saturation publication loses the child behind an ambiguous old scan.

The final read-only audit found that older MARK helper gap while checking the
universal retained-publication claim. The helper previously dirtied before its
temporary `ismarked` admission, accessed tokens after admission ended, and could
return after `ismarked == -1` with no queued obligation. The repaired helper
uses non-publishing expected-TAB authority through completion. Its saturation
check also bypasses a pre-converter legacy shortcut. This closes the demonstrated
public helper gap within the same protocol change; it does not claim every
unrelated collector path now has a complete nonblocking proof.

Earlier fixture failures are retained separately: the initial recovery run
exposed private self-REDIRTY, and the first phase-schedule attempts incorrectly
tried to append an owner SSB or start activation from a foreign thread. The final
phase schedule runs those owner operations on their real owner and uses the hook
only to establish the delayed publication interleaving. No correctness check was
removed to conceal these failures.

The first final canonical and integrated runs exposed a fixture scheduling
assumption in the saturated MARK test. A diagnostic linked against the exact
integrated archive observed zero worker work with MARK active, native gate open,
resume equal to cycle 1, a remaining 19,873 ns native allowance, and zero table
scans. That is the collector's legitimate scheduling deferral. The C-only
fixture now explicitly closes the MARK allowance before demanding one quantum;
it still pauses a real scan before its proof and requires the actual descendant.
`mark-scheduling-diagnostic.{c,log,json}` and `canonical-final.log` retain the
failure evidence. The corrected integrated runs use the unchanged archive in
`/tmp/lj-gc-coalescing-final-20260905-0eig4rlf/assert/src/libluajit.a`.

Root's separate integrated normal build (coalescing plus the concurrent JIT
change) completed unchanged `t-arena-state` in 5.620 seconds; the prior leaf plus
statistics build completed that same fixture in 63.341 seconds. This is an
integrated pilot, not an isolated causal timing result or a general throughput
claim. That pilot precedes the final MARK-helper repair and is not evidence for
the final source hashes above. The separate final-source normal runtime at
`/tmp/lj-gc-coalescing-final-20260905-0eig4rlf/normal` completes that unchanged
state-churn test in 5.625 seconds and passes all 387 interpreter and 509 JIT
stock tests. Its assertion archive passes the related traversal, recovery,
public table/FINREG, leaf/admission, weak-store/resize, scalar guard, arena
statistics/sweep, and deferred JIT retirement fixtures. The original coalescing
failure and corrected repeated runs against that same archive are preserved in
[the checked-in validation records](evidence/gc-sweep-table-coalescing-2026-09-05/README.md).
The earlier full performance pilot is recorded separately in
`gc-coalescing-performance-pilot-2026-09-05.md`.
This change adds no lock, but does not establish that the complete collector or
all reclamation protocols are nonblocking.
