GC gray-list publication slice

- Added shared acquire/release GC-list helpers for legacy `gray`, `grayagain`,
  and `weak` list heads.
- Routed gray/trace marking, table weak-list publication, thread `grayagain`
  publication, propagation pops, atomic list transfers, resets, and emptiness
  checks through those helpers.
- The legacy list head publication path now release-stores published heads and
  acquire-loads consumed heads without changing the existing list ownership
  protocol.

Verification:

- tools/ci/lua_test.sh m3_gc2_paranoia
- tools/ci/lua_test.sh m8_weak
- tools/ci/lua_test.sh m9_m10_gc
- tools/ci/lua_test.sh m5_jit_trace_publish
- tools/ci/lua_test.sh m6_jit_gcstep_guard
