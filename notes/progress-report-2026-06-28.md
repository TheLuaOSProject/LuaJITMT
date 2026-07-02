# Progress Report - 2026-06-28

## Current State

- Branch: `v2.1`
- Latest pushed commit before this CI/test-audit slice:
  `81265e77 notes: refresh lockless opportunities`
- Current in-progress slice: CI/test cleanup, behavior-first replacement of
  legacy source-text checks, and FFI C-call native-state helper refactoring.
- Safety priority: language semantics, memory safety, GC visibility, and stability remain higher priority than LuaJIT performance parity.

The production table resize-forward slice is landed, and the follow-up x64
closed-upvalue publication slice is also pushed. The current work is tightening
the test harness so source-text checks do not block better implementation patterns
when behavior fixtures can cover the same semantics. The previous audit
blockers addressed in recent slices include:

- Hash replacement sizing now accounts for visible hash keys even when their current value is nil, and resize publication uses a retry path with generation/flags CAS checks.
- Legacy GC and GC2 now resolve forwarded table slots during mark traversal and weak clearing.
- GC2 now marks retired separated-array backing memory, matching retired hash-node handling.
- The x64 TSET/TSETM fixture now expects old non-nil separated-array slots to become internal `FORWARD` sentinels.

Unrelated local scratch files are still present and intentionally ignored:

- `.luarc.json`
- `a.out`
- `m`
- `t`
- `tests/stock/test/m`

## Done And Pushed

Recent completed and pushed slices:

- `81265e77 notes: refresh lockless opportunities`
- `07f35b54 m6: publish all x64 closed upvalue stores`
- `85e4440f notes: update lockless progress report`
- `f885c2c5 m5: stress table resize forwarding`
- `1240fb5d m5: forward table resize generations`
- `e86c8acb m9: own GC stats snapshots`
- `cbbe57c6 m3: own SMR retire epoch queries`
- `92a335f5 m7: own cdata finreg notifications`

The broader project already has landed ownership/facade work across tables,
GC2, weak tables, traces, ctype, FFI, finalizers, hooks, and native-state
boundaries. Earlier CI source-text checks are now obsolete under the no-source-text-checks
policy.

## Completed In This Slice

Follow-up stress and liveness:

- Added `t-tab-resize-stress.lua` for resize vs weak clear, resize vs GC
  traversal, and resize vs traced stores.
- Added `m5_tab_resize_stress` as a focused CI wrapper and as part of
  `m5_concurrent_objects`.
- Narrowed `tab_rehash_hashcount()` so nil-key/non-nil-value hidden fixture
  slots are skipped unless the value is an active cdata finalizer publish claim.

Table resize forwarding:

- Added CAS helpers for array `next_gen`, node `next_gen`, and full node-header flags words.
- Made separated-array replacement publish `oldarray->next_gen` by CAS before retiring the old generation.
- Made hash replacement publish `oldnode->next_gen` and claim `TABNODE_FLAG_RETIRING` with a full flags-word CAS.
- Added retry cleanup for failed resize claims, including freeing unpublished replacement arrays/nodes and discardable retire records.
- Moved retire-list publication until after the old generation is successfully claimed.
- Added `tab_freeze_forward()` so old non-nil visible slots are CAS-stamped as internal `FORWARD` before migration.
- Left old nil slots as nil, avoiding ambiguous GC visibility for absent values.
- Migrated values with put-if-absent CAS so a concurrent successor write is not overwritten.
- Conservatively sizes hash replacement for visible hash keys and array shrink tails.

GC and weak-table correctness:

- Legacy GC table mark traversal resolves forwarded array/hash slots.
- Legacy weak clearing resolves forwarded slots and clears the successor slot, not the retired slot.
- GC2 mark traversal and weak processing now do the same.
- GC2 marks retired separated-array memory in addition to retired hash-node memory.

Tests and documentation:

- Updated C fixtures for production-forwarded old slots.
- Preserved nil-slot expectations where old slots were logically absent.
- Documented that `lj_tab_resize()` must freeze-forward array/hash slots and
  avoid direct snapshot copies in the migration path. Behavior fixtures own the
  observable forwarding contract.
- Documented the intended GC2 weak fields; M8 weak behavior fixtures own the
  observable weak-table/finalizer contract.
