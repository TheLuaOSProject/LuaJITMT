2026-06-20

Slice: trace exittab/exitstub release clears.

Changes:
- Added `trace_exittab_rel()` and `trace_exitstub_rel()` alongside the existing
  acquire helpers.
- `trace_exittab_free()` now release-clears the published `GCtrace.exittab`
  pointer before freeing the saved local vector address.
- `trace_exittab_free()` now release-clears `GCtrace.exitstub` instead of using
  a raw store.

Reasoning:
- Published trace consumers already acquire-load `exittab` and `exitstub` for
  debug/export/unwind/exit-target paths.
- Actual trace-body freeing happens after the SMR grace period; this slice does
  not change that lifetime rule.
- The clear side should still use the same acquire/release vocabulary, so a
  later diagnostic or defensive reader sees either the old, grace-protected
  pointer or the release-published `NULL`, not a raw teardown store.

Intentionally left raw:
- Initial `T2->exittab`/`T2->exitstub` setup in `lj_trace_alloc()` because the
  trace body is unpublished construction state there.
- Current-trace `J->cur` fields because they are recorder/assembler-owned until
  `trace_save()` publishes the copied body.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m6_jit_flush_hs.sh`
