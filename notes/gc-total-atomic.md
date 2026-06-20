# GC Total Atomic Accounting

`global_State.gc.total` is still the legacy heap-size counter used by public
APIs, stats, and the staged VM allocation checks. Multiple TGs can allocate or
free concurrently, so the counter now has a helper layer in `lj_gc.h`:

- `lj_gc_total_load()` acquire-loads snapshots for decisions and reporting.
- `lj_gc_total_store()` release-publishes initialization values.
- `lj_gc_total_add()`, `lj_gc_total_sub()`, and `lj_gc_total_adjust()` update
  the counter atomically with relaxed ordering. This is counter accounting; the
  ordering for object publication still lives in the object/table/string
  publication helpers.

Allocator, free, stack rehome, GC step, GC2 pacing, `collectgarbage()`, and
`gcinfo()` readers now go through these helpers. x86-64 VM allocation checks
also name their TSO loads through `x64_vm_gc_total_acq()`,
`x64_vm_gc_threshold_acq()`, `x64_vm_gc2_alloc_since_acq()`, and
`x64_vm_gc2_hard_bytes_acq()`.

Guard: `tools/ci/m5_gc_total_atomic.sh` rejects raw C-side `g->gc.total`
access outside the helper definitions and rejects raw x64 VM allocation-check
loads for the GC total/threshold and GC2 hard-limit fields.

Validation:
- `tools/ci/m5_gc_total_atomic.sh` passed.
- `tools/ci/m5_concurrent_objects.sh` passed.
- `tools/ci/m0_source_guard.sh` passed.
- `git diff --check` passed before staging.
