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
- `threading.cpucount()`, `threading.sleep()`, and `threading.fence()` are
  fork-local threading utilities used by tests, benchmarks, and the memory
  model litmus cases. They are not stock LuaJIT APIs, but they are the current
  documented `require("threading")` surface, not legacy aliases.

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
  active VM threads. Before activation, mutating calls claim the same
  `mt_gc_exclusive` gate used by legacy explicit GC so spawn/attach entrants
  cannot run Lua across the process-global locale update.
- `jit.profile` keeps stock behavior before threading activation; fallback
  profiler backends may drop samples while more than one VM thread is live.

Legacy removals should stay limited to stale fork-local threading or diagnostic
entry points. Do not remove or rename stock LuaJIT C API symbols, stock library
options, standard FFI behavior, or stock single-threaded behavior to simplify
the threading implementation.

## Attached Host Native Waits

`lj_threading_attach()` is an internal fork-local entry point for C fixtures and
FFI callback carriers. It is not part of the stock public C API. Once a foreign
OS thread attaches a child `lua_State`, that thread's `TGState` participates in
GC2 safepoint handshakes until `lj_threading_detach()`.

Raw host waits such as `pthread_join()` are invisible to the runtime. A host
thread that remains attached, or owns the main Lua state, must either detach
before a long host wait or bracket the wait with the internal native boundary
(`lj_native_enter()` / `lj_native_leave()`). The native mark tells a GC2
handshake that the thread is outside LuaJIT and can be acknowledged remotely.
Without that mark, another attached worker can call `lua_gc(L, LUA_GCSTEP, 0)`,
start a GC2 handshake, and wait for a host thread that cannot poll while blocked
inside the OS.

This does not add a public LuaJIT API and does not change stock behavior. The
behavioral regression lives in `tests/t-gc-active-collect-assist.c`: an attached
pthread runs explicit `lua_gc(LUA_GCSTEP)` while the host joins it from a native
section. Runtime-owned waits, including `threading.thread:join()`, already use
the same native boundary internally.
