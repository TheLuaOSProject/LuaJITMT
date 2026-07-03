# TSETM range helper

## 2026-06-20

- Added `lj_tab_storetvn_forvm_array()` for VM `BC_TSETM` range stores.
- The helper resolves each integer key and publishes with
  `lj_tab_trystoretv_cas()`, so a range-fit decision made before an array resize
  does not write through stale forwarded slots.
- The helper owns the post-publication barrier decision. It checks thread-global
  marking or black table after publishing the range, re-resolves the logical
  integer keys, runs GC2 pair barriers, runs one table barrier, and then performs
  the legacy black-table white-value scan.
- This preserves the old VM shape where `BC_TSETM` copied first and checked the
  barrier predicate afterward, avoiding a stale false predicate if marking starts
  during the range helper.
- The x64 VM saves `SAVE_PC` before calling the helper because key resolution
  can allocate on resize races.
- Kept `lj_tab_storetvn()` intact for existing raw contiguous-copy users.

Validation:

- `tools/ci/m5_x64_tset_nil_snapshot.sh`
- `tools/ci/m5_tab_cas_store.sh`
- `tools/ci/m6_jit_table_store_helper.sh`
- `git diff --check`

## 2026-06-20 current-retiring follow-up

- Replaced the helper's per-key `lj_tab_setint()` routing with a
  current-generation array resolver that raw-loads the table's current array
  mirror, detects a RETIRING current array, and hops through its `next_gen`
  before CAS-publishing.
- The post-publication TSETM barrier range uses the same resolver so a barrier
  scan cannot spin in `lj_tab_array_snapshot_acq()` when the table mirror still
  points at a retiring generation.
- Added `exercise_tsetm_helper_current_retiring()` to `t-tab-cas-store.c`. It
  restores the table mirror to a RETIRING old array with ordinary values, runs a
  range store, and asserts the old generation remains unchanged while the
  successor receives the new values.

Validation:

- `tools/ci/lua_test.sh m5_tab_cas_store`
- `tools/ci/lua_test.sh m10_generational`

## 2026-06-26 real-bytecode x64 coverage

- Extended `tests/t-x64-tset-forward.c` with a count-hook driven constructor
  case that reaches the real x64 `BC_TSETM` interpreter path.
- The hook catches the temporary table after the final multires call has
  produced its values but before `TSETM` executes, grows the array enough to
  satisfy the VM fit check, republishes the table mirror to an old generation
  with forwarded destination slots, and then lets `BC_TSETM` call
  `lj_tab_storetvn_forvm_array()`.
- The guard asserts the old slots remain `LJ_TFORWARD`, the successor
  generation receives the range values, and logical `lj_tab_getint()` reads
  through the old mirror to the successor values.

Validation:

- `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot`

## 2026-07-03 single-thread current-array fast path

- Added a narrow fast path inside `lj_tab_storetvn_forvm_array()` for the common
  single-thread/current-array case.
- The helper still remains the x64 VM entry point for `BC_TSETM`; DynASM does
  not bypass the parent-aware helper. The helper now direct release-copies the
  range only when:
  - no Lua thread is active or entering MT execution,
  - the destination range fits in the currently published array,
  - the separated array is not retiring,
  - none of the destination slots contains `FORWARD`, and
  - the table does not need weak-write handling.
- If any condition fails, the helper keeps the existing per-key resolver and
  keyed CAS path, so racy resizes and forwarded slots still republish into the
  logical table.
- The direct path still runs the existing post-copy table/range barrier when
  marking is active or the parent table is black.
- Added `LJ_TAB_TEST_HELPERS` counters to `t-tab-cas-store.c` so coverage proves
  the current-array path is taken and `mt_entering` forces the fallback without
  relying on source text.

Validation:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_cas_store`
