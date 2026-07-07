# GC2/JIT concurrent hardening

## 2026-07-07

This slice tightened several GC2 and JIT race edges found while running active
MT JIT stress:

- Current Lua call stack scanning in both collectors now uses the active bytecode
  result window plus live-local debug metadata, so weak-key/value collection no
  longer sees stale current-frame slots while FNEW/string allocation results are
  still being published.
- FNEW upvalue snapshotting and `string.rep` publish their transient source or
  result roots before allocation/GC checks can run.
- Non-weak `BC_TSETM` range stores publish the source range through the GC2 pair
  barrier before the fast contiguous copy path, preserving non-stack source
  ranges during active marking.
- Legacy gray-list duplicate rescan entries are detached before traversal and
  popped gray-list heads clear their `gclist` link after a successful CAS, which
  prevents stale self-cycles from spinning propagation.
- JIT trace slot readers now use safe trace-ref helpers before dereferencing
  entries that can be concurrently cleared or retired.
- Active-MT `setmetatable()` recording keeps the previous-nil fences for normal
  metamethods, but the `__metatable` absence guard uses the shared-table helper
  miss path so raw metatable changes can still trace after MT activation.
- Helper-backed JIT table stores treat recorded array/hash slots as hints and
  re-resolve current or forwarded generations before keyed CAS publication.
- Helper-backed JIT table reads can recover a current hash-generation value
  after a stale miss/read validation failure, avoiding unnecessary waits on
  current-retiring hash paths.
- GC2 allocation accounting increments the JIT hard-check counter when a hard
  assist is triggered from active trace execution.
- Single-thread `jit.flush()` uses the direct full-flush path even when no trace
  is currently active, preserving the ordinary TRACE `"flush"` VM event while
  leaving multi-TG scoped safepoint flushes eventless.

Test hardening in the same slice:

- `t-safepoint-handshake.c` now expects the full GC2 SSB flush to drain the
  active slot plus the fixed SSB ring.
- JIT table-store/read tests count live traces instead of assuming trace number
  1 survives helper-backed active-MT publication.
- The GC2 TBAR black-gate probe compares cycle and SSB/grey deltas instead of
  assuming a particular collector phase snapshot.
- `t-gc2-jit-hard-check.c` sets up fresh GC2 mark cycles per allocation kind and
  validates traceable hard checks for table, cdata, closure, and string
  allocations.
- `t-jit-util-flush-race.lua` caches `jit.util` reader functions and channel
  methods before the flush race, keeps the probe loop interpreted, and uses a
  bounded probe count with a longer default timeout.
- The VMEVENT flush test distinguishes direct single-TG flush events from
  intentionally eventless multi-TG safepoint-leader flushes.

Validation:

- `make -C src clean && make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_util_flush_race m6_jit_flush_thread_stress m6_jit_flush_thread_heavy_stress m6_jit_mt_activation_flush m6_jit_gcworkers_activation_flush m6_jit_vmevent_flush m6_jit_traceerr_format m6_jit_perftools_native m6_jit_gdbjit_publish m6_jit_io_native_stopreq m6_jit_cclosure_upvalue_flush m6_jit_trace_proto_gc m6_jit_env_mutation_flush m6_jit_threading_nyi_boundary m6_jit_buffer_method_shared_nyi`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m9_m10_gc`
- `tools/ci/lua_test.sh m6_jit_util_flush_race m6_jit_mcode_publish m6_jit_gc2_readiness m6_jit_table_store_helper m6_jit_barrier_xpoll m6_jit_tbar_gc2_black_gate m6_jit_vmevent_flush m3_vm_safepoint`
- `tools/ci/lua_test.sh m3_safepoint_handshake`
