# Progress report - 2026-06-28 size waits, locks, and CI cleanup

Policy: safety, stability, and Lua/FFI language semantics stay ahead of raw
LuaJIT performance. If a near-LuaJIT performance target requires weakening
these semantics, keep the semantics and document the performance gap.

## Current estimate

Overall correctness/stability progress: 69-79%.

- Runtime lockless substrate: 74-84%.
- FFI concurrency outside mutable `ffi.cdef`: 67-77%.
- Interpreter-side FFI parser fallback removal: 50-60%.
- FFI recorder read-only ctype paths: 82-90%.
- Legacy/compat bridge removal, excluding public API semantics: 83-89%.
- CI migration from source-text checks to behavior tests: 70-80%.
- Release-quality soak and benchmark readiness: 46-56%.
- Performance parity with stock LuaJIT: 35-45%, intentionally secondary.

## Done in this slice

- Removed parser-lock fallback from interpreted numeric cdata element-size
  readers in `lj_cdata_index_l()` and pointer add/sub/diff in `carith_ptr()`.
- Added `lj_ctype_size_wait()`, which retries the sequence-checked size
  snapshot and parks in native time while another parser owns the token.
- Fixed stale-pointer risk after native waits:
  `lj_cdata_index_l()` refetches `ct/info/size`, and `carith_ptr()` refreshes
  `CDArith` cached `CType *` values before falling through to meta/error paths.
- Converted the old element-size source-shape expectation into behavior:
  `t-ffi-element-size-snapshot.c` now holds the parser token, runs cdata
  indexing, pointer add, and pointer diff, confirms native wait, and checks the
  parser sequence only advances by the helper release.
- Added narrow CI guards only for the removed fallback path and stale-pointer
  helper shape.
- Removed a redundant exact source-text check from `m7_ffi_cdef_token`; the C
  STOPREQ fixture owns that behavior contract.
- Moved `t-ffi-finreg-free-invariant.c` into `m7_ffi_finreg` and removed the
  hardcoded `/tmp` compile from `tools/ci/m7_ffi_finreg.sh`.
- Recorded lock and legacy/compat audits in notes.

## Still remaining

- FFI string-key cdata field fallback still takes the parser token. Next safe
  target is a field wait/refetch helper with no returned stale `CType *`.
- `lib_ffi.c` layout/string parsing fallbacks still use parser serialization
  where string types or mutable layout errors are involved.
- GC2 still has real legacy bridge surfaces: weak clearing, finalizer ordering,
  pacing/threshold bridge behavior, and explicit legacy-GC exclusion.
- CI scripts must not contain source-text checks. Keep unobservable memory ordering
  and static ownership rules in comments/notes; convert observable failures
  into behavior or generated-artifact tests.
- Release confidence still needs long stress/soak, TSan-style runs where
  possible, and benchmark review after semantic closure.

## Time forecast

- Next FFI parser-fallback cleanup batch: 1-3 focused days.
- Correctness alpha for the current x86_64/Linux scope: about 2-4 focused
  weeks.
- Strong beta with broader FFI/JIT/GC stress and cleaner CI: about 6-10 focused
  weeks.
- Performance cleanup after semantic closure: about 4-10 focused weeks.
- Production-confidence soak: about 3-6 months of workload validation.

## Lockless direction

Good next lockless targets:

- FFI read paths that can snapshot, wait/retry, and refetch by stable ID.
- Source guards that can become active-token, rollback, trace-abort, or
  generated-code behavior tests.
- GC2 weak/finalizer bridges once behavior coverage proves GC2 owns the same
  liveness and ordering semantics.

Bad removal targets right now:

- Public `threading.mutex`, channels, joins, and sleeps.
- Mutable `ffi.cdef()` parser serialization.
- Table KEYLOCK/FORWARD sentinels.
- Legacy-GC exclusion until the specific `lua_gc` modes have concurrent-safe
  GC2 equivalents.

## Verification so far

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/lua_test.sh m7_ffi_carith_l m7_ffi_cdata_get_l m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_cdef_token m7_ffi_finreg`
- `tools/ci/lua_test.sh m7_ffi`
- `tools/ci/m7_ffi_finreg.sh`
- `git diff --check`
