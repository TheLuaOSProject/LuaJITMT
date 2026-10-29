# Sparse side frames and numeric-CData variadic FFI integration

Pinned source `0dc7716c8` combines the verified large-side-ancestry capacity
checkpoint with sparse parent snapshot bounds (`9faab9569`) and numeric-CData
variadic suffixes (`ae46bd0da`, hardened at `514e6915c`). No locks are added.
Individual changes and their four-profile evidence are documented in
`arm64-deep-side-capacity-2026-10-29.md`,
`arm64-sparse-side-frame-2026-10-29.md` and
`arm64-ffi-cdata-vararg-callxs-2026-10-29.md`.

## Ordinary combined builds

Logs are in `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`.

- `sparse-ffi-arm64-build.log`: ordinary assertion ARM64, no test helpers,
  profile `43227d6c11900fd73c057308e92d143a9cca2b4bfc39ab9514339c98dc37eb36`.
  Source and genuine original-v2-bytecode corpora each pass all 509 cases
  (`sparse-ffi-arm64-source.log`, `sparse-ffi-arm64-bytecode.log`).
- `sparse-ffi-x64-build.log`: same source, assertion x86_64 with `-Werror`,
  profile `60aba08769138dbaaf5f17cce6581e3b2751e3f85526b31be31a965ad92a5748`.
  Both corpora pass all 509 cases (`sparse-ffi-x64-source.log`,
  `sparse-ffi-x64-bytecode.log`).
- `sparse-ffi-arm64-restore.log`: successful ordinary assertion ARM64 rebuild
  after the x64 run, no test helpers, profile
  `8eba5c8d02c69c548721a6035fb8487ff55e9d66574ee43f5a7161d3e8be4118`.

The complete combined gate is running separately at exactly `0dc7716c8`.
This note does not promote that source as fully gate-verified yet. Lua vararg
forwarding, fixed-function open-result calls, floating-index numeric loops
and further FFI combinations remain active work. Full x86_64 parity is not
complete.
