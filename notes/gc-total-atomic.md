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
call `lj_gc_should_step_vm()` before the existing step helpers, so the shared GC
pacing reads stay in C helper code instead of interpreter assembly.

Coverage: `m5_gc_total_atomic` invariant: raw C-side `g->gc.total`
access outside the helper definitions and documents why raw x64 VM allocation-check
loads or reintroduced allocation-check load macros for the GC total/threshold
and GC2 hard-limit fields.

Validation:
- `tools/ci/m5_gc_total_atomic.sh` passed.
- `tools/ci/m5_concurrent_objects.sh` passed.
- passed.
- `git diff --check` passed before staging.
