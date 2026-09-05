The smallest dependency-closed fixture set for `m3_gc2_auto_control` is ten
new source/input files plus the existing unchanged M8 spawn-live Lua test.
`canonical-gate-proposal.json` gives every file hash, compiler/link argv,
environment, 37 GCC runtime argv arrays and per-case timeouts. Clang omits the
two exact GCC delayed-store RESTART schedules, giving 35 cases. No shared
tests or suite were edited.

The six C entrypoints and wrappers are:

| File | Link wrapper | Runtime arguments | Outer bound |
| --- | --- | --- | --- |
| t-stop-first-attach.c | lj_vm_cpcall | none | 25s |
| t-restart-first-attach.c | lj_vm_cpcall | none; GCC runtime only | 25s |
| t-restart-last-detach.c | none | none; GCC runtime only | 25s |
| t-auto-restart-numeric-max.c | none | none | 25s |
| t-auto-finalizer-controls-v3.c | none | all 16 peer/inner-error/outer-error/outer-STOP modes, then 1 | 25s each |
| t-auto-controls-v2.c | lj_native_enter | mode, boundary, peer, workers, peer Lua path, boundary Lua path | 50s each |

The control C entrypoint includes `t-string-retention.c` after renaming its
main function. Keep that required input beside it; do not compile both as
separate translation units. Its other required inputs are `peer-control.lua`
and `control-boundaries.lua`. The tenth new input is
`t-finalizer-spawn-query-enabled.lua`. Run that and the existing unchanged
`t-m8-finalizer-spawn-live.lua` with plain luajit and with `-joff`, using a
40-second outer bound. These are four Lua runtimes. Avoid factoring the
validated source bytes during registration.

The 13 worker-zero control argument prefixes are:

```
0 0 1 0
0 1 1 0
0 2 1 0
0 3 1 0
0 4 1 0
0 5 1 0
1 0 0 0
1 0 1 0
2 0 1 0
3 0 0 0
3 0 1 0
4 0 0 0
4 0 1 0
```

Append absolute resolved paths to the two Lua inputs. Boundary numbers mean
TNEW, TDUP, string.char, string.rep, C numeric conversion and FNEW, respectively.
Modes cover direct admission, STOP/restart, last/first child, actual native
entry/return and active STOP. The older t-auto-controls.c, standalone string
matrix and sequential STOP-overlap-restart-v2 fixture remain evidence rather
than necessary duplicate gate entrypoints. All six boundary kinds passed
candidate2; candidate3's selected matrix reruns the STOP/attachment/native/
active controls. Run the complete proposed set against final integration.

Use matching runtime headers/archive and `-std=gnu11 -O2 -g -Wall -Wextra
-Werror`, `-Wl,-E -lm -ldl -pthread`, plus only the specified wrappers. Strict
adds `-DLUA_USE_ASSERT -DLUA_USE_APICHECK` to both fixture and runtime builds.
ASan uses clang and adds `-fsanitize=address -fno-omit-frame-pointer` to fixture
compilation with the recorded ASan runtime. No fixture requires test-helper
macros. The JSON flags placeholder expands to zero, two or four argv tokens.

Set `LUA_PATH=${SRC}/?.lua;${TESTS}/lib/?.lua;;` and `RETENTION_JIT=0`; automatic
control C setup also explicitly disables the engine. ASan additionally sets
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`. Resolve the JSON path variables
as argv elements, not literal shell text.

No proposed fixture bytes contain `/tmp/`, `/workspaces/` or `/home/` paths.
Evidence runners and saved command logs resolve package paths. The exact
RESTART fixtures inspect emitted GCC x64 instructions/registers and fail
boundedly if the verified schedule is not observed; do not assume every
compiler emits them. ASan retains the separately verified first-live STOP,
numeric MAX and sequential paths.

Every canonical case must exit zero. Keep known worker-two exit-2 SWEEP
completion results as separate evidence, never expected passing gate results.
Registration does not establish nonblocking progress or concurrent string
reclamation. The parent owns canonical integration and the final source run.
