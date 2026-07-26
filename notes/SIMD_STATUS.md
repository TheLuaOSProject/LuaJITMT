# SIMD status

Keep this file short and current. Design rationale goes in `SIMD_DESIGN.md`.

## Where we are

* Branch: `simd`, forked from `upstream/v2.1` at `346ab587`, merged up to
  `a471ab78` (2026-07-26).
* Remote: `origin` = https://github.com/TheLuaOSProject/LuaJITMT
* Upstream: `upstream` = https://github.com/LuaJIT/LuaJIT.git
* **This branch is based on pristine upstream LuaJIT, not on `origin/v2.1`.**
  The two share only pre-fork history. See `SIMD_DESIGN.md` D13 for what that
  means; it is the first thing to revisit if this work is meant to land in
  the fork rather than stand alone.
* Latest pushed commit: see "Milestones" below.
* Target: **x86-64-v3** (SSE4.2 + AVX2 + BMI2), with complete 128-bit
  lowering and a full supported 256-bit YMM operation surface.
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
| M16 | `simd.shuffle` permutes by a runtime index vector (PSHUFB) | done |
| M17 | `simd.shl/shr/sar` accept a per-lane count vector (AVX2 VPSLLV/VPSRLV/VPSRAV) | done |
| M18 | `simd.fma` with a single rounding (VFMADD213PS/PD, runtime detected) | done |
| M19 | Kernel and per-operation benchmark suite (`test/simd/bench_ops.lua`) | done |
| M20 | Merge `upstream/v2.1` to `a471ab78`; both documented upstream bugs fixed | done |
| M21 | FMA form selection (132/213/231) so no register copy is needed | done |
| M22 | `simd.mulhi`, the high half of the lane product (PMULHW/PMULHUW) | done |
| M23 | Move transient IR marks to a scratch bitset, reserving type bit `0x20` for 256-bit width | done |
| M24 | 256-bit IR/KVEC values, YMM loads/stores/spills/exits, and AVX2 add/sub/direct-mul/div/logical lowering | done |
| M25 | Benchmarks compare scalar, 128-bit XMM, and 256-bit AVX2/YMM execution | done |
| M26 | 256-bit broadcasts, comparisons/equality, min/max, shifts, FMA, rounding, conversions, saturating arithmetic, movemask, mulhi, and 8/64-bit multiply emulations | done |
| M27 | 256-bit reductions, constant/runtime shuffles, two-source shuffles, and constant/runtime insert via explicit 128-bit half crossing | done |
| M28 | Cost-selective AVX2 `VPERMD`/`VPERMQ` lowering for 32/64-bit YMM shuffles; v2, AVX-only, and AVX2 CPU-model test matrix | done |
| M29 | Call-free packed 8- and 32-bit `mulhi` emulations for XMM and YMM | done |
| M30 | Production-sized scalar/XMM/YMM benchmarks: 1080p Gaussian blur, 64-tap audio FIR, particle simulation, and ChaCha20 | done |
| M31 | Per-lane AVX2 shifts for 8/16-bit XMM and YMM vectors via packed dword decomposition | done |
| M32 | Signed and unsigned 64-bit `mulhi` via four packed 32x32 partial products, XMM and YMM | done |
| M33 | Exact call-free `u32 -> float`, `i64/u64 -> double`, and `double -> i64/u64` lowering for XMM/YMM | done |
| M34 | Every equal-lane numeric conversion between native 16/32-byte vectors; VEX-clean mixed XMM/YMM moves | done |
| M35 | Generic Lua scalar FP code is VEX-128-clean on AVX, eliminating scalar/YMM transition stalls | done |
| M36 | Central call-site YMM preservation, including GC/indirect calls; Windows x64 upper-lane runtime correctness | done |
| M37 | Exact inline constant integer modulo, eliminating helper calls and YMM spill/reload traffic in mixed loops | done |
| M38 | Byte-aligned integer rotate idioms collapse to one packed shuffle; constant shuffle masks use memory only under register pressure | done |
| M39 | Fuse one-use vector loads into AVX arithmetic memory operands, while retaining safe separate loads for legacy SSE | done |
| M40 | Let ordinary FFI array temporaries fuse into AVX unary, shuffle and conversion memory operands; ignore only virtual sink stores | done |

## Commands that pass

