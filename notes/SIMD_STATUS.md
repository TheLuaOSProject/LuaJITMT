# SIMD status

Keep this file short and current. Design rationale goes in `SIMD_DESIGN.md`.

## Where we are

* Branch: `simd`, forked from `upstream/v2.1` at `346ab587`.
* Remote: `origin` = https://github.com/TheLuaOSProject/LuaJITMT
* Upstream: `upstream` = https://github.com/LuaJIT/LuaJIT.git
* **This branch is based on pristine upstream LuaJIT, not on `origin/v2.1`.**
  The two share only pre-fork history. See `SIMD_DESIGN.md` D13 for what that
  means; it is the first thing to revisit if this work is meant to land in
  the fork rather than stand alone.
* Latest pushed commit: see "Milestones" below.
* Target: **x86-64-v3** (SSE4.2 + AVX2 + BMI2), 128-bit vectors.
  Feature use is runtime detected, so the binary still runs on older CPUs.

## Milestones

| # | What | State |
|---|------|-------|
| M1 | Vector ctype classification, interpreter operator semantics, `ffi.simd` module, test harness | done |
| M2 | Vector IR types + constants, XLOAD/XSTORE/CNEW boxing, ExitState widening, register allocation, x64 lowering of arithmetic, recording of `+ - * /` and unary minus | done |
| M3 | Recording of `ffi.simd` (bitwise, compare, select, shifts, min/max, abs/sqrt/round, reductions, bitcast/convert), vector construction and memory round trips on trace, `==` on trace, codegen inspection tests | done |
| M4 | `ffi.simd` shuffle/insert recording, FFI call/callback ABI tests, benchmarks, user documentation | done |
| M5 | Fast-function ID budget fix; `simd.shuffle2` compiles | done |
| M6 | Vector alias analysis and self-describing VMOVMSK/VEXTRACT | done |
| M7 | x86-64-v3: AVX/AVX2 detection, VEX three operand encoding, 64-bit lane min/max | done |
| M8 | Variable lane index, scalar cdata operands, mask predicate guard polarity | done |
| M9 | Randomized program generator; snapshot replay, type-blind CSE and 64-bit `sar` fixes | done |
| M10 | NaN quieting in rounding, defined float-to-integer conversion, seed sweep | done |
| M11 | ASan and UBSan clean; integer-promotion UB removed from the reference implementation | done |
| M12 | Variable shift counts on 8-bit lanes and on 64-bit `sar` compile to packed code | done |
| M13 | IR consistency check and `jit.dump` handle vector types and 128-bit constants | done |
| M14 | Vector arguments and results by value in FFI callbacks (x86-64 SysV) | done |
| M15 | Vector store-to-load forwarding no longer synthesises a CONV between vector types | done |

## Commands that pass

