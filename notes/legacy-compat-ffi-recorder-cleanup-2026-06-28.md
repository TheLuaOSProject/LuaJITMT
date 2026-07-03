# Legacy/compat and FFI recorder cleanup - 2026-06-28

## Done in this slice

- Audited Lua 5.2 compatibility branches in active x86-64/Linux source and kept
  the stock `LUAJIT_ENABLE_LUA52COMPAT` path. Legacy cleanup is limited to
  fork-local/threading compatibility shims, not stock LuaJIT behavior.
- Kept `compat5.2` stock-test lanes and the `table.pack` stock fixture so the
  optional compatibility profile remains behavior-covered.
- Trimmed the install surface to x64 JIT disassembly support
  (`dis_x64.lua` plus its `dis_x86.lua` backend).
- Replaced FFI recorder parser-lock fallbacks with nonblocking `CTBUSY` trace
  aborts for read-only ctype snapshot races.
- Added behavior coverage that holds the ctype parse token only while recording
  and requires a trace abort instead of a parser-lock wait.
- Removed stale legacy wrappers and duplicate tests:
  `m5_tab_emptyhash.sh` test grep, the `m6_jit_buffer_method_shared_nyi`
  recorder-name grep, and duplicate libc `strerror()` helper-name wrappers.

## Kept deliberately

- FFI pointer/type compatibility helpers: these are C/FFI semantics.
- Stock Lua 5.2 compatibility mode: this is a LuaJIT build option, not a
  fork-local threading compatibility entry point.
- GC2 `legacy_*` bridge names and weak/sweep counters: these describe active
  safety bridges while GC2 still coordinates with the classic collector.
- Structural raw-field/accessor ownership rules as documentation or behavior
  fixtures. Do not keep source-text CI predicates for implementation spelling.

## Estimated progress

- Legacy/compat public/runtime surface removal: 80-85%.
- CI migration away from stale legacy wrappers: 65-75%.
- Lockless FFI recorder read-only ctype paths: 75-85%.

## Remaining work

- Decide whether to delete unsupported non-x64 backend files wholesale. That is
  a larger source-layout/build-plumbing change, not a small compatibility
  cleanup.
- Keep native STOPREQ/order rules covered by targeted behavior fixtures where
  practical, or by documentation when the rule is not directly observable.
- Continue replacing interpreter-side FFI parser-lock fallbacks only where the
  fallback can preserve normal language semantics.
