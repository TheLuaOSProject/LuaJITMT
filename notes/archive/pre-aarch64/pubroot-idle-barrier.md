# Root publication idle GC2 barrier

`lj_gc_pubroot()` used to call `lj_gc2_markobj()` directly for every GC object
stored through a root-like slot. That marked arena cells even while GC2 was
idle. Stack publication in `lua_pushcclosure()` exposed this: a newly-created C
closure was allocated white, then publishing it on the stack set its GC2 arena
mark bit before any active mark cycle.

The root helper now routes through `lj_gc2_barrier_tv_g()`. Active MARK/WEAK
phases still mark the published object, idle generational mode still goes
through the remembered-root path, and idle non-generational publication no
longer dirties the arena mark bitmap.

Verification:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m2_arena_gcsweep`
- `tools/ci/lua_test.sh m5_registry_root`
- `tools/ci/lua_test.sh m5_stock_api_surface`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
