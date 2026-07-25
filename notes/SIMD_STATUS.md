# SIMD status

Keep this file short and current. Design rationale goes in `SIMD_DESIGN.md`.

## Where we are

* Branch: `simd`, forked from `upstream/v2.1` at `346ab587`.
* Remote: `origin` = https://github.com/TheLuaOSProject/LuaJITMT
* Upstream: `upstream` = https://github.com/LuaJIT/LuaJIT.git
* Latest pushed commit: see "Milestones" below.
* Target: x86-64, SSE2 baseline, 128-bit vectors.

## Milestones

| # | What | State |
|---|------|-------|
| M1 | Vector ctype classification, interpreter operator semantics, `ffi.simd` module, test harness | done |
| M2 | Vector IR types + constants, XLOAD/XSTORE/CNEW boxing, ExitState widening, register allocation, x64 lowering of arithmetic, recording of `+ - * /` and unary minus | done |
| M3 | Recording of `ffi.simd` (bitwise, compare, select, shifts, min/max, abs/sqrt/round, shuffles, converts, reductions) and `==` on trace | pending |
| M4 | Snapshot/sink/side-exit coverage, spill and register-pressure tests, codegen inspection tests | pending |
| M5 | FFI call/callback ABI audit and tests, benchmarks, docs | pending |

## Commands that pass

```
make -j$(nproc)                       # release build
./src/luajit test/simd/run.lua        # full suite: interp, jit and mixed modes
./src/luajit test/simd/run.lua -interp
./src/luajit test/simd/run.lua -jit
./src/luajit test/simd/run.lua -mixed  # JIT on, but pre-loaded protos off
```

The "mixed" mode (`jit.off(true, true)` before loading the test file) produces
a very different set of traces than plain `-jit` and has already caught one
backend bug that neither of the other two modes reached. Keep it.

## Known failing / pre-existing

* **Pre-existing upstream bug, not caused by this work.** In a compiled loop,
  `ffi.cast("int64_t", x) < ffi.cast("int64_t", y)` produces the wrong result;
  the comparison always takes the true branch. Reproduced on a pristine build
  of the base commit `346ab587` (worktree build) with plain `uint32_t[4]`
  arrays and no vectors involved:

  ```lua
  local a = ffi.new("uint32_t[4]", {3341494851,1,4294967295,1})
  local b = ffi.new("uint32_t[4]", {411539894,2643763630,1032702654,529748659})
  local function mn()
    local t = {}
    for i = 0, 3 do
      local x, y = ffi.cast("int64_t", a[i]), ffi.cast("int64_t", b[i])
      t[i] = (x < y) and x or y
    end
    return t
  end
  -- interpreted: 411539894 1 1032702654 1
  -- compiled:    3341494851 2643763630 4294967295 1
  ```

  The SIMD test suite avoids this pattern in its reference implementations.
  Out of scope for this work; recorded so it is not mistaken for a regression.

* LuaJIT has no in-tree test suite, so the regression baseline is: the build
  succeeds, and `test/simd` plus the `-e` smoke tests in this file behave the
  same as the pristine base commit for non-vector code.

## Files touched so far

```
src/lj_ir.h                      IRT_V16I8..IRT_V2F64, IR_KVEC, vector IR ops
src/lj_ir.c   src/lj_iropt.h     lj_ir_kvec(), 128 bit constant interning
src/lj_asm.c                     vector register class, 4-slot spills,
                                 ra_left()/ra_rematk() for IR_KVEC, dispatch
src/lj_asm_x86.h                 vector XLOAD/XSTORE (MOVUPS)
src/lj_asm_x86_vec.h             the x86-64 vector backend (new)
src/lj_emit_x86.h                emit_prefix66(), emit_loadk128(), spills
src/lj_target_x86.h              packed SSE opcodes, 128 bit ExitState.fpr
src/vm_x86.dasc src/vm_x64.dasc  save full XMM registers at a trace exit
src/lj_snap.c                    16 byte restore, sunk vector boxes
src/lj_crecord.c                 crec_vec2irt(), crec_arith_vec(), boxing
src/lj_gc.c src/lj_opt_sink.c src/lj_opt_split.c
                                 skip the two payload slots of a KVEC
src/lj_traceerr.h                LJ_TRERR_NYIVEC
src/jit/dump.lua                 vector IR type names
src/lj_ctype.h  src/lj_ctype.c   lj_ctype_vecinfo(), VecKind, CTVecInfo
src/lj_simd.h   src/lj_simd.c    interpreter reference semantics (new)
src/lj_carith.c                  carith_vec(): operators on vector cdata
src/lib_simd.c                   ffi.simd module (new)
src/lib_ffi.c                    prereg of "ffi.simd"
src/lib_jit.c   src/lj_jit.h     JIT_F_SSSE3 / JIT_F_SSE4_2 detection
src/lualib.h    src/Makefile     *build.bat   build wiring
test/simd/*                      test suite (new)
notes/*                          design/status/matrix/testing notes (new)
```

## Next concrete steps

1. Record `==` on vectors (VCMPEQ + VMOVMSK + a guard).
2. Add recorders for the `ffi.simd` functions and the matching IR lowerings:
   bitwise, min/max, compares, select, shifts, abs/sqrt/round, saturating
   arithmetic, movemask, reductions, shuffles, insert, bitcast/convert.
3. `test_codegen.lua`: assert packed instructions and no scalarisation.
4. `test_ffi_abi.lua`: vector arguments, returns and callbacks.
5. Microbenchmarks.
