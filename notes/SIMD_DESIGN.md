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
    VSHUF2  ref carg  (two-source immediate shuffle/half permute)
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

  * `simd.bitcast(ct, v)` - reinterpret the vector bits, requires equal sizes.
  * `simd.convert(ct, v)` - numeric lane conversion, requiring equal lane
    counts. The interpreter accepts every numeric pair. The JIT compiles every
    pair whose source and destination vector sizes are each 16 or 32 bytes;
    pairs involving an 8- or 64-byte vector abort the trace and continue
    interpreted. D27 and D28 describe the packed and exact call-free
    emulations needed where x86 has no direct instruction.
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
`shuffle2` generally composes two of the same permutes. Its canonical
lane-local low/high interleaves instead use one `PUNPCK*`, including the
reversed operand forms. Insert already had the right packed select algebra;
widening its lane-number vector makes constant and runtime indices address
all YMM lanes without scalarisation.

## D23. Use direct AVX2 lane permutes only when they shorten the critical path

AVX2 has full-width `VPERMD` and immediate `VPERMQ`, but replacing every
constant byte shuffle with them would not be faster. A constant permutation
whose sources all remain in the corresponding 128-bit half is one
low-latency `VPSHUFB`; the full-width permutes have higher dependent latency
on the measured target.

The recorder therefore classifies 32- and 64-bit constant YMM shuffles by
their routes:

* a wholly same-half constant remains one low-latency lane-local shuffle;
* a 32-bit constant with any cross-half route becomes one `VPERMD`;
* a 64-bit constant with any cross-half route becomes one `VPERMQ`.

A runtime 32-bit index vector always becomes one `VPERMD`. Its hardware
semantics use the low three bits of each index, exactly matching the public
modulo-eight rule, and replace the generic control construction, two byte
shuffles, half swap, and packed select. Other runtime lane widths retain the
generic byte-control lowering because AVX2 has no matching variable
full-width permute for them.

This is intentionally a cost decision rather than an instruction-count
fashion: use the direct permute where it removes a cross-lane dependency
chain, and retain the cheaper lane-local operation where no crossing is
needed.

Constant shuffles are canonicalised before that route decision. Identity
returns the input IR directly. An exact exchange of the two 128-bit halves is
one `VPERM2I128`, including for byte and word vectors, instead of a half swap
followed by an identity `VPSHUFB`. Every 32/64-bit XMM permutation maps to an
immediate `PSHUFD`; YMM uses the same form when each half repeats one
lane-local pattern. This has the same one-cycle dependent latency as
`PSHUFB`, but the control is in the instruction and consumes neither a vector
register nor a constant-pool load.

## D24. Emulate narrow and wide `mulhi` with packed lane-local sequences

Only 16-bit lanes have a direct packed high-product instruction, but leaving
the 8- and 32-bit forms interpreted costs roughly two orders of magnitude in
a hot loop. Both missing widths have compact packed decompositions.

For byte lanes, one byte in each word is sign- or zero-extended while the
other is left scaled by `2^8`. `PMULHW`/`PMULHUW` then returns bits 8..15 of
the original byte product. Repeating this for the even and odd bytes and
merging their low bytes gives the result in 8 instructions unsigned or 10
signed.

For 32-bit lanes, `PMULDQ`/`PMULUDQ` first multiply the even dwords. Shifting
both inputs by 32 within each qword exposes the odd dwords for a second
multiply. One logical shift selects the high dwords of the even products and
`PBLENDW 0xcc` takes the high dwords of the odd products. This is a six
instruction sequence, or five when both operands are the same IR value.

Every step is confined to its 128-bit lane, so the same lowering works for
XMM and YMM by selecting the VEX width. On the measured host the dependent
latencies are about 1.8--1.9 ns for byte lanes and 1.5 ns for dword lanes,
down from roughly 60--80 ns of interpreter fallback; YMM performs twice as
many lanes at essentially the same latency.

## D25. Decompose narrow per-lane shifts into dword variable shifts

AVX2 provides variable shifts per 32- or 64-bit lane, but not per byte or
word. Falling back to the interpreter for the narrow forms was needlessly
expensive because a dword contains a fixed small number of independent
pieces.

For 16-bit lanes the recorder extracts the low and high words of every dword,
extracts their matching unsigned counts, performs two `VPSLLVD`, `VPSRLVD` or
`VPSRAVD` operations, masks the results, and recombines them. Byte lanes use
the same construction four times. Arithmetic shifts sign-extend each piece
before `VPSRAVD`; logical inputs are zero-extended.

The out-of-range semantics fall out of the construction without guards.
Logical results are masked back to their narrow lane, so counts from the lane
width through 31 become zero, and AVX2 itself flushes counts of 32 or more.
Sign-extended arithmetic pieces become all zeroes or all ones for any count
at or above their lane width. Counts are extracted with a byte/word mask, so
negative count lanes are treated as the large unsigned values required by the
interpreter.

The sequence is lane-local and identical at XMM and YMM width. Measured
dependent latency is about 1.23 ns for words and 2.22 ns for bytes, versus
32.5 ns and 35.1 ns in the interpreter. YMM processes twice the lanes at the
same latency.

Later cost measurements supersede most of this generic lowering: D38--D39
replace all byte cases with lookup/product identities, while D42--D43 replace
word left/logical-right shifts and YMM arithmetic-right shifts. The original
two-dword arithmetic-right sequence remains deliberately selected for XMM,
where it has better multi-chain throughput.

## D26. Build 64-bit `mulhi` from four 32-bit partial products

There is no packed 64x64-to-128 multiply before AVX-512, but `PMULUDQ`
provides all the pieces. Split each unsigned lane into `a0 + 2^32*a1` and
`b0 + 2^32*b1`, then compute:

```
w0 = a0*b0
t  = a1*b0 + high32(w0)
hi = a1*b1 + high32(t) + high32(low32(t) + a0*b1)
```

This needs four `PMULUDQ`s plus packed qword shifts and adds. All intermediate
sums fit in 64 bits by construction. Signed multiplication reuses the
unsigned result and applies the two's-complement correction
`hi -= (a < 0 ? b : 0) + (b < 0 ? a : 0)` with packed sign masks.

The interpreter uses the same identity at base 2^32, without depending on a C
`__int128` extension. Tests deliberately use a separate base-2^16 schoolbook
oracle, whose partial sums fit exactly in a Lua number, so they do not merely
repeat the production algorithm.

