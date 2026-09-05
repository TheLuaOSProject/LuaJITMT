# ROOT review: pending-root EOF work remains unbounded

I reviewed the actual EOF call, tail/overlap traversal, repair entry, and
constructed-chain commit loop against runtime eb8a5b2f. The ordinary prune
limit does not bound its complete EOF flush. A producer can be suspended after
committing a node but before following that node's next link; the MEMBER state
does not grant a helper permission to cut that link.

The measured fixture preserves complete object identity, after-main userdata
placement, payloads, EOF/READY refusal after a nonempty flush, and later real
collection. Ten normal/ASan runs pass; every timing is one observation and is
not a throughput benchmark or latency guarantee. All 541 owner artifacts and
225 runtime/generator inputs in each of its two trees were reverified here.
The owner's 21,222 tracked-input checks include archived notes/tests and must
not be described as that many runtime C inputs.

The design remains unimplemented. A durable whole-chain continuation is the
preferred direction to investigate, but its incarnation/lifetime authority,
complete-call consumer visibility, reentrancy and exactly-once publication
remain proof obligations. A continuation that only yields at worker-claim
boundaries would still depend on a preempted claim owner. This review neither
approves a partial implementation nor treats a detached chain as a semantic
root set. Root EOF and READY must preserve every existing completion gate.
