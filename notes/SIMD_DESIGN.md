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
  * Cdata assignments copy a reference to the same GC object. Making a lane
    writable would therefore introduce observable aliases (`b = a; a[0] = x`)
    and could force a register-resident value back into a materialised box.
  * It matches the way LuaJIT already treats 64-bit integer cdata: a boxed,
    immutable payload that the JIT keeps unboxed in a register.

Lane insertion is therefore a *functional* operation and lives in `ffi.simd`
(`simd.insert(v, i, x)` returns a new vector), which is exactly the kind of
operation constraint 7 reserves for the module. Its temporary box is sunk, so
the functional spelling does not imply an allocation or memory round trip in
compiled code.

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

## D14. Vectors by value in FFI callbacks (x86-64 SysV only)

Callbacks used to reject vector arguments and results outright. They are now
supported on x86-64 SysV, which needed four things to agree:

  * **A wide enough save area.** `CCallback.fpr` held 8 bytes per FPR, which
    is half a vector. `FPRCBArg` is now 16 bytes on this target. It is
    deliberately *not* over-aligned: `CTState` is an ordinary GC allocation
    and nothing guarantees 16-byte placement, so the trampoline uses `movups`
    rather than constraining where `CTState` may live.
  * **A trampoline that saves it.** `->vm_ffi_callback` now stores all 16
    bytes of xmm0-xmm7, and `->cont_ffi_callback` reloads all 16 bytes of
    xmm0 for the result. The Windows x64 path keeps the 8-byte `movsd` form
    and the narrow `FPRCBArg`, because vectors are not passed in registers
    there at all.
  * **Classification that counts registers, not eightbytes.** The generic
    code derives `n` from the argument size, so a 16 byte value would claim
    *two* XMM registers. SysV gives a vector one. `CALLBACK_HANDLE_REGARG`
    now uses `n2 = isvec ? 1 : n`, mirroring what `lj_ccall.c` already does
    for the outbound direction.
  * **16-byte stack alignment.** Once xmm0-xmm7 are used a vector goes on the
    stack, and the caller aligns it to 16 bytes. `callback_conv_args` has to
    apply the same rule or it reads half of one argument and half of the
    next. This is load-bearing and has a test that fails without it.

The three parts are tied together by `LJ_STATIC_ASSERT(!CCALL_VECTOR_REG ||
sizeof(FPRCBArg) == 16)` in `lj_ccall.h`, so widening the save area and
teaching the trampoline about it cannot drift apart silently.

Everything else is gated by `CCALL_VECTOR_REG`, which is 0 on Windows x64,
x86 and every non-x86 target. There `callback_isvec()` is constant-false and
`callback_checkfunc()` rejects vectors exactly as before, with the ordinary
"cannot convert" error. A vector wider than one register (32 bytes) is
rejected on every target, including SysV.

## D15. A vector value may be stored under a different lane type

An `XSTORE` that boxes a vector is typed with the *ctype it is boxed as*, not
with the lane type the value was computed with, and the two genuinely differ:
`ffi.simd.bitcast` re-boxes a value under a new lane type by design, and a
compare produces a mask whose IR type is the operand's lane type but whose
ctype is the corresponding integer vector.

That is deliberate -- it is what lets the very next load of the box forward
and lets the allocation be sunk -- but it broke `lj_opt_fwd_xload()`, which
reacts to a load/value type mismatch by synthesising an `IR_CONV`. There is
no CONV between two vector types. `asm_conv()` would take the integer path,
allocate a **GPR** for a value that lives in an XMM register, and either trip
`emit_loadk128`'s assert or, in a release build, quietly use the wrong
register.

The fix is to forward the value unchanged when both sides are vectors. This
is sound for a specific reason: every vector IR type is 128 bits wide and they
all share one register class, so a lane type difference is a pure
reinterpretation of bits that are already in the right place -- and, by the
rule in `SIMD_STATUS.md`, no backend routine derives instruction selection
from an *operand's* type. A vector against a scalar has no such guarantee and
falls back to a reload.

The alternative -- refusing to forward and reloading -- was measured and
rejected: it puts `lj_mem_newgco` back inside every compare/select and
movemask loop, because the box can no longer be sunk. The mask pipeline
depends on this forward.

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

## D16. Per-lane shift counts and runtime permutes: extend, do not add

Both new operations reuse an existing `ffi.simd` name with a *vector* second
operand instead of introducing a new function. That is partly good API design
-- `shl(a, n)` and `shl(a, nvec)` are the same operation, differing only in
whether the count is uniform -- and partly forced: `FF__MAX` is 256 and
`GCfunc.c.ffid` is a `uint8_t`, so there were **zero** spare fast-function IDs
(see the trap note in `SIMD_STATUS.md`). Any genuinely new entry point has to
free one first.

Dispatch is on the observed argument being a cdata, both in the library and in
the recorder. That is safe to specialise on, because LuaJIT already guards the
slot's type: a later call passing a number where the trace recorded a vector
exits on the base type guard, it does not silently take the wrong path.

