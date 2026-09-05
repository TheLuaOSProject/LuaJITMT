# Scalar-hit functional evidence, 2026-09-05

The runtime base is d680421c4cb50b85437d88255bc89358c5e3a6b1. The separate arena
change was excluded from these isolated runs. Final runtime/test identities
are in final-source-snapshot.json. final-validation.json records the last
helper/assert and ASan fixture runs, including the final key and retired-queue
assertions. Runtime sources did not change between the earlier canonical batch
and these final runs. validation-snapshot.json and initial-metadata.json are
historical snapshots, not the final fixture identity.

The preserved logs include the canonical ten-case pass, stock 387/509 passes,
ASan runs with leak detection enabled at runtime, real paused-reclaimer old-path
negative with SIGALRM, stopped negative stack, commands, and the read-only
independent review. Build/generator-only leak detection was disabled as shown
in asan-runs.json; runtime sanitizer checks have no suppressions. The independent
review's FFI documentation item was incorporated in the main note.

The original trees and executable artifacts remain at the absolute paths in
the records. They are not copied here. Normal timing evidence and its paired
control are in bench/tab-scalar-hit-2026-09-05/. Later combined Linux validation
is separate. artifact-manifest.json hashes every other file in this directory.
