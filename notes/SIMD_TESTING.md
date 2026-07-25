# SIMD testing

## Running

```
make -j$(nproc)
./src/luajit test/simd/run.lua            # every file, interpreted and JIT
./src/luajit test/simd/run.lua -interp    # interpreter only
./src/luajit test/simd/run.lua -jit       # JIT only
./src/luajit test/simd/run.lua test_lib   # one file, all modes
./src/luajit test/simd/bench.lua          # microbenchmarks
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
| `test_codegen.lua` | inspects `jit.dump`/`jit.util` output of representative traces to prove packed instructions are emitted and no scalarisation or permanent exit happens |
| `test_ffi_abi.lua` | vector arguments, returns, stack spilling, mixed argument lists, memory round trips and callbacks against a small C helper library that it compiles at test time |
| `bench.lua` | microbenchmarks: saxpy, dot product, horizontal max and clamp, each against equivalent scalar code |
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

**Determinism.** All randomness comes from `M.rng(seed)`, a xorshift32. The
seed is printed in every failure message together with the operand values, so
any failure can be replayed exactly. Override with `SIMD_SEED`.

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

## Reference-implementation hazards

The Lua reference code must avoid the pre-existing upstream bug documented in
`SIMD_STATUS.md` (`ffi.cast("int64_t", x) < ffi.cast("int64_t", y)` inside a
compiled loop). The min/max references therefore compare the lane values
directly instead of widening them first.

## Non-vector regression check

LuaJIT has no in-tree test suite, so the baseline for "nothing else changed" is
an output diff against a pristine build of the base commit:

```
git worktree add /tmp/ljbase 346ab587 && (cd /tmp/ljbase && make -j$(nproc))
./src/luajit          test/simd/test_noregress.lua > /tmp/new.txt
/tmp/ljbase/src/luajit test/simd/test_noregress.lua > /tmp/base.txt
diff /tmp/base.txt /tmp/new.txt      # must be empty
```

It covers numbers, strings, tables, metatables, closures, coroutines, sorting,
`pcall`, the bit library, FFI structs, arrays, 64-bit integers, casts,
callbacks, ctype reprs and two hot loops. It is currently byte-for-byte
identical.

## Build configurations exercised

```
make -j$(nproc)                                    # default release
make -j$(nproc) CCDEBUG=-g XCFLAGS=-DLUA_USE_ASSERT # assertions on
make -j$(nproc) XCFLAGS=-DLUAJIT_USE_VALGRIND       # if valgrind is available
make -j$(nproc) XCFLAGS=-DLUAJIT_DISABLE_JIT        # interpreter-only build
```
