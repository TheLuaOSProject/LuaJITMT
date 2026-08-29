# GC2 JIT tactical SMR marker

## Release-scale failure

Repeated ASAN/assertion runs of the threaded JIT-flush stress could leave the
sole collector spinning in the final explicit collection. The legacy state was
SWEEP with all logical work empty, but the typed activation was the absorbing
`NO_RECLAIM` state. The exact first pin came from `mcode_allocarea()`:
`lj_gc2_markmem_registered()` lost an ordinary `lj_gc2_smr_read_try()` while an
IDLE retired-body reclaimer owned the opportunistic writer gate.

This was not an activation mismatch or an unclassified semantic edge. A newly
allocated mcode retirement sidecar is locally owned before its active-list CAS;
the active or retired list owns it afterwards, and the recorder token excludes
the corresponding list reclaimer. The pre/CAS/post raw marks close a GC root
snapshot race, but they are not the sidecar's body-lifetime authority.

## Scoped fix

`lj_gc2_markmem_registered_publish_try()` now takes one nonwaiting outer SMR
admission. Its publication-specific name makes the required independent
local/list lifetime authority visible at every call site.
On success, the existing mandatory scoped marker runs under a reentrant nested
reader and keeps all of its registry, arena and mark validation. On ordinary
SMR contention, the tactical helper requests an active-phase root-scan retry,
wakes the collector through that retry path, and returns without changing the
typed activation. It remains a mark-only boolean and never grants a body lease.

Mcode sidecar allocation and the detached-list pre/push/post preservation use
this helper. The same proof applies to trace-vector retirement: the recorder
token plus the local/published retired-list root owns the exact vector across
both tactical marks. Mandatory GC root scans (`lj_mcode_markretired()` and
`lj_trace_markvecs()`) retain the fail-closed marker because they have no
independent permission to omit a required root.

The post-publication call is important. A defensive active-phase admission miss
after the CAS calls `gc2_root_scan_retry()` rather than silently certifying a
missed list. MARK and WEAK certificates are reopened, and a provisional SWEEP
snapshot is reopened when possible. The mark-round counter is only a scheduling
signal; it is not treated as an exact sidecar identity.

The physical-writer exclusion is stronger for allocation and trace-vector
retirement. SWEEP reclaim entry closes native JIT entry and rejects either a
published active JIT TG or an active recorder before it can set
`smr_reclaiming`. Mcode allocation and trace-vector retirement execute under
that JIT/recorder ownership, so they cannot use a tactical miss as permission
to overlap a READY SWEEP destructor.

`mcode_preserve_list()` has a separate transfer proof. A public full flush can
move the active mcode suffix to the retired list while trace state is IDLE and
a SWEEP writer owns `smr_reclaiming`; the writer then loses the same recorder
token needed by the transfer. The mandatory pre-READY JIT root scan already
marked every active sidecar, and closed SWEEP entry admits no new mcode
allocation. The token protects the active-to-retired list ownership move, so a
tactical mark miss does not invent a new unmarked allocation behind READY.

The reproduced allocation-side writer collision is IDLE. There, a skipped
pre-publication mark is safe under the exact local/list owner, and the next
cycle necessarily scans the already-published active or retired list. The
post-publication retry remains a conservative certificate repair for defensive
phase skew; it is not the READY-SWEEP lifetime proof.

The focused mcode fixture holds the exact IDLE reclaimer gate, clears the raw
sidecar mark, proves the tactical call neither marks nor pins the activation,
releases the writer, then proves the mandatory JIT root scan marks the still
active node.

## Unpublished trace pre-claim

The same ordinary collision existed one level later in trace compilation.
`lj_trace_free_unpublished()` retires a compact trace which has no public trace
number, epoch, or retire-list link. Its recorder token/local construction owner
is the sole body authority. The assembler copy is not a partially published
semantic trace: it has copied IR bytes, but no trace slot, prototype/root-spine
edge, snapshots, exit table, debugger descriptor or executable mcode. Its KGC
operands remain rooted by token-private `J->cur` until the failed recording is
aborted. Treating the copy as a semantic body required a looping post-claim
traversal and manufactured ownership for fields the allocation never acquired.

Unpublished retirement now sets the immutable
`TRACE_RETIRED_UNPUBLISHED` kind, claims an epoch, publishes the exact body on
the token-owned retire list, and performs only one-shot pre/post raw marks.
Admission loss requests a root retry and returns without reading the compact
payload. There is deliberately no `trace_preservebody()` pass. Root scans mark
the exact listed allocation without decoding children; mature reclaim skips
public-slot, inbound-link, debugger, root-spine, exit-table and mcode teardown,
validates the strict scratch shape, and frees that allocation exactly. Public
trace retirement keeps the existing semantic preservation policy.

The trace-retire fixture holds the IDLE metadata writer while the recorder
token calls the real `lj_trace_free_unpublished()` path. The scratch includes a
deliberately unmarked KGC operand, while snapshots, start prototype, exit table
and mcode remain NULL. Both tactical marks lose without waiting; the exact body
and KGC marks remain clear, SMR stays writer-owned with zero readers, and the
activation snapshot is unchanged. The tagged/listed body then survives young
grace and is physically reclaimed at the mature epoch.

## Adjacent audit item

`gc2_huge_observed_scoped()` has the same broad symptom: plain registry-SMR
contention currently pins `NO_RECLAIM` before returning an overflow/retry
result. Its callers and huge-body lifetime contract need a separate focused
audit. It is intentionally not generalized through the JIT raw-sidecar helper:
an observational cdata pointer does not automatically carry the independent
local/list ownership proved above. This remains an explicit follow-up rather
than silently weakening the mandatory huge-object path.
