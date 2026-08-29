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
use the VM-local predicate documented in `x64-gc-predicate-inline.md`,
which mirrors the shared C `lj_gc_should_step()` decision without a no-work C
call.

Coverage model: `m5_gc_total_atomic` covers GC accounting behavior through the
helper surface. The remaining x64 VM allocation-check operands are documented
beside the backend code because they are implementation details; observable GC
accounting behavior stays in the fixture.

Validation:
- `tools/ci/m5_gc_total_atomic.sh` passed.
- `tools/ci/m5_concurrent_objects.sh` passed.
- passed.
- `git diff --check` passed before staging.
