# SIMD design notes

## Goal

Make FFI vector cdata a first-class value type in this LuaJIT fork: ordinary Lua
operators perform packed operations, hot code compiles to real x86-64 SSE
instructions, and everything that cannot be spelled with a Lua operator lives in
`require("ffi.simd")`.

## D1. Vector types are the *existing* FFI vector ctypes

LuaJIT already parses `__attribute__((vector_size(N)))` (`lj_cparse.c`) and
represents the result as `CT_ARRAY | CTF_VECTOR` with an interned element ctype
(`ctype_isvector()`).  We build on that exclusively.  No second type system, no
new syntax, no new literals.

A vector ctype is *supported* by the SIMD machinery when

  * the element ctype is a plain number ctype (`CT_NUM`, not bool, not enum,
    not bitfield),
  * the element size is 1, 2, 4 or 8 bytes,
  * `float` implies size 4, `double` implies size 8,
  * total size is a power of two and `>= 2 * elemsize` (i.e. >= 2 lanes).

`lj_ctype.c:lj_ctype_vecinfo()` is the single place that classifies a ctype and
yields `{elemsize, lanes, kind}` where kind is one of the `VECK_*` codes in
`lj_ctype.h`.

## D2. Vectors are immutable values

Stock LuaJIT already marks vector (and complex) elements const:
`lj_cdata.c:lj_cdata_index()` ORs `CTF_CONST` into the qualifiers of an indexed
vector element ("Valarray elements are constant").  `v[i] = x` therefore raises
`attempt to write to constant location`.

**We keep that.**  Reasons:

  * It preserves existing observable behaviour (constraint: do not change
    behaviour of programs that already exist).
  * Immutability is what makes vectors behave as SSA values: a vector load from
    a cdata payload is a *pure* load, so it CSEs, hoists out of loops and never
    needs alias analysis against lane stores.
  * It matches the way LuaJIT already treats 64-bit integer cdata: a boxed,
    immutable payload that the JIT keeps unboxed in a register.

Lane insertion is therefore a *functional* operation and lives in `ffi.simd`
(`simd.insert(v, i, x)` returns a new vector), which is exactly the kind of
operation constraint 7 reserves for the module.

## D3. Value model in the JIT: "a vector is a wide int64 cdata"

The single most important design decision.  LuaJIT already has a complete,
battle-tested pipeline for *boxed immutable scalar payloads*: 64-bit integer
cdata.

    cdata ref  --FLOAD/ADD+XLOAD-->  raw value in a register
    raw value  --IR_CNEWI-->         freshly boxed cdata
    IR_CNEWI   --lj_opt_sink-->      elided in loops, rebuilt on exit by
                                     lj_snap.c:snap_unsink()

We reuse that pipeline verbatim, only wider:

  * `IR_XLOAD` with a vector IR type loads 16 bytes from the cdata payload.
  * `IR_CNEWI` with a vector operand allocates a cdata and stores 16 bytes.
  * `lj_opt_sink.c` sinks those allocations unchanged (it is type agnostic).
  * `lj_snap.c:snap_unsink()` re-materialises a sunk vector box at a side exit.

Consequences:

  * Raw vector values never appear in a Lua stack slot, so snapshots only ever
    reference *boxed* or *sunk* vectors.  The only new snapshot requirement is
    that `snap_unsink()` can read a full 128-bit register out of the exit state
    -> `ExitState.fpr[]` has to become 16 bytes per register (see D7).
  * PHIs, spills, reloads, renames and rematerialisation all work through the
    generic paths once the register class and slot size are right.

## D4. IR representation

