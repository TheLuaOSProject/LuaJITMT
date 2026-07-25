# SIMD support matrix

Legend: **I** = interpreter, **J** = JIT (x86-64), — = not supported (clean
error), n/a = not applicable.

**Target.** The supported target is **x86-64-v3** (SSE4.2 + AVX2 + BMI2). Every
row marked J is available there. Feature use is still *runtime* detected, so
the same binary keeps working on older CPUs: an operation whose instruction is
missing aborts the trace and stays interpreted with an identical result, it is
never scalarised and never wrong. `simd.features()` reports what was found.

When AVX is present the backend emits the VEX three operand form of every
packed instruction, so no register copy is needed before a binary operation.

## Vector shapes

A ctype is a *SIMD vector* when it is an FFI vector type
(`__attribute__((vector_size(N)))`) whose element is a plain number ctype of
1, 2, 4 or 8 bytes, whose total size is a power of two, and which has at
least two lanes.

| Total size | Interpreter | JIT | Notes |
|---|---|---|---|
| 16 bytes (128-bit) | yes | yes | the supported width |
| 8 bytes | yes | no (trace aborts, NYI) | correct results, stays interpreted |
| 32 bytes and wider | yes | no (trace aborts, NYI) | needs AVX, see DESIGN D9 |
| 4 bytes / 1 lane | — | — | no packed meaning, rejected |
| `_Bool` elements | — | — | the C parser drops `vector_size` for `_Bool` |
| elements of 16 bytes | — | — | e.g. `__int128`, rejected |

## Element kinds (128-bit)

| Lua name in tests | C element | lanes | I | J |
|---|---|---|---|---|
| `float4`  | `float`    | 4  | yes | yes |
| `double2` | `double`   | 2  | yes | yes |
| `i8x16`   | `int8_t`   | 16 | yes | yes |
| `u8x16`   | `uint8_t`  | 16 | yes | yes |
| `i16x8`   | `int16_t`  | 8  | yes | yes |
| `u16x8`   | `uint16_t` | 8  | yes | yes |
| `i32x4`   | `int32_t`  | 4  | yes | yes |
| `u32x4`   | `uint32_t` | 4  | yes | yes |
| `i64x2`   | `int64_t`  | 2  | yes | yes |
| `u64x2`   | `uint64_t` | 2  | yes | yes |

## Ordinary Lua operators

Operand rules for every row: either both operands are vectors of *exactly the
same* ctype, or one operand is a vector and the other is a Lua number or a
scalar cdata that converts to the element type. The scalar is converted with
the ordinary `lj_cconv` rules and then splatted. The result ctype is always
the vector operand's ctype.

| Operator | float32 | float64 | int8/16/32/64 | uint8/16/32/64 | Notes |
|---|---|---|---|---|---|
| `a + b` | I J | I J | I J | I J | integer lanes wrap around |
| `a - b` | I J | I J | I J | I J | |
| `a * b` | I J | I J | I J | I J | low half of the product |
| `a / b` | I J | I J | — | — | integer vector division has no packed form |
| `-a`    | I J | I J | I J | I J | FP negate flips the sign bit |
| `a == b`| I J | I J | I J | I J | **whole-vector** equality -> boolean |
| `a ~= b`| I J | I J | I J | I J | |
| `a < b`, `a <= b` | — | — | — | — | use `simd.lt` / `simd.le` |
| `a % b`, `a ^ b`  | — | — | — | — | |
| `#a`, `a .. b`    | — | — | — | — | unchanged from base LuaJIT |
| `a[i]` read | I J | I J | I J | I J | ordinary FFI array indexing |
| `a[i] = x` | — | — | — | — | vectors are immutable (base LuaJIT rule) |

`==` semantics: two vectors of the same ctype compare lane by lane with `==`
(so a NaN lane is never equal, and `+0 == -0`). A vector compared with a
convertible scalar splats the scalar first. A vector compared with anything
else (nil, string, table, a different vector ctype, a non-convertible cdata)
is `false` and never raises.

## Construction and conversion

| Form | I | J | Notes |
|---|---|---|---|
| `V()` | yes | yes | all lanes zero |
| `V(x)` | yes | yes | splat, existing FFI vector-init rule |
| `V(x0, x1, ...)` | yes | yes | per-lane init, existing FFI rule |
| `V(v)` | yes | yes | copy |
| `ffi.new(V, {..})` | yes | yes | table init |
| `((V*)p)[0]` | yes | yes | load, unaligned-safe (MOVUPS) |
| `((V*)p)[0] = v` | yes | yes | store, unaligned-safe |
| struct/array members | yes | yes | 16-byte aligned by the C parser |
| `simd.bitcast(ct, v)` | yes | yes | equal total size required |
| `simd.convert(ct, v)` | yes | yes | equal lane count required |

`simd.convert` performs numeric lane conversion with C semantics (FP->int
truncates toward zero, int->int sign/zero extends or truncates). Conversions
between different lane counts are rejected; use `bitcast` for pure
reinterpretation.

## `ffi.simd`

Binary entries accept a matching vector or a splattable scalar as the second
operand, exactly like the operators.

