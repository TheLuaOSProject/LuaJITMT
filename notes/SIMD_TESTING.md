# SIMD testing

## Running

```
make -j$(nproc)
./src/luajit test/simd/run.lua            # every file, interpreted and JIT
./src/luajit test/simd/run.lua -interp    # interpreter only
./src/luajit test/simd/run.lua -jit       # JIT only
./src/luajit test/simd/run.lua test_lib   # one file, all modes
./src/luajit test/simd/bench.lua          # scalar vs. XMM vs. YMM kernels
./src/luajit test/simd/bench_ops.lua      # kernels and operation throughput
SIMD_SEED=12345 ./src/luajit test/simd/run.lua   # different random seed
```

`run.lua` spawns one child process per (file, mode) pair so that `ffi.cdef`
state and compiled traces from one file cannot leak into another. The exit
status is non-zero if anything failed.

## Layout

| File | Covers |
|---|---|
| `simdtest.lua` | shared helpers: the vector type table, a deterministic xorshift PRNG, bit-exact vector comparison, and the *scalar reference implementations* |
| `test_types.lua` | ctype shape, sizeof/alignof, construction forms, lane reads, immutability, memory round trips, rejected shapes |
| `test_arith.lua` | the Lua operators, randomized against the scalar reference, splat operands, wraparound, FP edge cases, rejected operators |
| `test_lib.lua` | every `ffi.simd` function, randomized against the scalar reference, plus negative tests |
| `test_jit.lua` | interpreter/JIT differential: the same computation run interpreted and compiled, including loop-carried values, guards, side exits, spills |
| `test_codegen.lua` | inspects `jit.dump`/`jit.util` output of representative traces to prove packed instructions are emitted and no scalarisation or permanent exit happens; also checks vector IR/constants and explicit YMM operands |
| `test_ffi_abi.lua` | vector arguments, returns, stack spilling, mixed argument lists and memory round trips against a small C helper library compiled at test time; callbacks taking and returning vectors by value for every lane kind, ten vector arguments (registers plus stack), 16-byte stack alignment behind eight register arguments, mixed integer/FP/vector argument lists, and the rejection of vectors too wide for a register |
| `bench.lua` | scalar/XMM/YMM comparisons for small and heavy kernels, plus production-sized 1080p Gaussian blur, 64-tap audio FIR, 32-step particle gravity/collision simulation and 20-round ChaCha20 block processing |
| `bench_ops.lua` | realistic kernels, per-operation latency, and a direct XMM/YMM lane-throughput comparison for the AVX2 backend |
| `test_noregress.lua` | ordinary Lua and FFI behaviour with no vector types anywhere; its output is diffed against a pristine LuaJIT build |

## Method

**Scalar references.** `simdtest.lua` implements every operation lane by lane
in Lua, deliberately without using any vector operation. Integer lanes are
widened to `int64_t` before the arithmetic so the reference cannot itself
wrap; float lanes are computed in double and rounded once to float, which is
exact for `+ - * /` and `sqrt` because 53 >= 2*24+2.

**Bit-exact comparison.** `M.same()` compares the raw bytes of two vectors, so
NaN payloads, signed zeros and unsigned/signed confusion are all caught. Never
compare vectors by their printed lane values.

**Benchmark validation.** `bench.lua` consumes every result. Buffer-producing
kernels compute a sparse deterministic checksum after the hot loop and compare
scalar, XMM and YMM output, with a scale-relative tolerance only where
floating-point evaluation order can differ. Integer ChaCha20 must agree
exactly. The real-world group flushes the trace cache between scalar, XMM and
YMM runs so a width-specialised trace from a shared helper cannot turn the next
width into side-exit timing; the default best-of-five run excludes first-run
recording noise.

**Determinism.** All randomness comes from `M.rng(seed)`, a xorshift32. The
seed is printed in every failure message together with the operand values, so
any failure can be replayed exactly. Override with `SIMD_SEED`.

**The seed must determine the whole run.** `test_arith.lua` used to iterate
its operator table with `pairs()`. `LUAJIT_SECURITY_STRHASH` seeds the string
hash from the PRNG, so that order differs on *every process*, and with it the
order the operators were recorded in and the shape of the resulting traces.
The operand values were still seed-derived, so the suite looked reproducible
while it was not: a backend bug that only appeared in one ordering could not
be replayed from its seed. The permutation now comes from a separate seeded
RNG stream. Do not introduce `pairs()` over a string-keyed table anywhere that
affects what gets executed or in which order.

**Some bugs need repetition, not a seed.** The vector-constant reload failure
`SIMD_STATUS.md`) showed up in roughly 1 run in 300 and never twice at the
same seed. Running the same command in a loop and keeping the output of the
failing runs is the tool for that:

```
for i in $(seq 1 2000); do
  SIMD_SEED=$(( (i % 8) + 1 )) ./src/luajit test/simd/run.lua --one jit test_lib \
    > /tmp/h.txt 2>&1 || { echo "HIT $i"; cat /tmp/h.txt; }
done
```

Do not pipe the runner through `tail -1` while doing this: the failure detail
is printed by the child process before the final line, and `tail` throws it
away. Once it reproduces, put a `fprintf` at the failure point, rebuild, and
re-run the same loop under `gdb --batch -ex "break <file>:<line>" -ex run
-ex bt` to get the creator of the bad IR rather than only its consumer.

**Run more than one seed.** A wrong guard polarity in `simd.allof`/`anyof`
only showed up on some seeds, because it needed a mask whose answer differed
from whatever the previous operation had left behind. Before believing the
suite:

```
for s in 1 7 999 20260101 424242; do SIMD_SEED=$s ./src/luajit test/simd/run.lua; done
```

