# Threading extension surface

The fork-local threading APIs are intentionally isolated in
`require("threading")`. They must not leak into LuaJIT's public C API or alter
stock library option parsing.

Keep these active extension entry points:

- `threading.spawn()`, `threading.current()`, `threading.thread:*`,
  `threading.mutex()`, and `threading.channel()` are the user-facing threading
  substrate.
- `threading.gcstats()` exposes runtime telemetry for behavior tests and
  benchmarks. It replaces old source-shape guard tests with observable counters.
- `threading.gcworkers()` controls parked GC2 worker threads for lockless GC
  experiments and tests.
- `threading.gcmode()` keeps GC2 incremental/generational controls out of
  stock `collectgarbage()` and the public `lua_gc()` numeric mode surface.
- `threading.now()` gives benchmark code the monotonic clock used for timing
  lockless behavior.

Remove only stale fork-local compatibility shims. Stock LuaJIT compatibility
entry points, optional Lua 5.2 compatibility, standard FFI behavior, and
standard `lua*`/`luaL*` C API symbols stay intact.

Do not add CI or test cases that grep repository source to prove these rules.
The policy in `notes/ci-source-search-policy.md` applies here: test behavior
with Lua/C fixtures, inspect generated dump/ASM output when code generation is
the observable result, and document ownership/API boundaries in notes like this
one.
