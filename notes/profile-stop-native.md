# M5 Profile Stop Native Boundary

The pthread-backed `jit.profile` timer backend parks the caller in
`pthread_join()` while the profiler thread exits. That wait is now a
native-state region so a STOPREQ handshake can observe and signal the mutator
instead of waiting for the sleeping timer thread to return.

The timer stop helper returns pending safepoint actions to its caller. The
public `luaJIT_profile_stop()` API checks them after native cleanup, while the
Lua `jit.profile.stop()` wrapper clears its hidden callback coroutine/function
registry anchors before delivering STOPREQ. This keeps profiler state stopped
and Lua-level anchors cleared even when shutdown interrupts the stop call.

Validation:

- `tools/ci/m5_profile_stop_native.sh`
- `tools/ci/m5_state_owner.sh`
- `tools/ci/m5_concurrent_objects.sh`
