# threading.spawn native start gate

`threading.spawn()` publishes the child userdata and live root before it calls
`pthread_create()`, but the caller must be a native safepoint participant while
that OS call is in progress. A shutdown STOPREQ can then be acknowledged without
treating the caller as a runnable VM thread.

The child OS thread starts attached to its TG, then waits at an internal start
gate. The parent releases that gate only after `pthread_create()` returns and
the parent has checked for a fresh STOPREQ. If shutdown arrives during thread
creation, the parent aborts the gate and joins the child before throwing the VM
shutdown error, so the spawned Lua/C function is never run without a returned
thread object.

This is a threading-only fork invariant. It preserves the stock LuaJIT API
surface by keeping the behavior inside the explicit `threading` extension and
the `luaMT_spawn()` extension entry point.
