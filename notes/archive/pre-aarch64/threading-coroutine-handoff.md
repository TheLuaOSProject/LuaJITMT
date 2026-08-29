# Threading coroutine handoff semantics

Lua coroutine objects are movable shared values. A suspended coroutine may yield
on one OS thread and later be resumed by another OS thread, including when the
coroutine is carried directly through a `threading.channel` or indirectly inside
a `coroutine.wrap()` closure.

The runtime safety boundary is the per-`lua_State` owner word. `coroutine.resume`
and `coroutine.wrap` claim the target coroutine while moving arguments/results
and while the coroutine is running. A second OS thread racing to resume the same
currently running coroutine observes `thread busy` rather than reading or
mutating the live stack. Once the running resume yields or returns and drops the
temporary claim, any other thread may claim and resume the suspended coroutine.

A suspended coroutine state does not own a thread-group binding. Runtime resume
paths bind it to the resumer's thread group only while it is claimed and running,
then clear the binding before releasing the owner word. Generic stack claims used
by debug, status, GC, and inspection code intentionally do not migrate the
thread group because they are not running the coroutine.

Coverage:

- `tests/t-threading-coroutine.lua` verifies same-thread stock coroutine
  behavior, worker-to-worker resume handoff, worker-to-main resume handoff,
  suspended-coroutine debug APIs after a cross-thread yield,
  `coroutine.wrap()` handoff and error propagation, `pcall`/`xpcall` yields
  resumed on different threads, and racing resume contention.
- `m4_threading_coroutine` runs the behavior with the default JIT mode.
- `m4_threading_coroutine_joff` runs the same behavior under `-joff`.

Verification:

- `tools/ci/lua_test.sh m4_threading_coroutine m4_threading_coroutine_joff`
