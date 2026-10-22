# Native checkpoint and Rosetta validation interruption

The safe ARM64 bootstrap gate passed on frozen `857c44cc`, followed by the
standalone C `lua_setlocal` regression. The experimental gate on the same
checkout passed the native ARM64 and arm64e callback-result tests, then stopped
making progress in the x86-64 callback-result executable. The complete gate
therefore has **no passing result** at this checkpoint. The previous complete
bootstrap and experimental gate result remains frozen `709583ee`.

Observed process state on 2026-10-22:

- PID 72620 was in uninterruptible `U` state for over 12 minutes in
  `t-ffi-callback-result-x86_64`.
- Other isolated translated LuaJIT processes, including PID 60080, were also
  stuck in `U` state after earlier debugger work. Native ARM64 test processes
  continued to execute. A simple translated `/usr/bin/true` also completed.
- SIGKILL terminated the gate's shell wrappers but left the callback child
  pending in `U`, reparented to PID 1. No system service was stopped and no
  restart was attempted. This observation does not establish the underlying
  cause or prove a LuaJIT regression.

The frozen checkout is `/tmp/luajitmt-checkpoint-20261022.MeMQpT`; output is in
`/tmp/luajitmt-checkpoint-bootstrap.log` and
`/tmp/luajitmt-checkpoint-jit.log`. These are temporary local evidence paths.

The first subsequent native-only diagnostic used the callback contract's
existing `LJ_ARM64_CALLBACK_RESULT_SOURCE_ONLY=1` option. Later frozen-checkout
diagnostics omit only the x86-64 execution branch and retain both native
callback executions. All of these runs are partial cross-architecture
validation. The default gate and its required x86-64 execution are
unchanged. Recent source-upvalue tests had already passed native ARM64 and
Rosetta before these processes stalled; detailed results are in the related
source-upvalue and recursive-return notes.

The partial experimental gate completed successfully on frozen `dde9ecdb` in
`/tmp/luajitmt-native-checkpoint-20261022.Mkq7iD`, with output in
`/tmp/luajitmt-native-checkpoint-jit.log`. It includes the CNEW return-boundary
recorder test and all remaining gate contracts; callback runtime execution was
explicitly source-only in this run. It does not certify subsequent scalar-loop
or legacy-import changes.

## Imported-bytecode bootstrap checkpoint

The native-only bootstrap diagnostic passed on frozen `01b8c560`, including the
new genuine original-bytecode C fixture, all 387 applicable stock cases,
coroutines, FFI/callbacks, GC2 hard checks, and ARM64/arm64e OS-error unwinding.
The x86-64 unwind branch alone was omitted in the disposable checkout because
of the translated-process stall above. This is not a complete default gate.

The checkout path is recorded in `/tmp/luajitmt-legacy-checkpoint-workdir` and
its output in `/tmp/luajitmt-legacy-checkpoint-bootstrap.log`. The diagnostic
script `/tmp/luajitmt-native-diagnostic-gates.py` preserves a patch describing
its omissions inside each frozen checkout. Committed default gates retain all
cross-architecture requirements.
