# Integrated captured-prefix FFI checkpoint

Source `9cf2ba33c` combines the `ec98dbf5a` byte/forwarding checkpoint with
the corrected variadic-prefix ABI test and captured CData prefix upvalues
from `9861d5399`/`974b5db6a`.

All 27 fixtures in the broad table/CALLXS contract pass on ARM64 and ARM64e
in assertion and release profiles. This covers table and nested-table roots,
argument/source proofs, lifetime and callback cases, numeric and integer
arguments, CALLM producers, aggregate arguments/results, function pointers,
and variadic calls. The captured-upvalue fixture retains positional ABI
checks, requires the changed-type exit to come from its selected trace,
checks exact foreign side effects, and reuses that trace after full GC and
restoring the original value/type.

The wrapper restores ordinary assertion ARM64 without test helpers, profile
`60d81870a89b6f5244ad481ec20248d32fdb3a67f88e8a0aa01ddcafd83b026d`.
This build passes all 509 stock source and all 509 original v2 bytecode cases.

Evidence directory: `/Users/frityet/Scratch/ljmt-byte-wide-combined-20261030/`.
See `captured-prefix-source.txt`, `captured-prefix-all-ffi-four-profiles.log`,
`captured-prefix-stock-source.log`, and `captured-prefix-stock-bytecode.log`.
The earlier ordinary ARM64/x86_64 workload matrix remains scoped to
`ec98dbf5a`; the captured-value matched ordinary probe and its exact binary
provenance are recorded in the captured-prefix component note.

The separate full gate is still running at `ff86407a1`; it does not cover
these later additions. Full x86_64 parity remains incomplete.
