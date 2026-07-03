2026-06-20

Slice: trace-vector growth copy acquire.

Changes:
- `trace_findfree()` now reads the previous `TraceVec` with `tracevec_acq(J)`
  before copying its slots into the grown vector and retiring it.

Reasoning:
- `tracevec_publish()` initializes the vector, release-stores the size mirror,
  and release-publishes `J->tracev`.
- Normal trace-vector readers already pair with that publication through
  `tracevec_acq(J)`.
- Growth copies live slots from the old vector and then retires it through the
  same SMR path, so it should observe that old vector through the same acquire
  edge rather than a raw `J->tracev` load.

Validation:
- `tools/ci/m5_jit_trace_publish.sh`
- `make -C src -j$(getconf _NPROCESSORS_ONLN)` after the focused coverage
- `tools/ci/m6_jit_flush_hs.sh`

Note:
- An initial validation attempt ran `make -C src` concurrently with
  `tools/ci/m5_jit_trace_publish.sh`, and the standalone make hit a transient
  generated-header race (`FF_string_gmatch_aux` undeclared). The focused coverage
  itself passed, and the build passed when rerun without another build-owning
  wrapper in `src`.
