Buffer COW reference release-store slice

- COW buffer reset/free/grow paths now clear `SBufExt.cowref` with
  `setgcrefnullrel()`.
- GC and GC2 traversal already acquire-load `cowref` when a buffer is observed
  as COW; the clear side now uses the matching publication primitive.
- Follow-up cleanup centralizes `SBufExt.cowref`, `SBufExt.dict_str`, and
  `SBufExt.dict_mt` through `lj_bufx_*` acquire/release helpers. Buffer
  construction, COW setup/clear, serializer dictionary reads, `__concat`
  snapshot validation, GC/GC2 traversal, and the dictionary-barrier fixture now
  use the helper surface instead of direct GCRef loads/stores.
- While validating this slice, an incremental rebuild after editing `lj_buf.h`
  produced a stale crashing binary for `m5_buffer_publish`; a clean rebuild
  passed with the same source.

Verification:

- clean normal build + tools/ci/lua_test.sh m5_buffer_publish
- tools/ci/lua_test.sh m3_gc2_paranoia
- Current helper-surface follow-up verification: clean build,
  `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m3_gc2_paranoia`, and a
  direct dictionary/COW buffer encode/decode smoke passed.
- Buffer concat owner validation follow-up: `meta_buf_sbx()` now accepts the
  immutable `mainthread_acq(g)` root as a valid `SBufExt.L` owner, while keeping
  queued live-thread validation for non-main owners. This restores same-state,
  coroutine-owned, and cross-thread buffer `__concat` snapshots. Verified with
  direct main/spawn/coroutine smokes,
  `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_buffer_publish`, and
  `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m3_gc2_paranoia`.
