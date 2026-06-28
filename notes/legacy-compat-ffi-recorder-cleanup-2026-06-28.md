# Legacy/compat and FFI recorder cleanup - 2026-06-28

## Done in this slice

- Removed Lua 5.2 compatibility branches from active x86-64/Linux source:
  base/table/debug/io/os/string libraries, parser, metamethod helpers,
  fast-function recorder, generic recorder, package registration, x64 VM
  template, and buildvm library marker handling.
- Removed skipped `compat5.2` stock-test lanes and the deleted `table.pack`
  stock fixture.
- Trimmed the install surface to x64 JIT disassembly support
  (`dis_x64.lua` plus its `dis_x86.lua` backend).
- Replaced FFI recorder parser-lock fallbacks with nonblocking `CTBUSY` trace
  aborts for read-only ctype snapshot races.
- Added behavior coverage that holds the ctype parse token only while recording
  and requires a trace abort instead of a parser-lock wait.
- Removed stale source/test guards:
  `m5_tab_emptyhash.sh` test grep, the `m6_jit_buffer_method_shared_nyi`
  recorder-name grep, and duplicate libc `strerror()` source scans.

## Kept deliberately

- FFI pointer/type compatibility helpers: these are C/FFI semantics.
- GC2 `legacy_*` bridge names and weak/sweep counters: these describe active
  safety bridges while GC2 still coordinates with the classic collector.
- Structural raw-field/accessor CI scans where behavior tests cannot yet prove
  ownership boundaries.

## Estimated progress

- Legacy/compat public/runtime surface removal: 80-85%.
- CI migration away from stale source guards: 65-75%.
- Lockless FFI recorder read-only ctype paths: 75-85%.

## Remaining work

- Decide whether to delete unsupported non-x64 backend files wholesale. That is
  a larger source-layout/build-plumbing change, not a small compatibility
  cleanup.
- Convert remaining native STOPREQ/order source scans into targeted behavior
  fixtures where practical.
- Continue replacing interpreter-side FFI parser-lock fallbacks only where the
  fallback can preserve normal language semantics.
