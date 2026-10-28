# LuaJIT SIMD

> [!IMPORTANT]
> This is the single-threaded SIMD. It does **not** include or enable LuaJITMT multithreading support!

This adds native JIT support for FFI vector values with SSE for x86-64-v2 and AVX/AVX2 for x86-64-v3, and aarch64!

## Benchmarks!

Measured on P-cores (`taskset -c 8`) of an Intel Core i9-13900F, Linux x86-64, GCC 14.2.0. These are the best of 9 runs from the normal release build. The C reference was compiled with exactly `-O2` as the optimisation
level:

```sh
taskset -c 8 ./src/luajit test/simd/bench.lua 9
cc -O2 -std=c11 -Wall -Wextra -Wpedantic test/simd/bench.c -lm -o simd-bench-c
taskset -c 8 ./simd-bench-c 9
```

All time columns are msec. Lua code for the benchmark [here](https://github.com/TheLuaOSProject/LuaJITMT/blob/simd/test/simd/bench.lua) and C code [here](https://github.com/TheLuaOSProject/LuaJITMT/blob/simd/test/simd/bench.c).

| Workload | Lua scalar | Lua XMM | Lua AVX2 | AVX2 speedup | C XMM `-O2` | C AVX2 `-O2` | Lua AVX2 vs C |
|---|---:|---:|---:|---:|---:|---:|---:|
| SAXPY (float) | 7.4 | 1.4 | 1.1 | 6.87x | 1.1 | 0.7 | ~64% |
| Dot product (float) | 4.8 | 1.2 | 0.6 | 8.09x | 1.2 | 0.6 | ~100% |
| Horizontal max (int32) | 3.6 | 3.7 | 1.9 | 1.91x | 0.6 | 0.4 | ~21% |
| Clamp (float) | 28.1 | 1.5 | 1.2 | 23.44x | 0.8 | 0.7 | ~58% |
| Delta encode (int32) | 5.1 | 2.0 | 2.1 | 2.39x | 1.6 | 1.5 | ~71% |
| 8-tap FIR (float) | 3.4 | 0.9 | 0.6 | 5.62x | 0.5 | 0.5 | ~83% |
| Degree-11 polynomial (double) | 4.2 | 2.1 | 1.4 | 3.07x | 2.1 | 1.1 | ~79% |
| Mandelbrot, 64 iterations (double) | 6.7 | 7.4 | 4.1 | 1.63x | 3.7 | 1.9 | ~46% |
| RGBA channel merge, 1080p | 13.4 | 13.0 | 12.4 | 1.08x | 12.5 | 12.4 | ~100% |
| 32-byte block checksum, 16 MiB | 7.5 | 1.5 | 1.2 | 6.33x | 1.2 | 1.2 | ~100% |
| 32-byte block SAD, 16 MiB | 20.8 | 2.8 | 2.5 | 8.20x | 2.3 | 2.3 | ~92% |
| 4K 16-pixel luma sum | 4.1 | 2.3 | 1.6 | 2.55x | 2.2 | 1.6 | ~100% |
| 4K depth tile extrema | 6.3 | 1.4 | 1.5 | 4.13x | 1.0 | 1.2 | ~80% |
| PCM16 peak envelope | 9.1 | 2.1 | 2.3 | 4.05x | 2.5 | 2.8 | ~122% |
| Float PCM noise gate | 47.1 | 4.5 | 3.2 | 14.87x | 2.9 | 2.8 | ~88% |
| PCM16 16-tap decimator | 13.1 | 2.4 | 1.6 | 7.98x | 2.3 | 1.5 | ~94% |
| 4K residual codebook | 151.7 | 7.6 | 6.2 | 24.64x | 6.4 | 5.9 | ~95% |
| 4K four-rate quantize | 127.0 | 9.2 | 5.5 | 23.10x | 6.7 | 4.3 | ~78% |
| INT8 activation range, 16 MiB | 13.1 | 1.9 | 2.1 | 6.19x | 2.2 | 2.5 | ~119% |
| INT8 32-tap ternary filter | 28.0 | 2.8 | 2.0 | 14.24x | 3.6 | 3.8 | ~190% |
| 5x5 Gaussian, 1080p | 10.4 | 2.3 | 1.9 | 5.37x | 1.5 | 1.2 | ~63% |
| 3x3 dilation, 4K | 452.9 | 7.4 | 6.5 | 69.56x | 7.2 | 7.2 | ~111% |
| Weighted 4x4 point transform | 22.6 | 8.7 | 4.4 | 5.09x | 5.0 | 4.6 | ~105% |
| 3D voxel index checksum | 9.6 | 11.6 | 8.2 | 1.18x | 10.1 | 10.3 | ~126% |
| 64-tap audio FIR | 8.7 | 2.2 | 1.5 | 5.86x | 1.6 | 1.2 | ~80% |
| Gravity particles x32 | 58.0 | 19.6 | 10.7 | 5.44x | 13.3 | 7.1 | ~66% |
| ChaCha20 block core | 28.8 | 3.5 | 2.0 | 14.06x | 3.6 | 1.9 | ~95% |

## API

> Also [available in the docs](https://github.com/TheLuaOSProject/LuaJITMT/blob/simd/doc/ext_ffi_simd.html)

Alongside regular FFI vector operators, the `ffi.simd` module provides
operations that ordinary Lua syntax cannot express:

```lua
local ffi = require("ffi")
local simd = require("ffi.simd")
```

`ffi` must be loaded first. Vector types are declared by the program, the module does not install names such as `float4` or `i32x4` globally. The examples below use these declarations:

```lua
ffi.cdef [[
    typedef float    float4  __attribute__((vector_size(16)));
    typedef double   double2 __attribute__((vector_size(16)));
    typedef int8_t   i8x16   __attribute__((vector_size(16)));
    typedef uint8_t  u8x16   __attribute__((vector_size(16)));
    typedef int16_t  i16x8   __attribute__((vector_size(16)));
    typedef uint16_t u16x8   __attribute__((vector_size(16)));
    typedef int32_t  i32x4   __attribute__((vector_size(16)));
    typedef uint32_t u32x4   __attribute__((vector_size(16)));
    typedef int64_t  i64x2   __attribute__((vector_size(16)));
    typedef uint64_t u64x2   __attribute__((vector_size(16)));

    typedef float    float8  __attribute__((vector_size(32)));
    typedef int32_t  i32x8   __attribute__((vector_size(32)));
]]

local float4 = ffi.typeof("float4")
local double2 = ffi.typeof("double2")
local i8x16 = ffi.typeof("i8x16")
local u8x16 = ffi.typeof("u8x16")
local i16x8 = ffi.typeof("i16x8")
local u16x8 = ffi.typeof("u16x8")
local i32x4 = ffi.typeof("i32x4")
local u32x4 = ffi.typeof("u32x4")
local i64x2 = ffi.typeof("i64x2")
local u64x2 = ffi.typeof("u64x2")
local float8 = ffi.typeof("float8")
local i32x8 = ffi.typeof("i32x8")
```

Vector constructors accept one value per lane. A one-argument constructor splats that value into every lane:

```lua
local a = float4(1, 2, 3, 4)
local b = float4(2)             -- { 2, 2, 2, 2 }
local c = a * b + 1             -- { 3, 5, 7, 9 }
```

### Common rules

- Lane indices are 0 based, unlike Lua tables which are 1 based.
- A supported vector has at least two lanes, a power-of-two total size of at most 64 bytes, and plain numeric elements of 1, 2, 4, or 8 bytes.
- Vector lanes are immutable. Reading `v[i]` is allowed, but `v[i] = x` raises an error. Use `simd.insert(v, i, x)` to produce an updated copy (see below).
- For binary operations, the first operand is a vector. Later operands may be a vector of exactly the same ctype or a Lua number/scalar cdata. Scalars are converted using the normal FFI rules and splatted into every lane.
- Comparison functions return a signed integer mask with the same lane count and lane width as the input. A true lane contains all one bits; a false lane contains all zero bits.
- The interpreter supports all valid vectors. Th JIT backend is complete for 16-byte vectors and supports full 32 byte vectors on AVX2. An unsupported native path exits the trace and continues in the interpreter with the same result.

Regular operators already provide lane-wise `+`, `-`, `*`, floating-point `/`, and unary `-`. Integer multiplication returns the low half of each lane's product. Regular `a == b` does an entire vector comparison returning one boolean, so use `simd.eq(a, b)` for a lane mask.

### Bitwise operations

Bitwise operations work on every vector element type, including `float` and `double`. On floating-point vectors they operate on the raw IEEE bit patterns.

#### `simd.band(a, b)`

Returns the lane-wise bitwise AND, `a & b`.

```lua
local r = simd.band(u32x4(0xff, 0x0f, 0xaa, 0x55), 0x0f) -- r is { 0x0f, 0x0f, 0x0a, 0x05 }
```

#### `simd.bor(a, b)`

Returns the lane-wise bitwise OR, `a | b`.

```lua
local r = simd.bor(u32x4(0x10, 0x20, 0x40, 0x80), 3) -- r is { 0x13, 0x23, 0x43, 0x83 }
```

#### `simd.bxor(a, b)`

Returns the lane-wise bitwise exclusive OR, `a ^ b`.

```lua
local r = simd.bxor(u32x4(0xff), 0x0f) -- every lane is 0xf0
```

#### `simd.bandn(a, b)`

Returns `(~a) & b`. The operand order matters.

```lua
local r = simd.bandn(u32x4(0x0f), u32x4(0x33)) -- every lane is 0x30
```

#### `simd.bnot(a)`

Returns the lane-wise bitwise complement, `~a`.

```lua
local r = simd.bnot(u32x4(0, 0xff, 0xffff, 0xffffffff)) -- r is { 0xffffffff, 0xffffff00, 0xffff0000, 0 }
```

### Shifts

Shifts accept integer vectors only. The count can be a scalar integer or an integer vector with the same lane width and count as the value. A vector count shifts each lane independently.

Counts are interpreted as unsigned. A count at least as large as the lane width produces zero for `shl` and `shr`, or a complete sign fill for `sar`.

#### `simd.shl(a, n)`

Shifts each lane left, filling low bits with zero.

```lua
local r = simd.shl(u32x4(1, 2, 3, 4), 2) -- r is { 4, 8, 12, 16 }

local per_lane = simd.shl(
  u32x4(1, 1, 1, 1),
  u32x4(0, 1, 2, 31)
)
-- per_lane is { 1, 2, 4, 0x80000000 }
```

#### `simd.shr(a, n)`

Logically shifts each lane right, always filling high bits with zero.

```lua
local r = simd.shr(u32x4(0x80000000, 16, 7, 1), 1) -- r is { 0x40000000, 8, 3, 0 }
```

#### `simd.sar(a, n)`

Arithmetically shifts each lane right, replicating its sign bit.

```lua
local r = simd.sar(i32x4(-8, -1, 8, 1), 2) -- r is { -2, -1, 2, 0 }
```

### Comparisons

All comparison functions are lane-wise. Integer comparisons use the input ctype's signedness. Floating-point `ne` is unordered, so it is true if either operand is NaN; the other five floating-point comparisons are ordered and therefore false for a NaN lane.

The examples in this section use:

```lua
local a = i32x4(1, 4, 3, 8)
local b = i32x4(2, 4, 1, 9)
```

#### `simd.eq(a, b)`

Returns a mask for `a == b`.

```lua
local m = simd.eq(a, b)
assert(simd.movemask(m) == 0x2) -- only lane 1 is true
```

This differs from ordinary `a == b`, which returns one boolean for the whole
vector.

#### `simd.ne(a, b)`

Returns a mask for `a ~= b`.

```lua
local m = simd.ne(a, b)
assert(simd.movemask(m) == 0xd) -- lanes 0, 2, and 3
```

#### `simd.lt(a, b)`

Returns a mask for `a < b`.

```lua
local m = simd.lt(a, b)
assert(simd.movemask(m) == 0x9) -- lanes 0 and 3
```

#### `simd.le(a, b)`

Returns a mask for `a <= b`.

```lua
local m = simd.le(a, b)
assert(simd.movemask(m) == 0xb) -- lanes 0, 1, and 3
```

#### `simd.gt(a, b)`

Returns a mask for `a > b`.

```lua
local m = simd.gt(a, b)
assert(simd.movemask(m) == 0x4) -- lane 2
```

#### `simd.ge(a, b)`

Returns a mask for `a >= b`.

```lua
local m = simd.ge(a, b)
assert(simd.movemask(m) == 0x6) -- lanes 1 and 2
```

### Mask operations

#### `simd.select(mask, a, b)`

Selects bits using `(mask & a) | (~mask & b)`. With a comparison mask this selects `a` for true lanes and `b` for false lanes. `a` is a vector and `b` can be the same vector type or a scalar splat. The mask must have the same total size as `a`.

```lua
local v = float4(-3, 2, -1, 4)
local positive = simd.gt(v, 0)
local relu = simd.select(positive, v, 0) -- relu is { 0, 2, 0, 4 }
```

Because selection is bitwise, masks with only some bits set blend those bits rather than choosing a whole lane.

#### `simd.movemask(v)`

Returns an integer whose bit `i` is the sign bit of lane `i`.

```lua
local bits = simd.movemask(i32x4(-1, 0, -5, 6))
assert(bits == 0x5) -- sign bits are set in lanes 0 and 2
```

#### `simd.anyof(v)`

Returns `true` when at least one lane has its sign bit set. It is intended for comparison masks but it is not a general "any numeric lane is nonzero" test.

```lua
local over_limit = simd.gt(float4(2, 5, 1, 9), 8)
assert(simd.anyof(over_limit))
```

#### `simd.allof(v)`

Returns `true` when every lane has its sign bit set. It is normally used with a comparison mask.

```lua
local nonnegative = simd.ge(float4(2, 5, 0, 9), 0)
assert(simd.allof(nonnegative))
```

### Arithmetic helpers

#### `simd.min(a, b)`

Returns the smaller value in each lane, using the input ctype's signedness.

```lua
local r = simd.min(i32x4(7, -2, 10, 4), i32x4(3, 5, 10, -1)) -- r is { 3, -2, 10, -1 }
```

Floating-point `min` follows x86 `MINPS`/`MINPD` semantics exactly: the second operand wins when either operand is NaN or when the operands compare equal. For example, `simd.min(float4(0), float4(-0.0))` returns negative zero.

#### `simd.max(a, b)`

Returns the larger value in each lane, using the input ctype's signedness.

```lua
local r = simd.max(i32x4(7, -2, 10, 4), i32x4(3, 5, 10, -1)) -- r is { 7, 5, 10, 4 }
```

Floating-point `max` has the same second-operand-wins NaN/equality behavior as the x86 packed maximum instructions.

#### `simd.mulhi(a, b)`

Returns the high half of each full-width integer product. It supports signed and unsigned 8-, 16-, 32-, and 64-bit lanes, taking signedness from the vector ctype. Ordinary `a * b` returns the low half.

```lua
local a = u16x8(60000)
local lo = a * 2
local hi = simd.mulhi(a, 2)
assert(lo[0] == 54464)
assert(hi[0] == 1)
```

Together, `*` and `mulhi` expose both halves of a fixed-point or wide integer product.

#### `simd.abs(a)`

Returns the absolute value of every lane. For floating-point vectors it clears the sign bit, including the sign of zero and NaN. Unsigned integer vectors are unchanged. Signed integer minimum values wrap and remain negative because their positive counterpart is not representable.

```lua
local r = simd.abs(i32x4(-7, 2, -1, -2147483648)) -- r is { 7, 2, 1, -2147483648 }
```

#### `simd.sqrt(a)`

Returns the square root of each lane. It accepts only `float` and `double` vectors, and a negative finite input produces NaN.

```lua
local r = simd.sqrt(float4(1, 4, 9, 16)) -- r is { 1, 2, 3, 4 }
```

#### `simd.fma(a, b, c)`

Computes `a * b + c` in each floating-point lane with one IEEE 754 rounding. This can differ from the two roundings performed by ordinary `a * b + c`. Both `b` and `c` may be scalar splats.

```lua
local r = simd.fma(float4(1, 2, 3, 4), 3, 0.5) -- r is { 3.5, 6.5, 9.5, 12.5 }
```

The JIT emits a fused instruction when the CPU has FMA. Otherwise this operation leaves the trace and runs in the interpreter so its rounding never changes.

#### `simd.floor(a)`

Rounds each floating-point lane toward negative infinity.

```lua
local r = simd.floor(float4(1.9, -1.1, 2.0, -0.2)) -- r is { 1, -2, 2, -1 }
```

#### `simd.ceil(a)`

Rounds each floating-point lane toward positive infinity.

```lua
local r = simd.ceil(float4(1.1, -1.9, 2.0, -0.2)) -- r is { 2, -1, 2, 0 }
```

#### `simd.trunc(a)`

Rounds each floating-point lane toward zero.

```lua
local r = simd.trunc(float4(1.9, -1.9, 2.1, -2.1)) -- r is { 1, -1, 2, -2 }
```

#### `simd.round(a)`

Rounds each floating-point lane to the nearest integer, with halfway cases rounded to even. This is not round-away-from-zero.

```lua
local r = simd.round(float4(0.5, 1.5, 2.5, -1.5))
-- r is { 0, 2, 2, -2 }
```

All four rounding helpers quiet NaNs. `round` also preserves negative zero.

#### `simd.adds(a, b)`

Performs saturating addition on signed or unsigned 8 and 16 bit integer lanes. Wider integers and floating-point vectors are rejected.

```lua
local signed = simd.adds(i8x16(120), 20)
local unsigned = simd.adds(u8x16(250), 20)
assert(signed[0] == 127)
assert(unsigned[0] == 255)
```

#### `simd.subs(a, b)`

Performs saturating subtraction on signed or unsigned 8- and 16-bit integer lanes.

```lua
local signed = simd.subs(i8x16(-120), 20)
local unsigned = simd.subs(u8x16(10), 20)
assert(signed[0] == -128)
assert(unsigned[0] == 0)
```

### Horizontal reductions

Reductions return one value of the vector's element ctype. They use a fixed pairwise halving tree rather than a left-to-right fold, keeping interpreted and JIT-compiled floating-point/NaN results identical. Integer sums wrap at the element width just like packed addition.

#### `simd.hsum(v)`

Returns the sum of all lanes.

```lua
local total = simd.hsum(i32x4(1, 2, 3, 4))
assert(total == 10)
```

A typical vectorized dot-product ends with one horizontal sum:

```lua
local acc = float4(0)
for i = 0, count - 1 do
  acc = acc + lhs[i] * rhs[i]
end
local dot = simd.hsum(acc)
```

#### `simd.hmin(v)`

Returns the smallest lane, using the same integer signedness and floating-point edge behavior as `simd.min`.

```lua
local smallest = simd.hmin(i32x4(7, -3, 12, 0))
assert(smallest == -3)
```

#### `simd.hmax(v)`

Returns the largest lane, using the same integer signedness and floating-point edge behavior as `simd.max`.

```lua
local largest = simd.hmax(i32x4(7, -3, 12, 0))
assert(largest == 12)
```

### Lane construction and permutation

#### `simd.insert(v, index, value)`

Returns a copy of `v` with one lane replaced. `index` is zero-based and must be in range. `value` is converted to the element type using normal FFI rules. The original vector is unchanged.

```lua
local v = i32x4(10, 20, 30, 40)
local changed = simd.insert(v, 2, 99)
-- changed is { 10, 20, 99, 40 }, v is still { 10, 20, 30, 40 }
```

#### `simd.shuffle(v, i0, ...)`

Returns a permutation of one vector. Supply exactly one zero-based index per output lane; each constant/numeric index must be in range. Output lane `k` receives the input lane selected by argument `ik`.

```lua
local v = i32x4(10, 20, 30, 40)
local reversed = simd.shuffle(v, 3, 2, 1, 0)
-- reversed is { 40, 30, 20, 10 }
```

`shuffle` also accepts one runtime integer index vector with the same lane width and count as `v`. Each raw index is interpreted as unsigned and reduced modulo the lane count, so negative and out-of-range values are valid:

```lua
local v = i32x4(10, 20, 30, 40)
local indices = i32x4(3, 0, 6, -1) -- indices modulo 4 are { 3, 0, 2, 3 }
local selected = simd.shuffle(v, indices) -- selected is { 40, 10, 30, 40 }
```

#### `simd.shuffle2(a, b, i0, ...)`

Permutes the conceptual concatenation of two vectors of exactly the same ctype. Supply one zero-based index per output lane. For vectors with `n` lanes, indices `0..n-1` select from `a`, while `n..2n-1` select from `b`.

```lua
local a = i32x4(1, 2, 3, 4)
local b = i32x4(10, 20, 30, 40)
local zipped_low = simd.shuffle2(a, b, 0, 4, 1, 5) -- zipped_low is { 1, 10, 2, 20 }
```

### Type conversion

#### `simd.bitcast(ctype, v)`

Reinterprets the bits of `v` as another vector ctype with exactly the same total byte size. No numeric conversion takes place. Pass a ctype returned by `ffi.typeof` (an existing cdata value is also accepted as a type source).

```lua
local bits = simd.bitcast(u32x4, float4(1.0, -0.0, 0.0, 2.0))
assert(bits[0] == 0x3f800000)
assert(bits[1] == 0x80000000)
```

This is useful for examining IEEE representations and for explicitly changing the lane shape of masks or lookup data.

#### `simd.convert(ctype, v)`

Numerically converts every lane to the destination vector ctype. Source and destination must have the same lane count.

```lua
local ints = simd.convert(i32x4, float4(1.9, -1.9, 2.1, -2.1)) -- ints is { 1, -1, 2, -2 }
```

Integer-to-integer conversion sign or zero extends or truncates like C. Integer-to-floating-point conversion rounds normally. Floating-point to integer truncates toward zero. NaN or a value outside the destination's signed range yields the integer-indefinite bit pattern: the minimum signed value of that width, including for an unsigned destination.

The JIT compiles every equal-lane conversion for which both vectors are 16 or 32 bytes, but a 32-byte side requires AVX2. Valid 8 or 64 byte conversions remain available in the interpreter.

### Introspection

#### `simd.isvector(x)`

Returns whether `x` is a supported vector cdata value or vector ctype. Scalars, arrays, pointers, tables, and ordinary Lua values return `false`.

```lua
assert(simd.isvector(float4))
assert(simd.isvector(float4(1)))
assert(not simd.isvector(ffi.typeof("float")))
assert(not simd.isvector(ffi.new("float[4]")))
```

#### `simd.lanes(x)`

Returns a vector value or ctype's lane count. It raises a type error for a non vector.

```lua
assert(simd.lanes(float4) == 4)
assert(simd.lanes(i8x16(0)) == 16)
```

#### `simd.elementtype(x)`

Returns the FFI ctype of one vector lane.

```lua
local element = simd.elementtype(float4)
assert(element == ffi.typeof("float"))
```

#### `simd.features()`

Returns a new table describing the CPU features detected at runtime:

| Field | Meaning |
| --- | --- |
| `sse2` | SSE2 instructions are available |
| `sse3` | SSE3 instructions are available |
| `ssse3` | SSSE3 instructions are available |
| `sse4_1` | SSE4.1 instructions are available |
| `sse4_2` | SSE4.2 instructions are available |
| `avx` | AVX instructions and OS vector-state support are available |
| `avx2` | AVX2 instructions are available |
| `fma` | FMA3 instructions are available |
| `vecsize` | Maximum vector width compiled natively: `32`, `16`, or `0` bytes |

```lua
local cpu = simd.features()

if cpu.avx2 and cpu.vecsize >= 32 then
  -- Hot loops using 32-byte vectors can compile to YMM/AVX2 code.
  local zero = float8(0)
end
```

Use feature detection only when choosing an algorithm or vector width. Normal code does not need to guard individual operations: an unavailable packed instruction simply prevents that operation from being compiled into a trace.

### Loads and stores

Loads and stores use ordinary FFI pointers; there is no separate module API. Vector memory access is unaligned-safe:

```lua
local values = ffi.new("float[8]", {1, 2, 3, 4, 5, 6, 7, 8})
local vectors = ffi.cast(ffi.typeof("$ *", float4), values)

local first = vectors[0]       -- {1, 2, 3, 4}
vectors[1] = vectors[1] * 2    -- stores {10, 12, 14, 16}
```

Vectors may be passed to and returned from C functions by value. FFI callbacks with by-value vector arguments/results are supported for 8- and 16-byte vectors on x86-64 System V. Windows x64, x86, and 32-byte-or-wider callback signatures are rejected, use pointers for those cases.

## AI usage

This was developed with extensive help with Claude Opus 5 and GPT 5.6 Sol. (This release was written by a human though! 😄)

README for LuaJIT 2.1
---------------------

LuaJIT is a Just-In-Time (JIT) compiler for the Lua programming language.

Project Homepage: https://luajit.org/

LuaJIT is Copyright (C) 2005-2026 Mike Pall.
LuaJIT is free software, released under the MIT license.
See full Copyright Notice in the COPYRIGHT file or in luajit.h.

Documentation for LuaJIT is available in HTML format.
Please point your favorite browser to:

 doc/luajit.html