On the measured target dependent XMM latency is 3.33 ns signed and 2.75 ns
unsigned, down from roughly 30 ns interpreted. YMM executes the same
lane-local sequence in 3.24/2.67 ns while producing twice as many lane
results.

## D27. Synthesize exact unsigned and qword floating-point conversions

AVX2 still lacks packed `u32 -> float` and every packed conversion between
qwords and doubles. Falling back to the interpreter forfeits the entire hot
loop, so the recorder expands the missing integer-to-FP forms into exact
packed arithmetic.

For `u32 -> float`, split each lane into two 16-bit pieces. Encode each piece
directly into an IEEE-754 float mantissa under a fixed exponent, subtract the
matching bias from the high piece, then add the two floats. Both pieces are
exact; the final packed add performs the one correctly rounded conversion.
This covers all values through `0xffffffff` without ever feeding a negative
value to signed `CVTDQ2PS`.

For `i64/u64 -> double`, split each qword into low and high dwords and encode
those exact pieces under binary64 exponents separated by 32 bits. Unsigned
conversion subtracts a fixed combined bias. Signed conversion first XOR-biases
the high dword by `0x80000000` and uses the corresponding signed bias. Again,
the final `ADDPD` is the only rounding operation.

There is no packed `double -> i64/u64` before AVX-512. The backend therefore
uses the fastest available call-free sequence: one `CVTTSD2SI r64` per lane,
`PSHUFD` to expose the high double, and `PUNPCKLQDQ` to assemble the qwords.
YMM uses `VEXTRACTF128`/`VINSERTF128` around the same two-lane sequence. The
unsigned destination intentionally receives the same result bits and signed
indefinite value as every other FP-to-unsigned conversion in this API.

Four-way throughput measurements on the AVX2 test host are 0.51 ns/vector for
`u32 -> float`, 0.58/0.51 ns for signed/unsigned qword-to-double, and
0.64 ns for double-to-qword at XMM width. The packed integer-to-FP sequences
process twice the lanes at the same YMM cost. Double-to-qword issues twice as
many scalar conversions in a YMM value, so its per-lane throughput stays
constant rather than doubling.

## D28. Compile every native cross-width conversion and keep it VEX-clean

An equal lane count can still change the physical vector width: `float4 ->
double4`, for example, is XMM to YMM. Every directed conversion whose source
and destination are each 16 or 32 bytes now compiles, covering all 38
cross-width pairs as well as the existing equal-width pairs.

The common cases map directly onto AVX2:

* integer widening uses `VPMOVSXBW/WD/DQ` or `VPMOVZXBW/WD/DQ`;
* integer narrowing uses a lane-local `VPSHUFB`, followed by one `VPERMQ` to
  compact the two surviving qwords into the low XMM result;
* `float4 <-> double4` and signed dword/double conversion use the matching
  `VCVT*` instruction;
* word-to-float widens to dwords first, while float-to-word checks the signed
  destination bounds before `VCVTTPS2DQ` and packed narrowing.

The instruction-set holes remain exact and call-free. `u32x4 -> double4`
zero-extends to qwords, ORs in a binary64 `2^52` exponent, then subtracts the
bias. Qword-to-float has no packed AVX2 instruction, so each lane uses a
scalar `VCVTSI2SS` and the four results are packed in registers. Unsigned
qwords use the standard exact `(x >> 1) | (x & 1)` conversion followed by a
doubling when the sign bit is set; this avoids the double-rounding error of
converting through binary64.

Mixed widths exposed a separate performance rule. A legacy SSE instruction
executed while a YMM upper half is live incurs a severe AVX-to-SSE transition
on affected CPUs. The loop PHI move for an XMM result was still `MOVAPS`, even
though the same trace carried YMM inputs, making otherwise sub-nanosecond
conversions cost roughly 20 ns. On an AVX host, all vector register moves,
constants, loads, stores, spills, GPR transfers and extracts now use VEX-128
even for XMM values. VEX-128 preserves the intended low-half semantics,
zeroes the upper half, and avoids the transition. A codegen test rejects
legacy `MOVAPS` and `MOVUPS` in a representative mixed XMM/YMM loop.

## D29. AVX traces must keep ordinary scalar FP operations VEX-clean too

Vector-only emission is not enough. Lua loop state routinely mixes a YMM value
with an ordinary number, a scalar FFI field, a bound check, or `math.sqrt`.
One legacy scalar `ADDSD`, `MOVSD`, `UCOMISD`, or `SQRTSD` beside a live YMM
upper half causes the same AVX-to-SSE transition as a legacy packed move.

When AVX is active, the generic x86 backend therefore uses VEX-128 for all
scalar FP register moves, constants, spills, loads/stores, arithmetic,
conversions, comparisons, square root, and SSE4.1 rounding. The VEX
three-operand form keeps the legacy destination-as-left semantics by encoding
the destination as the merge/source operand. CPUs without AVX retain the
original SSE encodings byte for byte.

The distinction between VEX scalar forms is load-bearing:

* arithmetic, conversion, sqrt, and rounding use VEX.vvvv as a real merge
  source and go through `emit_vexopv`;
* memory `VMOVSD`/`VMOVSS` loads and stores are two-operand forms whose
  VEX.vvvv field is reserved, so they must go through `emit_vexop`.

Encoding a memory scalar move with the destination as VEX.vvvv appears to work
for XMM0 because its inverted field happens to be the reserved all-ones bit
pattern, but every other destination raises `#UD`. A test suite run caught
that immediately; do not generalise register-register scalar move rules to
the memory form.

On the measured AVX2 host, a dependent YMM add costs 0.38 ns/iteration. Adding
an independent ordinary Lua-number add formerly raised that to 76.50 ns;
after the generic VEX cleanup the mixed loop remains 0.38 ns. A stronger
codegen test mixes scalar memory load/store, add, multiply, sqrt, comparison
and YMM arithmetic, and rejects any legacy instruction that names XMM.

## D30. Preserve upper lanes in every control-flow-safe call setup

`VZEROUPPER` is itself a full-YMM clobber. On Windows x64 this matters even
for XMM6-XMM15: the ABI preserves their low 128 bits only. The original
wide-value eviction in `asm_setupresult()` covered ordinary recorded calls,
but not backend paths that invoke C directly, including conditional GC
steps, allocation helpers, barriers, and indirect FFI calls. A GC side trace
could therefore keep a live value in YMM6, execute `VZEROUPPER`, and resume
with valid low lanes and destroyed high lanes.

