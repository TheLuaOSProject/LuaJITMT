# Profiler lifecycle under threading

LuaJIT exposes one process-wide profiler singleton. Stock `luaJIT_profile_start`
and `luaJIT_profile_stop` keep their void API and same-VM replacement behavior,
but the lockless threading fork must serialize the singleton lifecycle because
several Lua OS threads can call `jit.profile.start()` and `jit.profile.stop()`
at the same time.

The profiler now has an internal lifecycle state: idle, starting, active, and
stopping. Start claims the singleton before publishing callback data and timer
state. Stop claims the active session before stopping the timer, waits for any
active callback to leave, then clears callback data and the shared stack-dump
buffer. Stale profile hooks check that the active profiler still belongs to the
current VM before loading callback state.

The Lua `jit.profile.start()` wrapper first stops any old same-VM profiler
session, clears old registry anchors, anchors the new hidden callback coroutine
and Lua function in the registry, and only then starts the C profiler. The timer
callback can run as soon as the profiler becomes active, so the callback
registry slots must already exist before `luaJIT_profile_start()` enables the
timer. If the start is an other-VM no-op, the wrapper clears those fresh anchors
again. This avoids clearing anchors from underneath an active callback, invoking
a timer callback before its Lua function is anchored, or leaving anchors behind
after an other-VM no-op start.

On x86_64 Linux, the stock backend uses process-wide `ITIMER_PROF`. The signal
does not identify a Lua thread group, and async dispatch updates cannot safely
perform the multi-thread redispatch handshake. While more than one Lua thread is
attached, profiler timer samples are therefore dropped instead of setting a
profile hook. This keeps `jit.profile` lifecycle operations safe under
threading; per-thread sampled profiling needs a future routed timer/handoff
design before samples can be delivered concurrently.

Behavior coverage is in `tests/t-profile-stop-native.c` via
`m5_profile_stop_native`: it covers sticky STOPREQ cleanup, callback-error
containment, busy callback coroutine ownership, and concurrent
`jit.profile.start()`/`stop()` from Lua threads.
