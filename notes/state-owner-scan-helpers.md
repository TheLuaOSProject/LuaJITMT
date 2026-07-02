# lua_State owner and scan helper surface

Slice: route `lua_State` ownership and GC2 scan publication words through
shared helper accessors.

Changes:
- Added `lj_state_owner_*()` and `lj_state_scan_*()` helpers in `src/lj_obj.h`.
- Routed C-side owner claim/release, bootstrap owner publication, state
  initialization, GC2 owner checks, and scan-epoch publication through helpers.
- Routed focused owner/GC2 fixtures through the same helpers for synthetic busy
  owners and scan assertions.
- Documented the invariant formerly checked by `m5_state_owner`: raw C-side
  `thr_owner`/`scan_epoch`/`scan_dirty_epoch` access in production and focused
  fixtures.

Note:
- The x64 VM still has intentional inline stores for interpreter ownership
  transitions; this slice guards C-side access only.