IR *type* space is the scarce resource (`IRT_TYPE` is 5 bits, 24/32 used, "room
for 8 more").  IR *opcode* space is cheap (~110/255 used).  So:

  * 6 new IR types encode **lane width + float/int only**:
    `IRT_V16I8, IRT_V8I16, IRT_V4I32, IRT_V2I64, IRT_V4F32, IRT_V2F64`.
    All have size 16 and live in FP registers.
  * Signedness is *not* in the IR type.  Where an operation depends on it, the
    recorder (which still has the CType) picks a different opcode
    (`IR_VMIN`/`IR_VMINU`, `IR_VSHR`/`IR_VSAR`, ...) or biases the operands
    (unsigned compares).  This is safe because the bit patterns of `int32_t[4]`
    and `uint32_t[4]` are identical, so CSE across signedness is *correct*.

New IR opcodes (all pure/`N`/`C`, none guarded):

    VSPLAT  ref ___   broadcast a scalar to all lanes
    VADD    ref ref   VSUB VMUL VDIV
    VAND    ref ref   VOR VXOR VANDN(~a & b)
    VSHL    ref ref   VSHR VSAR      (op2 = uniform INT shift count)
    VMIN    ref ref   VMAX VMINU VMAXU
    VCMPEQ  ref ref   VCMPGT VCMPGE  (result = all-ones/all-zero mask)
    VSQRT   ref ___
    VABS    ref ___
    VROUND  ref lit   (SSE4.1 rounding mode)
    VSHUF   ref lit   (32-bit lane permute, imm8)
    VSHUFB  ref ref   (SSSE3 byte permute by mask vector)
    VUNPKL  ref ref   VUNPKH  (interleave, used by shuffle2/convert)
    VCONV   ref lit   (lane conversion, see D6)
    VEXTRACT ref lit  (lane -> scalar)
    VMOVMSK ref ___   (lane sign bits -> INT)
    VADDS   ref ref   VSUBS VADDSU VSUBSU (saturating, 8/16-bit lanes)
    KVEC    cst ___   (128-bit constant, interned in a new k128 pool)

Deliberately *not* IR ops, because they lower to the above:

  * unary minus  -> `VSUB(0,v)` (int) / `VXOR(v, signmask)` (float)
  * select/blend -> `VOR(VAND(m,a), VANDN(m,b))` (SSE2, 3-operand-free)
  * insert       -> `select(lanemask, VSPLAT(x), v)`
  * `a ~= b`, `a <= b` on masks -> combinations of VCMPEQ/VCMPGT + VXOR
  * horizontal reductions -> shuffle + op chains, then `VEXTRACT`

## D5. Operator semantics (what ordinary Lua does)

| Lua               | vector meaning                                          |
|-------------------|---------------------------------------------------------|
| `a + b`           | lane-wise add, both operands the same vector ctype      |
| `a - b`           | lane-wise sub                                           |
| `a * b`           | lane-wise mul                                           |
| `a / b`           | lane-wise div, **floating-point vectors only**          |
| `-a`              | lane-wise negate                                        |
| `a == b`          | **whole-vector** equality -> boolean                    |
| `a ~= b`          | negation of the above                                   |
| `v[i]`            | read lane i (existing FFI array indexing, unchanged)    |
| `v[i] = x`        | error, unchanged (D2)                                   |
| `V(x)`            | splat, `V(a,b,..)` element-wise init (existing FFI)     |
| `a < b`, `a <= b` | error (a boolean is not a useful answer) -> `simd.lt`   |
| `a % b`, `a ^ b`  | error (no packed semantics worth inventing)             |
| `#v`              | unchanged (error) -> `simd.lanes(v)`                    |

`==` must return a boolean because the VM coerces the `__eq` result to one, so
lane-wise comparison *cannot* be expressed by an operator; it lives in
`ffi.simd` and returns a mask vector.

Mixed operands:

  * vector `op` vector: the two raw ctypes must be identical, else error
    (`attempt to perform arithmetic`).  Interned ctypes make this an id compare.
  * vector `op` scalar (Lua number, or any cdata that converts to the element
    type): the scalar is converted to the element type with the ordinary
    `lj_cconv_ct_tv` rules and then splatted.  Result ctype = the vector ctype.
  * scalar `op` vector: same, operand order preserved.
  * `/` with an integer vector, `%`, `^`: error.

Rationale for splatting rather than rejecting: it is the established behaviour
of every C vector extension and of every SIMD language, it needs no new
coercion rule (it reuses `lj_cconv`), and it cannot affect non-vector programs.

Lane-count/element mismatches never silently reinterpret: `float4 + int4` is an
error, `simd.bitcast` / `simd.convert` are explicit.

## D6. Conversions

  * `simd.bitcast(ct, v)` - reinterpret the 16 bytes, requires equal sizes.
  * `simd.convert(ct, v)` - numeric lane conversion.  Supported pairs are the
    ones with a direct packed instruction:
      f32x4 <-> i32x4/u32x4 (truncating, C semantics), f64x2 <-> i32 (low 2),
      f32x4 <-> f64x2 (low 2 lanes), and integer widen/narrow between
      neighbouring lane widths.
    Everything else raises a clear error instead of silently scalarising.
  * `V(x)` construction from a Lua number uses the *existing* FFI conversion.

## D7. `ExitState` becomes 128-bit wide

`lj_target_x86.h:ExitState.fpr` was `lua_Number fpr[16]` and the VM exit handler
saved each xmm with `movsd`.  A sunk vector box needs all 128 bits at exit
time, so `fpr` becomes `ExitFPR fpr[16]` (16 bytes each, `movups` in
`vm_x86.dasc`).  This is an internal VM/JIT ABI change only; it is not visible
through any public API.  `jit.attach("texit")` register dumps keep reporting the
low `double` of each register, so that Lua-visible interface is unchanged.

The 32-bit x86 exit handler is updated symmetrically so the shared C code has a
single layout, but vectors are only ever created on x64 (see D8).

## D8. Target gating

  * Vector IR is only ever emitted when `LJ_TARGET_X86ORX64 && LJ_64` and the
    CPU has SSE2 (guaranteed on x86-64).
  * `lj_ctype_vecinfo()` accepts a vector ctype on every target, so the
    *interpreter* semantics are identical everywhere; only the JIT is gated.
    On a non-x64 target the recorder raises `LJ_TRERR_NYIVEC` and the trace
    aborts - the program still runs, with identical results.
  * SSE3/SSSE3/SSE4.1 instructions are used only behind the existing
    `JIT_F_SSE3`/`JIT_F_SSE4_1` runtime flags; new `JIT_F_SSSE3` is added.
    Where an SSE2 fallback sequence exists we emit it; where it does not
    (`simd.round`, byte shuffles) the recorder aborts with NYI on old CPUs and
    the interpreter fallback produces the same values.

## D9. Widths

128-bit vectors are the JIT-supported width.  That is the width guaranteed by
the x86-64 baseline (SSE2) and it maps 1:1 onto LuaJIT's existing FP register
class without a second register class or a variable-width spill area.

Other widths (`vector_size(8)`, `vector_size(32)`, ...) keep working in the
interpreter with identical semantics; the recorder reports NYI, so hot loops
using them stay interpreted.

AVX2 is available on the v3 target, so 256-bit is no longer blocked on the
instruction set. What blocks it is the IR type encoding: `IRT_TYPE` is 5 bits,
24 slots were already taken, the six 128-bit vector types take 24..29 and only
two slots are left. A 256-bit group needs six more, so it needs a different
encoding for the vector types (splitting the size out of `irt_type()`, or a
six bit type field, which currently collides with `IRT_MARK`/`IRT_ISPHI`/
`IRT_GUARD`). On top of that it needs a 32-byte-per-register `ExitState`, eight
spill slots per value, and `vzeroupper` discipline, because VEX-256 leaves the
upper YMM state dirty and every legacy SSE instruction after it then pays a
transition penalty. That is a separate, invasive change and is deliberately
not attempted here.

Note that the *128-bit* VEX forms this backend emits do not have that problem:
VEX-128 zeroes the upper YMM half, so mixing them with the legacy SSE
loads/stores and with the interpreter's own SSE code leaves the state clean and
costs nothing. No `vzeroupper` is needed anywhere.

## D12. Shifts without an instruction are rewritten, never scalarised

x86 has no 8-bit packed shift at all, and no 64-bit packed arithmetic shift
right before AVX-512. Both are rewritten by the *recorder* into packed
sequences rather than being scalarised or left to the interpreter:

  * 8-bit: shift the 16-bit lanes and then clear the bits that crossed the
    byte boundary. The mask is `(0xff << n) & 0xff` for a left shift and
    `0xff >> n` for a right shift, which is naturally zero for an out of range
    count -- the same answer the interpreter gives. `sar` additionally applies
    the usual `(x^s)-s` sign fill with `s = 0x80 >> n`.
  * 64-bit `sar`: `((x >>u n) ^ m) - m` with `m = (1<<63) >>u n`.

With a *constant* count every mask is a constant and folds away. With a
*variable* count the masks are built at runtime from the same count:

  * The byte masks come from shifting a constant `0x00ff`/`0x0080` in 16-bit
    lanes and broadcasting the low byte into both halves of the lane with
    `PMULLW` by `0x0101` -- `v * 0x0101 == v | (v << 8)` for `v <= 0xff`, it
    cannot carry, and it needs only SSE2. `PSHUFB` would be one instruction
    instead of two but would drag in an SSSE3 dependency for a case that does
    not otherwise need one.
  * `m` for the 64-bit `sar` comes from a `PSRLQ` of a constant sign-bit
    vector.

The counts differ in one important way. The 16-bit shift used for the 8-bit
rewrite flushes to zero exactly when the byte shift should, so no clamping is
needed there. The 64-bit `sar` *must* clamp: an unclamped `PSRLQ` with a count
of 64 or more would flush `m` to zero as well and lose the sign fill, which
the interpreter does not do. The clamp is branchless and guard-free, so the
trace stays valid for every count:

```
d  = n & ~lim            /* non-zero iff n < 0 or n > lim */
m  = (d | -d) >>a 31     /* all ones iff d != 0           */
nc = (n | m) & lim
```

A guard would have been shorter but would specialise the trace on the count
range and re-trace whenever a loop walked past it. The clamp costs six GPR
instructions, all outside the vector unit.

Both rewrites apply the interpreter's own definition of an out of range
count: `lj_simd_shift()` reads the count as `uint32_t`, so a negative count is
a very large one, logical shifts flush to zero and arithmetic shifts fill with
the sign bit.

## D13. The branch is based on upstream LuaJIT, not on this repo's `v2.1`

Worth stating explicitly, because it is not visible from the code and it
determines where this work can land.

`origin` is `TheLuaOSProject/LuaJITMT`, whose `v2.1` is a heavily modified
LuaJIT (multithreaded VM, a different GC, restructured `lj_asm`/`lj_ctype`/
`lj_crecord`; about 118k inserted lines under `src/` relative to upstream).
The `simd` branch is *not* based on it. It was cut from `upstream/v2.1` at
`346ab587` -- pristine LuaJIT -- and `git merge-base simd origin/v2.1` is
`b925b3e3`, so the two share only pre-fork history and `origin/v2.1` carries
about 2600 commits that this branch does not.

Consequences, none of which are hidden:

  * Every claim in these notes -- interpreter/JIT agreement, the regression
    diff against a pristine build, the benchmark numbers -- is a claim about
    **upstream LuaJIT plus this branch**, and was measured that way.
  * This branch is therefore not directly mergeable into `origin/v2.1`. The
    design (vector ctypes, the IR type block, the boxing/sinking value model,
    the x86-64 backend file) carries over, but the patch does not: the files
    it touches most are the ones the fork rewrote most.
  * Nothing here was written against the fork's threading model. The vector
    paths add no new global state -- `lj_simd.c` is pure, the ctype and IR
    additions live in existing per-state structures -- but that is an
    observation, not a tested claim about a multithreaded VM.

Rebasing onto `origin/v2.1` would be a re-implementation in a substantially
different codebase, not a merge, so it is deliberately not attempted here. If
the work is meant to land in the fork rather than stand alone, that is the
decision to revisit first, before any further feature work.

## D11. x86-64-v3 target, but runtime feature detection

The supported target was raised to **x86-64-v3**: SSE4.2, AVX, AVX2, BMI2.
Two things follow.

*Three operand encoding.* With AVX every packed instruction is emitted in its
VEX form, so `dest = a op b` needs no `movaps dest, a` first. `emit_vexrr()`
in `lj_emit_x86.h` always uses the three byte VEX prefix, which needs no
special casing for the high registers or for the 0F 38 / 0F 3A maps, and
`emit_vrr3()` in the vector backend picks the VEX form or falls back to copy
plus SSE. An 8-bit lane multiply went from 12 instructions with four copies to
9 with none.

*Feature use stays runtime detected.* A build for v3 still runs on older CPUs:
`JIT_F_AVX`/`JIT_F_AVX2`/`JIT_F_SSE4_2`/... are probed at startup and an
operation whose instruction is missing aborts the trace and stays interpreted.
That is strictly better than a compile-time assumption and costs one predicted
branch at code generation time, not at run time. AVX detection also checks
CPUID.1:ECX.OSXSAVE and `XGETBV(0) & 6`, because using VEX when the OS has not
enabled the YMM state faults.

*FMA is deliberately not used.* `a*b+c` with FMA rounds once instead of twice,
so it would give different results from the interpreter. Interpreter/JIT
agreement outranks the extra throughput.

*64-bit lane min/max.* There is still no instruction for it in v3 (VPMINSQ is
AVX-512), so the recorder expands it to PCMPGTQ plus a three instruction
blend. That is fully packed, and it is why 64-bit min/max is no longer a NYI
row in the matrix. Equal lanes may take either side of the blend, which is
harmless because they are bit-identical.

## D10. `require("ffi.simd")`

A builtin C module (`lib_simd.c`) registered as `"ffi.simd"` the same way
`jit.util` is registered, so `require` finds it in `package.loaded`.  Every
exported function is a `LJLIB_CF` fast function with a recorder in
`lj_ffrecord.c`, so the module is traceable and natively lowered - not a Lua
shim over per-lane loops.

The module contains only what operators cannot express: bitwise ops (Lua 5.1 has
no bitwise operators and this fork does not add them), lane-wise comparisons and
masks, select, shuffles, bitcasts/converts, insert, horizontal reductions,
min/max, sqrt/abs/round, saturating arithmetic and introspection.
