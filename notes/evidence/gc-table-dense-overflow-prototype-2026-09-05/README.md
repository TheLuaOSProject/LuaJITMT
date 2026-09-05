# Dense overflow prototype: preserved functional evidence

This package preserves the isolated study originally recorded in
`/tmp/lj-dense-overflow-20260905-7tl6kcfk`. Its exact base is
`d680421c4cb50b85437d88255bc89358c5e3a6b1`. The
[study note](../../gc-table-dense-overflow-prototype-2026-09-05.md) is the concise
review entry. The original `audit.md` is copied unchanged; its statements about
which shared files had been edited describe the time the isolated author
finished, before this packaging step.

`dense-W.patch` is the tested and measured four-file change.
`dense-W-candidate.patch` adds only `accessor-comment.patch`; it does not
identify the frozen binaries. `source-manifest.json`,
`candidate-source-manifest.json` and `final-validation.json` retain the
original source/build/binary identities. The later arena/scalar/direct-hit and
Huge-tail changes are outside this package. `metadata.json` records the
initial functional-only scope; `final-validation.json` also covers the later
authorized cost study.

The complete text artifacts from the study's top-level directory are
preserved. Cost-related files are in the
[companion bench directory](../../../bench/gc-table-dense-overflow-prototype-2026-09-05).
`original-artifact-index.json` maps every copied original to its durable path
and hash. Full source trees, static archives, objects and executables remain
at their original temporary paths; binary contents are not checked in.
`supplemental-snapshot.json` adds packaging-time identities for the negative
variants, generated objects and unchanged fixture/harness dependencies. These
supplement the original records, without representing a new runtime run.

The build commands and runtime options are in `final-validation.json`.
`strict-results.json` and `asan-results.json` contain commands, exits and
captured output. Runtime ASan used `detect_leaks=1:abort_on_error=1`; only
build-time generators used `detect_leaks=0`. `negative-results.json` records
the three deliberately broken protocol variants and their assertion failures.
Normal stock results for the base and dense build are also retained.

The `fnew-existing` pass in the first full fixtures is diagnostic only. That
inherited setup lied about the selected allocator, leaving unfinished
construction lanes. The valid full fixture is `t-dense-fnew-consistent.c`,
with results in `fnew-consistent-results.json`; its malformed-identity
negative fails in both strict and ASan builds. The earlier original capacity
failure, settled diagnostic source, and adapter warning remain preserved.
The [later production FNEW repair](../../fnew-fixture-valid-allocator-2026-09-05.md)
provides separate allocator-predicate coverage on a later base.

Development edit scripts and failed builds are historical evidence. Rebuild
from the exact base plus the frozen final patch and hashed final fixtures;
do not replay intermediate edit scripts onto production. For the final full
FNEW case, use the consistent fixture's recorded compile/run commands rather
than treating the settled fixture's exit status as validation. Original
drivers refer to their original temporary layout and write result files;
reproduce future experiments in a new directory.

`packaging-validation.json` records hash verification, patch reconstruction,
result classification and cost-summary regeneration. Packaging did not build
or execute the runtime and did not modify frozen originals. Its manifest
covers all package text, the top-level study note, and the companion bench
manifest.
