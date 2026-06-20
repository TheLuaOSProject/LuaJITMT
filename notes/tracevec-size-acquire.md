2026-06-20

Slice: trace-vector size publication.

Changes:
- Added trace_sizetrace_acq()/trace_sizetrace_rel() for jit_State.sizetrace.
- tracevec_publish() now release-stores the trace vector size before
  release-publishing the vector pointer.
- Production trace-table readers in trace lookup, flush, scoped-retire, exit-PC
  mapping, and jit.util trace lookup now acquire-load sizetrace instead of
  reading J->sizetrace raw.
- tests/t-jit-tracevec.c now verifies the mirror through trace_sizetrace_acq().

Notes:
- J->trace remains the recorder-token-held slot mirror used by trace slot
  publish/clear helpers. traceref() continues to acquire the TraceVec pointer
  and bounds against TraceVec.sizetrace, so a stale or fresh sizetrace snapshot
  cannot make it index past the acquired vector.

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- tools/ci/m6_jit_flush_hs.sh
