# Table/String Claim Audit - 2026-07-05

This records the current status of two reported hot-path lock claims after the
latest `v2.1` inspection.

## Table structure owner

The old global `global_State.gc2.tab_struct_owner` claim is stale. Structure
ownership is now per table via `GCtab.struct_owner`, so two different tables can
resize independently. The remaining waits are same-table structure windows and
transient resize publication states, routed through `lj_thr_retry_yield()` rather
than the earlier fixed 1 ms sleep.

The remaining bridge is same-table resize migration. The intended next
improvement is cooperative per-generation copy/helping so contenders either help
finish a resize or hop to the next generation instead of waiting behind the
per-table owner.

## String interning

The per-intern shared reader-count claim is stale. Ordinary `lj_str_new()` no
longer increments/decrements a global reader counter. It acquire-loads the
current string-table header, publishes a TG-local active header/depth marker,
rechecks the header/resize bit, then uses per-bucket lookup and a bucket-head
CAS only when publishing a new interned string.

The remaining non-pure-CAS bridge is the TG-local active-header pin and
resize/sweep claim bit. That is required while resize and legacy sweep still
destructively relink/copy/free buckets. Removing it is not a safe small change;
it should wait for helper-copy/deferred-unlink/Harris-style sweep.

Small tunables such as larger `LJ_STRID_BLOCK` or `LJ_STRNUM_BLOCK` can reduce
global fetch-add frequency in unique-string storms, but they should be benchmarked
because they affect transient accounting slack and resize timing.
