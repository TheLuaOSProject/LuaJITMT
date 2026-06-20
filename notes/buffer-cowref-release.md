Buffer COW reference release-store slice

- COW buffer reset/free/grow paths now clear `SBufExt.cowref` with
  `setgcrefnullrel()`.
- GC and GC2 traversal already acquire-load `cowref` when a buffer is observed
  as COW; the clear side now uses the matching publication primitive.
- While validating this slice, an incremental rebuild after editing `lj_buf.h`
  produced a stale crashing binary for `m5_buffer_publish`; a clean rebuild
  passed with the same source.

Verification:

- clean normal build + tools/ci/lua_test.sh m5_buffer_publish
- tools/ci/lua_test.sh m3_gc2_paranoia
