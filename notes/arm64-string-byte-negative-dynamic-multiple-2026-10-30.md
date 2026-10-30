# Negative dynamic bounds for multiple string.byte results

ARM64 now admits fixed multi-result `string.byte(s, first, last)` calls when
both dynamic indices are negative. This closes the specific x86_64 native
shape where the byte base is `len + first` and the exclusive end is
`len + last + 1`. The dynamic first/end must still come from the exact adjacent
three-argument source call and the same immutable string root.

The semantic walk independently binds both negative-index guards, the shared
`str.len` load, the nonnegative base guard, and a checked `end - base` whose
guarded equality proves a positive result width. The `last < 0` guard
dominates the `len + last` operation, and `LJ_MAX_STR < INT32_MAX` proves
both normalization additions cannot overflow and that the exclusive end is
no greater than the string length.
Every `STRREF` is checked against the same base and a nonnegative displacement
strictly smaller than the proven width. The post-RA walk reconstructs these
relations independently. The existing raw U8 snapshot exclusion and emitter
rules are unchanged.

The four-profile C fixture covers INT/INT, NUM/INT, INT/NUM, and NUM/NUM
arguments, widths 2, 3, 4, 8, and 16, and starting positions 1 and 2. It
checks weighted byte values, native entry and exits, peer activity, full-GC
reuse, and cold boundary/type cases. Semantic and post-RA negatives mutate
the first/last sign guards, base guard, checked subtraction relation, and
formal source binding. The existing positive dynamic-range and negative-end
cases remain in the same contract.

## Validation

`tools/ci/arm64_jit_string_byte_contract.sh` passed all four ARM64/ARM64e
assertion/release profiles on this source. The final log is
`/Users/frityet/Scratch/ljmt-negative-start-multiple-20261030/four-profiles-final.log`.
Profile identifiers were ARM64 assert `f8665608fddef70b78d39088e323ce8f2fd2c0cd460b16c43b4cc9edb95784a3`, ARM64e assert `dd80e6a9f1893289e6c5cb91a6af2eaae439c5dea5d0c4b3246ccb6c7fee5641`, ARM64 release `fb25a042021a5cd389265ab7bc9d4ef4fccc184e2cb74651f18059b1ff2b2ca6`, and ARM64e release `bd30d477a33a27200d1850d2f952dcf48724c807301acfe01c055de902a70a5a`.
Each negative-bound case also requires selected-root entry and exit evidence
before and after full GC.

The ordinary ARM64 build used `-DLUAJIT_MT_ARM64_BOOTSTRAP
-DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL` without test helpers. The matching
40-row ordinary probe is recorded at
`/Users/frityet/Scratch/ljmt-negative-start-multiple-20261030/arm64-ordinary.log`;
its ordinary profile identifier is `913aa56047f7fd10814d1919b4949b8abdb04ecc3c6673c8fded4e28cea44d6a`. All rows published a selected root and recorded eight exits both before and
after GC. The same probe source passed on x86_64 in the frozen baseline at
`/Users/frityet/Scratch/ljmt-expanded-parity-20261030/negative-start-multiple-x86_64-before.log`
(40 rows, eight exits before and after GC). The pre-change ARM64 refusal is
preserved at
`/Users/frityet/Scratch/ljmt-expanded-parity-20261030/negative-start-multiple-arm64-before.log`.
