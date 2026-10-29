# Calls, nested sides and aggregate integration

This branch combines the bounded nested-side implementation through
`decfe27e9`, partial-bank HFA lowering and mandatory ABI reconstruction,
captured two-result child calls (`ecc4eaaf6`, `c6272d26b`), and small integer
stack composites (`3cf03f9d9`, `9d17d2e7f`). All source merges were clean.

Review retained the exact mapped P32 and physical-cell NORESTORE rejection
for active-MT nested sides, bounded certificate ancestry, and depth-ordered
ARM64 retirement. Ordinary nested probes now verify the candidate header,
immediate parent and canonical root, and mcode addresses across GC before
requiring native nested exits. The earlier full-IR-copy test-hook crash remains
unlocalized and is recorded in the feature note; the compact private mapped
source test has positive and negative controls and passes its focused suite.

The captured-call review corrected forward liveness: a later second-result
write must not erase a read-before-write requirement. Both unchanged-slot
checks now recognize the complete two-result CALL output window. JFUNCF
accepts stale caller top only after the VM supplies its exact checked callee
frame end; loop entry retains its prior top bound.

Small integer composite placement was checked against AppleClang-generated
callers and callees and explicit numeric oracles. The interpreter and native
emitter now distinguish payload width from the eight-byte composite slot.
The mapped CData root proof and runtime synchronization are unchanged.

Each feature's focused profiles passed independently. Combined full-gate and
ordinary ARM64/x86-64 corpus/topology/aggregate validation is pending under
`/Users/frityet/Scratch/ljmt-calls-sides-integrated-20261029/`.
Nil-terminated ITERL, broader call/return forms and further ABI shapes remain
outside this checkpoint; full architecture parity is not yet established.
