# Recursive call-unroll guard window

The `m6_jit_recursive_call_unroll` test intermittently failed while the runtime
had actually recorded and stabilized an up-recursion trace. One failing run had
the first up-recursion trace at 22, but the guard only scanned traces 2..20.
Another run recorded 42 traces after the first `fib(30)`, while the guard had a
fixed first-run ceiling of 40.

Stock LuaJIT shows the same trace-number variability for this `fib(30)` shape;
in a 500-run sample, the first-run trace count ranged from 17 through 64 and
the first up-recursion trace ranged from 2 through 46. This fork showed a
similar 500-run range of 19 through 68, with the second `fib(30)` adding only
2 through 4 traces.

The guard now uses a 160-trace observation window, requires an up-recursion
trace somewhere in that window, rejects saturation of the window, and keeps the
existing check that the second run must not continue re-recording.

Verification:

- `/usr/bin/luajit` 500-run comparison of the exact fib shape
- `src/luajit` 500-run comparison of first/second trace counts
