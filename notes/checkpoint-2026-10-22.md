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

## Scalar integration and recovered translation

Scalar production source `6182f769` plus its fixtures passed both 509-case stock
runs on native ARM64 at frozen `82320b43`: ordinary source and genuine original
v2 bytecode through the FIFO compiler harness. Broader integration exposed
stale shell source checks after splitting the validators and admitting adjacent
numeric shapes. `1355cd2d`, `57ae11b2`, and `cc3c8d0a` update those checks;
production arithmetic and FORL behavior were not changed by these repairs.
The numeric-argument contract retains all 37 modes and 222 default native
ARM64/arm64e executions, including its ABI, emitted-code and lifecycle checks.

Later on 2026-10-22, the previously stuck translated processes were no longer
present. No system-service action or reboot was performed by this task. A fresh
x86-64 build of `57ae11b2` executed successfully. Both 509-case stock runs then
passed under Rosetta, and the full FNEW fixture passed in both default and
`LJ_TEST_TRACED_FNEW=1` modes. The CNEW return-boundary, pending-capture scan and
SSB-preserve fixtures passed too. The callback-result and OS-error unwind
contracts were rerun with their default cross-architecture branches intact and
passed on ARM64, arm64e/BTI and x86-64.

This recovery also exposed a real shared-JIT conformance failure in the larger
legacy-bytecode fixture. An `ipairs` loop which stores closures capturing its
key/value omits a closure after native compilation. The exact source program
fails too; interpreter mode and original LuaJIT pass. A reduced 14-line source
case reports a missing closure at key 4 with hotloop/hotexit set to 1. This
failure is under investigation and prevents claiming full closure/import JIT
compatibility despite the passing stock corpus.

Temporary evidence: `/tmp/luajitmt-scalar-stock-{source,import}.log`,
`/tmp/luajitmt-x64-stock-{source,import}.log`,
`/tmp/luajitmt-x64-fnew{,-traced}.log`,
`/tmp/luajitmt-x64-capture-fixtures.log`,
`/tmp/luajitmt-scalar-full-{callback,unwind}.log`, and
`/tmp/luajitmt-generic-cell-repro{,-dump}.log`. The x86-64 checkout path is in
`/tmp/luajitmt-x64-recheck-workdir`; it is now used for isolated diagnosis, so
its current contents must not be assumed to remain frozen at that commit.

The remaining native scalar integration contracts also completed as separate
diagnostic runs. These cover exits and trace retirement, root entry, live
flush/reuse, recorder and VM safepoints, and source upvalue identity. The root
entry fixture needed `1685aaa5`: its synthetic trace now includes the initial
guard snapshot and loop XPOLL required by the independent scalar capability.
The repair retains the existing metadata mutations and adds missing guard,
snapshot-slot and poll negative controls. Its complete contract passes ARM64
and arm64e. The diagnostic tail passes both targets as well.

These component results are not a new complete umbrella-gate result. A fresh
default gate remains necessary after the shared capture fixes and numeric spill
work land. Temporary outputs are
`/tmp/luajitmt-scalar-checkpoint-final-contracts.log`,
`/tmp/lj-root-entry-capability-contract.log`, and
`/tmp/luajitmt-scalar-checkpoint-last-contracts.log`; the first records the
obsolete synthetic root fixture failure before its independent repaired rerun.

## Shared-capture repair and complete bootstrap checkpoint

`25275a13` fixes the captured-iterator, imported repeat/conditional and
immutable-capture snapshot failures described above. All 509 source and 509
imported stock cases, the complete original-bytecode C fixture, and FNEW pass
under Rosetta. Source/v2 little-endian/v2 big-endian capture-loop tests require
actual x64 native exits and pass with default optimization, `-O0` and `-O-cse`.
Details and the four distinct causes are in
[`jit-cell-loop-conformance-2026-10-22.md`](jit-cell-loop-conformance-2026-10-22.md).

The unchanged default ARM64 bootstrap gate completed on frozen `25275a13`.
No Rosetta or other runtime branch was omitted: the required x64 unwind test
ran alongside ARM64 and arm64e. The new source/import loop Lua fixture also
passes directly on the resulting assertion interpreter build; `e6356fd6`
adds that invocation to future default bootstrap runs. The experimental
umbrella gate has not yet been rerun at this checkpoint.

Temporary evidence: `/tmp/luajitmt-capture-checkpoint-bootstrap.log`, with the
frozen checkout path in `/tmp/luajitmt-capture-checkpoint-workdir`.
