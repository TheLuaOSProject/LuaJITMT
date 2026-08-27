# AArch64 JIT integer-spill proof (2026-08-26)

## Scope

This checkpoint proves that the admitted macOS AArch64 integer root-loop
surface can execute with real register-allocation spills. It deliberately
keeps three allocator shapes separate:

1. the fixed VM reserve, with no trace-local stack adjustment;
2. the smallest measured dynamic frame, with one canonical 16-byte
   adjustment; and
3. a heavier 128-byte pressure root that reaches spilled PHIs, copy-spill
   shuffles, and a checked body addition in spill slot 4.

The fixture is `tests/t-arm64-jit-integer-spills.c`; the repeatable driver is
`tools/ci/arm64_jit_integer_spill_contract.sh`.

## Workload and shape selection

All roots use explicit integer arguments. A local counter `i` starts at zero;
the loop increments `i` and every carried `aj`; the function returns `i` and
all carried values. This keeps exit restoration observable instead of merely
creating dead register pressure.

The plain family has the exact formulas:

- semantic instructions: `4*N + 9`;
- snapshots: `2*N + 7`;
- loop/XPOLL snapshot: `N + 4`; and
- final condition exit: `2*N + 6`.

Direct measurement found:

- plain `N=22`: no spills;
- plain `N=23`: the first fixed-reserve shape, using exactly slots 2 and 3;
- plain `N=24`: an abrupt allocator transition to a large copy-spill frame,
  so it cannot serve as a minimal dynamic-frame proof;
- `N=23` plus one invariant integer `k`, with `a1=a1+k`: the smallest stable
  dynamic shape, using slots 2 through 5 and `spadjust=16`;
- adding a second invariant grows beyond that minimal topology; and
- plain `N=26`: the first measured plain return-all shape whose body `a2`
  `ADDOV` is assigned spill slot 4. Plain `N=25` has body spills only in slots
  2 and 3.

## Exact allocator contracts

### Fixed reserve: plain N=23

- `spadjust=0`, semantic count 101, snapshots 53, snapshot-map entries 1103.
- Exactly two semantic instructions carry spills:
  - left/pre-loop `i ADDOV`, semantic offset 24, slot 3;
  - `n SLOAD`, semantic offset 48, slot 2.
- The terminal suffix has 25 exact `RENAME`s. It covers the 24 left PHI
  operands and a second historical rename for the spilled `i` value.
- The ordered effective snapshot-location set has 997 dynamic tuples.
  In snapshot 2, Lua slot 26 restores semantic ref 24 from spill slot 3;
  at loop snapshot 27, the historical rename makes the same value effective
  in `X3`.
- Compact-IR fingerprint: `8e987d05ec986c8b`.
- Snapshot/map/effective-RegSP fingerprint: `a5e15ce0c3358d84`.

### Minimal dynamic frame: N=23 plus k

- `k` is returned and used as the runtime right operand of both `a1 ADDOV`s.
- `spadjust=16`, semantic count 102, snapshots 53, snapshot-map entries 1105.
- Exactly four semantic instructions carry spills:
  - initial `i SLOAD`, semantic offset 0, slot 5;
  - invariant `k SLOAD`, offset 2, slot 2;
  - left/pre-loop `i ADDOV`, offset 25, slot 3;
  - `n SLOAD`, offset 49, slot 4.
- All PHIs remain register-backed. The terminal suffix has 23 exact
  `RENAME`s for the carried `a1..a23` left operands.
- The ordered effective snapshot-location set has 999 dynamic tuples. The
  fixture explicitly checks the spill-3 `i` and spill-2 `k` restorations at
  pre-loop, loop, and body snapshots.
- Compact-IR fingerprint: `9daea8d07564bb57`.
- Snapshot/map/effective-RegSP fingerprint: `3074ad891371262f`.
- Native code contains exactly one `SUB SP, SP, #16`. On BTI builds it follows
  the entry `BTI J`; otherwise it is the first instruction. The one conditional
  backedge targets `mcode + mcloop`, which is strictly after this prologue.

### Copy-spill and overflow pressure: plain N=26

- `spadjust=128`, semantic count 113, snapshots 59, snapshot-map entries 1361.
  This is intentionally not presented as the minimal dynamic frame.
