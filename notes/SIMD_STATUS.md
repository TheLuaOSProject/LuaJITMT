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
| M41 | Expand byte multiply in IR so invariant shifts CSE; use binary constant memory operands only under vector-register pressure | done |
| M42 | Fuse one-use vector loads into the compatible FMA form while retaining accumulator coalescing and alias safety | done |
| M43 | Fold AVX2 per-lane count loads into shifts only under register pressure, retaining faster prefetched loads otherwise | done |
| M44 | Lower variable byte left shifts through a packed power-of-two lookup and IR-visible byte multiplication | done |
| M45 | Lower logical and arithmetic variable byte right shifts through lookup factors and isolated word products | done |
| M46 | Shorten signed qword `mulhi` correction and reuse cross-products/sign masks when squaring | done |
| M47 | Compute signed/unsigned byte `mulhi` squares through absolute bytes and full word products | done |
| M48 | Lower variable word left shifts through packed power-of-two factors and one native word multiply | done |
| M49 | Lower word logical-right and YMM arithmetic-right shifts through packed factors and native high products | done |
| M50 | Canonicalise identity, half-swap, and immediate 32/64-bit constant shuffles before allocating byte controls | done |
| M51 | Collapse lane-local two-source low/high interleaves to one packed unpack instruction | done |
| M52 | Collapse single-source and equal-source `shuffle2` controls to the ordinary one-source permute path | done |
| M53 | Lower native two-source dword/qword shuffles and YMM half concatenations through direct immediate instructions | done |
| M54 | Lower representable constant same-position two-source blends through one `PBLENDW` | done |
| M55 | Fuse one-use final array loads into native AVX two-source shuffle memory operands | done |
| M56 | Collapse lane-local and full-width contiguous two-source windows through `PALIGNR` | done |
| M57 | Lower non-repeating full-width YMM dword/qword blends through one `VPBLENDD` | done |
| M58 | Reduce signed/unsigned byte vectors through `PSADBW` qword partial sums | done |
| M59 | Collapse unsigned-word horizontal min/max through `PHMINPOSUW` | done |
| M60 | Map signed-word horizontal min/max to biased `PHMINPOSUW` reductions | done |
| M61 | Widen byte extrema into word pairs and finish with `PHMINPOSUW` | done |
| M62 | Fuse 16-bit `hsum(a*b)` into `PMADDWD` pair-dot reductions | done |
| M63 | Fuse 8-bit `hsum(a*b)` into even/odd `PMADDWD` pair-dot reductions | done |

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

Byte multiply now exposes its two word products to the optimiser instead of
expanding an opaque operation in the backend. The low-byte cleanup is one
`VAND` instead of two shifts, and all chains share an invariant shifted
multiplier. The dependent and four-chain measurements move from roughly
1.93 to 1.89 ns/op and 0.67 to 0.62 ns/op respectively. Register-rich
parallel kernels benefit much more:

```
independent chains       old       new      improvement
8                       5.30 ms   3.79 ms       28%
12                      7.97 ms   5.43 ms       32%
14                      9.27 ms   6.25 ms       33%
```

The stressed three-trace dump drops from 957 to 829 instructions. Its
`PSRLW`/`PSLLW` count drops from 340 to 139. A shared pressure heuristic keeps
the `0x00ff` mask in a vector register for small loops and lets `VPAND` read
the interned constant from memory when retaining the register would instead
cause repeated spill/reload instructions. AVX2 retains the same dependent
latency as XMM while multiplying twice as many byte lanes.

FMA now selects between its compatible 132/213 forms after the destination
operand is fixed, allowing a one-use multiplier or addend load to occupy the
instruction's memory source. Four XMM/YMM root-and-loop traces lose eight
`VMOVUPS` instructions and shrink from 1572 to 1536 bytes. The latency-bound
single chain remains flat, while twelve and fourteen independent streaming
chains improve by roughly 2% and 5% because the loaded operand no longer
needs a temporary vector register. The conflict scan skips only FMA's
non-emitting `CARG` carrier; a real aliased store still blocks fusion.

AVX2 per-lane shifts now apply the same pressure-sensitive memory policy.
Unconditional count fusion made the ordinary streaming benchmark about 8%
slower because it prevented the count load from issuing early, so
register-rich traces deliberately retain `VMOVUPS`. At fifteen live
accumulators, using memory for the final count operands instead removes spill
traffic and improves about 8% (7.33 to 6.75 ms). The three-kernel dump falls
from 1612 to 1564 instructions and from 59 to 46 spill reloads, while twelve-
and fourteen-chain timings remain flat.

