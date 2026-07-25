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
| M2 | Vector IR types + constants, XLOAD/XSTORE/CNEWI, ExitState widening, register allocation, x64 lowering of arithmetic, recording of `+ - * /` and unary minus | pending |
| M3 | Recording of `ffi.simd` (bitwise, compare, select, shifts, min/max, abs/sqrt/round, shuffles, converts, reductions) | pending |
| M4 | Snapshot/sink/side-exit coverage, spill and register-pressure tests, codegen inspection tests | pending |
| M5 | FFI call/callback ABI audit and tests, benchmarks, docs | pending |

## Commands that pass

```
make -j$(nproc)                       # release build
./src/luajit test/simd/run.lua        # full SIMD suite, interp + jit modes
./src/luajit test/simd/run.lua -interp
./src/luajit test/simd/run.lua -jit
```

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

1. Add `IRT_V16I8 .. IRT_V2F64` to `IRTDEF`, plus `irt_isvec()`.
2. Add the vector IR opcodes from `SIMD_DESIGN.md` D4 and `IR_KVEC` with a
   128-bit constant pool in `lj_ir.c`.
3. Widen `ExitState.fpr` to 16 bytes/register in `lj_target_x86.h` and
   `vm_x86.dasc`; update `lj_snap.c` and `lj_trace.c` readers.
4. Teach `lj_asm.c` that vector values live in FPRs and need 4 spill slots,
   and `lj_emit_x86.h` to spill/reload with MOVUPS.
5. Record vector arithmetic in `lj_crecord.c` (`crec_arith_vec`), boxing with
   `IR_CNEWI`.
6. Lower the arithmetic opcodes in `lj_asm_x86.h`.
