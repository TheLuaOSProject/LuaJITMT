# Foreground SWEEP scheduling contract

The preserved sole-main JIT failure is a service-rate problem after the SWEEP
bridge has completed. Fifty hard exits each consume 64 SSB slots, then renew
the native lease; roughly another 512 KiB of allocation precedes the next
handoff. Recovery reaches 258,143 independently verified identities. Existing
physical-reclamation guards correctly retain that work.

The source review identifies a possible cycle-tagged scheduling request, with
new publication restricted to a TG actually executing a trace. Its current
JIT intent prevents an owned, closed-gate acknowledgement or cycle completion
from passing the delayed publisher. A worker could acknowledge under that
same claim and generation. The request would own scheduling intent only;
existing queues retain all semantic identities. Adding an interpreter or
worker-control publisher would require a broader publication proof.

The missing contract is what committed service earns another native allocation
turn. Current positive drain returns mix SSB transfers, attempted recovery,
suffix handshakes, grace and arena work. They cannot serve as completed-rescue
receipts. A lease or failed claim must end the automatic invocation with unpaid
work retained. An exact owned result must separate refusal, committed graph
work, other progress and genuine cycle completion. Stores can create work
without new allocation, so a fixed byte allowance has no universal conversion
to a count of completed graph jobs.

STOP/FINPAUSE, threshold stores and advertised worker count cannot erase that
obligation. Worker count is published before pthread startup and can remain
positive during stop or failed join. Snapshot restoration, TEXIT and JIT-intent
release must still precede collection. An early inference that TNEW repeats
its GC predicate was corrected: its return label follows the predicate and
continues to allocate. No retry-loop counterexample was established.

No runtime patch, build or test was performed for this design checkpoint.
The native-turn/credit rule and exact leaf receipt propagation remain open.
Keeping JIT entry closed for the entire remaining cycle is a concrete policy
alternative, but can withhold native execution behind a retained owner; it
must not be introduced as a harmless threshold optimization. The synchronous
handshake and whole-chain EOF limits also remain.

All 231 owner artifacts and 225 accepted runtime inputs are verified. The
source copy contains worker+fair GC code; the later scalar-next commit changes
only lj_tab.c/h and is recorded separately. The earlier runtime diagnosis and
its original bounds remain the acceptance evidence, without being recast as
new tests of this design.

See the [detailed design](evidence/gc-jit-foreground-design-2026-09-05/owner/DESIGN.md),
[handoff](evidence/gc-jit-foreground-design-2026-09-05/owner/HANDOFF.md),
[root verification](evidence/gc-jit-foreground-design-2026-09-05/root/verification.json),
and [archive manifest](evidence/gc-jit-foreground-design-2026-09-05/manifest.json).
The [original diagnosis](evidence/gc-worker-sweep-2026-09-05/jit-diagnosis/HANDOFF.md)
and [runtime matrix](evidence/gc-worker-sweep-2026-09-05/jit-matrix/HANDOFF.md)
remain unchanged.