All x86 call setup paths now share one helper that adds live YMM registers to
their eviction set. Ordinary calls use it from `asm_setupresult()`; custom
paths use it before establishing any conditional-call join label. This
placement matters with reverse code generation: emitting a restore from
inside the conditional block would put it only on the call-taken edge, while
the fast edge would skip it. Before any direct or indirect call on an AVX
host, the setup therefore:

* finds every currently live 256-bit value in the FPR register file;
* gives it a full eight-slot spill/reload when needed;
* records all FPR upper halves in a separate `vecmodset`.

`vecmodset` is deliberately separate from the ordinary register `modset`.
Only 256-bit loop invariants avoid those registers or get restored at the
loop boundary; scalar and XMM invariants still exploit the Windows
callee-saved low halves. This preserves the existing 128-bit allocation
quality while making ordinary calls, GC checks, barriers, scalar conversion
helpers, and indirect FFI calls YMM-safe. Indirect FFI calls also get the
missing `VZEROUPPER` discipline.

## D31. Keep constant integer modulo out of vector loops

An ordinary Lua integer operation can dominate an otherwise packed loop.
LuaJIT formerly lowered every non-power-of-two `int % constant` to
`lj_vm_modi`. Beside a live YMM value that means a call, `VZEROUPPER`, and
full-width spill/reload protection even though the divisor is known while
assembling the trace.

On x64, signed constant division now uses the exact reciprocal multiplier and
shift from Hacker's Delight. The generated quotient is truncating, so a final
branchless `LEA`/`TEST`/`CMOV` correction gives Lua's floor-modulo semantics
for positive and negative divisors. The remainder stays exact for every
signed 32-bit numerator and divisor, including `INT_MIN`; divisors `1` and
`-1` become zero without executing the overflowing `INT_MIN / -1` operation.
The x86 backend uses inline `CDQ`/`IDIV` with the same branchless correction,
because its smaller register file makes the reciprocal sequence a poor
tradeoff.

This is deliberately a generic scalar optimization rather than a SIMD-only
opcode. It removes the call boundary that forced wide-vector preservation and
benefits scalar traces too. In a loop carrying a YMM add, `i % 37` fell from
about 1.70 ns to 0.85 ns per iteration on the AVX2 test host; the vector-only
loop is about 0.37 ns. Machine-code tests require reciprocal multiplies and
reject both `CALL` and `IDIV` in the x64 YMM loop.

## D32. Recognise byte rotates before lowering their component shifts

Crypto and hashing kernels commonly spell a packed rotate with the existing
portable operations:

```
simd.bor(simd.shl(x, n), simd.shr(x, bits-n))
```

For a byte-aligned count, three packed instructions are unnecessary.
`PSHUFB` can select the rotated bytes in one instruction. The recorder
recognises complementary logical shifts of the same integer vector beneath
either `bor` or `bxor`, in either operand order, and emits one `VSHUFB`.
Non-byte counts retain the exact shift/shift/logical sequence. This is an
idiom fold rather than a new API or IR operation, so interpreter behaviour,
vector ctypes, snapshots and the public ABI do not change.

The shuffle mask is local to each integer lane. That preserves 16-, 32- and
64-bit lane boundaries and naturally repeats in both 128-bit halves of a YMM
register. Dead-code elimination removes the two component shifts after the
replacement has become their only consumer.

Constant-mask placement is pressure-sensitive. In a small loop, keeping the
mask in an XMM/YMM register is faster than reading it through the instruction.
In ChaCha20, sixteen live vector state words make that same invariant register
get evicted and reloaded repeatedly. The x64 backend therefore uses the
RIP-relative memory form of `PSHUFB` only when allocating the mask would
consume one of the final two free vector registers. The constant is interned
in the aligned mcode constant area; traces with room keep the original
register form. This retains 0.19--0.25 ns low-pressure shuffle/conversion
costs while eliminating the reload churn in the high-pressure loop.

## D33. Fuse vector loads into arithmetic only when AVX makes it safe

VEX three-operand arithmetic can read its final source directly from memory.
Keeping a one-use `XLOAD` as a separate `VMOVUPS` wastes an instruction and a
vector register, especially in coefficient-heavy filters and polynomial
kernels. The AVX backend now gives a fusable `XLOAD` directly to the generic
ModRM emitter. For commutative operations it may exchange the two IR operands
so the load occupies the encodable memory-source position. Floating-point
add and multiply are excluded from that exchange: although numerically
commutative, reversing their hardware operands can select a different NaN
payload or sign bit. They still fuse when the load is already the right
operand. The IR and observable operation order are unchanged.

This must not be backported mechanically to legacy SSE. A cdata vector payload
is not guaranteed to be 16-byte aligned. Legacy `ADDPS xmm, m128`, for
example, raises a general-protection fault for an eight-byte-aligned operand;
the equivalent VEX instruction explicitly permits an unaligned memory source.
The backend therefore retains `MOVUPS` plus register arithmetic whenever AVX
is absent, and the generic x86 fusion gate independently rejects a vector
`XLOAD` on that path.

Fusion is deliberately limited to a real, one-use `XLOAD`. Letting the generic
load helper turn an arbitrary spilled vector PHI into a memory operand bypasses
the loop spill-slot synchronisation and can read the previous iteration's
value. Constants have their separate pressure-sensitive policy from D32.

In the degree-11 polynomial benchmark, twelve coefficient loads in both the
root and loop portions become memory-source `VADDPD`s. The XMM trace falls
from 189 to 165 instructions (1015 to 895 bytes) and the YMM trace from 234
to 210 instructions (1229 to 1109 bytes). On the measured host XMM improves
by about 8--9%; the dependency-bound YMM chain keeps essentially the same
elapsed time with smaller code.

## D34. Treat sunk vector boxes as virtual when fusing AVX loads

Indexing an FFI vector array has value semantics: the recorder represents the
result with an `XLOAD`, a temporary `CNEW`, and an `XSTORE` into that temporary.
Sink optimisation removes the allocation and store, but the backend's
conflict scan still counted the sunk `XSTORE` as a real alias and as a second
use of the loaded value. Consequently an ordinary expression such as
`dst[i] = simd.abs(src[i])` retained a standalone `VMOVUPS` even though the
only emitted consumer could read `src[i]` itself.

