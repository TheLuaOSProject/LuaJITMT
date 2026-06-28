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
- Legacy/compat bridge removal, excluding public API semantics: 84-90%.
- CI migration from source guards to behavior tests: 73-83%.
- Release-quality soak and benchmark readiness: 47-57%.
- Performance parity with stock LuaJIT: 35-45%, intentionally secondary.

## Done in this slice

- Removed the parser-lock fallback from `lj_cdata_index_l()` string-key field
  lookup. Struct fields, constructor constants, and pointer auto-deref now use
  ID-rooted wait/retry helpers.
- Added `lj_ctype_getfieldq_wait()`, `lj_ctype_ptrstruct_wait()`, and
  `lj_ctype_info_wait()`. These helpers park in native time while the parser
  token is busy and avoid retaining table-owned `CType *` pointers across the
  wait.
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

## Still remaining

- `lib_ffi.c` still has parser-token fallbacks for string parsing and layout
  queries where errors, VLA/VLS size, and rollback behavior need careful
  snapshot equivalents.
- `lj_clib.c` namespace lookup still has a parser fallback path.
- GC2 legacy bridge names and tests remain. The highest-value cleanup is to
  convert exact-name source guards around sweep/finalizer/weak behavior into
  semantic tests, then rename or remove the old bridge names.
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
