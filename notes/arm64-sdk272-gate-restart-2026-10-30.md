# Complete gate restart after Xcode replacement

The full gate frozen at `f752aa882` did not complete. During its run on
2026-10-30, the Xcode installation was replaced: the recorded
`MacOSX27.0.sdk` path disappeared around 06:13-06:16 UTC, and Xcode then
reported 27.2 (27B52), Apple clang 21.0.0 (clang-2100.4.1.1), and
`MacOSX27.2.sdk`. The immediate failure was `otool`/`xcodebuild` resolving the
vanished SDK. The outer runner recorded gate exit 1 and restore exit 72.
This is incomplete validation, not a passing gate or an established source
regression. Original logs remain in
`/Users/frityet/Scratch/ljmt-negative-first-math-complete-20261030/`.

The restarted gate is frozen at `74a48b1b8`, including the already reviewed
negative-first/computed-math, both-negative substring endpoints and closed
root-context no-peer upvalue normalization. It excludes numeric-concat and
child-side work. Before the restarted gate, an ordinary ARM64 rebuild passed
with the current SDK, profile
`bb697c8a925f900f65e9158b71af844b19f884f3a9bda1bdaa83c2b1a59d037a`.
New source, toolchain, preflight, full-gate and eventual restoration logs are
kept separately under
`/Users/frityet/Scratch/ljmt-closed-upvalue-complete-sdk272-20261030/`.

The last complete passing source checkpoint remains `9ebec6139`, promoted on
`codex/arm64-parity` with an independent ordinary rebuild. Its evidence belongs
to the preceding toolchain. Do not promote the newer source on the basis of
the interrupted `f752aa882` run.
