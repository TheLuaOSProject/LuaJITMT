# Threading Live Root Validation

Date: 2026-07-07

## Problem

The lockless `threading.thread` live-root list can be scanned by shutdown,
legacy GC, and GC2 while another thread has tombstoned a live-node userdata
reference. Those scans read `o->gch.gct` directly from the node reference before
proving the candidate was still a valid queued GC object.

## Change

- Added `lj_thread_live_udata_acq()` as the shared live-node accessor.
- Validated the node userdata reference with `lj_gc2_obj_valid_queued()` before
  reading the object type tag or userdata payload type.
- Routed shutdown, legacy GC live-root marking, and GC2 live-root scanning
  through the shared accessor.
- Added `t-threading-live-root.c` to exercise live, null, wrong-userdata-type,
  and unregistered-pointer candidates.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m4_threading_live_root`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m4_threading_join_gcscan`
- `tools/ci/lua_test.sh m3_gc2_scaffold`
