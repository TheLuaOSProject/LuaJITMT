# Initial SWEEP coalescing full-harness pilot

These are the original measurement records from
`/tmp/lj-gc-jit-combined-20260905-6cxpl6mp/performance`, generated on
2026-09-05 UTC. The runtime predates the subsequent MARK table-barrier repair
and empty-reclaimed arena optimization. It is a historical cost measurement,
not validation of their final source or a release acceptance result.

`metadata.json` records the base, source overlays, binary hashes, pinned stock
revision, harness hash, and measurement limits. Apply `measured-runtime.patch`
with `git apply --unidiff-zero` to its runtime base to reconstruct the four
production source overlays. Zero context keeps patch syntax compatible with
the repository whitespace check. The
benchmark harness is the unchanged `plan/aux/bench/bench.lua` in that tree;
`benchmark.py` records original temporary paths and commands. `build.py` is the
original build driver; only its normal build was measured.

The JIT run completed all 15 rows. The interpreter process was killed at its
180-second limit after six rows; nine rows are missing. Do not derive an
interpreter geometric mean from the partial sample. Every reported row is the
minimum of five in-process rounds, with one process per runtime/mode. The
recorded full-harness geometric mean is unweighted; it does not describe total
wall time, which is dominated by the slow cases. CPU pinning did not provide
exclusive host or frequency control.

Raw stdout, stderr, JSON, and driver contents are copied unchanged. CSV line
endings alone are normalized from CRLF to LF; fields and values are unchanged.
`artifact-manifest.json` hashes every included file except itself.
