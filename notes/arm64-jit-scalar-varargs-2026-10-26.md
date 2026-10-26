# ARM64 JIT promoted scalar variadic calls (2026-10-26)

The experimental Darwin ARM64 CALLXS path now classifies a variadic function's
named scalar prefix and validates its promoted unnamed arguments. It uses the
existing recorder, native-frame lifecycle and ARM64 call emitter. There is no
new signature-specific call wrapper, runtime lock, or shared call authority.

## Admission and calling convention

The CType classifier records whether a function is variadic while retaining
the existing named-argument GPR/FPR banks and compact Darwin stack offsets.
The recorder already emits a callable CARG wrapper carrying the exact CTypeID
and, when extras exist, one NIL marker between the named and unnamed operands.
Admission checks the wrapper against the guarded callable cdata and requires
the marker at precisely that prefix boundary. A variadic call with no extras
uses the named prefix without a marker. The physical CARG list remains bounded
to 32 entries, including a marker when present.

Unnamed Lua numbers must reach C as doubles, booleans as integer zero/one,
strings as their data pointers, and nil as a null pointer. Integer-valued Lua
numbers therefore require an exact INT-to-NUM conversion, including an
equivalent folded constant. Dynamic cdata payload loads, aggregates, vectors,
complex values and other unproven suffix forms remain excluded.

Both admission passes reconstruct fixed register and stack allocation and the
unnamed stack-only suffix. Each extra consumes an eight-byte slot after the
fixed stack extent is rounded to eight bytes; an integer payload occupies four
bytes within its slot. Post-RA validation includes the entire outgoing extent
when checking spill separation and frame capacity. This matches the existing
Darwin branch of `asm_gencall`; no emitter change is needed.

## Exact source values and continuations

The two passes independently bind every normalized argument to the source
snapshot. A real recorder probe confirmed that cdata `__call` setup shifts the
explicit arguments by one slot. With ARM64's two-word frames and source CALL
register A, the FFI function, return PC, callable cdata and first explicit
argument occupy snapshot slots A+2, A+3, A+4 and A+5 respectively. The XSAVE
footer must identify that exact FFI function and frame base; the saved return
PC must match the source CALL or supported CALLM continuation. Source entries
must be ordered, bounded and fully restorable.

C integer conversion can replace a guarded Lua ADDOV/SUBOV/MULOV expression
with plain ADD/SUB/MUL. Named arguments accept that equivalence only when both
are INT, the Lua expression retains its overflow guard, the C carrier is
unguarded, and both operand references are identical. Loop PHI marking is
allowed without changing that proof. This form is exercised by the native
GPR-overflow case.

Canonical boolean and nil snapshot constants are now accepted with exact
primitive type/opcode and zero entry flags. Unsupported or malformed primitive
entries remain rejected. Completed-call snapshots retain the existing
interpreter-only continuation and native pin release, including the bounded
direct scalar producer CALL to CALLM consumer form.

## Validation

The focused assertion-enabled ARM64 fixture passed with profile
`6ca014313909833ffe01f221daf8a8fc98bf70fbbf8e57663a32ca6e5023ca49`.
Its C oracles confirm native-frame entry, function identity and argument
values for a variadic call without extras, mixed suffixes, named GPR/FPR
overflow, ten mixed extras, negative zero and exact double bits. Ordinary CALL
and direct producer/CALLM cases force a post-call exit and verify exact call
counts and the continuation PC, demonstrating that completed calls are not
replayed.

Separate semantic and post-RA clones reject missing, moved and duplicate NIL
markers, unpromoted integer tails, swapped same-typed tail sources, wrong frame
and footer PCs, a displaced callable cdata slot, NORESTORE source entries, and
an undersized allocated frame. Stack capacity is an allocator-only property;
that mutation deliberately leaves semantic admission successful.
The GPR-overflow trace also rejects a different same-typed named argument and
an unchecked arithmetic carrier whose operand no longer matches its guarded
Lua source. The expanded focused fixture passed in
`/tmp/ljmt-varargs-focused-final.log` with the same runtime profile.

