# Table Rehash Wait Boundaries

`lj_tab_resize()` now uses the L-aware retry path while counting old hash keys
before it acquires `GCtab.struct_owner`. Those waits can safely observe fresh
STOPREQ because no replacement array/hash vector has been published and no
structural owner has been claimed yet.

After `lj_tab_struct_enter()` succeeds, resize publication still uses yield-only
waits until the attempt either commits or jumps through `retry_resize`. That is
intentional: an L-aware wait can raise fresh STOPREQ, and a longjmp inside the
publication window would leak the per-table structural owner and the temporary
replacement storage reserved for that attempt. The retry path releases/reverts
claims, frees unpublished storage, leaves the owner, and only then performs the
L-aware wait.

Focused validation:

- `tools/ci/lua_test.sh m5_tab_struct_owner m5_tab_resize_stress m5_tab_capi_resize_stress m5_tab_colocated_resize`
