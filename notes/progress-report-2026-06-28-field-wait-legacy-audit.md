# Progress report - 2026-06-28 field waits and legacy audit

Policy: safety, stability, and Lua/FFI language semantics stay ahead of raw
LuaJIT performance. Near-LuaJIT speed is not a reason to weaken rollback,
native-wait, or language-visible FFI behavior.

## Current estimate

Overall correctness/stability progress: 70-80%.

- Runtime lockless substrate: 74-84%.
- FFI concurrency outside mutable `ffi.cdef`: 70-80%.
- Interpreter-side FFI parser fallback removal: 58-68%.
- FFI recorder read-only ctype paths: 82-90%.
- Legacy/compat bridge removal, excluding public API semantics: 90-94%.
- CI migration from source guards to behavior tests: 74-84%.
- Release-quality soak and benchmark readiness: 47-57%.
- Performance parity with stock LuaJIT: 35-45%, intentionally secondary.

## Done in this slice

- Removed the parser-lock fallback from `lj_cdata_index_l()` string-key field
  lookup. Struct fields, constructor constants, and pointer auto-deref now use
  ID-rooted wait/retry helpers.
- Added `lj_ctype_getfieldq_wait()` and `lj_ctype_info_wait()`. These helpers
  park in native time while the parser token is busy and avoid retaining
  table-owned `CType *` pointers across the wait. The earlier
  `lj_ctype_ptrstruct_wait()` helper was later removed when pointer auto-deref
  moved to the ID-rooted info wait path.
- Fixed field snapshot qualifier handling so failed/retried attempts do not
  publish partial qualifier accumulation.
- Converted the cdata field source expectation into behavior coverage:
  `t-ffi-field-snapshot.c` now holds the parser token across field get/set,
  pointer auto-deref, misses, metatype dispatch, and constructor constants.
- Extended rollback coverage so failed cdefs cannot leak constructor constants
  or constructor fields.
- Added narrow implementation guards only for the non-observable safety shape:
  no parser lock/direct field lookup in `lj_cdata_index_l()`, no sequence-free
  field snapshot miss, and ID-rooted wait helpers.
- Completed a legacy/compat audit pass. Real removal targets are old
  `legacy_*` GC2 bridge/source guards and duplicate guard scripts. FFI pointer
  compatibility helpers are semantic compatibility logic and should not be
  deleted just because they contain `compat`.
- Removed the exact internal M10 legacy mark-suppression helper name. The
  replacement, `lj_gc2_minor_roots_skip_bridge_mark()`, names the actual
  behavior without implying that legacy marking itself is disabled.
- Removed another internal legacy helper name from the sweep-close bridge. The
  replacement, `lj_gc2_sweep_bridge_close()`, owns the bridge driver's choice
  between real `SWEEP -> IDLE` closure and preserving full-GC fast-forward
  closure.
- Renamed the GC2 sweep close-readiness latch and boundary helpers from
  `sweep_legacy_ready` / `lj_gc2_sweep_legacy_*` names to
  `sweep_bridge_ready` / `lj_gc2_sweep_bridge_*`. This removes another
  fork-era legacy label while preserving the same root-sweep boundary behavior.
- Renamed local/diagnostic legacy wording in GC pacing and GC2 paranoia tests:
  `legacy_live` / `legacy_step` are now current-purpose local names, and the
  paranoia reverse oracle is exposed as `lj_gc2_test_paranoia_root_diff()`.
- Removed duplicate/tombstone-only source guards: M3 no longer duplicates M5's
  x64 `barrierback` guard, M10 no longer duplicates M9's stats-builder guard,
  and old removed-helper tombstones were dropped where current helper checks and
  behavior coverage already protect the boundary.
- Renamed GC2 weak completion telemetry and helper/test surfaces to
  `weak_bridge_*`, including the public developer stats keys and benchmark/stat
  smoke coverage. No old stat aliases are kept.
- Removed tombstone-only M3 CI guards for old weak/sweep phase aliases and old
  paranoia diff aliases; behavior fixtures and current helper guards now own
  those contracts.
- Removed another duplicate set of M3 finalizer source scans. M8 remains the
  owner for close-time finalizer, callback-stack, and finalizer-spawn behavior
  gates, while M3 keeps its positive scheduler ownership checks.
- Renamed the remaining GC2 lifecycle helper surface from fork-era legacy names
  to purpose names: `lj_gc2_mark_begin()`,
  `lj_gc2_preserve_abort_to_idle()`, and `lj_gc2_cycle_to_idle()`. The
  worker-scheduler guard now blocks direct classic-GC calls to the lower-level
  cycle close helper by its current name.
- Fixed the Lua-suite build harness so build-profile signatures persist across
  `lua_test.sh` processes. Default/JIT cases now force a clean rebuild after a
  previous alternate-XCFLAGS build instead of reusing stale disabled-JIT outputs.
  Added `m0_build_profile_switch` as a behavior regression test for that path.
- Removed stale old-name CI tombstones for deleted root/trace/finalizer wrapper
  names and renamed active test/source/tool wording to current classic-GC or
  direct semantic terminology. Active `src`, `tests`, and `tools` no longer
  contain standalone old-compat wording; only FFI `compatptr` conversion helper
  symbols remain because they encode language semantics.

## Still remaining

- `lib_ffi.c` still has parser-token fallbacks for string parsing and layout
  queries where errors, VLA/VLS size, and rollback behavior need careful
  snapshot equivalents.
- `lj_clib.c` namespace lookup still has a parser fallback path.
- GC2 bridge names and tests remain in finalizer areas. The highest-value
  cleanup is to convert exact-name source guards around finalizer behavior into
  semantic tests, then rename or remove old bridge names where the names are
  only fork-era scaffolding.
- Aggregate CI still has many source guards. Keep guards for memory ordering,
  ABI, and generated-code boundaries; convert source-shape checks when behavior
  can observe the invariant.
- Release confidence still needs long stress/soak, sanitizer-style runs where
  practical, and benchmark review after semantic closure.

## Time forecast

- Remaining FFI parser-fallback cleanup: 1-3 focused days for the next batch,
  likely longer if layout/VLA semantics expose corner cases.
- GC2 legacy bridge/source-guard cleanup: 3-7 focused days for the first
  meaningful pass.
- Correctness alpha for x86_64/Linux scope: about 2-4 focused weeks.
- Strong beta with broader FFI/JIT/GC stress and cleaner CI: about 6-10 focused
  weeks.
- Performance cleanup after semantic closure: about 4-10 focused weeks.
- Production-confidence soak: about 3-6 months of workload validation.

## Verification so far

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback m7_ffi_cdata_get_l m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi`
- `tools/ci/m10_generational.sh`
- `tools/ci/m3_gc2_paranoia.sh`
- `tools/ci/m3_gc2_scaffold.sh`
- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/lua_test.sh m2_arena_gcsweep`
- `tools/ci/m8_weak.sh`
