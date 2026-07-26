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
| 16 bytes (128-bit) | yes | yes | complete operation surface below |
| 8 bytes | yes | no (trace aborts, NYI) | correct results, stays interpreted |
| 32 bytes (256-bit) | yes | partial (AVX2) | core YMM slice; see below |
| 64 bytes and wider | yes | no (trace aborts, NYI) | AVX-512 is out of scope |
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

## Element kinds (256-bit)

All ten ordinary numeric lane kinds have interpreter support and native
32-byte loads, stores, constants, spills, snapshots, and side exits on an
AVX2 host: `float8`, `double4`, `i8x32`, `u8x32`, `i16x16`, `u16x16`,
`i32x8`, `u32x8`, `i64x4`, and `u64x4`.

The current YMM operation surface compiles:

* the complete ordinary operator surface, including all integer multiply lane
  widths, whole-vector equality, unary minus, and dynamic scalar splats;
* logical operations, comparisons/masks, select, min/max, shifts,
  abs/sqrt/rounding, FMA, saturating arithmetic, 16-bit `mulhi`, equal-size
  bitcast, and the direct 32-bit lane conversions;
* unaligned loads/stores, loop-carried values, 32-byte spills, and reconstruction
  of sunk boxes from a trace exit.

Reductions, `insert`, `shuffle`, and `shuffle2` still abort the trace with
`NYIVEC` and run interpreted. This is a staged cross-lane boundary, not silent
128-bit truncation.

## Ordinary Lua operators (complete 128-bit surface)

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

`simd.convert` performs numeric lane conversion. Conversions between different
lane counts are rejected; use `bitcast` for pure reinterpretation.

* **integer -> integer**: sign- or zero-extends, or truncates, like C.
* **integer -> float**: exact where the value fits, otherwise correctly
  rounded.
* **float -> integer**: truncates toward zero, and yields the *integer
  indefinite* value (the minimum signed value of the destination width) for a
  NaN or for anything outside the destination's **signed** range. That is
  exactly what `CVTTPS2DQ` and the rest of the packed conversions do, and
  since there is no packed instruction that converts to unsigned, unsigned
  destinations get the same signed truncation. Choosing anything else would
  either be undefined C behaviour or would force the operation to differ
  between the interpreter and the JIT.

For 256-bit types, zero/multi-lane construction, dynamic splats, copies,
constants, memory loads/stores, and equal-size `bitcast` are native.

Rounding (`floor`/`ceil`/`trunc`/`round`) returns a NaN operand **quieted**,
which is what `ROUNDPS`/`ROUNDPD` do. The reference implementation handles NaN
lanes explicitly, because libm's `floor()` need not quiet a signalling NaN.

## `ffi.simd` (complete 128-bit surface)

Binary entries accept a matching vector or a splattable scalar as the second
operand, exactly like the operators.

| Function | Result | float32/64 | int lanes | uint lanes | I | J |
|---|---|---|---|---|---|---|
| `band/bor/bxor/bandn(a,b)` | same ctype | bitwise | bitwise | bitwise | yes | yes |
| `bnot(a)` | same ctype | bitwise | bitwise | bitwise | yes | yes |
| `min(a,b)`, `max(a,b)` | same ctype | yes | yes | yes | yes | yes |
| `mulhi(a,b)` | same ctype | — | 8/16/32-bit | 8/16/32-bit | yes | 16-bit only |
| `abs(a)` | same ctype | clears sign bit | wraps at the min value | identity | yes | yes |
| `sqrt(a)` | same ctype | yes | — | — | yes | yes |
| `fma(a,b,c)` | same ctype | yes | — | — | yes | yes (FMA) |
| `floor/ceil/trunc/round(a)` | same ctype | yes | — | — | yes | yes (SSE4.1) |
| `shl/shr/sar(a,n)` | same ctype | — | yes | yes | yes | yes* |
| `shl/shr/sar(a,nvec)` | same ctype | — | yes | yes | yes | 32/64-bit lanes only (AVX2) |
| `eq/ne/lt/le/gt/ge(a,b)` | mask | yes | yes | yes | yes | yes |
| `select(m,a,b)` | ctype of a | yes | yes | yes | yes | yes |
| `movemask(a)` | integer | yes | yes | yes | yes | yes |
| `allof(a)`, `anyof(a)` | boolean | yes | yes | yes | yes | yes |
| `adds/subs(a,b)` | same ctype | — | 8/16-bit only | 8/16-bit only | yes | yes |
| `hsum/hmin/hmax(a)` | element | yes | yes | yes | yes | yes |
| `insert(a,i,x)` | same ctype | yes | yes | yes | yes | yes |
| `shuffle(a,i...)` | same ctype | yes | yes | yes | yes | yes (const i, SSSE3) |
| `shuffle(a,idxvec)` | same ctype | yes | yes | yes | yes | yes (SSSE3) |
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
* `fma(a,b,c)` is `a*b + c` with a **single** rounding, per lane, for
  floating-point vectors only. The interpreter uses C99 `fma`/`fmaf`, which
  are the IEEE 754 fusedMultiplyAdd and correctly rounded, and the JIT emits
  `VFMADD213PS`/`PD`, which computes the same thing. Without the FMA feature
  the trace aborts and the interpreter still gives the single-rounded
  result, so the two never disagree. Integer lanes are rejected: there is no
  packed integer FMA and an integer `a*b+c` is already exact.
