# Cell frame addresses across recursive returns

The current-frame cell helpers introduced in `b3b6b850` form an address with
`ADD BASE <byte offset>`. `IR_RETF` changes `BASE` when a trace returns below its
entry frame. The existing optimizer guarded `SUB` and `EQ` expressions involving
`BASE` against CSE across that boundary, but not these new `ADD` expressions.

The standalone `tests/t-jit-cell-retf.lua` regression recursively sums captures
made while unwinding. Before the fix an assertion-enabled x86-64 GC64 build on
Rosetta returns **5 instead of 90** with default optimizations. `-O-cse`, `-O0`,
and the installed stock LuaJIT all return 90. Its IR dump shows `ADD BASE +32`
at trace 5 instruction 8 reused by closure helper calls after `RETF` instructions
23 and 40, and after further returns in the same trace.

Adding `ADD BASE KINT` and `ADD BASE KINT64` to the existing `fold_base` rule uses
the same `IR_RETF` CSE boundary. The isolated Rosetta build passed this semantic
case with default optimizations, `-O-cse`, and `-O0`. Stock LuaJIT passed the same
three modes. The permanent fixture also preserves an explicitly disabled JIT.
ARM64 currently rejects native `RETF` traces, so an ARM64 interpreter pass alone
does not validate this optimizer path.

Validation build flags were `TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64'`
and `XCFLAGS='-DLUAJIT_ENABLE_GC64 -DLUA_USE_ASSERT -DLJ_FUNC_TEST_HELPERS'`, with
`MACOSX_DEPLOYMENT_TARGET=13.0`. Run the regression from the repository root:

```sh
LUA_PATH='./src/?.lua;;' src/luajit tests/t-jit-cell-retf.lua
LUA_PATH='./src/?.lua;;' src/luajit -O-cse tests/t-jit-cell-retf.lua
LUA_PATH='./src/?.lua;;' src/luajit -O0 tests/t-jit-cell-retf.lua
```

A separate review concern remains in `rec_celluv_cnewref`: its search for an
earlier cell allocation does not stop at `IR_RETF`, although structural frame
offset equality cannot establish identity across that boundary. This note does
not claim a separate runtime reproduction or fix for that lookup. Further
Rosetta validation stopped when the host stopped executing x86-64 processes
following a separate debugger attempt; the completed three-mode evidence above
predates that host problem.
