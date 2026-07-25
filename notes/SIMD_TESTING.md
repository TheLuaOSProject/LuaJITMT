# SIMD testing

## Running

```
make -j$(nproc)
./src/luajit test/simd/run.lua            # every file, interpreted and JIT
./src/luajit test/simd/run.lua -interp    # interpreter only
./src/luajit test/simd/run.lua -jit       # JIT only
./src/luajit test/simd/run.lua test_lib   # one file, both modes
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
| `test_ffi_abi.lua` | vector arguments, returns, memory round trips and callbacks against a small C helper library |
| `test_stress.lua` | randomized program generator: builds random vector expressions, runs them interpreted and compiled, compares |

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

## Build configurations exercised

```
make -j$(nproc)                                    # default release
make -j$(nproc) CCDEBUG=-g XCFLAGS=-DLUA_USE_ASSERT # assertions on
make -j$(nproc) XCFLAGS=-DLUAJIT_USE_VALGRIND       # if valgrind is available
make -j$(nproc) XCFLAGS=-DLUAJIT_DISABLE_JIT        # interpreter-only build
```