Two lowering notes worth keeping:

  * The AVX2 per-lane shifts are VEX-only -- there is no legacy SSE encoding
    for `VPSLLVD` -- so they must always go through the VEX emitter. The same
    opcode byte selects 32-bit lanes with `VEX.W=0` and 64-bit lanes with
    `VEX.W=1`, which is why `emit_vexrr` grew an explicit W parameter.
  * PSHUFB is byte granular, so permuting a wider lane by a runtime index
    means scaling the index to a byte offset and spreading it over its lane.
    Masking the index with `lanes-1` *first* is what keeps every scaled offset
    below 16, so the control byte's high bit stays clear and PSHUFB never
    takes its "write zero" path. That is also what makes the operation total,
    so it needs no guard.

A new vector IR opcode is not enough on its own: `asm_ir()` in `lj_asm.c` has
an explicit `case` list that routes vector opcodes to `asm_vec()`. Forgetting
to add the new opcodes there does not fail to build and does not fail a
differential test -- the trace just aborts with "cannot assemble IR
instruction N" and the interpreter produces the right answer. The codegen
tests are what catch it.

## D17. `simd.fma` is opt-in, and that is the whole point

x86-64-v3 includes FMA, so the instruction is available. It is still *not*
used to contract an ordinary `a*b + c` written with Lua operators, because
contraction changes the result: FMA rounds once, the two-step form rounds
twice. Silently doing that in compiled code and not in the interpreter would
break the one invariant this work is built on.

Exposing it as an explicit `ffi.simd` entry keeps both properties:

  * The user asks for the fused form, so the different rounding is intended.
  * Interpreter and JIT still agree **bit for bit**, because C99 `fma`/`fmaf`
    are the IEEE 754 fusedMultiplyAdd operation and correctly rounded, which
    is exactly what `VFMADD213PS`/`PD` compute. On a CPU without FMA the trace
    aborts and the interpreter's `fma()` still produces the single-rounded
    result -- the fallback is *correct*, not merely slower.

Three operands do not fit in one IR instruction, so the second and third
travel in an `IR_CARG` pair: `VFMA(a, CARG(b, c))`. That is the existing
LuaJIT idiom for extra operands, `asm_ir()` already treats a CARG as a no-op,
and the loop optimizer already understands CARG chains, so it costs no code
and no new machinery. `VFMADD213` computes `dest = vvvv * dest + rm`, so the
destination doubles as the second multiplicand: `b` is moved into it with
`ra_left()` and `a` and `c` are allocated out of it so that move cannot
clobber them.

Note which tests can and cannot see this. Disabling the lowering leaves every
*value* test passing, because the interpreter produces the identical
single-rounded answer -- that is the fallback working correctly. Only the
codegen test notices that `vfmadd213ps` disappeared. Any operation whose
fallback is exact needs a codegen test to back up a claim of JIT support.

## D18. Which FMA form to emit

x86 has three encodings of the same fused multiply-add. They differ only in
which operand the destination register doubles as:

    132: dest = dest * rm   + vvvv
    213: dest = vvvv * dest + rm
    231: dest = vvvv * rm   + dest

Every one of them overwrites the destination, so whichever operand the form
puts there has to be moved in first. Always using one form makes that move
unconditional, and it is not free: with 213 a loop-carried accumulator chain
compiled to two MOVAPS plus the fused instruction, three instructions where
the separate multiply and add it was meant to replace needs two. Measured, the
"fused" version was *slower*.

The backend now picks the form whose operand is already where it needs to be:

  * A **PHI operand** wins outright. It is the loop-carried accumulator, so it
    must share a register with this instruction's result anyway; putting
    anything else in the destination forces a copy out and a copy back every
    iteration. `fma(a, b, acc)` therefore picks 231, `fma(acc, b, c)` picks
    132, and both become a single instruction.
  * Otherwise the **first multiplicand** goes in the destination (132). In a
    chain of fmas that is the result of the previous one: a temporary that
    dies here and so is given the destination for free.

Preferring the addend whenever it merely "has no register yet" looks like the
same idea but is wrong, and was tried: a loop-invariant addend has no register
at that point either, yet it is still needed on the next iteration, so it has
to be copied out. Restricting the test to IR constants is also not enough,
because a vector operand read from a cdata upvalue is a hoisted load, not an
IR constant.

What is left is one unavoidable copy at the *head* of a chain whose innermost
operands are all loop-invariant: nothing dies there, so something must be
moved. A degree-4 Horner chain went from four copies to one.

The benchmark reports fma in two shapes on purpose. On a latency-bound
loop-carried chain it is 1.75x, close to the 8-cycle versus 4-cycle ratio it
should be. On a loop whose iterations are independent it is slightly *slower*,
because the loop is bound by the separate accumulator's dependency chain and
the one remaining copy lands on the critical path. Reporting only the first
number would oversell the operation.

## D19. Keep the six lane types; encode 256-bit width in IR type bit `0x20`

