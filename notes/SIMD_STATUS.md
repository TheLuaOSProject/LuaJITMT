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
| M3 | Recording of `ffi.simd` (bitwise, compare, select, shifts, min/max, abs/sqrt/round, reductions, bitcast/convert), vector construction and memory round trips on trace, `==` on trace, codegen inspection tests | done |
| M4 | `ffi.simd` shuffle/insert recording, FFI call/callback ABI tests, benchmarks, user documentation | done |
| M5 | Fast-function ID budget fix; `simd.shuffle2` compiles | done |
| M6 | Vector alias analysis and self-describing VMOVMSK/VEXTRACT | done |

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

* LuaJIT has no in-tree test suite, so the regression baseline is an output
  diff of `test/simd/test_noregress.lua` against a pristine build of the base
  commit `346ab587`. It is currently **byte-for-byte identical**. See
  `SIMD_TESTING.md` for the exact commands.

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

## Traps found the hard way (do not regress these)

* The fold engine key packs the IR opcode into **7 bits**
  (`key = fins->o << 17` in `lj_opt_fold()`), so any opcode >= 128 spills into
  the neighbouring field and matches a rule that belongs to a completely
  different instruction. The vector opcodes are all above 128 and are now
  excluded from the lookup, guarded by a static assert on `IR_VSPLAT <= 128`.
* `ra_left()` special-cases constants by opcode and fell through to
  `emit_loadi()` for `IR_KVEC`, i.e. it emitted `mov r32, imm32` with an XMM
  register number. Any new constant kind must be added there as well as to
  `ra_rematk()`.
* Long emitted sequences must call `checkmclim()` in the middle: the mcode
  red zone is only 64 bytes.
* `aa_xref()` in `lj_opt_mem.c` decided ALIAS_MUST from *size plus FP-ness*.
  Two different vector types are both 16 bytes and both non-FP by that test,
  so a `float4` store forwarded into an `int4` load and `lj_opt_fwd_xload()`
  synthesised a `CONV vi4.vf4`, an instruction the backend was never designed
  to assemble. Vectors now only forward between identical types.
* Nothing in the backend may derive instruction selection from an *operand's*
  IR type: CSE and store-to-load forwarding can legitimately hand over a value
  whose lane type differs. `VMOVMSK` and `VEXTRACT` used to read
  `IR(ir->op1)->t`; they now carry the source type in their own literal.
* A box's `XSTORE` type must match the ctype it is boxed as. Masks and
  bitcast results used to be stored with the *source* lane type, so the very
  next load of that cdata could not forward and the allocation could not be
  sunk.

## Trap: LuaJIT only has 255 fast function IDs

`GCfunc.c.ffid` is a **uint8_t**. `ffi.simd` originally added 40 `LJLIB_CF`
functions, which pushed `FF__MAX` to 263. The IDs of the last seven functions
were silently truncated and collided with `FF_LUA`/`FF_C`, so
`isluafunc()`/`iscfunc()` lied about them and the recorder looked up the wrong
handler. The visible symptom was that a loop containing `simd.shuffle2` always
ended with `TRACE ... stop -> stitch`, as if the function had no recorder,
while `simd.shuffle` right next to it compiled fine.

Fixed by getting back under the limit without changing the API:

* `simd.ne`, `simd.lt` and `simd.le` are now Lua wrappers over `bnot(eq(..))`,
  `gt(b,a)` and `ge(b,a)`, which is exactly how the recorder lowered them
  anyway. They inline into a trace just like a fast function.
* `simd.isvector/lanes/elementtype/features` are plain C functions registered
  by hand. They are never recorded, so they do not need an ID.

`lj_ff.h` now carries `LJ_STATIC_ASSERT(FF__MAX <= 256)` so this can never be
reintroduced silently: the build fails instead.

## Benchmarks

`./src/luajit test/simd/bench.lua`, N=65536, 200 passes, best of 3, on this
machine:

```
saxpy (float)              scalar     5.4 ms   vector     1.4 ms    3.79x
dot product (float)        scalar     5.0 ms   vector     1.2 ms    3.97x
horizontal max (int32)     scalar     3.8 ms   vector     3.7 ms    1.02x
clamp (float)              scalar    24.7 ms   vector     1.6 ms   15.67x
```

saxpy and the dot product reach ~4x, which is the ceiling for 4 lanes. Clamp
wins much more because the scalar version is branchy and the vector version is
branchless. Horizontal max shows no gain: both versions stream 256 KB per pass
and are memory bound, not ALU bound.
