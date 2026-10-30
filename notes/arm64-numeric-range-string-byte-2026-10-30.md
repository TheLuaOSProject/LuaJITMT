# Numeric positive range ends for string.byte

The constant-offset dynamic-end range proof now also permits the recorder's
existing truncating NUM-to-INT conversion when that converted value is the
guarded range end. Both semantic and allocated passes independently bind the
conversion to the upper length guard, the checked subtraction and the
positive width equality for the same string's later STRREF. General
unchecked conversions remain outside this admission path. This emits no
additional instructions and adds no locks.

The fixture now records `byte(s,2,last)` with `last=4.9` as well as integer
ends 2 and 4. It checks interpreted results, native entry/exits, unchanged code
after full GC, range and type transitions, empty/short/NUL/high-byte strings,
NaN, both infinities, integer conversion limits and very large magnitudes.
The existing six malformed range proofs fail independently in both passes.
The numeric recording uses the equivalent `ULE(end,len)` guard ordering; the
fixture was corrected to locate both UGE and ULE before mutation.

All four ARM64/ARM64e assertion/release string-byte profiles pass on the final
source. Restored ordinary ARM64 passes all 509 stock source and 509 stock v2
bytecode tests. In the ordinary matched probe, numeric end 4.9 now selects a
root and produces eight selected exits for eight calls both before and after
full GC, matching the x86_64 executable from `4b8c2f880`. Integer and nil ends
retain those witnesses. Negative dynamic ends and independently dynamic
start/end recordings remain separate ARM64 gaps.

Evidence directory: `/Users/frityet/Scratch/ljmt-numeric-range-byte-20261030/`.
The final passing contract log is `four-profiles-ule.log`; `four-profiles.log`
is the earlier test-shape failure. Ordinary evidence is in
`arm64-ordinary-probe.log`, `x86_64-baseline-probe.log`, `stock-source.log` and
`stock-bytecode.log`. The restored ordinary profile is
`51f51a2053dab2110b0bfe729105c5187ff3630c1fbff33db3e551e4aafcf00c`.
The separately running full gate at `9581ed718` does not include this change.
