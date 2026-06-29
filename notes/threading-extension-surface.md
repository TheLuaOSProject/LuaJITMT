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

## 2026-06-29 stock-compatibility boundary

The public C headers should continue matching stock LuaJIT's API surface.
Current `src/lua.h` and `src/luaconf.h` differ from the local `upstream/v2.1`
reference only by whitespace/comment text, and `luaMT_*`, `LUA_THREADINGLIBNAME`,
`LUA_GCGENERATIONAL`, and `LUA_GCINCREMENTAL` are not public API.

The visible deviations that remain are tied to the threading experiment:

- `require("threading")` is the Lua-facing extension point and is registered
  through package preload, while its C opener stays hidden from public headers.
- `lua_gc(L, LUA_GCCOLLECT, ...)` and `collectgarbage("collect")` keep stock
  behavior when no secondary Lua thread is live. With live secondary threads,
  they complete/request the GC2 active-thread path instead of entering the
  single-threaded legacy full-GC collector.
- `os.setlocale()` queries remain stock, but locale mutation is rejected after
  threading activation because process-global locale mutation is not safe across
  active VM threads.
- `jit.profile` keeps stock behavior before threading activation; fallback
  profiler backends may drop samples while more than one VM thread is live.

Legacy removals should stay limited to stale fork-local threading or diagnostic
entry points. Do not remove or rename stock LuaJIT C API symbols, stock library
options, standard FFI behavior, or stock single-threaded behavior to simplify
the threading implementation.
