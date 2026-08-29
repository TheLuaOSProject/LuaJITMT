## Recorder protected-call external error propagation

The fresh b1.2.0 M6 aggregate reached `m6_jit_perftools_native` and timed out
after 30 seconds. The fixture deliberately interrupts the perf-map FIFO writer
with a STOPREQ while trace publication is in a native stdio boundary.

GDB showed that the reader had observed native state, completed the handshake,
opened the FIFO, and was waiting for the main thread to finish. The main thread
was spinning in `lj_trace_ins()` with the recorder in active `LJ_TRACE_ERR`.
The protected callback itself was never entered again.

### Root cause

Internal recorder failures throw an integer `LJ_TRERR_*` object. The old
`lj_trace_ins()` loop treated every nonzero `lj_vm_cpcall()` result as that
internal protocol: it marked `LJ_TRACE_ERR` and retried the protected recorder.

STOPREQ is an external ordinary Lua error with a string object. Its unwind can
also finish recorder cleanup and clear the published `J->L` owner. The retry
therefore called `lj_vm_cpcall(NULL, ...)`, received `LUA_ERRRUN` immediately,
and marked active `LJ_TRACE_ERR` forever. The outer `lua_pcall()` never received
the shutdown error.

### Boundary

`lj_trace_ins()` now snapshots its initiating `lua_State` before entering the
protected recorder. After an error:

- a `LUA_ERRRUN` carrying a numeric object remains the internal trace-error
  retry protocol;
- every other error first cancels any still-unpublished owner state with
  `lj_trace_abort_owner()`, then rethrows the original status and object through
  the saved state.

The cleanup is between `trace_state()` invocations, not reentrant within an
active recorder callback. If the unwind already made the recorder idle and
released its token, the owner cleanup is a no-op.

### Validation

- The original exact `8cd8e348` aggregate failure remains recorded in
  `/tmp/lj-rc-jit-full-8cd8e348-m6_jit.log` (elapsed 658.67 seconds).
- A debug perftools build completes the native STOPREQ FIFO fixture.
- The exact production `m6_jit_perftools_native` case completes, including its
  clean perftools build and clean default-build restoration.
- The same fixture passes with both perftools and `LUA_USE_ASSERT` enabled;
  trace-error formatting, recursive call/unroll, and VM-event flush cases pass
  on the restored production build.
