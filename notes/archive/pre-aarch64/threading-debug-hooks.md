# Threading debug hook semantics

LuaJIT's debug hook state is process-global in stock LuaJIT, even though the
Lua API accepts an optional thread argument. LuaJITMT keeps that API shape:
`debug.sethook()` publishes one global hook function, event mask, and count.

With multiple OS threads alive, a hook mode change updates the global dispatch
template and asks live thread groups to redispatch at safepoints. Hook callbacks
therefore run on the `lua_State` that hit the hook, and `threading.current()`
inside the hook must identify that worker's OS thread. Clearing the hook before
workers continue running more Lua code must stop further count/line/call/return
callbacks after the redispatch handshake completes.

Coverage:

- `tests/t-threading-hooks.lua` starts worker threads first, installs a count
  hook from the main thread, verifies that each live worker observes the hook
  with its own `threading.current():id()`, clears the hook, then verifies
  workers can continue executing Lua without more hook callbacks.
- `m4_threading_hooks` runs the behavior with the default JIT mode.
- `m4_threading_hooks_joff` runs the same behavior under `-joff`.

Verification:

- `tools/ci/lua_test.sh m4_threading_hooks m4_threading_hooks_joff`