```
make -j$(nproc)                       # release build (x86-64-v3 target)
./src/luajit test/simd/run.lua        # full suite: interp, jit and mixed modes
./src/luajit test/simd/run.lua -interp
./src/luajit test/simd/run.lua -jit
./src/luajit test/simd/run.lua -mixed  # JIT on, but pre-loaded protos off
./src/luajit test/simd/bench.lua       # microbenchmarks
for s in 1 7 999 31337; do SIMD_SEED=$s ./src/luajit test/simd/run.lua; done
make clean && make -j$(nproc) XCFLAGS=-DLUAJIT_DISABLE_JIT   # also passes
make -j$(nproc) CCDEBUG=-g XCFLAGS=-DLUA_USE_ASSERT          # also passes
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

* **Pre-existing upstream difference, not caused by this work.**
  `tonumber(u)` for a `uint64_t` at or above 2^63 can differ by one ulp
  between the interpreter and the JIT, because the JIT converts as signed and
  then adds 2^64, which rounds twice. Reproduced on a pristine build of
  `346ab587` with no vectors involved:

  ```lua
  local v = ffi.new("uint64_t[1]")
  v[0] = 58ULL * 0x40d0f21a8ce41712ULL
  -- interpreted 0x1.5eadb407d75a7p+63, compiled 0x1.5eadb407d75a8p+63
  ```

  `test_jit.lua` accumulates 64-bit lane values exactly instead of through
  doubles so the suite does not depend on it.

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
src/lj_opt_mem.c                 vector alias analysis; vectors forward as a
                                 reinterpretation instead of a CONV
src/lj_ccallback.c               vector args/results in callbacks (SysV)
src/lj_ccall.h  src/lj_ctype.h   16 byte FPRCBArg, tied by a static assert
src/vm_x64.dasc                  save/restore full XMM in the callback trampoline
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
* `snap_replay_const()` has its own switch over constant opcodes, separate
  from `ra_rematk()`, `lj_ir_kvalue()` and `snap_restoredata()`. It silently
  returned `TREF_NIL` for `IR_KVEC`, so a side trace that replayed a sunk
  vector box holding a constant got garbage. Any new constant kind has to be
  added to *all four*.
* `src/jit/dump.lua` keeps **three** parallel tables keyed by IR type:
  `irtype_text`, `colortype_ansi` and the `irt_*` CSS classes in
  `header_html`. Adding a type name to the first one only is not enough:
  `colorize_ansi()` does `format(colortype_ansi[t], s)`, so a missing entry
  raises inside the dump handler and the IR dump simply stops at the first
  instruction of that type, with no error shown. `formatk()` needs a case too
  -- a 128-bit constant arrives as a hex string and was being cut to 20
  characters by the generic string formatting. `test_codegen.lua` now dumps a
  vector trace in all three colour modes and checks the constant is 32 hex
  digits.
* A 128-bit constant occupies **three** IR slots, and every loop that walks the
  constant range from `nk` has to skip the two payload slots or it will decode
  them as instructions. `gc_traverse_trace()`, `lj_opt_sink()` and the
  constant loop in `lj_asm.c` do this with `irt_isvec()`;
  `rec_check_ir()` in `lj_record.c` did not, so the assert build tripped
  "IRMref op2 out of range" on a payload word. It stayed hidden until a new
  constant happened to contain bytes that decode as an opcode with reference
  operands -- the check only fails for *some* payloads. `lj_opt_split.c` has
  the same shape of loop but is only built for 32-bit and soft-float targets,
  which never see vector IR.
* `lj_opt_cse()` matches on opcode and operands but **not on type**. That is
  fine for scalars, where a type pun gives the same bits, and wrong for
  vectors: `VCMPEQ` is PCMPEQB at V16I8 and PCMPEQD at V4I32. Vector opcodes
  now use a type-aware CSE in `lj_opt_fold()`.
* A test that passes a *variable* where the recorder needs a constant silently
  tests nothing: the trace aborts and the interpreter answers. The 64-bit
  arithmetic shift emulation was wrong for every input and the shift tests
  still passed, because they used a loop variable as the count.
* A fast function whose result is recorded as a **guard** must leave that
  result in `G(L)->tmptv2`. `LJ_POST_FIXGUARD` reads it to decide whether to
  flip the guard, so without it the compiled code can answer the opposite of
  the interpreter. `lj_carith_op()` does this for the operators;
  `simd.allof`/`simd.anyof` did not, and only failed on some random seeds.
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
* The consequence of the rule above is that an `XSTORE`'s type is routinely
  **not** the type of the value it stores. `lj_opt_fwd_xload()` reacts to that
  mismatch by synthesising an `IR_CONV`, and there is no CONV between two
  vector types: `asm_conv()` takes the integer path and allocates a **GPR**
  for a value that lives in an XMM register. Vectors now forward as a pure
  reinterpretation instead (`SIMD_DESIGN.md` D15). Symptom in an assert build
  was `emit_loadk128: vector constant needs an FP register`, appearing in
  roughly 1 run in 300 of `test_lib` and never twice with the same seed --
  see the testing notes on why the seed did not pin it down.
* `SIMD_SEED` must actually determine the whole run. `test_arith.lua` iterated
  its operator table with `pairs()`, and with `LUAJIT_SECURITY_STRHASH` the
  string hash seed differs on every process, so the order the operators were
  recorded in -- and therefore the shape of the traces -- changed run to run.
  A rare backend bug found that way could not be replayed. The permutation is
  now derived from a separate seeded RNG stream, which keeps the coverage and
  restores reproducibility.

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

## Remaining deliberate limitations

Each of these is a decision, not an unfinished edge. None of them can give a
wrong answer: where the JIT has no lowering the trace aborts with
`LJ_TRERR_NYIVEC` and the interpreter produces the same value.

1. **128-bit only in the JIT.** Other widths keep working interpreted.
   256-bit is blocked by the IR type encoding, not by AVX2; see
   `SIMD_DESIGN.md` D9 for the full list of what it would take.
2. **Non-constant lane index in `shuffle`/`shuffle2`.** Rejected at record
   time. A runtime permutation would need a `PSHUFB` control mask assembled
   from N separate variable indices, which costs more than it saves.
   `insert` *does* support a variable index.
3. **Vectors by value in FFI callbacks are x86-64 SysV only.** Supported for 8
   and 16 byte vectors there (see `SIMD_DESIGN.md` D14). On Windows x64, x86
   and non-x86 targets, and for any vector wider than one register, `ffi.cast`
   still rejects the callback with the ordinary "cannot convert" error; pass a
   pointer instead.
4. **No FMA.** `a*b+c` stays a multiply and an add. FMA rounds once where the
   interpreter rounds twice, and interpreter/JIT agreement is worth more here
   than the throughput.
5. **This branch does not sit on `origin/v2.1`** -- see D13 above.

## Benchmarks


`./src/luajit test/simd/bench.lua`, N=65536, 200 passes, best of 3, on this
machine:

```
saxpy (float)              scalar     6.8 ms   vector     1.8 ms    3.72x
dot product (float)        scalar     6.1 ms   vector     1.5 ms    3.99x
horizontal max (int32)     scalar     4.7 ms   vector     4.0 ms    1.16x
clamp (float)              scalar    29.1 ms   vector     1.9 ms   15.07x
```

saxpy and the dot product reach ~4x, which is the ceiling for 4 lanes. Clamp
wins much more because the scalar version is branchy and the vector version is
branchless. Horizontal max shows no gain: both versions stream 256 KB per pass
and are memory bound, not ALU bound.