* `mulhi(a,b)` is the **high** half of the full-width lane product; `a*b` is
  the low half. Together they are the exact product, which is what the test
  checks. Signedness comes from the lane kind, as it does for min/max and the
  shifts. 64-bit lanes are rejected: that would need a 128-bit product.
* `abs` on signed integers wraps for the most negative lane value (like PABS).
* `shl`/`shr`/`sar` also accept a **vector** count with the same lane width
  and lane count, shifting each lane by its own amount. The per-lane count is
  read as unsigned of the lane width and follows exactly the same
  out-of-range rule as a scalar count, which is also what the AVX2
  instructions do natively.
* `shuffle(a, idxvec)` takes one index per lane from a **signed or unsigned
  integer vector of the same lane width and lane count**. Each index is
  reduced modulo the lane count (`idx & (lanes-1)`), so every value, including
  a negative or huge one, selects a real lane. No guard and no error: this
  mirrors the existing constant-index rule and keeps the operation total.
* Reductions use a fixed pairwise halving tree
  (`t[i] = op(t[i], t[i+n/2])`), *not* a left-to-right scan, so interpreter
  and JIT agree bit for bit for non-associative float addition and for the
  asymmetric NaN behaviour of MIN/MAX.

## Backend gaps (JIT lowers to a call-free packed sequence unless noted)

| Case | Status |
|---|---|
| `min/max` on 64-bit integer lanes | no instruction before AVX-512, so it lowers to PCMPGTQ plus a three instruction blend (still fully packed). Needs SSE4.2. |
| `sar` on 64-bit lanes | no instruction before AVX-512, so the recorder rewrites it into `((v>>n)^m)-m` with `m = (1<<63)>>n`. Constant and variable counts are both packed; for a variable count `m` is built at runtime and the count is clamped to 63 with six branchless GPR instructions |
| `lt/le/gt/ge` on 64-bit integer lanes | requires SSE4.2 (PCMPGTQ); otherwise JIT NYI |
| `floor/ceil/trunc/round` | requires SSE4.1 (ROUNDPS); otherwise JIT NYI |
| `fma` | requires the FMA feature; otherwise JIT NYI. The backend picks between VFMADD132/213/231 so the operand that has to live in the destination is already there, see `SIMD_DESIGN.md` D18. Ordinary `a*b+c` written with operators is **never** fused into it, because that would round once where the interpreter rounds twice |
| `shuffle`/`shuffle2` with 8/16-bit lanes | requires SSSE3 (PSHUFB); otherwise JIT NYI |
| `mulhi` on 8 and 32-bit lanes | no instruction: 8-bit has none at all, and 32-bit would need two PMULDQ plus shuffles to reassemble the four high halves. Both stay interpreted with identical results |
| `abs` on 8/16/32-bit integer lanes | uses SSSE3 PABSB/W/D; without SSSE3 it stays interpreted |
| `abs` on 64-bit integer lanes | packed SSE2 sequence (PSRAD + PSHUFD to broadcast the sign, then `(v^m)-m`) |
| shifts on 8-bit lanes | no instruction; rewritten into a 16-bit shift plus a byte mask. Constant and variable counts are both packed; for a variable count the mask is built with the same shift applied to a constant and broadcast across the byte halves with `PMULLW` by `0x0101` |
| non-constant lane index in `insert` | supported: the range is guarded and the lane mask is built with a packed compare against a constant vector of lane numbers |
| per-lane count **vector** in `shl`/`shr`/`sar` on 8/16-bit lanes | rejected at record time, stays interpreted. AVX2 has no per-lane shift narrower than 32 bits (VPSLLVW is AVX-512), and emulating one costs an unpack, two shifts and a pack per direction |
| per-lane count **vector** on 64-bit `sar` | no VPSRAVQ before AVX-512, so it lowers to a clamped VPSRLVQ plus the sign-bias trick: eight packed instructions, no call. The clamp is needed because VPSRLVQ flushes to zero past the lane width, which would take the sign bias with it |
| runtime index **vector** in `shuffle` | supported: `shuffle(a, idxvec)` builds the PSHUFB control at runtime. A byte vector is one PSHUFB; a wider lane costs five packed instructions (mask, scale, replicate the byte over its lane, add 0..esize-1, permute) |
| separate non-constant lane indices in `shuffle`/`shuffle2` | still rejected at record time. Assembling a control mask from N *scalar* indices costs more than it saves; pass an index vector instead |
| scalar **cdata** as the second operand of an `ffi.simd` binary call | supported: it is unboxed, converted with the ordinary FFI rules and splatted |
| 256-bit operation surface | broad direct-operation support; reductions and shuffle/insert remain staged as listed above |

