# Threading State Thread Validation

Date: 2026-07-07

## Problem

Each Lua state can carry a raw `mt_thread` reference to its owning
`threading.thread` userdata. Attach and GC stack-scan paths loaded that
reference and marked or cached it without first proving that the referenced
object was still a valid queued userdata object.

## Change

- Factored the thread-userdata candidate check used by live-root nodes.
- Added `lj_thread_state_udata_acq()` for validated `lua_State.mt_thread`
  reads.
- Routed attach, legacy GC stack scans, and GC2 stack scans through the new
  accessor before marking or caching the userdata.
- Extended `t-threading-live-root.c` to cover state `mt_thread` null, live,
  wrong-type, and unregistered-pointer candidates.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m4_threading_live_root`
- `tools/ci/lua_test.sh m4_threading_join_gcscan`
- `tools/ci/lua_test.sh m3_gc_active_thread_roots`
- `tools/ci/lua_test.sh m4_threading_shutdown`
