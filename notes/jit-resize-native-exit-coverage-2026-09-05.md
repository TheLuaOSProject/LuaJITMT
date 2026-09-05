# JIT resize coverage from worker native exits

The resize store/read fixture now observes a native exit from a participating
worker after the concurrent phase starts. Its former subtraction of global
live trace counts did not establish that coverage: collection can retire
traces, a peer recorder can deny `traceinfo` snapshots, workers share
prototypes, and the store worker measured its warmup before the actual resize
loop. This is a test-evidence correction, not a demonstrated runtime JIT fix.

The first broader Linux concurrency run failed the old store trace-count
assertion. That exact failure did not reproduce in 80 fresh focused store
processes; additional separate smoke, single-CPU, and diagnostic runs also
passed. The preserved failure therefore does not identify which of the old
observer's limitations occurred in that process.

## Observer contract

`jit.attach` is universe-wide. One controller installs a stable `texit` handler
after every worker is ready and before releasing their start channels. It
prepopulates a table with those exact worker IDs. The callback records only
monotone positive witnesses for those IDs; controller execution cannot satisfy
the assertion. The controller joins every worker before detaching the handler.
The observer and its callback/cleanup are explicitly interpreted.

The VM event dispatch uses the emitting actor's TG for `th.current()`, even
though the controller created the handler closure. The worker's post-start
execution contains the target loop; channel calls do not create a stitched
continuation from the preceding warmup. A native exit therefore witnesses
execution in the concurrent worker phase. The existing value, resize, and
join assertions remain intact.

Native events can be dropped when registry/body admission, observer ownership,
or hook/profiling state prevents delivery. A positive event is useful evidence;
zero is missing evidence, not proof that native execution was absent. The
failure messages state that distinction. The fixture requires at least one
worker witness, not one trace or event from every worker.

An initial replacement attached and detached callbacks separately in each
worker. That design was rejected because those attachments replace each other
globally. Its three positive failures are preserved with its source patch;
they are not counted as passes of the final controller design.

## Frozen validation

All isolated runtime production files match
`d680421c4cb50b85437d88255bc89358c5e3a6b1`. The normal Linux executable SHA-256
is `981164c388957d9dbfc8d63665a0403a2fb76942a8e387dea9711558850e9a2a`.
No allocator/scalar pending production changes entered these runs.

The controller-design fixture SHA-256
`c6bec5eda4035acc69407f5d27c44d2e105a6b80c7716b108be8e746d2099751`
passed 80 fresh positive processes: 50 stores, 20 reads, five complete
store/read/iterator cases, and five single-CPU store/read cases. Four negative
controls failed the intended coverage assertion: store/read with global JIT
off, and store/read with only the worker prototype disabled while the
controller's JIT remained enabled. There were no timeout successes.

The final source adds only the event-loss comment and clearer error wording;
its SHA-256 is
`d42f9e5e618c8cb225ee76f61f2296eb5c447cef0c5de94619231122c285feff`.
One final complete positive case and all four final negative controls behaved
as intended. The measured controller design also passed the later combined
arena/scalar Linux validation; that integrated fixture precedes only this
comment/error-wording change.

Evidence is in `notes/evidence/jit-resize-native-exit-coverage-2026-09-05/`:
original failure, unsuccessful reproduction attempts, both observer revisions,
negative controls, every raw result, fixture identities, and replay patches.
The original runtime and executable artifacts remain under the absolute `/tmp`
paths in those records. These checks do not establish general trace lifecycle
progress or release readiness.