Variable byte left shift no longer pays for four dword variable-shift
decompositions. A saturated byte add maps valid counts into a repeated
power-of-two `PSHUFB` table and maps every out-of-range count to the shuffle's
zeroing controls; the result then uses the IR-visible modulo-byte multiply.
Dependent latency improves from about 2.17 to 1.61 ns/vector. Median
streaming throughput improves from 1.88 to 1.00 ns/vector for XMM and from
2.48 to 1.14 ns/vector for YMM. The four representative traces shrink from
634 to 558 instructions, with all 16 `VPSLLVD` operations and 36 dword
extraction shifts removed.

Logical and arithmetic variable byte right shifts now use lookup factors and
two isolated word products instead of four dword variable shifts. Median
streaming logical-shift time improves from 1.85 to 1.03 ns/vector for XMM and
from 2.26 to 1.26 ns/vector for YMM, about 45%. Arithmetic shift improves
from 1.95 to 1.11 and from 2.25 to 1.36 ns/vector, about 43%/39%.
Representative logical root/loop traces shrink from 634 to 570 instructions
and arithmetic traces from 638 to 586, removing 32 variable and 88 immediate
dword shifts in total. `bench_ops.lua` now tracks dependent XMM/YMM latency
for all three per-lane byte shifts.

Signed qword `mulhi` now forms both input-sign corrections in parallel and
applies one final subtraction, reducing dependent latency from about 3.15 to
2.99 ns/vector. When both operands are the same IR value, one of the four
32-bit partial products, one qword shift, and half the sign-mask work
disappear. Signed square latency improves from 3.34 to 2.90 ns/vector and
eight-chain throughput by about 20--23%; unsigned square throughput improves
about 8--12%. The XMM/YMM signed/unsigned square dump shrinks from 412 to 392
instructions and from 32 to 24 `VPMULUDQ`s. `bench_ops.lua` now keeps both
dependent square paths visible.

Byte `mulhi` squares now use two full word products instead of arranging two
high-word products. Signed inputs first take packed absolute bytes, with
`-128` retaining the correct magnitude bit pattern. Unsigned dependent
latency improves from about 1.83 to 1.67 ns/vector; signed dependent latency
is flat, while eight-chain throughput improves about 9% unsigned and 12%
signed. The XMM/YMM dependent/parallel dump shrinks from 1532 to 1418
instructions and eliminates all 144 high-word multiplies. Dedicated
`bench_ops.lua` rows retain both square paths.

Variable word left shift now constructs packed power-of-two factors and
applies one native `VPMULLW`, replacing the former dword extraction and
reassembly data path. Invariant factor construction hoists completely:
dependent XMM latency improves from about 1.17 to 0.89 ns/vector and YMM from
1.23 to 0.89 ns/vector. With changing counts, median streaming throughput
improves from 1.01 to 0.91 ns/vector for XMM and from 1.70 to 1.18 ns/vector
for YMM. The representative dynamic dump shrinks from 566 to 550
instructions, with packed ANDs falling from 24 to 8 and dword right shifts
from 8 to 4. Codegen tests pin both the hoisted and dynamic XMM/YMM forms.

Variable word right shifts now use packed power-of-two factors and native
high-word products where measurement supports them. Logical right uses
`VPMULHUW` for XMM and YMM; streaming throughput improves from about 0.97 to
0.95 ns/vector for XMM and from 1.42 to 1.06 for YMM. Arithmetic right uses
`VPMULHW` at YMM width, improving dependent latency about 4% and streaming
throughput from 1.39 to 1.05 ns/vector. XMM arithmetic deliberately retains
the prior dword decomposition: it is about 2% faster in a streaming loop and
10% faster with eight live chains. Invariant root/loop dumps shrink from 420
to 407 instructions; dynamic dumps grow from 408 to 411 while removing the
YMM data-dependent shift chain. `bench_ops.lua` now reports all three
per-lane word shifts at both widths.

Constant lane shuffles now discard identity, lower an exact YMM half exchange
directly to `VPERM2I128`, and use immediate `PSHUFD` for every XMM 32/64-bit
permutation and every repeated lane-local YMM pattern. This removes a control
vector without lengthening the critical path. Eight independent YMM chains
with distinct controls improve from 0.169 to 0.138 ns/shuffle, about 18%;
XMM remains neutral near 0.14 ns. Differential and codegen tests cover
identity, local reversal, pure half exchange, and arbitrary cross-half
permutations for all lane kinds.

Canonical `shuffle2` low/high interleaves now emit one `PUNPCK*` at every
lane width, in either operand order, instead of two zero-masked byte shuffles
and an OR. The recogniser follows each 128-bit hardware half at YMM width.
Dependent XMM/YMM latency improves from about 0.390 to 0.235 ns/vector,
roughly 40%, and eight-chain throughput from 0.137 to 0.096 ns/op, about 30%.
Arbitrary two-source permutations retain the general lowering.