Duplicating the six vector base types for 256-bit values would consume six
additional type IDs, but the five-bit base-type field only has two free.
Widening that field would collide with the PHI and guard flags and would make
the compact eight-byte `IRIns` representation substantially harder to keep.

Width is orthogonal to lane kind, so 256-bit values will reuse the same six
base types and carry `IRT_VEC256` in bit `0x20`. For example, `IRT_V4I32`
still describes 32-bit integer lanes; adding the width flag changes the value
from four lanes in an XMM register to eight lanes in a YMM register.

That bit used to be the transient `IRT_MARK` flag shared by dead-code
elimination, loop-PHI handling, allocation sinking, and register allocation.
Those marks now live in an 8192-byte scratch bitset with one bit for every
16-bit IR reference. The bitset is cleared at recording setup and before each
assembly attempt. This frees the width bit without growing `IRIns` or changing
the meaning of persistent IR.

## D20. The first YMM slice includes storage and exits, not just arithmetic

Setting VEX.L on `VADDPS` is the small part of 256-bit support. A real value
also has to survive constant interning, register moves, spills, allocation
sinking, loop PHIs, parent snapshots, and every kind of trace exit. The first
YMM milestone therefore treats width as an end-to-end value property:

* `IRT_VEC256` is preserved by TRefs, CSE, PHIs, snapshot replay, and alias
  checks; `irt_size()` reports 32 and a KVEC owns four payload slots.
* YMM values use eight consecutive 32-bit spill slots. Constants, cdata
  payloads, and spills use unaligned `VMOVUPS`; register copies use
  `VMOVAPS`.
* On x64, `ExitFPR` is 32 bytes. The exit handler reserves 32 bytes per FP
  register and branches on `JIT_F_AVX`: an AVX-capable host saves YMM0-YMM15,
  while an older host executes only legacy XMM stores into the low half of
  each slot. This keeps one binary runnable on x86-64-v2 hardware.
* The AVX exit path executes `VZEROUPPER` after saving YMM state, and generated
  calls do the same immediately before entering C. On Windows, a live
  256-bit value is evicted even from XMM6-XMM15 because that ABI preserves
  only their low 128 bits.

The recorder exposes only operations whose whole lowering is width-aware:
add/sub, direct FP or 16/32-bit integer multiply, FP division, unary minus,
logical operations, select, and bitcast. Everything else still raises
`NYIVEC` while recording and resumes in the interpreter. That boundary is
intentional: an operation is not called YMM-supported until its tests inspect
the emitted `ymm` operands and exercise upper lanes, spills, and exit rebuilds.

## D21. Widen direct operations first; keep cross-lane semantics explicit

Most packed instructions become a correct YMM operation by setting VEX.L:
comparisons, min/max, shifts, sqrt/round, FMA, saturating arithmetic, mulhi,
and the direct conversions all preserve lane independence. The emitter
therefore has width-aware forms of its two-operand, three-operand, immediate,
and shift helpers. The 8- and 64-bit multiply emulations are also lane-local,
so applying the same width to every instruction makes them operate on both
128-bit halves without changing their algebra.

Broadcasts use the AVX2 `VPBROADCAST*` forms instead of trying to extend an XMM
shuffle into an undefined upper half. `VMOVMSKPS/PD` and `VPMOVMSKB` directly
cover most mask shapes. Sixteen-bit lanes need one extra compression:
`VPACKSSWB` produces `[lo, lo, hi, hi]` because it packs independently in each
128-bit half, so the GPR result explicitly selects `lo` and `hi` into adjacent
bytes. A differential with distinct low/high masks caught the tempting but
wrong `raw | (raw >> 8)` version, which mixed the duplicate low byte into the
high byte.

Reductions and shuffles were intentionally kept out of the direct-operation
slice. Their 128-bit lowering uses byte shifts or `PSHUFB`, whose AVX2 forms
remain confined to each 128-bit lane. Merely setting VEX.L would compile, but
would implement the wrong full-vector permutation. D22 adds the explicit
cross-lane step.

## D22. One explicit half-swap completes YMM cross-lane operations

`IRVSHUF_SWAP128` represents a swap of the low and high 128-bit halves and
lowers to `VPERM2I128`. Keeping this operation explicit prevents a lane-local
`VPSHUFB` or `VPSRLDQ` from silently pretending to be a whole-YMM shuffle.

Horizontal reductions first combine the original vector with the half-swapped
copy, then reuse the existing lane-local halving shifts. Operand order in the
low half matches the interpreter's fixed reduction tree, including asymmetric
floating-point MIN/MAX behaviour for NaNs and signed zero.

A constant shuffle applies `VPSHUFB` to the original and half-swapped sources
with disjoint masks, then ORs the results when both routes are populated. An
all-same-half shuffle omits the swap; an all-cross shuffle omits the empty
shuffle and merge. A runtime index vector uses control bit 4 to determine
whether each requested byte belongs to the output's current half: it shuffles
both source arrangements and selects between them with packed masks.
`shuffle2` composes two of the same permutes. Insert already had the right
packed select algebra; widening its lane-number vector makes constant and
runtime indices address all YMM lanes without scalarisation.
