# Table successor readiness and safe stack scans

Baseline: `81a84fba083867b72a39b249362cc2f993068434`.

During hash resize, the old slot can publish `FORWARD` before the successor
slot has received its value and before the successor becomes the table root.
The bounded node snapshot retry used to return that private successor after its
retry budget, allowing VM/global and metamethod reads to observe a transient
`nil`.

The fix keeps the bounded snapshot fallback on the published root, retries a
nil reached through an explicit forwarding hop, and makes helper-backed VM and
metamethod reads copy and validate the current generation before returning.
`lj_meta_tget()` returns copied values through per-TG scratch storage so API,
VM, nested `__index`, and FFI callers never receive a pointer to a C local.

Resize stress also exposed a transitional C frame header to GC2 stack scans.
Both mutator and worker scanners now use the existing validated frame walk.
If validation cannot prove a frame chain, they conservatively scan stack
storage and validate candidate TValue tags before following GC references.

Validation completed:

- deterministic delayed successor publication for raw hash reads, `_G.type`,
  and metatable `__index` dispatch;
- `lua_getfield`, `lua_gettable`, VM TGETS/TGETV, nested `__index`, and FFI
  table-`__index` return storage;
- x64 TGET array/hash forwarding fixtures;
- traced resize read/store/iterator stress;
- 20 consecutive metadispatch-only debug stress passes;
- nine consecutive full aggregate resize-stress passes after the conservative
  safe-walk fallback change.

A later aggregate repeat exposed an independently owned GC2 recovery
stall while WEAK-to-SWEEP drained a HugeTab recovery lane. That recovery and
activation issue is intentionally not changed here; this patch must be composed
onto its fix before the final repeated aggregate release gate.