The x86 conflict scan now ignores a sink-tagged `XSTORE`. Such a store
initialises only a virtual box and emits no memory access. If a snapshot needs
that box, snapshot allocation independently keeps the source value live in a
register or spill slot, which makes it ineligible for one-use fusion. Real
stores and genuinely shared loads retain the existing conflict checks.

This makes D33 apply to normal FFI array expressions, not just vectors already
stored in Lua tables. The same alignment-safe AVX rule is extended to packed
square root, integer absolute value, rounding, immediate permutations,
`VPERMD`, direct packed numeric conversion, and integer widening/narrowing.
Each uses the instruction's unaligned memory-source form. Nehalem and every
other legacy-SSE path still issue `MOVUPS` before the packed instruction.

In a six-kernel XMM/YMM streaming dump covering square root, rounding and
absolute value, 48 standalone `VMOVUPS` instructions disappear across the
root and loop traces (210 down to 162); total instructions fall from 2008 to
1960. Square root remains execution-unit-bound on the measured host, so this
is primarily a code-size, decode, and register-pressure win there. Cheaper
operations have more opportunity to benefit when the surrounding trace is
front-end or register-pressure limited.

## D35. Expose byte multiplication to IR optimisation

x86 has no packed byte multiply. The original backend treated `i8/u8`
multiplication as one opaque `IR_VMUL` and expanded each instance into two
word multiplies, four shifts and an OR. That sequence is correct, but hiding
it in the assembler prevents common-subexpression elimination. A kernel with
many byte multiply chains redundantly shifted the same invariant multiplier
once for every chain.

The recorder now expresses modulo-byte multiplication in ordinary packed IR:

```
even = (wordmul(a, b) & 0x00ff)
odd  = wordmul(a >> 8, b >> 8) << 8
result = even | odd
```

The word view changes only lane interpretation in IR; it does not change any
bits or materialise a bitcast. Keeping the low byte with one `VAND` replaces
the former shift-left/shift-right pair. More importantly, the optimiser now
shares an invariant `b >> 8` across every multiplication in the trace.
Signed and unsigned byte multiplication use the same modulo-256 bit result,
so the expansion is valid for both ctypes and preserves interpreter
semantics.

Pressure-sensitive constant placement from D32 is now shared by generic AVX
binary operations. With ample registers, `0x00ff` remains a loop-invariant
XMM/YMM value. When allocating it would consume either of the last two free
vector registers, `VPAND` reads the aligned interned constant directly from
the mcode area. This removes reload instructions without forcing every
low-pressure multiply to spend a load uop.

On the measured host, a dependent byte-multiply chain improves about 2% and
four independent chains about 7.5%. Eight, twelve and fourteen independent
chains improve by 28%, 32% and 33% respectively because they share the
shifted multiplier. Across those three stressed root/loop traces, total code
falls from 957 to 829 instructions; word shifts fall from 340 to 139.

## D36. Fuse the encodable FMA input without sacrificing the accumulator

FMA has a memory-source form just like ordinary VEX arithmetic, but its three
encodings make load fusion inseparable from destination selection. The
destination still has to be the loop-carried operand chosen by D18; moving a
different operand there merely to expose a load would put copies back on the
critical path.

Once that operand is fixed, the backend chooses between the two compatible
forms when a one-use array load can occupy the ModRM input. For example,
`fma(acc, k, array[i])` keeps `acc` in the destination and selects 213 so the
addend is read from memory. `fma(acc, array[i], c)` retains 132 and reads the
multiplier from memory. This preserves the original operand order and still
removes the separate `VMOVUPS`.

The third and fourth operands travel through an adjacent `CARG`. That IR node
is only a structural carrier and emits no code, but the generic load-conflict
scan would otherwise count it as a second consumer. The FMA path skips exactly
that carrier while retaining the ordinary alias check. A real intervening
`XSTORE` still prevents fusion, including when two FFI pointers alias.

Across four XMM/YMM addend/multiplier root-and-loop traces, eight vector loads
disappear and machine code falls from 1572 to 1536 bytes. A dependent FMA
chain remains execution-latency bound at about 0.76 ns/vector. With twelve and
fourteen independent streamed accumulators, eliminating the temporary vector
register improves elapsed time by about 2% and 5% respectively.

## D37. Fuse variable-shift counts only when registers are exhausted

AVX2 `VPSLLV`/`VPSRLV`/`VPSRAV` can read their per-lane count vector from
memory. Doing so unconditionally is smaller but slower on the measured host:
with ample registers, a standalone `VMOVUPS` lets the out-of-order core fetch
the counts before the shift is ready. The production-sized low-pressure shift
benchmark regressed from roughly 0.23 to 0.25 ms when every count load was
folded.

The constant-placement pressure heuristic from D35 now covers one-use count
loads too. The normal path retains the separately schedulable load. Only when
allocating the count would consume one of the final two vector registers does
the shift use its memory operand. Fifteen live shift chains are the crossover
on this register file: twelve- and fourteen-chain timings remain flat, while
fifteen chains improve from about 7.33 to 6.75 ms, roughly 8%.

Across the full twelve/fourteen/fifteen-chain trace dump, 35 variable shifts
take a memory source. Total instructions fall from 1612 to 1564, standalone
vector loads from 146 to 98, and spill reloads from 59 to 46. This is why
memory-operand fusion is a cost decision rather than a blanket code-size
rule.

## D38. Turn variable byte left shifts into lookup multiplication

AVX2 has no variable shift per byte. The general lowering from D25 split each
dword into four bytes and issued four `VPSLLVD` instructions, along with the
extraction masks and recombination needed around them. A left shift has a
cheaper byte-specific identity:

```
x << n = x * 2^n (mod 256), for n < 8
x << n = 0,                 otherwise
```

The recorder builds `2^n` with one `VPSHUFB` from a repeated eight-entry
table, then feeds that factor into the IR-visible byte multiply from D35. The
shuffle control is `unsigned_saturating_add(n, 0x78)`: counts zero through
seven become indices `0x78` through `0x7f`, selecting table slots 8 through
15 in each 128-bit half. Every larger unsigned byte becomes at least `0x80`,
so `VPSHUFB`'s control-bit rule returns zero. Negative signed count lanes have
the same byte encodings as large unsigned counts and therefore also return
zero, matching the interpreter without a compare or guard.

