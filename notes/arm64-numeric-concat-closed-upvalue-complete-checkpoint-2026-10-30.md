# Numeric concatenation and closed-upvalue complete checkpoint

Production source `68c122f2a8772c1fbdca44007ff30271b4bd0f0d` completes the
full experimental gate on Xcode 27.2 (27B52), Apple clang 21.0.0
(clang-2100.4.1.1), MacOSX27.2.sdk. The outer runner reports
`gate exit=0; restore exit=0`. All four combined M7 FFI profiles and the
actual VM/recorder safepoint runtime tests pass. Ordinary assertion ARM64
without test helpers is restored at profile
`823d225ea5930426a6ad6aea3e3a9cfec022109853cc5ccf28677920803eec6a`.

Evidence is frozen under
`/Users/frityet/Scratch/ljmt-numeric-concat-complete-sdk272-20261030/`:
`gate-source.txt`, `toolchain.txt`, `complete-gate.log`, and
`gate-restoration.log`. The preceding independent gate at `74a48b1b8` also
passes all four M7 profiles and safepoint runtimes, restoring ordinary ARM64
at `0b949a87a07a0d0551b2125cb12dd9d4949b79aa85954e2d4dafc809c757ed0a`.
Its logs are in `ljmt-closed-upvalue-complete-sdk272-20261030/` beside these.
The earlier interrupted `f752aa882` gate remains an environment failure,
not passing evidence: Xcode was replaced while it referenced SDK 27.0.

Compared with the previous complete `9ebec6139` checkpoint, this source adds
negative-first and both-negative fixed substring ranges, exact preservation
of computed numeric helper results across loop calls, closed root-context
UGET normalization through the existing rooted helper, and NUM TOSTR for
bounded numeric concatenation. The substring and concatenation publication
fixtures observe fresh output while the native trace lease is active.
Independent semantic and allocated proofs remain in place. The stock
formatting emitter/helper is reused and no lock is added.

The exact-source ordinary matrix already passes both 509-case stock corpora
on ARM64 and strict x86_64, along with the preceding loop, FFI, byte,
substring, identity, upvalue, and numeric-concat probes. Evidence is in
`/Users/frityet/Scratch/ljmt-numeric-concat-ordinary-sdk272-20261030/`; its
ordinary ARM64 restoration profile is
`e26d7ace1aa5097ec0bbfac56d3749e203a1f532841eec54268fd4488a7b3b06`.

This is a complete gate for the named source, not full x86_64 parity.
Later INT TOSTR, INT modulo and child-return witness changes are excluded
from this frozen run. General calls/returns, child-frame side publication,
recursion, protected calls and remaining iterator/FFI restrictions stay open.
An independent rebuild of the promoted main parity checkout follows.

## Independent promoted-checkout rebuild

The promoted `codex/arm64-parity` checkout at `9f53b4019` was rebuilt from
scratch as ordinary assertion ARM64 without test helpers. Both 509-case stock
corpora and the complete preceding ordinary ARM64 matrix pass, including
negative/both-negative substrings, computed math, no-peer transition and
closed-upvalue cases, and required native numeric concatenation. Evidence:
`/Users/frityet/Scratch/ljmt-numeric-concat-main-rebuild-20261030/`.
The final ordinary restoration profile is
`8b9265fa2089f7380ba5402bf53c559faf3faf0ef5481e71b150e85bee7e0e61`.
