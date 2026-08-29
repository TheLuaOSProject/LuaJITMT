2026-06-20

Slice: side-trace child-count update discipline.

Changes:
- Added la_cas16() to the atomic helper layer.
- Added trace_nchild_cas_acqrel(), trace_nchild_inc_acqrel(), and
  trace_nchild_dec_acqrel() for GCtrace.nchild.
- trace_stop() now increments root nchild through the CAS helper when a side
  trace is linked.
- scoped side-trace retirement now decrements root nchild through the CAS
  helper after unlinking a side trace from the root side chain.
- Updated focused tests to exercise inc/dec helper behavior directly and to
  read nchild through trace_nchild_acq().

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit_token.sh
- tools/ci/m6_jit_flush_hs.sh