| Function | Result | float32/64 | int lanes | uint lanes | I | J |
|---|---|---|---|---|---|---|
| `band/bor/bxor/bandn(a,b)` | same ctype | bitwise | bitwise | bitwise | yes | yes |
| `bnot(a)` | same ctype | bitwise | bitwise | bitwise | yes | yes |
| `min(a,b)`, `max(a,b)` | same ctype | yes | yes | yes | yes | yes |
| `abs(a)` | same ctype | clears sign bit | wraps at the min value | identity | yes | yes |
| `sqrt(a)` | same ctype | yes | — | — | yes | yes |
| `floor/ceil/trunc/round(a)` | same ctype | yes | — | — | yes | yes (SSE4.1) |
| `shl/shr/sar(a,n)` | same ctype | — | yes | yes | yes | yes* |
| `eq/ne/lt/le/gt/ge(a,b)` | mask | yes | yes | yes | yes | yes |
| `select(m,a,b)` | ctype of a | yes | yes | yes | yes | yes |
| `movemask(a)` | integer | yes | yes | yes | yes | yes |
| `allof(a)`, `anyof(a)` | boolean | yes | yes | yes | yes | yes |
| `adds/subs(a,b)` | same ctype | — | 8/16-bit only | 8/16-bit only | yes | yes |
| `hsum/hmin/hmax(a)` | element | yes | yes | yes | yes | yes |
| `insert(a,i,x)` | same ctype | yes | yes | yes | yes | yes (const i) |
| `shuffle(a,i...)` | same ctype | yes | yes | yes | yes | yes (const i, SSSE3) |
| `shuffle2(a,b,i...)` | same ctype | yes | yes | yes | yes | yes (const i, SSSE3) |
| `bitcast(ct,a)` | ct | yes | yes | yes | yes | yes |
| `convert(ct,a)` | ct | yes | yes | yes | yes | yes |
| `lanes/elementtype/isvector/features` | — | yes | yes | yes | yes | no: plain C functions, deliberately not fast functions (see `SIMD_STATUS.md`) |

`yes*` marks operations whose JIT lowering depends on the lane width; see
"Backend gaps" below.

Mask representation: a lane-wise comparison returns a **signed integer vector
of the same lane width and lane count** (`float4 -> i32x4`, `u16x8 -> i16x8`,
...) with all-ones for true and all-zero for false. `movemask` returns the
sign bit of each lane, bit *i* for lane *i*. `allof`/`anyof` are defined in
terms of `movemask`.

Semantics worth pinning down:

* `min(a,b)` is `a < b ? a : b` and `max(a,b)` is `a > b ? a : b`, per lane.
  For FP that is exactly MINPS/MAXPS: a NaN in either operand yields `b`, and
  `min(+0,-0)` yields `-0`.
* `ne` on FP lanes is unordered (`!(a == b)`), matching CMPNEQPS; the other
  five FP comparisons are ordered.
* Shifts take the count as *unsigned*: a count `>= lane bits` gives zero for
  `shl`/`shr` and a full sign fill for `sar`, matching the x86 packed shifts.
* `abs` on signed integers wraps for the most negative lane value (like PABS).
* Reductions use a fixed pairwise halving tree
  (`t[i] = op(t[i], t[i+n/2])`), *not* a left-to-right scan, so interpreter
  and JIT agree bit for bit for non-associative float addition and for the
  asymmetric NaN behaviour of MIN/MAX.

## Backend gaps (JIT lowers to a call-free packed sequence unless noted)

| Case | Status |
|---|---|
| `min/max` on 64-bit integer lanes | no instruction before AVX-512, so it lowers to PCMPGTQ plus a three instruction blend (still fully packed). Needs SSE4.2. |
| `sar` on 64-bit lanes | no instruction before AVX-512, so the recorder rewrites it into `(v^S)>>n - (S>>n)`; needs a **constant** count, otherwise it stays interpreted |
| `lt/le/gt/ge` on 64-bit integer lanes | requires SSE4.2 (PCMPGTQ); otherwise JIT NYI |
| `floor/ceil/trunc/round` | requires SSE4.1 (ROUNDPS); otherwise JIT NYI |
| `shuffle`/`shuffle2` with 8/16-bit lanes | requires SSSE3 (PSHUFB); otherwise JIT NYI |
| `abs` on 8/16/32-bit integer lanes | uses SSSE3 PABSB/W/D; without SSSE3 it stays interpreted |
| `abs` on 64-bit integer lanes | packed SSE2 sequence (PSRAD + PSHUFD to broadcast the sign, then `(v^m)-m`) |
| shifts on 8-bit lanes | no instruction; rewritten into a 16-bit shift plus a mask, needs a **constant** count |
| non-constant lane index in `insert`/`shuffle` | rejected at record time, stays interpreted |
| scalar **cdata** as the second operand of an `ffi.simd` binary call | stays interpreted; a Lua number operand compiles |

"JIT NYI" always means: the trace aborts with a NYI reason, the code keeps
running interpreted, and the result is identical. It never means a wrong
result and never means a silent per-lane scalarisation.

## ABI

| Case | Status |
|---|---|
| passing a 128-bit vector to a C function | supported (SysV: SSE class, one XMM register; Windows x64: by reference, per the platform ABI) |
| returning a 128-bit vector from a C function | supported |
| vectors in structs passed by value | follows the existing LuaJIT struct classification |
| vector arguments/returns in FFI **callbacks** | **rejected with a clear error.** LuaJIT's callback trampoline has never classified vector arguments or results and this work does not add that; silently passing the wrong register would be far worse. Pass a pointer to a vector instead, which works and is covered by `test_ffi_abi.lua`. |