- Current CI/test cleanup blocks repository source reads through aggregate
  result helpers, caches repeated same-profile clean builds inside a single Lua
  test process, and thins the M7
  callback-runtime wrapper to behavior coverage.

## Validation Passed

- `make -C src -j2`
- `tools/ci/m5_tab_forward_filter.sh`
- `tools/ci/m5_tab_cas_store.sh`
- `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot`
- `tools/ci/m6_jit_table_store_helper.sh`
- `tools/ci/m8_weak.sh`
- `tools/ci/m10_generational.sh`
- `tools/ci/m9_m10_gc.sh`
- `git diff --check`
- `tools/ci/m5_tab_resize_stress.sh`
- `tools/ci/lua_test.sh m5_tab_keylock_lookup`
- `tools/ci/lua_test.sh m5_concurrent_objects`
- `make -C src clean`
- `make -C src -j2`
- `make -C src clean`
- `make -C src amalg`
- `make -C src clean`

The amalgam build passed with existing warning noise.

## Best Lockless Opportunities

Worth doing:

- Table resize stress coverage and remaining edge proofs.
  The implementation is now much more lockless, but it still deserves multi-thread stress around resize, weak clear, GC traversal, JIT stores, and finalizer interaction.

- Source local/upvalue ownership.
  This is a high-value correctness area because closure/upvalue semantics are core Lua behavior. It is worth making ownership explicit even if it costs performance.

- Traced FFI native-state protocol.
  Worth doing for safety. Do not try to make mutable `ffi.cdef` fully lockless unless requirements change; that is a lower-value, high-risk target.

- Selected GC2 handoff queues and wait helpers.
  Worth tightening where the state is already single-owner or CAS-owned. Avoid changing lifecycle parking/shutdown paths just for lockless purity.

Probably not worth making more lockless:

- User-facing `threading.mutex` and channel blocking semantics.
  These are synchronization APIs by design.

- Safepoint leadership and GC2 lifecycle parking.
  These are rare, semantic coordination points; locks or blocking waits are acceptable if they keep shutdown and phase changes stable.

- GDBJIT descriptor locking.
  This is tooling/debugger integration, not a hot language path.

- `ffi.cdef` mutation.
  Keep it serialized unless a very specific workload proves it is worth the complexity.

## Percent Complete Forecast

- Foundational atomic/helper/facade migration: 80-88%
- GC2/legacy GC ownership scaffolding: 75-85%
- Weak/finalizer/cdata-finalizer ownership: 75-85%
- Table semantics and resize forwarding: 65-75%
- JIT safety and trace/native-state handling: 60-70%
- FFI safety excluding mutable cdef concurrency: 55-65%
- Test and source-text-check infrastructure: 80-88%
- Performance parity with LuaJIT: 35-45%
- Overall safety/stability objective: 65-75%
- Overall safety/stability plus near-LuaJIT performance: 50-60%

## Time Remaining Forecast

Assuming one focused senior engineer and continued subagent review:

- Table resize-forward follow-up stress/proof pass: 1-3 focused days.
- Correctness alpha, with major known semantic races closed and guarded: 2-4 focused weeks.
- Strong beta, with broader stress coverage and major JIT/FFI/upvalue gaps closed: 6-10 focused weeks.
- Performance pass toward near-LuaJIT behavior on key workloads: 4-10 additional weeks, safest after semantic closure.
- Production-confidence soak and workload validation: 3-6 months.

If safety/stability stays ahead of LuaJIT-level speed, the credible path is shorter: close semantic races first, then optimize only large, localized regressions.

## Immediate Next Steps

1. Finish validating and push the CI/test cleanup slice.
2. Keep source-text checks retired; add behavior fixtures where the semantic path is
   observable at runtime.
3. Add the `lua_getlocal()` local-cell acquire-read follow-up.
4. Continue the traced FFI native-state protocol behind disabled traced calls.
5. Run fresh pinned benchmarks after correctness work lands, then update the performance forecast with data.

## Main Risks

- Table resize remains the largest correctness risk because it combines replacement generation publication, concurrent readers/writers, GC visibility, weak clearing, and JIT fast paths.
- Colocated arrays still need careful treatment because they cannot use separated-array `next_gen`.
- More lockless is not automatically better; rare lifecycle coordination should stay simple if lockless replacement would weaken stability.
- Performance can be recovered later, but optimizing before semantic closure risks reintroducing races.
