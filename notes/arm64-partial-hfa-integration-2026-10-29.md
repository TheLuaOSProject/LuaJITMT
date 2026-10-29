# Partial-bank HFA integration

The integration at `19b39379c` combines partial-bank HFA stack groups and the
required fixed-call emission plan with the validated stack/tail baseline.
The first full gate passed GC2 and stopped at the strict emitter compilation:
the new spill extent compared unsigned MSize with signed evenspill. Commit
`421172380` uses a bounded int32_t extent and the gate was restarted.

The restarted gate reached the first-side production lifecycle test, where
the append/flush coordinator timed out before the worker had observed the
publication pause (`attached=1`, `pause=0`, `entering=0`). The coordinator was
created before the main thread started recording; it immediately spent its
request-poll budget, then called the pause-release hook on timeout. That could
release the pause before the publisher arrived and leave the worker unable
to observe it. This was a fixture ordering error.

The coordinator now waits for the worker's entering-handshake flag, which is
published only after it observes the actual side-publication pause. Request
publication, exact epoch/ack, token ownership, retirement and interpreter
result assertions remain intact. No production handshake code changed.
The full focused first-side contract passed with its default two runs per
helper profile, covering ARM64 and ARM64e gc-claim/scoped/full-flush modes and
ordinary ARM64 smoke. The wider gate continuation and ordinary comparison
remain pending.

Evidence: `/Users/frityet/Scratch/ljmt-partial-hfa-integrated-20261029/`.
`initial-full-gate.log` is the strict warning failure; `full-gate.log` is the
later append/flush fixture failure. `append-readiness-first-side.log` records
the successful focused run. The `append-failure/` directory preserves the
runtime/archive and relevant objects remaining after the failed run; the
temporary fixture itself had already been removed by its script.
