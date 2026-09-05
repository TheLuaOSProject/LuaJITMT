# Final table coalescing validation

The JSON files and scheduled fixture driver are copied unchanged from the
source paths recorded in their contents. `integrated-validation-initial.json`
preserves the original coalescing fixture's MARK scheduling failure alongside
passing related fixtures and the normal runtime's stock and state-churn tests.
The scheduled records use the corrected fixture and the exact same integrated
runtime archive, with no production source change. Their fixture, archive,
executable, and source hashes are independently checked before publication.

The integrated source metadata records the original fixture snapshot. The
final fixture hash is recorded by `scheduled-fixture-metadata.json`; its
complete source is committed at `tests/t-gc2-sweep-table-coalescing.c`.
The implementation note explains the scheduling correction and earlier
failures, links the full temporary logs, and records remaining limitations.
Normal runtime timings are functional-test completion observations, not paired
performance measurements. All temporary paths identify original artifacts;
reproductions should build the committed source in their own checkout.

`artifact-manifest.json` hashes every included file except itself.