This remains an ordinary IR graph rather than an opaque backend sequence.
The optimiser can share constants and shifted byte-multiply inputs, and the
same lane-local lowering naturally covers XMM and YMM. Right and arithmetic
byte shifts retain the general dword decomposition because multiplication
does not express their bit movement.

On the measured host, dependent byte-left-shift latency falls from about
2.17 to 1.61 ns per vector, roughly 26%. Median repeated streaming runs
improve from 1.88 to 1.00 ns per XMM vector and from 2.48 to 1.14 ns per YMM
vector, roughly 47% and 54%. Across its XMM/YMM root and loop traces, total
instructions fall from 634 to 558, all 16 `VPSLLVD` instructions and all 36
dword extraction shifts disappear.

## D39. Divide byte lanes through signed and unsigned word products

Logical and arithmetic byte right shifts also admit packed multiplication
identities, but they need the high part of a word-sized intermediate rather
than byte multiplication modulo 256. For an unsigned byte:

```
x >> n = (x * 2^(7-n)) >> 7, for n < 8
x >> n = 0,                  otherwise
```

The same saturated-add/`VPSHUFB` control as D38 looks up byte factors
`128, 64, ..., 1` and produces zero for every out-of-range unsigned count.
Even and odd data/factor bytes are isolated into word lanes, multiplied with
`VPMULLW`, shifted right by seven, and merged. This replaces four
`VPSRLVD` operations and their dword extraction/recombination graph with two
word multiplies and lane-local word shifts.

Arithmetic shift first clamps each unsigned count to seven with `VPMINUB`.
Its data bytes are sign-extended to words before using the same factors:

```
sar8(x, n) = (sign_extend(x) * 2^(7-min(n,7))) sar 7
```

At the clamped limit the factor is one, so the final word arithmetic shift
already gives zero for a non-negative byte and minus one for a negative byte.
That is the required full sign fill for every original count at or above
eight, including negative signed count encodings, without a compare or
select.

On the measured host, dependent XMM latency falls from about 2.15 to
1.87 ns for logical right shift and from 2.34 to 2.08 ns for arithmetic
right shift. The wider gain is in streaming throughput: logical XMM/YMM
improves about 45% (1.85 to 1.03 and 2.26 to 1.26 ns/vector), while
arithmetic XMM/YMM improves about 43%/39% (1.95 to 1.11 and 2.25 to
1.36 ns/vector). Logical root/loop traces shrink from 634 to 570
instructions and arithmetic traces from 638 to 586; together they remove 32
dword variable shifts and 88 dword extraction shifts.

## D40. Reuse the cross-product and sign correction in qword squares

The high half of a signed 64-bit product is obtained from its unsigned high
half with:

```
signed_hi = unsigned_hi
          - (a < 0 ? b : 0)
          - (b < 0 ? a : 0)
```

The two conditional values depend only on the inputs, so the backend now
adds them before the unsigned product is ready and performs one final packed
subtraction. This keeps the same modulo-64-bit arithmetic while removing one
operation from the result's critical dependency chain. Ordinary signed
`mulhi` latency falls from about 3.15 to 2.99 ns/vector on the measured host;
unsigned multiplication is unchanged.

When both IR operands are identical, the 32-bit partial products
`a_hi*a_lo` and `a_lo*a_hi` are identical too. The backend retains that
cross-product instead of issuing it twice, aliases the two extracted high
halves, and builds one sign mask which it doubles for the signed correction.
The specialised square uses three `VPMULUDQ` instructions instead of four,
one fewer qword shift, and half as many sign-mask instructions.

Dependent signed square latency improves from 3.34 to 2.90 ns/vector, about
13%; unsigned improves from 2.67 to 2.59 ns. With eight independent square
chains, signed XMM/YMM throughput improves about 20--23% and unsigned about
8--12%. Across signed/unsigned XMM/YMM square root-and-loop traces, total
instructions fall from 412 to 392, `VPMULUDQ` from 32 to 24, and qword shifts
from 48 to 40.

## D41. Compute byte `mulhi` squares as full word products

A general byte `mulhi(a, b)` has to arrange one operand for `PMULHW` or
`PMULHUW`, because x86 has no packed byte high-product instruction. A square
is simpler: the full product of two bytes always fits in one 16-bit lane.
The recorder recognises identical IR operands, zero-extends the even and odd
bytes, issues two `VPMULLW`s, and directly extracts bits 8 through 15.

Signed squares use `VPABSB` first. A square is non-negative and
`abs(-128)` deliberately remains the byte bit pattern `0x80`, which is the
unsigned magnitude 128 required by `(-128)^2`. This removes the signed
word-extension chain as well as both high-word multiplies.

Unsigned dependent square latency improves from about 1.83 to 1.67 ns/vector,
roughly 9%. Signed dependent latency is flat within measurement noise, while
eight independent chains improve about 12% signed and 9% unsigned. Across
the signed/unsigned XMM/YMM dependent and eight-chain traces, total
instructions fall from 1532 to 1418; all 144 high-word multiplies are
replaced with low-word products and the former 144 left shifts and 72
arithmetic word shifts disappear.

## D42. Turn variable word left shifts into packed multiplication

AVX2 has per-lane variable shifts only for 32- and 64-bit elements. The
original 16-bit lowering split every pair of words into dwords, shifted the
four pieces separately, masked them, and combined them again. A left shift is
multiplication by a power of two modulo the lane width, so the data path can
instead use the native packed word multiply:

```
factor[i] = count[i] < 16 ? 1 << count[i] : 0
result    = value * factor                 /* modulo 2^16 */
```

Two `VPSLLVD`s construct the low- and high-word factors in parallel dword
lanes. The low factors are masked to 16 bits; the high factors are shifted
into the upper words; one `VOR` then forms the word vector consumed by
`VPMULLW`. Signed and unsigned vectors share the same bitwise operation.
Negative signed count encodings and every count at or above 16 naturally
produce zero because x86 variable dword shifts flush counts at or above 32,
while factors for counts 16 through 31 overflow when placed into their word.

When the count vector is invariant, ordinary CSE and loop-invariant-code
motion hoist the entire factor construction, leaving one `VPMULLW` in the
hot loop. Dependent XMM latency improves from about 1.17 to 0.89 ns/vector;
the corresponding YMM measurement improves from 1.23 to 0.89 ns/vector.
With dynamically changing counts, median streaming throughput improves from
1.01 to 0.91 ns/vector for XMM and from 1.70 to 1.18 ns/vector for YMM.
The representative dynamic root-and-loop dump shrinks from 566 to 550
instructions: packed ANDs fall from 24 to 8 and dword right shifts from 8 to
4, while four data-path `VPMULLW`s replace the old extraction work.

