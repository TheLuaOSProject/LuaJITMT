# GC2 worker-token release missed-wake repair (2026-07-11)

Status: tactical scheduler repair on top of `0077fa62`. The plan files were not
edited.

Parked GC2 workers wait on `gc2.worker_wake`, but `gc2_worker_release()` clears
and wakes only `gc2.worker_active`. A worker drain returning zero does not prove
the collector is idle: it can mean that the worker lost the single owner token.
Both workers could therefore consume the last `worker_wake` publication, lose
the token, park, and miss its later release while SSB/grey/weak/sweep work
remained visible.

The repair keeps IDLE waits infinite, preserving the zero-polling idle path.
MARK, WEAK, and SWEEP waits use a 10 ms timeout as a missed-notification
backstop. This avoids advancing `worker_wake` on every token release (and the
resulting wake storm), while ensuring an active collector retries work even
when the only notification was delivered on `worker_active`. Stop and ordinary
work publications retain their immediate sequence-based wake behavior.

`test_two_worker_contention()` now publishes one counted SSB item, forces both
workers to lose `worker_active` and commit to parking on the same wake value,
then releases and wakes only `worker_active`. It requires asynchronous drain
progress and an empty SSB while proving that `worker_wake` did not change. The
fixture passes in 50 consecutive local repetitions.

This is deliberately not recorded as formal lock-freedom. A thread preempted
while holding the global worker token still obstructs the owner side of the
current grey deque; detached SSB suffixes and reserved weak ranges are not yet
helpable. Removing that dependency requires per-owner deques, published
in-flight work, and replay/help states rather than a shorter timeout.
