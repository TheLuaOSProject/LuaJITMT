Trace retire/size acquire slice

- Added acquire helpers for `GCtrace.nsnapmap` and `GCtrace.exittab`.
- Routed trace body sizing, trace retirement marking, exittab freeing/reset,
  and legacy GC trace-size accounting through acquired trace metadata.
- Left recorder-owned trace allocation/copy fields raw; those are token-held
  construction paths, not published trace readers.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- tools/ci/m9_gc_stats.sh