```
make -j$(nproc)                       # release build (x86-64-v3 target)
./src/luajit test/simd/run.lua        # full suite: interp, jit and mixed modes
./src/luajit test/simd/run.lua -interp
./src/luajit test/simd/run.lua -jit
./src/luajit test/simd/run.lua -mixed  # JIT on, but pre-loaded protos off
./src/luajit test/simd/bench.lua       # scalar/XMM/YMM kernel benchmarks
./src/luajit test/simd/bench_ops.lua   # kernels and XMM/YMM operation costs
for s in 1 7 999 31337; do SIMD_SEED=$s ./src/luajit test/simd/run.lua; done
make clean && make -j$(nproc) XCFLAGS=-DLUAJIT_DISABLE_JIT   # also passes
make -j$(nproc) CCDEBUG=-g XCFLAGS=-DLUA_USE_ASSERT          # also passes
make clean && make -j$(nproc) HOST_CC=gcc \
  CROSS=x86_64-w64-mingw32- TARGET_SYS=Windows               # cross-builds
WINEDEBUG=-all wine src/luajit.exe test/simd/run.lua \
  -jit test_codegen                                           # passes

# Every file and mode also passes under these QEMU CPU models:
# Nehalem-v1 (x86-64-v2/no AVX), SandyBridge-v1 (AVX/no AVX2),
# and Haswell-v2 (AVX2/FMA).
```

The "mixed" mode (`jit.off(true, true)` before loading the test file) produces
a very different set of traces than plain `-jit` and has already caught one
backend bug that neither of the other two modes reached. Keep it.

## Known failing / pre-existing