"JIT NYI" always means: the trace aborts with a NYI reason, the code keeps
running interpreted, and the result is identical. It never means a wrong
result and never means a silent per-lane scalarisation.

## Inherited LuaJIT rule: non-canonical NaNs are not Lua numbers

Turning a floating-point lane into a Lua number (`v[i]`, `simd.hsum`,
`simd.hmin`, `simd.hmax`) uses the ordinary FFI conversion, which **does not
canonicalise NaNs** -- see the comment in `lj_cconv.c`:
*"Numbers are NOT canonicalized here! Beware of uninitialized data."* In GC64 a
negative NaN whose payload reaches into the top mantissa bits collides with the
type tag space, so it cannot be represented as a `TValue` at all.

This is **not specific to vectors**. Pristine LuaJIT at the base commit
`346ab587` segfaults on:

```lua
local p = ffi.new("double[1]")
ffi.cast("uint64_t *", p)[0] = 0xfff9c624cdc00000ULL
local x = p[0]
return x + 1        -- boom, in the interpreter
```

`simd.hsum(v)` on a float vector is exactly `v[0]` is exactly `doubleptr[0]`,
so it inherits the same rule, and the interpreter and the JIT agree. Such bit
patterns can only be *created* by applying bitwise operations to a
floating-point vector; the documented mask idiom (`simd.band(v, mask)` with an
all-ones/all-zero mask) cannot produce one. `test_stress.lua` therefore does
not feed bitwise results on float lanes into a scalar query, and its failure
messages print raw bytes rather than lane values.

## ABI

| Case | Status |
|---|---|
| passing a 128-bit vector to a C function | supported (SysV: SSE class, one XMM register; Windows x64: by reference, per the platform ABI) |
| returning a 128-bit vector from a C function | supported |
| vectors in structs passed by value | follows the existing LuaJIT struct classification |
| 8 and 16 byte vector arguments in FFI **callbacks** | supported on x86-64 SysV: classified like an FP argument but consuming one whole XMM register, spilling to a 16-byte-aligned stack slot once xmm0-xmm7 are used |
| 8 and 16 byte vector **results** from FFI callbacks | supported on x86-64 SysV, returned in xmm0 |
| vectors wider than one register in callbacks (e.g. 32 byte) | rejected at `ffi.cast` time with the ordinary "cannot convert" error |
| vector arguments/returns in callbacks on Windows x64, x86 and non-x86 | rejected at `ffi.cast` time. `CCALL_VECTOR_REG` is 0 there, so `callback_isvec()` is constant-false and the check degrades to the pre-existing behaviour. Pass a pointer to a vector instead. |
