# Interpreter FFI lifetime-cost evidence

This directory preserves the bounded CPU-29 diagnosis performed against the
frozen rooted-positive-hit normal runtime, its prior integrated control and
pinned stock LuaJIT. See
[the review note](../../notes/ffi-interpreter-lifetime-cost-2026-09-05.md)
for findings and the separate, unimplemented admission-reuse design.

- diagnosis-original.md is an unchanged copy of the original completed
  diagnosis. Its limited full-run observation is intentionally preserved.
- metadata.json, bench.lua, tiny-runs.json, scaling-runs.json and their
  stdout/stderr files describe nine fresh normal runs of the unmodified
  filtered harness, JIT off and GC enabled.
- candidate-filtered-profile.* and the symbolized text reports describe one
  separate instrumented process. Its timing is not included in the normal
  comparisons.
- symbol-link.json, symbol-reports.json and provenance.json document the
  matching symbols-only relink from frozen objects. Measurements used the
  original stripped CLI.
- extended-interpreter/ records root's later complete, untouched full-harness
  observation: exit 0, all 15 rows, 727.427096 seconds under a 900-second bound.
  This is an additional completed observation, not a revision of the earlier
  360-second timeout or the original bounded diagnosis.
- original-local-manifest.json preserves the original /tmp inventory, including
  hashes of local binary artifacts. Those binary artifacts are not copied here.
- provenance.json maps exact text copies to their source paths.
  artifact-manifest.json hashes the durable files and the accompanying note.
- publication-source-audit.md and publication-source-manifest.json record the
  separate design-only queue-failure/throw review and its exact source versions.

No perf.data, executable, object, archive or raw ELF section is included.
All copied files are UTF-8 text with no NUL bytes. Packaging and design review
ran no additional benchmark and changed no production source or test.