* **Both previously documented upstream bugs are now fixed** by merging
  `upstream/v2.1` up to `a471ab78` ("FFI: Fix widening semantics for 64 bit
  arithmetic", `e4d80516`). They are kept here only so the history makes
  sense:

  1. In a compiled loop, `ffi.cast("int64_t", x) < ffi.cast("int64_t", y)`
     on a `uint32_t` source took the wrong branch. `simplify_conv_i64_num()`
     folded a `u32 -> i64` conversion to the bare 32 bit value on x64,
     dropping the zero-extension. Verified fixed: interpreter and JIT now
     agree on the reproducer that used to differ.
  2. `tonumber(u)` for a `uint64_t` at or above 2^63 differed by one ulp
     between the interpreter and the JIT. Also verified fixed.

  The SIMD test suite never depended on either, so nothing in it changed.

* LuaJIT has no in-tree test suite, so the regression baseline is an output
  diff of `test/simd/test_noregress.lua` against a pristine build of the
  current upstream head `a471ab78`. It is **byte-for-byte identical**. See
  `SIMD_TESTING.md` for the exact commands.

* The Windows x64 upper-YMM issue is fixed. Wine now passes the dynamic-splat,
  scalar-call, cross-width guard, side-exit, GC-side-trace, register-pressure,
  and machine-code inspection coverage. The bug was broader than the ABI
  setup path: conditional GC/allocation helpers bypassed it, executed
  `VZEROUPPER`, and retained only the ABI-preserved low half of XMM6-XMM15.
  Every control-flow-safe call setup now evicts live YMM values and records
  upper-lane loop clobbers. The codegen harness also makes the Windows CRT's
  root-relative `os.tmpname()` result local before passing it to `jit.dump`.

* MinGW's interpreter-side `fma` under Wine still differs from the required
  single-rounded result. The hardware-VFMADD JIT result is correct; this is
  now the only known Windows SIMD runtime discrepancy.

## Files touched so far

```
src/lj_ir.h                      IRT_V16I8..IRT_V2F64, IR_KVEC, vector IR ops
src/lj_ir.c   src/lj_iropt.h     lj_ir_kvec(), 128/256 bit constant interning
src/lj_asm.c                     vector register class, 4/8-slot spills,
                                 ra_left()/ra_rematk() for IR_KVEC, dispatch
src/lj_asm_x86.h                 vector XLOAD/XSTORE (MOVUPS/VMOVUPS)
src/lj_asm_x86_vec.h             the x86-64 vector backend (new)
src/lj_emit_x86.h                VEX width encoding, emit_loadkvec(), spills
src/lj_target_x86.h              packed SSE/AVX opcodes, 256 bit ExitState.fpr
src/vm_x86.dasc src/vm_x64.dasc  save full XMM/YMM registers at a trace exit
src/lj_snap.c                    16/32 byte restore, sunk vector boxes
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
* A vector constant occupies **three or five** IR slots, and every loop that
  walks the constant range from `nk` has to skip its two or four payload slots
  or it will decode them as instructions. `gc_traverse_trace()`,
  `lj_opt_sink()` and the
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
* The same trap has a second form. A new vector IR opcode also has to be added
  to the explicit `case` list in `asm_ir()` (`lj_asm.c`) that routes vector
  opcodes to `asm_vec()`. Miss it and the build succeeds, the differential
  tests pass, and every trace silently aborts with "cannot assemble IR
  instruction N". Only the codegen tests notice. This actually happened when
  `VSHLV`/`VSHRV`/`VSARV` were added.
* There are **two** dispatch tables, and both need the new opcode: the `case`
  list in `asm_ir()` (`lj_asm.c`) that routes to `asm_vec()`, and the switch
  inside `asm_vec()` itself. Missing the second one is worse than missing the
  first: instead of a clean NYI abort the assembler falls through its
  `default`, leaves the destination register unwritten, and the trace dies
  with "inconsistent register allocation". This happened when `VMULHI` was
  added, and again the differential tests passed the whole time because the
  interpreter was answering.
* A codegen test whose operands are all loop-invariant proves nothing: the
  operation is hoisted out of the loop and the body contains only the
  accumulate. Make one operand loop-carried.
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

## Trap: the fast function ID budget is *full*

`FF__MAX` was exactly 256 before `simd.fma` was added, i.e. **zero** spare
IDs. Any new recorded `ffi.simd` entry point has to free one first. `anyof`
was converted to a Lua wrapper over `movemask` for this (`movemask(a) ~= 0`),
which costs nothing at run time and also removes its `LJ_POST_FIXGUARD`
dependency, since the comparison is now ordinary Lua.

Prefer *extending* an existing function with a new operand shape over adding
a new name. `shl(a, nvec)` and `shuffle(a, idxvec)` both did that and cost no
ID at all. Check the budget before designing a new entry point:

```
FF__MAX is asserted <= 256 in lj_ff.h; print it from a scratch C file that
includes lj_ff.h if you need the exact headroom.
```

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

1. **Separate non-constant scalar indices in `shuffle`/`shuffle2`.** Rejected
   at record time. Pass one runtime index vector instead; that form and
   variable `insert` are supported for both XMM and YMM vectors.
2. **Vectors by value in FFI callbacks are x86-64 SysV only.** Supported for 8
   and 16 byte vectors there (see `SIMD_DESIGN.md` D14). On Windows x64, x86
   and non-x86 targets, and for any vector wider than one register, `ffi.cast`
   still rejects the callback with the ordinary "cannot convert" error; pass a
   pointer instead.
3. **`a*b+c` written with operators is never contracted into an FMA.**
   Contraction would round once where the interpreter rounds twice. The fused
   form is available explicitly as `simd.fma()`, which rounds once in *both*
   the interpreter (C99 `fma`/`fmaf`) and the JIT (`VFMADD213`), so the two
   still agree bit for bit.
4. **This branch does not sit on `origin/v2.1`** -- see D13 above.

## Benchmarks


`./src/luajit test/simd/bench.lua`, N=65536, 200 passes, best of 5, on this
AVX2 machine:

```
saxpy (float)              scalar     5.5 ms   XMM     1.4 ms  3.87x   YMM     1.1 ms  4.85x (1.25x/XMM)
dot product (float)        scalar     5.0 ms   XMM     1.2 ms  3.98x   YMM     0.7 ms  7.49x (1.88x/XMM)
horizontal max (int32)     scalar     3.7 ms   XMM     3.7 ms  0.99x   YMM     1.9 ms  1.92x (1.95x/XMM)
clamp (float)              scalar    28.5 ms   XMM     1.7 ms 17.27x   YMM     1.2 ms 23.91x (1.39x/XMM)

Heavy kernels: FIR 32768x60, polynomial 32768x60, Mandelbrot 16384x4
8-tap FIR (float)          scalar     3.4 ms   XMM     0.8 ms  4.10x   YMM     0.6 ms  5.52x (1.35x/XMM)
degree-11 poly (double)    scalar     4.0 ms   XMM     2.1 ms  1.90x   YMM     1.5 ms  2.77x (1.46x/XMM)
Mandelbrot 64-it (double)  scalar     6.9 ms   XMM     7.4 ms  0.93x   YMM     6.8 ms  1.00x (1.08x/XMM)

Real-world kernels: 1080p blur, 5.46s audio, 131072 particles, 131072 ChaCha blocks
5x5 Gaussian (1080p)       scalar     8.5 ms   XMM     2.3 ms  3.70x   YMM     2.3 ms  3.73x (1.01x/XMM)
64-tap audio FIR           scalar     8.9 ms   XMM     2.3 ms  3.85x   YMM     1.5 ms  6.10x (1.58x/XMM)
gravity particles x32      scalar    58.2 ms   XMM    19.4 ms  3.00x   YMM    13.9 ms  4.18x (1.39x/XMM)
ChaCha20 block core        scalar    29.2 ms   XMM    10.6 ms  2.75x   YMM     6.3 ms  4.63x (1.68x/XMM)
```

The dot product gets close to the expected second width doubling: 4 lanes are
3.98x scalar and 8 lanes are 7.46x. SAXPY improves less because it streams two
loads and one store and becomes memory-bound. Clamp wins much more because the
scalar version is branchy and the vector version is branchless. Horizontal max
is memory-bound at either width, but YMM still nearly doubles XMM throughput.
The heavier group covers overlapping unaligned loads, a dependent Horner
chain, and divergent mask updates rather than only one-instruction kernels.
AVX load/arithmetic fusion removes the Horner trace's separate coefficient
loads: the XMM/YMM traces shrink from 189/234 to 165/210 instructions and the
XMM run improves from about 2.3 ms to 2.1 ms. YMM is already bound by its
dependent multiply chain, so its elapsed time stays roughly flat while its
code shrinks by the same 24 instructions.
The production-sized group adds four different bottlenecks. The separable
Gaussian processes a complete 1920x1080 float frame and reaches memory
bandwidth at XMM width. The windowed-sinc FIR exposes four independent
64-tap accumulator chains and gains another 1.58x from YMM. The particle
kernel keeps position and velocity vectors live through 32 gravity/collision
steps, exercising divide, square root, masks, select and long arithmetic
chains. ChaCha20 treats SIMD lanes as independent blocks and runs the full
20-round wrapping add/xor/rotate core with sixteen live vector state words.
Its run is deliberately eight times larger now, both to give stable timings
and to make it a genuinely sustained integer-SIMD workload. Byte-aligned
rotates reduce the main XMM/YMM loop from 527/543 to 465/481 instructions;
against the pre-fold binary, representative pinned runs improve XMM from
11.8--12.1 ms to 10.5--11.1 ms and YMM from 6.7--6.8 ms to 6.3--6.6 ms.
Every buffer-producing kernel validates a deterministic checksum against its
scalar result outside the timed hot loop.

`bench_ops.lua` also compares dependent XMM and YMM operations directly. On
this machine the YMM instruction or packed sequence has essentially the same
latency while doing twice the lane work:

```
                                  XMM       YMM   lane throughput
float add                     0.38 ns   0.38 ns       1.99x
float mul                     0.76 ns   0.76 ns       2.00x
float div                     1.99 ns   2.08 ns       1.91x
float sqrt                    2.26 ns   2.22 ns       2.04x
float min                     0.76 ns   0.72 ns       2.11x
float fma                     0.72 ns   0.72 ns       2.00x
int32 add                     0.21 ns   0.22 ns       1.95x
int32 mul                     1.89 ns   1.89 ns       2.00x
int32 xor                     0.25 ns   0.25 ns       2.00x
int32 shl const               0.21 ns   0.21 ns       2.01x
int32 rol 8 idiom             0.38 ns   0.38 ns       2.00x
int16 shl per-lane            1.23 ns   1.17 ns       2.11x
int8 shl per-lane             2.22 ns   2.29 ns       1.94x
int32 select                  0.62 ns   0.62 ns       2.00x
int32 shuffle const           0.21 ns   0.57 ns       0.73x
int32 shuffle vector          0.30 ns   0.57 ns       1.04x
int16 mulhi                   0.90 ns   0.89 ns       2.02x
int32 mulhi                   1.51 ns   1.43 ns       2.11x
uint32 mulhi                  1.46 ns   1.51 ns       1.93x
int64 mulhi                   3.33 ns   3.24 ns       2.05x
uint64 mulhi                  2.75 ns   2.67 ns       2.06x
int8 mulhi                    1.80 ns   1.70 ns       2.12x
uint8 mulhi                   1.89 ns   1.92 ns       1.97x
uint8 saturated add           0.20 ns   0.20 ns       2.01x
```

The constant row is a deliberately dependent full reversal: `VPERMD` has
more latency than XMM `PSHUFD`, but is still faster than the former
half-swap-plus-byte-shuffle chain. Runtime YMM indices benefit much more in
instruction count: one `VPERMD` replaces control expansion, two
`VPSHUFB`s, a half swap, and a packed select. Same-half constants are not
represented by this row; they retain one low-latency `VPSHUFB`.

The narrow per-lane shifts replace interpreter fallback (about 32.5 ns for
words and 35.1 ns for bytes) with packed sequences at 1.23 ns and 2.22 ns:
roughly 26x and 16x faster. YMM runs the same lane-local sequence at the same
latency while shifting twice as many lanes.

The 64-bit `mulhi` decomposition is 3.33 ns signed and 2.75 ns unsigned,
versus about 30 ns in the interpreter. The YMM form is 3.24/2.67 ns and
therefore slightly more than doubles per-lane throughput.

The conversion slice stays packed for every integer-to-FP direction. Four-way
throughput on this host is:

```
                                  XMM       YMM   lane throughput
uint32 -> float               0.51 ns   0.51 ns       2.00x
int64 -> double               0.58 ns   0.58 ns       2.00x
uint64 -> double              0.51 ns   0.51 ns       2.00x
double -> int64               0.64 ns   1.29 ns       1.00x
double -> uint64              0.64 ns   1.29 ns       1.00x
```

The last two rows are deliberately different: x86 has no packed
double-to-qword conversion before AVX-512, so YMM performs four call-free
scalar `CVTTSD2SI` instructions and retains XMM's per-lane throughput.

Cross-width conversion throughput, best of five on the same host:

```
i8x16 -> i16x16             0.20 ns
i16x16 -> i8x16             0.26 ns
i16x8 -> float8             0.27 ns
float8 -> i16x8             0.72 ns
float4 -> double4           0.29 ns
double4 -> float4           0.27 ns
u32x4 -> double4            0.32 ns
i64x4 -> float4             1.98 ns
u64x4 -> float4             2.08 ns
```

The qword-to-float rows use four exact scalar hardware conversions because
AVX2 has no packed form; all other rows are packed. During this work an XMM
loop PHI was found to use legacy `MOVAPS` while YMM values were live. That
AVX-to-SSE transition cost about 19--22 ns per iteration: for example
`i16x16 -> i8x16` fell from 19.69 ns to 0.26 ns after vector moves, loads,
stores, constants and spills were made VEX-128-clean whenever AVX is active.
A codegen assertion now forbids legacy `MOVAPS`/`MOVUPS` in a representative
mixed-width loop.

The same audit found a broader transition in mixed ordinary-number/SIMD code.
A loop carrying one YMM `float8` add and one Lua-number add took 76.50 ns per
iteration even though the YMM-only loop took 0.38 ns. The scalar `ADDSD` was
still legacy SSE. The generic x86 backend now emits VEX-128 forms for scalar
FP arithmetic, moves, constants, loads/stores, conversions, comparisons,
sqrt, and rounding whenever AVX is active:

```
YMM add only                 0.38 ns
YMM add + Lua scalar add     0.38 ns   (was 76.50 ns)
YMM add + integer % 37       0.85 ns   (was  1.70 ns)
```

The codegen test carries memory traffic, multiply, sqrt, comparison and both
scalar/YMM loop PHIs at once, then rejects every non-VEX instruction naming an
XMM register. A second mixed loop requires constant integer modulo to use
reciprocal multiplies with no helper call or `IDIV`, avoiding the
`VZEROUPPER` and wide spill/reload sequence that a call would require. Dumps
of every trace produced by both benchmark suites are likewise free of legacy
XMM instructions whenever that trace uses YMM.

`simd.fma` is worth measuring separately, because whether it helps depends
entirely on whether the loop is arithmetic bound:

```
degree-4 Horner chain, no loads   mul+add 10.6 ms   fma  6.1 ms   1.75x
degree-5 Horner over an array     mul+add 54.2 ms   fma 52.3 ms   1.03x
```

The second loop is dominated by the array load and the result boxing, not by
the multiply-adds, so halving the arithmetic buys almost nothing. Use
`simd.fma` for the single rounding it guarantees; treat the throughput as a
bonus that only shows up in ALU-bound code.

FFI array indexing records a temporary cdata box even when sink optimisation
later removes it. Its virtual initializer used to stop the load-fusion scan as
if it were a real aliasing store. Ignoring only that sink-tagged `XSTORE`
allows the existing arithmetic fusion and the new unary/shuffle/conversion
forms to consume array memory directly. A six-kernel square-root,
rounding and absolute-value dump changes as follows:

```
                         instructions   standalone VMOVUPS
before                         2008              210
after                          1960              162
```

This includes XMM and YMM root and loop traces. The square-root timings remain
flat because its execution unit, rather than the front end, is the bottleneck;
the shorter code and freed vector register are still useful in larger,
pressure-heavy traces. Runtime codegen tests validate the corresponding
memory forms for `VPERMD`, `VPERMQ`, packed conversions and integer widening,
and the Nehalem model continues to reject every legacy-SSE unaligned memory
arithmetic form.
