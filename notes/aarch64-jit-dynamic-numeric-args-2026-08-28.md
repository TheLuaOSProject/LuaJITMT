# ARM64 JIT all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one more exact macOS ARM64 root while keeping the broad
recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, step)
  while x < limit do
    x = x + step
  end
  return x
end
```

The accumulator, limit and step are all live `NUM` parameters. The change adds
no backend opcode, spill rule, call path, heap operation or new publication
mechanism. It is a second prototype/snapshot certificate over the ten-reference
dynamic-NUM kernel already used by the fixed-initializer dynamic-step root.

## Exact recorder certificate

The admitted prototype is fixed to:

- `framesize = 5`, `sizebc = 13`, `numparams = 3`;
- no upvalues, numeric constants or GC constants;
- exactly `PROTO2_CELLOPS` in `flags2`; and
- root `startpc = proto_bc(pt) + 5`.

All thirteen bytecodes are checked. The body is the exact `CGET/CGET/ISGE/JMP`
preheader followed by `LOOP`, two parameter `CGET`s, one `ADDVV`, one `CSET`,
the backedge, final `CGET`, and `RET1`. Opcode, A, D/C, B and jump fields are
mutation-tested; the B-field test deliberately flips bit 24 rather than
mistaking the C field for B.

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `STEP` from slot 4;
3. PHI-marked `X_PRE = STEP + X`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT > X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE + STEP`;
9. guarded `X_BODY < LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, and footer PCs are
`{bc+6,bc+2,bc+11,bc+6,bc+11}`. The five restored entries are slots
`{2,5,2,2,2}` referencing `X_PRE` four times and `X_BODY` once. The base delta
is zero.

The post-RA certificate requires one terminal `NOP`, zero stack adjustment,
no spills or renames, and FPR-only numeric values. The loop-carried
`X_PRE/X_BODY/PHI` family must share one register. `STEP`, `LIMIT` and that
family must be pairwise distinct, and the original `X` cannot alias `STEP`.
`X` may alias `LIMIT` or the PHI family after its first-add use dies.

Observed ordinary ARM64 allocation was `X=d2`, `STEP=d1`,
`X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`. Ordinary ARM64 emitted 136 bytes with
`mcloop = 76`; ARM64e/BTI emitted 140 bytes with `mcloop = 80`, differing by
the leading authenticated-entry instruction. Both contain exactly two double
FADDs and two double FCMPs with the certified ordered guard polarity.

## Runtime and fail-closed proof

The native fixture runs direct and randomized-register builds on ARM64 and
ARM64e/BTI. It proves:

- root publication, native entry and reuse while all three NUM arguments
  change;
- a sensitive exact-binary reuse tuple `(0.25, 1.0, 0.375) -> 1.0`, for which
  stale recorded `x`, `limit`, or `step` each changes the result;
- all five exits, including interpreter conversion and re-entry after an
  integer accumulator exits at the first type guard;
- no side trace after repeated hot exits;
- profile and STOPREQ arrival at the post-admission/native-entry boundary,
  including XPOLL cleanup and re-entry;
- post-admission replacement of the live accumulator by quiet NaN and positive
  infinity, with the ordered preheader guard exiting to the interpreter;
- trace flush restoring the original `BC_LOOP`; and
- continued separation from the fixed-half and fixed-initializer dynamic-step
  profiles.

Inclusive comparison, subtraction, multiplication, division and an extra-add
recurrence remain rejected. The synthetic contract exhaustively mutates the
IR tuples, snapshot headers/entries/footer PCs, prototype fields and bytecodes,
and checks the legal and illegal post-RA alias matrix.

## Commits and validation

The tranche was published incrementally:

- `96e8cf4f` - admit the exact semantic and post-RA root;
- `950de3e8` - add the synthetic mutation/allocation certificate;
- `ad7c5740` - add the ARM64/arm64e native runtime and lifecycle proof; and
- `664b402f` - update the umbrella success summary.

Validation completed on this Apple Silicon host:

- focused semantic/post-RA and native runtime contracts, including direct and
  two randomized-register runs on both ARM64 and ARM64e/BTI;
- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella, including all
  prior root, first-side, publication, exit, retirement, GDBJIT, callback,
  flush/reuse and safepoint contracts;
- the native ARM64 JIT-enabled vendored suite: 509 passed;
- the thin macOS x86_64 platform smoke, a real Rosetta JIT loop with
  `jit.os == "OSX"`, `jit.arch == "x64"`, and `linktype == "loop"`, plus the
  x86_64 vendored suite: 509 passed; and
- restoration of the thin ARM64 experimental helper build followed by native
  publication of fixed-half, dynamic-step, dynamic-argument and mixed-NUM
  roots.

The recurring diagnostics were the pre-existing ARM64 unused
`ccall_rawchild_wait` warning and x86_64 unused `topofs` warning. The x86_64
platform builder's discarded preliminary non-GC64 clean diagnostic is also
pre-existing; the actual target build and smoke passed.

## Next bounded tranche

The smallest next widening is the same all-parameter recurrence with an
inclusive condition:

```lua
while x <= limit do
  x = x + step
end
```

It should remain an exact prototype/snapshot/IR/post-RA certificate. Before
admission, the comparison orientation and AArch64 ordered/unordered condition
codes must be proved for NaN and infinity mutations; no broad relational-family
or arbitrary-arithmetic admission should be inferred from this root.
