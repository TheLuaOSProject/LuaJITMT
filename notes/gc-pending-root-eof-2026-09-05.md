# Pending-root flush exceeds the ordinary prune budget

One real end-of-list operation flushes all 262,144 pending objects while
holding its worker claim. Normal observations take about 5–6 ms. Tail search,
overlap validation and global-list repair all traverse complete chains, so the
ordinary 256-entry prune limit does not bound this work.

Ten normal/ASan controls on runtime `eb8a5b2f` verify every object identity,
userdata placement, payloads and later collection. These timings are individual
observations, not a latency distribution.

A durable continuation for whole chains is the preferred design direction.
Cutting a prefix is unsafe while a constructor can still follow its links.
Before implementation, the continuation needs proven lifetime, ownership and
visibility to every existing complete-flush caller. Yielding between claims
alone would still leave a dependency on a suspended claim owner.

See the [root review](evidence/gc-pending-root-eof-2026-09-05/root/review.md),
[measurements and handoff](evidence/gc-pending-root-eof-2026-09-05/owner/HANDOFF.md),
[source proposal](evidence/gc-pending-root-eof-2026-09-05/owner/PROPOSAL.md)
and [artifact manifest](evidence/gc-pending-root-eof-2026-09-05/manifest.json).
This is a measured open issue; no runtime change is included.