## D43. Shift words right through the high half of a product

For a logical word right shift with a non-zero count, the identity

```
x >> count = high_u16(x * 2^(16-count))
```

replaces two data-dependent dword variable shifts with one native
`VPMULHUW`. Two `VPSRLVD`s build and pack the factors for the low and high
words of each dword. Count zero produces a factor that overflows the word to
zero, so an equality mask restores the original input; count 16 uses factor
one and naturally returns zero; larger unsigned counts flush the factor.

Arithmetic right shift uses signed `VPMULHW` after clamping the unsigned count
to 16. Factors for counts 2 through 16 are positive and directly give the
arithmetic quotient. The count-one factor is the signed word `-32768`, so the
high product is `floor(-x/2)`; adding the original word changes it to
`floor(x/2)`. The same masked addition handles count zero, where the factor
is zero. A clamped count of 16 and factor one naturally return zero or minus
one according to the input sign.

This is a cost-selected lowering. Logical right uses it at both XMM and YMM
width. Arithmetic right uses it only for YMM: the old two-dword
`VPSRAVD` sequence remains about 2% faster for streaming XMM and about 10%
faster with eight live XMM chains, despite losing on one- and two-chain
latency. YMM benefits from the shorter data dependency and lower register
pressure.

On the measured host, logical dependent latency improves about 2--3%.
Streaming logical throughput improves from roughly 0.97 to 0.95 ns/vector
for XMM and from 1.42 to 1.06 ns/vector for YMM. YMM arithmetic latency
improves about 4%, and streaming throughput from 1.39 to 1.05 ns/vector,
about 24%; XMM arithmetic is deliberately unchanged. Four invariant
XMM/YMM root-and-loop traces shrink from 420 to 407 instructions because
factor construction hoists. With evolving counts they grow slightly from
408 to 411 instructions, but the YMM throughput gain shows why critical-path
and pressure measurements take precedence over static instruction count.

## D44. Canonicalise constant lane shuffles before allocating controls

The general constant-shuffle lowering starts from a byte control vector. That
is necessary for arbitrary byte/word permutations, but it was unnecessary
work for three common shapes:

* identity now returns its input without emitting an IR instruction;
* a pure exchange of the YMM halves lowers directly to `VPERM2I128`;
* XMM 32/64-bit permutations and repeated lane-local YMM permutations encode
  their control in `PSHUFD`'s immediate.

The last case matters under pressure even though isolated latency is
unchanged. Eight independent YMM permutation chains with eight different
controls improve from 0.169 to 0.138 ns per shuffle on the measured host,
about 18%, because the controls no longer occupy registers or become memory
operands. XMM remains neutral at roughly 0.14 ns. Differential coverage runs
identity, lane-local reverse, half swap, and cross-half reverse for every YMM
lane kind; machine-code checks forbid the eliminated byte-control operations.

## D45. Collapse two-source interleaves to one unpack

A general `shuffle2(a, b, ...)` needs two zero-masked byte permutations and
an OR. The common structure-of-arrays interleave is already a native x86
operation: `PUNPCKL*` or `PUNPCKH*`. The recorder now recognises low and high
interleaves at every lane width, in either `a,b` or `b,a` order, and emits the
existing `VUNPKL/VUNPKH` IR directly.

The recognition follows the hardware's 128-bit lane boundaries. A YMM low
dword unpack is `[a0,b0,a1,b1,a4,b4,a5,b5]`, not a whole-vector interleave of
lanes zero through three. This keeps XMM, AVX, and AVX2 semantics identical
to their respective `PUNPCK*` instructions while arbitrary full-width
patterns retain the general lowering.

On the measured host, dependent XMM/YMM interleave latency improves from
about 0.390 to 0.235 ns/vector, roughly 40%. Eight independent chains improve
from 0.137 to 0.096 ns/op, about 30%. The generated hot path is one unpack
instead of two `PSHUFB`s plus `POR`.

## D46. Route degenerate two-source shuffles through one source

`shuffle2` is often produced by generic permutation code even when a
particular control uses only `a`, only `b`, or the caller supplied the same
vector twice. Running two masked permutations and an OR in those cases is
pure overhead. The constant one-source canonicalizer is now shared: controls
from the second input are rebased, and equal-source controls are reduced
modulo the lane count before normal identity/`PSHUFD`/`VPERMD`/`VPERMQ`
selection.

With a loop-carried add, a single-source identity improves from about 0.571
to 0.238 ns/vector because the shuffle disappears. A YMM cross-half reverse
improves from 1.137 to 0.760 ns/vector, and an arbitrary equal-source YMM
control from 1.170 to 0.759 ns/vector. The latter two become one direct
permute instead of the generic dual-source route.

## D47. Represent native two-source immediate shuffles directly

The remaining general `shuffle2` path still hid native one-instruction
patterns behind two byte shuffles and an OR. `VSHUF2` carries two vector
references plus a constant mode/immediate in an adjacent `CARG`, the same
extra-operand convention already used by FMA. The x86 backend maps it to:

* `SHUFPS` for 32-bit selections where each 128-bit half takes its low two
  outputs from one input and its high two from the other;
* `SHUFPD` for the corresponding 64-bit selection;
* `VPERM2I128` when each output half is copied intact from either half of
  either YMM input.

Both `SHUFPS` and `SHUFPD` recognise reversed input order. YMM dword controls
must repeat the same immediate pattern in both hardware halves; qword
controls use the instruction's independent high-half bits. Anything that
does not match exactly continues through the general byte route.

Measured XMM/YMM dword and qword patterns improve from about 0.571 to 0.386
ns/vector, roughly 32%. A mixed YMM half concatenation improves from 1.138 to
0.759 ns/vector, about 33%. The direct-operation benchmark reports 0.19
ns/vector at both XMM and YMM width when the operation is isolated.

## D48. Lower constant same-position blends to one instruction

A `shuffle2` control that chooses lane `i` from either `a[i]` or `b[i]` is a
blend, not a general permutation. `VSHUF2` now also lowers these controls to
`PBLENDW` when SSE4.1 is available. The recogniser works in 16-bit word
units, which captures every 16/32/64-bit lane blend and byte blends whose two
bytes in each word select the same source.

