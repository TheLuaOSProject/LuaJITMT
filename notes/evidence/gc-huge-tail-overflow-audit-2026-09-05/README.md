# Evidence scope

This package preserves an unimplemented Huge-tail design audit. `review.md`
is the original full audit. `source-and-geometry.json` records the reviewed
dense-overflow source hashes, Linux page size, and proposed tail arithmetic.

`reviewed-dense-W.patch` and its source manifest identify the implementation
being reviewed: that existing prototype uses a separate calloc/free for the
Huge wide proof. They contain no Huge-tail implementation and are not evidence
that the proposed tail controls have run.

`geometry-model.py` checks the recorded arithmetic examples and residue
window. Its output is mathematical validation only, not a runtime allocation,
resident-memory measurement, benchmark, or completed proof-lifetime test.
The targeted runtime controls in the audit remain proposed requirements.

`artifact-manifest.json` hashes the preserved text artifacts and the top-level
audit note. Any later tail implementation or performance comparison needs a
different source boundary and separate evidence.
