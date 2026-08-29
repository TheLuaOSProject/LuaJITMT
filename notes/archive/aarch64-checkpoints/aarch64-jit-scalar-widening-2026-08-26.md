# ARM64 spill-free scalar IR widening

## Scope

This checkpoint expands the executable macOS ARM64 `BC_LOOP` surface without
changing its publication topology or memory-safety boundary. The admitted
integer value producers are now:

- checked stack loads (`SLOAD int`);
- checked addition (`ADDOV int`);
- checked subtraction (`SUBOV int`); and
- checked multiplication (`MULOV int`).

The admitted signed guards are `LT`, `GE`, `LE`, `GT`, `EQ`, and `NE` over
those integer values and `KINT` constants. The root must still be an optimized,
self-linked `BC_LOOP` with one adjacent `XPOLL`, terminal integer PHIs, exact
snapshots, no sunk objects, no helper calls, and no post-allocation spill.

This deliberately does not enable ordinary unchecked `ADD`/`SUB`/`MUL`,
unsigned comparisons, bit operations, conversions, `KNUM`/floating point,
loads from heap objects, stores, allocations, FFI, side/stitch traces,
`BC_FORL`, or `JFUNCF` entry.

## Why this is the next safe unit

The stock ARM64 assembler already lowers these instructions without runtime
helpers or GC-visible side effects:

- `SUBOV` shares the flag-setting integer path used by `ADDOV` and guards on
  signed overflow;
- `MULOV` emits a signed widening multiply and verifies that the 64-bit result
  is the sign extension of its 32-bit result; and
- all six signed comparisons lower to integer compare/conditional branches.

Consequently the widening does not introduce a new global-state access, call
edge, allocation point, write barrier, or object-bearing snapshot. Every live
value remains an immediate integer restored by the existing snapshot path.

`src/lj_asm.c` applies the same policy twice: once before IR scratch growth or
mcode reservation and again against the compact post-RA IR. The post-RA gate
still requires `spadjust == 0`, untouched spill cursors, no instruction with a
spill slot, and only bounded register-only `RENAME` records (or the spare
`NOP`) after the semantic IR. `SUBOV` and `MULOV` were added to the integer
value-provenance set so PHIs, snapshots, and allocator renames can name them;
comparisons remain guards and can never be used as values.

The final allocator gate range-checks register IDs before testing a register
set. It also walks every snapshot map after allocation: constants and the root
frame sentinel need no register, while every dynamic value must have no spill
and, after applying every `RENAME` effective at that snapshot, name an
allocated, non-fixed GPR. This mirrors `snap_renameref`, prevents `RID_NONE` or
a malformed out-of-range byte from reaching snapshot restoration as an
unsupported no-register rematerialization, and avoids undefined oversized
RegSet shifts.

## Native workloads

`tests/t-arm64-jit-scalar-loop.c` records and executes exact semantic
opcode/type and snapshot-ref sequences for:

1. descending sum: `ADDOV`, `SUBOV`, and `GT`;
2. inclusive ascending sum: pre-loop `GE` and loop `LE`;
3. descending inclusive sum: `SUBOV` and `GE`;
4. unequal termination: `NE`;
5. an invariant true branch: `EQ` together with the existing `GT`/`LT`;
6. checked multiplication: `MULOV` together with `ADDOV`, `GT`, and `LT`; and
7. runtime-operand arithmetic: `x = (x-s)*m`, proving both operands of the
   native `SUBOV` and `MULOV` are trace values rather than integer constants.

For each independent universe the fixture verifies the complete semantic
opcode/type sequence, exact snapshot-ref sequence, self-link topology,
`BC_JLOOP` patch, trace-1 identity, integer-only constants, zero spills,
`topslot == framesize`, zero stack adjustment, non-empty mcode, the ARM64
admission marker, the exact allocator suffix (two register-only `RENAME`s for
the two-PHI roots and one for the one-PHI underflow root), exact return value,
exact normal-exit snapshot, no runnable trace slot above trace 1, unchanged C
frame, valid non-fixed GPRs for every dynamic snapshot entry, and TG
native/VM-state quiescence. The `EQ` root is also called with a
false invariant (`n=1`, `k=6`), returning `-1` through exactly snapshot 2.

## Overflow and XPOLL evidence

The multiplication root is recorded with `f(10, 1) == 59049`. Calling the
same root as `f(2, 357913941)` performs one safe interpreted multiplication,
then enters native code and overflows the post-loop `MULOV`. Snapshot 7 restores
the operation and the exact final Lua number is `3221225469`.

The subtraction root is recorded near the lower integer boundary. Calling it
with `-2147483647` reaches `-2147483648` before native entry; the first native
body `SUBOV` then underflows. Snapshot 4 restores the operation and the exact
final Lua number is `-2147483649`.

The multiplication root also repeats the deterministic post-admission request
test:

- a TG-local profile publication exits native code through the inherited LOOP
  snapshot 5, consumes the request, re-enters the same trace, and exits through
  the final condition snapshot 8;
- a counted STOPREQ publication at the same boundary produces exactly one
  trace entry and one XPOLL exit at snapshot 5, acknowledges the new epoch,
  clears counted request words, and returns the shutdown error; and
- after the sticky stop flag is explicitly cleared, the same trace remains
  runnable and exits normally through snapshot 8.

No test publishes a side trace, invokes a JIT helper, or permits an allocator
spill to stand in for this proof.

## Validation

`tools/ci/arm64_jit_scalar_loop_contract.sh` owns the shared build lock and:

- checks the exact source workload and semantic IR inventories;
- rebuilds and runs the native fixture three times as ordinary `arm64`;
- reruns the synthetic admission/rejection contract;
- rebuilds and runs the same fixture three times as
  `arm64e -mbranch-protection=bti`;
- runs one additional ARM64e pass with randomized mcode placement
  (`LUAJIT_MCODE_TEST=R`) so the authenticated far-exit path is exercised; and
- restores the shared checkout to the ordinary experimental ARM64 build.

The full contract passed on this Apple AArch64 host. The only build warnings
were the pre-existing unused `szmcode` local in `lj_trace.c` and unused
`ccall_rawchild_wait` helper in `lj_ccall.c`.

The focused prior gates were also repeated after the final fixture shape:
native LOOP execution, live FLUSHJ/reuse, IDLE/MARK/SWEEP phase gates, root
entry, exit ownership, mcode retirement, and the comprehensive fail-closed
suite all passed. The fail-closed suite's correctly configured build also
passed the ARM64 emitter contract. After the effective-RegSP hardening, the
standalone admission contract and the complete ordinary/ARM64e scalar contract
were run again and passed.

The pure admission fixture additionally covers valid `SUBOV`/`MULOV` value
provenance, a 3-by-3 positive `ADDOV`/`SUBOV`/`MULOV` producer matrix, and every
signed guard, while retaining negative tests for wrong types, missing overflow
guards, forward operands, all four unsigned comparisons, NUM
constants/loads, helpers, allocations, malformed PHIs, malformed snapshots,
and non-`BC_LOOP` topology.

## Remaining boundary

This is a validated, executable scalar checkpoint, not general ARM64 JIT
support. The next widening should keep register pressure separate from
semantic breadth: either prove the fixed JLOOP spill reserve and snapshot spill
restore as a dedicated tranche, or add more spill-free integer operations with
equally exact native shapes. Floating point, heap access, calls, FORL/function
entry, side traces, and FFI still require their own lockless lifetime and
safepoint proofs.
