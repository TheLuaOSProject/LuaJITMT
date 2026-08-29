# Root-Spine Repair Epoch

Legacy root-spine repair now uses `global_State.gcroot_repair_epoch` and
`global_State.gcroot_repaired_epoch` to avoid a full Floyd cycle scan on every
collector boundary. Root-spine cycles can only come from root publication, so
each successful publication bumps the repair epoch. The repair pass scans once
for that publication epoch and records the covered epoch afterward.

The epoch is not part of object lifetime or color semantics. It is only a cache
for an idempotent consistency repair: if another publisher advances the epoch
while a scan is running, recording the old epoch leaves the new publication
visible to the next repair call. The root links themselves remain ordered by the
existing release publication operations.

This keeps the defensive repair path available without making full collection
or mark-fixpoint setup repeatedly walk the whole legacy spine after no new
legacy-root publication has occurred.