The immediate is shared by both 128-bit halves of a YMM instruction. The
recorder therefore verifies that the high-half word mask repeats the low-half
mask. Independent byte selections or a non-repeating YMM mask remain on the
general two-shuffle path. This check is semantic, not merely an instruction
preference: `PBLENDW` cannot express those controls.

Paired-byte and dword dependent blends improve from about 0.571 to 0.386
ns/vector at both XMM and YMM widths, roughly 32%. Isolated dword blend cost
is about 0.19 ns/vector and preserves the usual 2x YMM lane throughput.

## D49. Let native two-source shuffles read their final input from memory

The x86 encodings for `SHUFPS`, `SHUFPD`, `PBLENDW`, and `VPERM2I128` all
accept their final vector source as a memory operand. `VSHUF2` carries that
source through an adjacent, non-emitting `CARG`, so the ordinary load-fusion
conflict scan initially treated the carrier as a second use and allocated a
temporary vector register.

The backend now skips only that structural carrier while checking a one-use
`XLOAD`. A real intervening `XSTORE` still blocks fusion. On AVX the final
array load is emitted directly in the shuffle; legacy SSE deliberately keeps
the separate unaligned `MOVUPS`, preserving the established alignment safety
boundary.

Ordinary two-stream loops are neutral to a few percent faster because the
fused instruction has the same loaded execution work but less decode and
register-allocation traffic. The difference becomes material when live
vectors fill the register file: a sixteen-chain array benchmark improves from
about 5.9 to 5.2 ns per sixteen XMM shuffles (roughly 13%) and from about
11--13 to 9.4--10.0 ns per sixteen YMM shuffles (roughly 10--23%). The
production benchmark suite now includes a full-HD RGBA channel-routing pass
that streams two input layers through this exact blend shape.

## D50. Collapse contiguous two-source windows with `PALIGNR`

A shifted window over the concatenation of two vectors is another common
`shuffle2` shape. The generic lowering used two zero-masked byte shuffles and
an OR even though SSSE3 provides exactly that operation. Within each 128-bit
hardware lane, the recorder now recognises both input orders and every
lane-aligned split, then emits one `PALIGNR`.

YMM has an additional semantic boundary: `VPALIGNR` operates independently
in its two 128-bit halves. A full 256-bit window must first bridge the middle
with `VPERM2I128`, then align both halves in parallel:

```
bridge = VPERM2I128(a, b, 0x21)  /* a.high, b.low */
result = VPALIGNR(bridge, a, shift)
```

For shifts beyond the middle, the same bridge is aligned against `b`.
The exact half-width shift was already the one-instruction
`VPERM2I128` case. These routes cover every non-trivial lane-aligned
contiguous window without changing the arbitrary permutation fallback. A
one-use final array load can occupy the bridge instruction's memory operand
for low-half shifts.

On the measured host, a true two-vector dependency chain improves from about
0.452 to 0.253--0.260 ns/window for both XMM and lane-local YMM, roughly
43--44%. Eight independent dword chains improve from 0.204 to 0.189 ns/op
for XMM and from 0.224 to 0.189 for YMM. Full-width YMM chains improve from
1.026 to 0.756 ns/window for a low shift and from 1.210 to 0.756 for a high
shift, roughly 26% and 38%.

`bench.lua` now includes a production-shaped first-difference/delta encoder
over 262,144 int32 values. It carries one block between iterations and uses
the aligned window to bring in the next lane. The measured scalar time is
about 5.2--5.4 ms versus 2.2--2.3 ms at either SIMD width, a 2.3--2.4x
speedup; the YMM form is memory/cross-lane bound rather than arithmetic
bound.

## D51. Use `VPBLENDD` for independent YMM-half masks

`PBLENDW` has one eight-bit immediate even at YMM width, so its word mask
repeats in both 128-bit halves. A same-position dword or qword blend with
different choices in the high half therefore fell through to two
`VPSHUFB`s and `VPOR`.

AVX2 `VPBLENDD` assigns one immediate bit to each of the eight YMM dwords.
The recorder now uses it for non-repeating same-position 32-bit controls and
for 64-bit controls whose two component dword bits select the same source.
Repeating masks retain the existing `PBLENDW` route; byte/word controls that
cannot be represented at dword granularity retain the generic permutation.
The normal `VSHUF2` memory-source path also lets `VPBLENDD` consume a one-use
final array load directly.

Dependent dword blend time improves from about 0.583 to 0.447--0.454
ns/vector, roughly 22--23%. Eight independent chains improve from 0.220 to
0.172 ns/op, also about 22%. The qword pattern improves more modestly from
0.600 to 0.567 ns/vector. A full-HD streaming probe was deliberately not
added as another headline benchmark: it saturated memory bandwidth and was
neutral despite the shorter code, while the permanent operation benchmark
isolates the execution and register-pressure gain.

## D52. Reduce byte sums through qword partial sums

The ordinary horizontal-reduction tree halves a vector with `PSRLDQ` and
combines it at the original lane width. That is appropriate for floating
point, min/max, and wider integer sums, but it spends eight instructions on
an XMM byte sum and ten on a YMM byte sum.

`VSADU8` represents the packed unsigned-byte sum-of-absolute-differences
primitive and produces one qword partial sum per eight input bytes. For byte
`hsum`, the recorder supplies a zero second operand, then runs the existing
halving tree over the two XMM or four YMM qword partials. A loop-invariant
zero vector is rematerialised with `PXOR`; AVX can still fuse a one-use byte
array load into the `VPSADBW` memory source.

Using unsigned byte bit patterns for a signed reduction is exact here. Both
the interpreter and packed byte adds compute modulo 256, and

```
sum(unsigned_byte_bits) mod 256 == sum(signed_bytes) mod 256
```

The existing byte extraction then sign-extends or zero-extends that same low
byte according to the source ctype. XMM consequently needs one `PSADBW`, one
qword shift and one `PADDQ`; YMM needs those plus one half swap and one qword
add. Dependent latency improves by about 35--40%, with a similar gain across
eight independent chains.

The production benchmark suite now writes one additive checksum per 32-byte
block across a 16 MiB payload. Its scalar path is explicitly unrolled across
all 32 bytes, while XMM uses two reductions and YMM one, so the comparison
measures packed reduction rather than an inner-loop bookkeeping advantage.

## D53. Use the horizontal unsigned-word minimum instruction