`test_jit.lua` now also pins that case deterministically ("mask predicates
record the right guard polarity").

**Corner values.** `M.randlanes` biases the generator towards 0, 1, -1, the
all-ones pattern, and for FP towards +-0, +-inf and NaN, so those cases appear
in every run instead of being separate hand-written tests.

**Interpreter vs JIT.** Every file runs in both modes. In addition,
`test_jit.lua` runs the *same closure* interpreted once and compiled many
times within one process and compares, which is what catches recorder bugs
that only appear after a trace is linked.

## CPU feature-level matrix

The release binary is also run under explicit QEMU x86-64 CPU models. Invoke
the child mode directly: the normal runner starts another process without the
QEMU prefix and would accidentally test the native host instead.

```
tests=(test_types test_arith test_lib test_jit test_codegen test_ffi_abi test_stress)
for cpu in Nehalem-v1 SandyBridge-v1 Haswell-v2; do
  for mode in interp jit mixed; do
    for testname in "${tests[@]}"; do
      qemu-x86_64 -cpu "$cpu" ./src/luajit test/simd/run.lua \
        --one "$mode" "$testname"
    done
  done
done
```

`Nehalem-v1` exercises the x86-64-v2/SSE path with no AVX,
`SandyBridge-v1` exercises VEX/XMM lowering with AVX but no AVX2, and
`Haswell-v2` exercises AVX2/YMM and FMA. Every test file passes in
interpreter, JIT, and mixed mode on all three. This is separate from the
native feature check: it proves that runtime dispatch never executes an AVX
or AVX2 instruction on an older advertised CPU.

## Reference-implementation hazards

The min/max references compare lane values directly instead of widening them
to `int64_t` first. That was originally a workaround for the upstream
zero-extension bug described in `SIMD_STATUS.md`, which the merge up to
`a471ab78` fixed. The workaround is kept: comparing the lane values directly
is what the operation actually means, and not widening keeps the reference
independent of the conversion path it is meant to be checking.

## Non-vector regression check

LuaJIT has no in-tree test suite, so the baseline for "nothing else changed" is
an output diff against a pristine build of the upstream commit the branch
is currently merged up to:

```
git worktree add /tmp/ljbase a471ab78 && (cd /tmp/ljbase && make -j$(nproc))
./src/luajit          test/simd/test_noregress.lua > /tmp/new.txt
/tmp/ljbase/src/luajit test/simd/test_noregress.lua > /tmp/base.txt
diff /tmp/base.txt /tmp/new.txt      # must be empty
```

It covers numbers, strings, tables, metatables, closures, coroutines, sorting,
`pcall`, the bit library, FFI structs, arrays, 64-bit integers, casts,
callbacks, ctype reprs and two hot loops. It is currently byte-for-byte
identical.

The callback coverage deliberately includes a **nine double** callback (eight
in FPRs, the ninth on the stack) and a mixed float/int/double one. Widening
the callback FPR save area for vectors changes the offsets the trampoline and
`lj_ccallback.c` agree on, and an ordinary integer-argument callback would not
notice. Mis-storing one XMM register into the neighbouring slot changes this
file's output, which is how that edit is guarded.

## Build configurations exercised

```
make -j$(nproc)                                     # default release
make -j$(nproc) CCDEBUG=-g XCFLAGS=-DLUA_USE_ASSERT # assertions on
make clean && make -j$(nproc) XCFLAGS=-DLUAJIT_DISABLE_JIT  # no JIT at all

# AddressSanitizer. The sanitizer flags must go to the *target* only, or the
# host build tools fail. LUAJIT_USE_SYSMALLOC routes allocations through
# malloc so ASan can actually see them; it works because GC64 has no low-2GB
# constraint.
make clean && make -j$(nproc) CCDEBUG=-g \
  TARGET_CFLAGS="-DLUAJIT_USE_SYSMALLOC -fsanitize=address -fno-omit-frame-pointer" \
  TARGET_LDFLAGS="-fsanitize=address"
ASAN_OPTIONS=detect_leaks=0 ./src/luajit test/simd/run.lua

# UndefinedBehaviorSanitizer, same flag placement.
make clean && make -j$(nproc) CCDEBUG=-g \
  TARGET_CFLAGS="-DLUAJIT_USE_SYSMALLOC -fsanitize=undefined -fno-omit-frame-pointer" \
  TARGET_LDFLAGS="-fsanitize=undefined"
./src/luajit test/simd/run.lua 2>&1 | grep "runtime error"
```

All of these pass. UBSan reports nothing at all in `lj_simd.c`, `lj_crecord.c`
or `lj_record.c`; the remaining reports come from stock LuaJIT files
(`lj_parse.c`, `lj_buf.c`, `lj_bcread.c`, `lib_jit.c`) and from the
deliberately unaligned mcode stores, which the compiler attributes to the
inlining site rather than to the macro: `lj_asm.c` (`emit_setvmstate`, itself
unmodified in `lj_emit_x86.h`) and `lj_ccallback.c`, a file this work does not
touch at all. All pre-existing.

Run the **assert** build with more than one seed, not just once. The assert
build is the only configuration that validates IR structure, and one of its
checks (`rec_check_ir`) only trips when a vector constant's *payload bytes*
happen to decode as an invalid instruction, which depends on the constants a
given test run interns.

All three pass. In the JIT-disabled build the runner skips the `jit` and
`mixed` modes and `test_jit.lua`/`test_codegen.lua` skip themselves, so the
interpreter semantics still get their full randomized coverage. Remember
`make clean` when switching build options: the generated VM does not depend on
`XCFLAGS` in the dependency file.
