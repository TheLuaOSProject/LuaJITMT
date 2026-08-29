Profile lockless hook dispatch slice
====================================

Status: implemented and guarded.

Changes:

- Removed the profiler hook/dispatch mutex path from `lj_profile.c`,
  `lj_profile.h`, and `lj_dispatch.c`.
- Added atomic helpers for profiler shared state: active VM, callback,
  callback data, sample count, and sampled VM state.
- Added `dispatchmode_*` helpers and made `lj_dispatch_update()` claim a
  `DISPMODE_UPDATE` token before rewriting the global dispatch template.
  Async profiler timer/signal triggers return instead of spinning if they
  interrupt a thread that already owns the token; the owner revalidates the
  requested mode before leaving the update path. Non-async dispatch updaters
  that find the token busy now wait in no-`lua_State` native sleep slices
  instead of burning CPU in a pause loop.
- `tests/t-safepoint-handshake.c` covers both dispatch-token contention
  branches: regular callers must mark the waiting TG native before retrying,
  and async profiler-style callers must return while the update bit remains
  owned by the interrupted updater.
- Added profiler-specific hookmask helpers. The profile hook atomically claims
  `HOOK_VMEVENT`, drains samples with an exchange, invokes the callback, and
  restores the saved hook mask while preserving concurrent hook event/profile
  changes.
- The timer path stores the sampled VM state before release-publishing
  `HOOK_PROFILE`, then requests a dispatch update.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m5_hookmask_atomic.sh`
- `tools/ci/m5_hook_state_atomic.sh`
- `tools/ci/m5_state_owner.sh`
- `tools/ci/m0_matrix.sh`