Degenerate `shuffle2` controls now share the one-source canonicalizer when
all lanes select only the first input, only the second input, or both inputs
are the same IR value. Single-source identity with a loop-carried add improves
from about 0.571 to 0.238 ns/vector. YMM cross-half reverse improves from
1.137 to 0.760 ns/vector, and an arbitrary equal-source control from 1.170 to
0.759 ns/vector, by replacing masked dual routing with one direct permute.

Native two-input shuffle shapes now have a `VSHUF2` IR lowering to
`SHUFPS`, `SHUFPD`, or `VPERM2I128`. Reversed input order is recognised, and
non-matching controls retain the general path. Dword/qword XMM/YMM patterns
improve from about 0.571 to 0.386 ns/vector, roughly 32%; mixed YMM half
concatenation improves from 1.138 to 0.759 ns/vector, about 33%. The isolated
direct benchmark costs 0.19 ns at either width.

Constant same-position `shuffle2` controls now use one `PBLENDW` when each
16-bit word selects a single source and a YMM high-half mask repeats its low
half. Paired-byte and dword dependent blends improve from about 0.571 to
0.386 ns/vector at both widths, roughly 32%; isolated dword cost is about
0.19 ns/vector. Unpaired bytes and non-repeating controls retain the generic
route.

Byte `hsum` now forms eight-byte qword partial sums with `PSADBW` against
zero, then combines only those partials. The low byte of an unsigned
bit-pattern sum is identical to modular signed or unsigned byte addition, so
the existing final sign/zero extension preserves both lane semantics.
Dependent XMM/YMM reductions improve from roughly 0.96--1.11 to
0.62--0.79 ns, and eight-chain throughput from 0.86--1.05 to
0.52--0.68 ns/op. `bench.lua` now includes a 16 MiB, 32-byte block-checksum
workload with explicitly unrolled scalar, XMM and YMM implementations.

Unsigned-word `hmin` now maps directly to `PHMINPOSUW`; YMM first combines
its two hardware halves and then applies the XMM-only instruction. `hmax`
uses the exact identity `max(x) = ~min(~x)`, with the final complement in the
scalar result path. Eight-chain throughput improves about 34--39% for XMM
and 32--36% for YMM. YMM dependent latency improves about 43% for min and
25% for max. A full-4K hierarchical-depth tile benchmark improves from about
1.7 to 1.4 ms at XMM width and 2.0 to 1.5 ms at YMM width.

Signed-word extrema now flip the ordering bit before the same
`PHMINPOSUW` path: min uses XOR `0x8000`, while max uses XOR `0x7fff`; the
same mask restores the scalar result. Eight-chain XMM throughput improves
about 34%. YMM dependent latency improves 29--32% and eight-chain throughput
21--22%. A one-minute PCM16 waveform-envelope benchmark improves from about
2.4 to 2.1 ms at XMM width and 2.8 to 2.3 ms at YMM width.

Byte min/max now zero-unpacks low/high byte groups into word pairs, takes
their unsigned packed minimum, and finishes with `PHMINPOSUW`. Signed and max
forms reuse the `0x80`/`0x7f` and `0xff` ordering transforms. XMM dependency
improves 6--26% and eight-chain throughput 20--31%; YMM dependency improves
18--25% and throughput 7--18%. A 16 MiB INT8 activation-range benchmark
improves from about 2.3 to 1.8 ms for XMM and 2.5 to 2.0 ms for YMM.

Word `hsum(a*b)` now bypasses the materialised low-word product and uses one
`PMADDWD` to form full dword pair sums. Final word narrowing makes this exact
for signed and unsigned modulo-16-bit semantics. The XMM/YMM reduction falls
from 7/9 to 5/7 vector instructions. Dependent cost improves about 11%/8% and
eight-chain throughput about 14%/12%. A 16 MiB PCM16 16-tap polyphase
decimator improves from about 2.8 to 2.5 ms at XMM width and 1.9 to 1.7 ms at
YMM width, reaching roughly 5.5x/7.8x scalar throughput.

Byte `hsum(a*b)` now recognises the exact packed byte-multiply expansion and
reduces its even and odd bytes with two `PMADDWD` instructions. Every term
discarded by the rewrite is divisible by 256, so final byte narrowing
preserves signed and unsigned wraparound exactly. Dependent cost improves
about 11% for XMM and 7% for YMM; eight-chain throughput improves about 9%
and up to 3%. A 16 MiB INT8 32-tap ternary filter improves about 9--10% at
XMM width and 4--5% at YMM width, reaching roughly 9.6x/14.2x scalar
throughput.