- There are 36 spill-bearing semantic instructions; the highest slot is 35.
- The key exact assignments are:
  - initial `i/a1/a2 SLOAD`s: slots 35/34/33;
  - left `i/a1/a2 ADDOV`s: slots 32/31/29;
  - `n SLOAD`: slot 30;
  - body `i/a1/a2 ADDOV`s: slots 3/2/4;
  - spilled `i/a1 PHI`s and their right operands: slots 3/2.
- The left/right PHI pairs are deliberately unsynchronised before loop
  assembly: left slots 32/31 must be copied to right/PHI slots 3/2 by
  `asm_phi_copyspill()`.
- The suffix is the one exact spare `NOP`; there are no terminal `RENAME`s.
- The ordered effective snapshot-location set has 1243 dynamic tuples.
  Overflow exit 33 maps Lua slot 4 to the pre-operation `a2` ref in copy-spill
  slot 29. The next snapshot maps the successful body result ref to slot 4.
- Compact-IR fingerprint: `0e434c1ea83db142`.
- Snapshot/map/effective-RegSP fingerprint: `697804b1a9938ca1`.

The fingerprints are FNV-1a over the complete ordered compact IR or over every
snapshot entry plus every effective `(ref, register, spill)` tuple after
applying terminal `RENAME`s with the same historical rule used by exit
restoration. They are intentionally brittle: an allocator-layout change must
be reviewed and remeasured, not silently accepted.

## Native lifecycle evidence

For every root, the fixture executes full positive and negative return vectors
through native entry and verifies every returned value. Seeds `1000+j` produce
`1000+21*j` at `n=20`; seeds `-1000-j` produce `-1000+19*j`. The invariant
variant returns `k` unchanged.

The 16-byte root additionally proves:

- ordinary final exit 52 with one publish and one native exit;
- PROFILE publication after post-admission `jit_base`: XPOLL exit 27,
  re-entry, then final exit 52, with two publishes and two exits;
- STOPREQ publication at the same stage: exact XPOLL exit 27, VM-shutdown
  error, epoch acknowledgement, and cleared poll/request words; and
- reuse after STOPREQ cleanup.

The N=26 root calls the compiled function with `n=2` and `a2=2147483644`.
The first addition remains an integer; the body `ADDOV` assigned slot 4
overflows on the second addition. Exit 33 restores the pre-operation integer,
the interpreter re-executes the addition, and the returned `a2` is the exact
Lua number `2147483648.0` (checked as a `TValue` number, not an integer).

Every lifecycle assertion also checks that `cframe` is restored, TG `jit_base`
is null, TG native depth is zero, VM state returns to the captured idle value,
and no runnable trace slot above root 1 exists.

## Validation driver

The driver owns the shared build lock and statically binds the test to:

- the three exact source/pressure shapes and six fingerprints;
- fixed and canonical dynamic spill-layout validation;
- acquire-safe snapshot/RENAME effective-location validation, including
  bounded snapshot slot counts and decoded prototype PC/base metadata;
- two root-entry post-RA layout checks under the `jit_base` lifetime lease;
- root-only recorder/native-entry gates; and
- the absence of FFI, `require`, and a measurement bypass.

It performs repeated ordinary ARM64 runs, the synthetic IR/post-RA admission
contract, repeated ARM64e+BTI runs, randomized/far ARM64e mcode placement, and
then restores the ordinary experimental ARM64 build. On 2026-08-26 the complete
driver passed with three ordinary fixture runs, one admission-contract run, two
ARM64e+BTI fixture runs, one randomized/far ARM64e run, and a successful final
ordinary ARM64 rebuild. The fixture was compiled with `-O2 -Wall -Wextra
-Werror` in both architecture modes.

The hardened integration tree also passed the scalar-loop, strict root-entry,
native-loop, live flush/reuse, phase-gate, exit, mcode-retirement, and emitter
contracts. The only build diagnostics were the two already-known unused-code
warnings for `szmcode` and `ccall_rawchild_wait`.

## Limits

This is a deterministic synthetic integer-root proof, not broad LuaJIT
compatibility. It does not admit or validate side traces, stitched traces,
JFUNCF native entry, calls, allocations, floating-point spills, FFI IR,
`ffi.cdef`, or `require`. It proves the tested spill layouts and lifecycle
paths only; broader register-allocation patterns still need separate gates and
stress coverage.

Post-RA root-entry revalidation assumes that the admitted compact body
pointers, counts, and `nk` remain immutable and authentic under the `jit_base`
trace-body lifetime lease. It acquire-loads and validates the values within
that published geometry; this checkpoint does not claim safe recovery from
arbitrarily corrupted trace headers or forged body pointers.