The fixtures are part of `m7_ffi_callxs_arm64_scalar`. Frozen source checkpoint
`328fcb232cb9660c3d189eeaaf6445b4f5ef8e5e` passed that entire suite in all four
profiles: ARM64 assertion/release and arm64e/BTI assertion/release. This includes
the existing fixed scalar/pointer admission, ABI, callback, remote-flush, native
frame and C++ unwind/recovery cases alongside the new variadic fixture.

ARM64 and signed x86-64/Rosetta each passed all 509 source cases and all 509
original-v2 imported bytecode cases at that checkpoint. The x64 build profile
was `6e8395a322746f2b3bffe7c544c18804482373127356d766f858359de36c41cb`.
The ordinary ARM64 experimental/assertion runtime was then restored and passed
all eight variadic smoke cases with published machine code, without test-helper
symbols or imported libatomic helpers. Its verified bound profile is
`7bb5df116693a9188aecdd05e094cf1eeefb02c5d8faf9a3957de2a48eddedc1`, runtime
SHA-256 `281c9602a35b8ee4b28cad2817c27c484aaa409e13c703952865c7de220e8e29`.

The standalone matrix driver initially returned failure at its final clean-tree
assertion because the driver's own `src/.lj-test-run.lock/` was untracked. All
suite and corpus commands had already passed. A separate check after automatic
lock cleanup verified the unchanged source checkpoint, clean worktree, ARM64
architecture, code signature and absence of test/atomic helper imports. This
was a harness cleanliness error, not a failed runtime test; the local driver
now excludes only its own lock from that final assertion.

This focused result is not a claim of complete ARM64 FFI or JIT support. The
full experimental/bootstrap umbrella was last run at the preceding local-cell
checkpoint, as recorded in `README.md`. Empty fixed
signatures, general multires/tail calls, arbitrary cdata suffixes, aggregates
and unrestricted side traces retain their existing admission limits.

Local diagnostic evidence: `/tmp/ljmt-varargs-probe.log`,
`/tmp/ljmt-varargs-gpr-probe.log`, `/tmp/ljmt-varargs-native-third.log` and
`/tmp/ljmt-varargs-focused.log`. These are local logs; the checked-in fixtures
provide the repeatable validation path. Temporary diagnostic prints were
removed before the passing focused build.

Frozen validation logs: `/tmp/ljmt-varargs-matrix.log`,
`/tmp/ljmt-varargs-m7-arm64.log`, `/tmp/ljmt-varargs-m7-arm64e.log`,
`/tmp/ljmt-varargs-m7-arm64-release.log`,
`/tmp/ljmt-varargs-m7-arm64e-release.log`,
`/tmp/ljmt-varargs-stock-source.log`, `/tmp/ljmt-varargs-stock-import.log`,
`/tmp/ljmt-varargs-x64-stock-source.log`,
`/tmp/ljmt-varargs-x64-stock-import.log`, `/tmp/ljmt-varargs-ordinary.log` and
`/tmp/ljmt-varargs-final-check.log`.

Reference: Apple's [ARM64 calling convention](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).

## Main checkout integration

The validated branch was fast-forwarded into `v2.1` at `0c615818` and rebuilt
in `/Users/frityet/Projects/LuaJITMT` with the ordinary ARM64 experimental
assertion configuration, without test-helper defines. All eight variadic smoke
cases and both rooted local-cell/upvalue suites passed, including their
concurrent cases. Code signing and the absence of libatomic/test-helper imports
were checked. The integrated runtime profile is
`f19b7f3e1965e2a8e2880945be7e959908f71eda3a00561fa8b262d12042df24`, with
runtime SHA-256
`6f04d0b3c2f50eb4b4cf82585c0e60840614b426660f2c81fa36a552083f5b99`.
Evidence: `/tmp/ljmt-varargs-main.log`. The following commit changes only this
validation note; production sources remain those tested at `328fcb23`.
