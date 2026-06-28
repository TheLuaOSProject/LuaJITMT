# Progress report - 2026-06-28 C-closure, locks, and CI

Scope: `v2.1` x86_64/Linux lockless LuaJIT fork. Priority remains safety,
stability, and Lua semantics over matching LuaJIT performance when those goals
conflict.

## Current status

Estimated overall completion: 65-75%.

Confidence split:
- Semantics/correctness scaffolding: 70-80%.
- x86_64/Linux runtime lockless substrate: 70-80%.
- FFI concurrency: 55-65%.
- Weak/finalizer/generational GC completion: 50-65%.
- Performance closure versus LuaJIT: 30-45%.
- Release readiness with reliable CI and soak gates: 45-55%.

These are engineering estimates, not guarantees. The branch has substantial
implemented M0-M7/M10 scaffolding and many focused behavior guards, but the
remaining risk is concentrated in long-tail shared object reads, FFI fallback
paths, finalizer/weak-table ordering, benchmark drift, and CI guard quality.

## Done in this slice

- Added acquire snapshots for public C API reads of C-closure upvalue pseudo
  indices in `src/lj_api.c`.
- Release-publishes numeric-to-string coercions back into C-closure upvalues
  for `lua_tolstring`, `luaL_checklstring`, `luaL_optlstring`, and
  `lua_objlen`.
- Converted `string.gmatch` hidden upvalue reads to full `TValue` acquire
  snapshots and its position update to a full primitive `TValue` release store.
- Replaced the old helper-name source guard with behavior coverage in
  `tests/t-cclosure-upvalue-snapshot.c`.
- Kept only narrow static tripwires for raw partial `string.gmatch` position
  writes that are hard to observe deterministically.

Behavior coverage now exercises:
- `lua_pushvalue`, `lua_copy`, type predicates, numeric readers, and coercions
  through `lua_upvalueindex`.
- Table reads/writes, metatable/env APIs, nested upvalue APIs, userdata/thread
  readers, and `luaL_callmeta` through C-closure upvalues.
- `string.gmatch` normal position advancement and `debug.setupvalue` mutation
  of subject, pattern, and position upvalues.

## Verification

Passed:
- `git diff --check`
- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
- `tools/ci/m5_upvalue_publish_gc.sh`
- `tools/ci/lua_test.sh m4_threading_upvalue`
- `tools/ci/lua_test.sh m5_cell_ops`
- Stock string suite: `test.lua --quiet lib/string` (`59 passed`)

Not a current-regression failure:
- `tools/ci/lua_test.sh m4_threading_capi` hangs in
  `threading_join_core` result-claim sleeping. A clean `HEAD` worktree at
  `89f6364d` also timed out, so this appears pre-existing. It should get an
  explicit timeout and a smaller diagnostic fixture before being used as a
  gating signal.

CI lesson: do not run clean/building Lua test cases in parallel in the same
worktree. They race on `src/` build outputs and can delete `libluajit.a` while
another case is compiling.

## Locks outside direct `ffi.cdef`

No broad runtime `pthread_mutex`, `pthread_rwlock`, `CRITICAL_SECTION`, or
`lua_lock`/`lua_unlock` style VM lock was found outside the intentionally
exposed sync APIs. The remaining coordination is mostly CAS tokens, owner
claims, futex/native parking, and short sentinel claims.

Necessary or intentionally semantic:
- FFI parser mutation token also affects `ffi.typeof`, `ffi.cast`,
  `ffi.metatype`, and related string-ctype paths outside `ffi.cdef`.
- `threading.mutex` is a user-facing synchronization API and should stay a
  lock.
- Channels and joins need blocking waits to preserve API semantics and deliver
  STOPREQ/native-state transitions.
- Per-`lua_State` owner claims prevent concurrent mutation of one coroutine.
- Safepoint handshake leadership coordinates GC, shutdown, redispatch, and
  trace flush.
- Threading shutdown / legacy-GC exclusion bridges current full-GC/step
  semantics while secondary threads exist.
- JIT recorder token is nonblocking; losing recorders keep interpreting.
- GDBJIT descriptor locking is debugger-facing and low value to optimize.

Temporary or bridge-like:
- FFI read fallback locks after snapshot attempts.
- Table `KEYLOCK` and claim sentinels during publication/resize.
- GC2 single-owner bridges such as assist, worker, and finalizer active tokens.

Questionable later candidates:
- Dispatch update token: likely acceptable, but the cleanest future candidate
  for a more lockless protocol.
- GC2 worker lifecycle token: rare path; do not replace until shutdown/start
  proofs are stronger.

Worth doing next for "more lockless":
- Shrink FFI read fallback locks one helper at a time, preserving the current
  snapshot-first/lock-fallback shape until rollback and abandoned-entry proofs
  are strong.
- Improve bounded-wait assertions and stress coverage for table claims, GC2
  owner bridges, and join/STOPREQ paths.

Not worth doing now:
- Removing `threading.mutex`, channel/join waits, state owner claims,
  safepoint leadership, table `KEYLOCK`, GDBJIT locking, or GC2 worker
  lifecycle tokens. Those are currently safety/semantics mechanisms, not
  performance-only locks.

## Test and CI audit

Refactored now:
- `m5_upvalue_publish_gc` no longer pins C helper names for C-closure upvalue
  handling. The behavior fixture carries the real contract.

Still legacy/blocking:
- Several `tools/ci/*.sh` wrappers contain broad `awk`/`grep` source bans while
  milestone aggregates call Lua suite cases directly. Static checks in wrappers
  can be bypassed by aggregate execution.
- M7 FFI wrappers still carry shell-only static guards that should move into
  aggregate-included Lua suite cases or be deleted when behavior fixtures cover
  them.
- M6 native/STOPREQ wrappers still pin helper names in places where behavior
  and dump checks should be primary.
- Local-cell/JIT source guards in `tests/lib/suite_cell_ops.lua` duplicate
  runtime and dump behavior coverage. Keep only non-observable memory-order
  tripwires.
- `read_source_file()` remains an escape hatch despite the harness warning that
  source-file assertions are not behavior tests. Add a required reason/category
  for every deliberate source guard: `non_observable_memory_order`,
  `ABI_boundary`, or `temporary_until_fixture`.
- `m4_threading_capi` needs a timeout and a narrower diagnostic split because
  it hangs on baseline and can otherwise block CI indefinitely.

Preferred rule going forward: every source guard should either be converted to
a behavior/dump/stress test, or explicitly marked as guarding a non-observable
memory-order or ABI invariant.

## Remaining work and time prediction

Short term, 1-3 days:
- Finish C API/upvalue audit follow-ups and land more behavior tests for any
  remaining C-closure pseudo-index edge cases.
- Add a bounded timeout/split for `m4_threading_capi`.
- Start moving shell-only static guards into Lua suite cases where they still
  matter.

Medium term, 1-2 weeks:
- Reduce FFI snapshot fallback locks and add stronger rollback/abandoned-entry
  tests.
- Broaden weak/finalizer ordering stress.
- Audit table claim waits and GC2 active-owner bridges for bounded waits and
  clearer failure diagnostics.

Performance/closure, 1-2 additional weeks:
- Run the M9 benchmark matrix, identify regression hot spots, and decide where
  safety-preserving optimization is worth it.
- Only chase LuaJIT-close performance where it does not weaken semantics or
  introduce fragile synchronization.

Release-quality estimate: 3-5 focused engineering weeks from this snapshot if
no major hidden semantic issue appears. If a deep GC/FFI ordering bug is found,
add 1-3 weeks.