The generic eight-lane unsigned-word min/max reduction needs three shuffle
and packed-extrema stages, or six vector instructions. SSE4.1 already gates
these operations and also provides `PHMINPOSUW`, which returns the minimum
unsigned word in result lane zero. `VHMINPOSU16` exposes that low-128-bit
operation directly.

XMM `hmin` is one `PHMINPOSUW`. For YMM, which has no 256-bit encoding, the
recorder first swaps and minimum-combines the two 128-bit halves, then applies
VEX-128 `VPHMINPOSUW` to the low half. Clearing the unused upper half is
harmless because reduction extracts only result lane zero.

Maximum uses the monotonic complement identity

```
max(x) = ~min(~x)
```

for unsigned words. The input complement remains packed; after
`PHMINPOSUW`, the output complement is emitted as a scalar integer XOR before
the ordinary u16 narrowing. This avoids spending another vector instruction
on lanes that are dead.

The one-chain XMM case is limited by `PHMINPOSUW` latency and improves about
4--5%, while eight independent XMM chains improve 34--39%. YMM removes most
of a longer cross-half tree: min/max dependent latency improves about
43%/25%, and eight-chain throughput about 36%/32%.

`bench.lua` now builds min/max metadata for every 16-pixel tile in a full 4K
uint16 depth frame, the structure used by hierarchical-Z culling and depth
pyramids. Its scalar comparison tree is explicitly unrolled with stable
branch directions. The optimized XMM/YMM paths improve about 18%/25% over
the prior packed reduction, reaching roughly 4.8x/4.3x the scalar throughput.

## D54. Bias signed-word order into the unsigned min-position domain

Two's-complement signed-word order becomes unsigned order by flipping its
sign bit. Signed horizontal minimum can therefore reuse D53 exactly:

```
min_i16(x) = min_u16(x ^ 0x8000) ^ 0x8000
```

For maximum, complementing the biased unsigned domain and simplifying the
two output transforms gives a single different mask:

```
max_i16(x) = min_u16(x ^ 0x7fff) ^ 0x7fff
```

The input XOR stays packed and loop-invariant masks are shared normally. The
output XOR is scalar before the existing i16 narrowing, so only the live low
word is transformed. YMM still collapses its two biased halves with
`VPMINUW` before the XMM-only `VPHMINPOSUW`. CPUs without SSE4.1 retain the
original signed `PMINSW`/`PMAXSW` shuffle tree.

XMM dependency latency stays approximately flat because `PHMINPOSUW` itself
is the critical path, but eight independent chains improve about 34%. YMM
removes most of the cross-half tree: dependent min/max improve about
32%/29%, and eight-chain throughput about 22%/21%.

The production benchmark suite now generates min/max waveform metadata for
one minute of signed 48 kHz PCM16 audio in 16-sample buckets. It shares the
fully unrolled, stable scalar comparison tree used by the depth benchmark.
The optimized XMM/YMM paths improve about 12%/18% over the prior SIMD code,
reaching roughly 4.4x/4.0x scalar throughput.

## D55. Widen byte extrema into the word min-position domain

There is no horizontal byte-extrema instruction, but zero-unpacking the low
and high byte groups gives two vectors of unsigned words. Their packed
minimum contains every original byte exactly once:

```
lo = PUNPCKLBW(x, 0)
hi = PUNPCKHBW(x, 0)
pairs = PMINUW(lo, hi)
result = PHMINPOSUW(pairs)
```

At YMM width the unpacks operate lane-locally. After the first `VPMINUW`,
the existing half swap and second word minimum combine their results before
the XMM-only horizontal instruction. XMM therefore falls from eight vector
instructions to four; YMM falls from ten to six.

Unsigned max complements bytes with `0xff` on input and the scalar result.
Signed min and max use the same order-preserving `0x80` and `0x7f` masks as
D54, now repeated per byte. All widening is zero-extension after that
mapping, so the unsigned word minimum has exactly the desired byte order.
The final extraction still uses the original byte ctype and preserves signed
or unsigned result extension.

Across signed/unsigned min/max, dependent XMM cost improves about 6--26% and
eight-chain throughput 20--31%. YMM dependent cost improves 18--25%; its
already parallel generic tree leaves a smaller 7--18% eight-chain gain.

The production benchmark suite now scans 16 MiB of signed INT8 neural
activations and emits min/max metadata for every 32-value block, as used by
quantization calibration and saturation diagnostics. Compared with the prior
SIMD reduction it improves about 22% at XMM width and 20% at YMM width,
reaching roughly 6.9x/6.2x the fully unrolled scalar path.

## D56. Fuse word multiply plus horizontal sum into pair dots

The ordinary `i16/u16` multiply keeps each product's low word, after which
`hsum` reduces those words modulo 65536. When that product has no semantic use
outside the reduction, the same value can be computed with signed
`PMADDWD`: it forms full products and adds adjacent pairs into dwords. Adding
the dword partials and narrowing only once at the end has the same low word as
narrowing each product first.

Unsigned inputs are exact for the same modular reason. Reinterpreting an
unsigned word as signed subtracts either zero or 65536. Expanding the signed
product changes the unsigned product only by multiples of 65536, which cannot
affect the final word. The `-32768 * -32768` pair-overflow case also wraps in
the dword domain and retains the same low word.

The recorder recognises `IR_VMUL` directly feeding a word `hsum` and emits a
dword-result `VPMADW` over the multiply's original operands. XMM then needs
two dword shuffle/add stages instead of three word stages; YMM needs the same
two local stages plus its cross-half combine. The complete multiply/reduction
therefore falls from seven to five vector instructions for XMM and from nine
to seven for YMM.

The unused original multiply remains in immutable IR until backwards DCE.
The x86 alias scan now ignores any pure instruction that DCE will discard, so
that dead node does not falsely count its array operand as a second run-time
use. AVX can consequently retain the `VPMADDWD` memory-source form, while
legacy SSE keeps its alignment-safe standalone `MOVUPS`.

Dependent signed/unsigned dots improve about 11% at XMM width and 8% at YMM
width; eight independent chains improve about 14% and 12%. The production
suite adds a 16 MiB PCM16 16-tap polyphase decimator whose bounded samples
make every dot mathematically exact without overflow. It improves from about
2.8 to 2.5 ms for XMM and 1.9 to 1.7 ms for YMM, reaching roughly 5.5x/7.8x
the explicitly unrolled scalar implementation.
