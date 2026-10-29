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

Each feature's focused profiles passed independently. The combined segmented
gate and ordinary ARM64/x86-64 corpus/topology/aggregate validation subsequently
completed, together with small HFA stack groups and nil-terminated ITERL; see
`arm64-abi-iterator-integration-2026-10-29.md` for the final source IDs, evidence
and scope. Full architecture parity is not yet established.

## Selected return-exit pause

The combined gate at `1c6988a38` passed through the scalar capability contract
and reached the Lua leaf suite. Its controlled old-tag test paused at the
newly runnable `argument-int` leaf's JFUNCF entry before the selected runner's
JFORL entry. Diagnostic `base[0]` was integer 9 and the current frame function
was exactly the expected leaf. The fixture's assumption that the next native
entry belonged to its runner no longer held.

The test-only entry pause now accepts an optional trace number. Existing
callers retain any-entry behavior; the return fixture explicitly selects its
already verified runner trace. The original function identity, old-tag,
terminal snapshot, exact exit, result, GC and same-trace reuse checks remain.
No ordinary-build pause state or runtime branch was added. The focused return
fixture and its Lua companion passed on ARM64/ARM64e assert/release: each C
profile verified 97 cases, 96 controlled tag mismatches and terminal exits,
402 native runs, 97 GC reuses and both independent admission-negative sets.

Evidence: `return-pause-diagnostic.log` and `return-selected-pause.log` in the
integration evidence directory. The complete leaf suite passed after this
fixture repair; the remaining continuation is recorded below and in the final
integration note.

## Preserve the selected numeric adaptation side

The continuation passed through first-side publication/entry/retirement,
GDBJIT, VM-next, callback result lifetime, FUNCF/FORL/ITERL, and numeric loop
contracts. The dynamic-argument fixture then failed its required division
adaptation-child assertion. Its `maxtrace=2, hotexit=1` setup filled both trace
slots with the root and direct side, then requested another side. The normal
`trace_findfree`/`trace_start` capacity path requested a full flush. Four warm
calls alternated between root allocations with child counts 1, 0, 1, 0;
trace ID 1 was reused and abort counters did not change.

Once that fixture's selected direct child is published, it now raises hotexit
to stop heating additional sides. It retains the exact child allocation,
trace number and machine-code address through warmup and the 100-call native
reuse check. Required native child exits, exact numerical results, lifecycle
checks and the two-slot budget remain. Production cache/side behavior is
unchanged. The complete dynamic-argument contract passes on ARM64 and ARM64e
with direct/randomized placement: 37 modes per process, 222 executions.

Evidence: `numeric-side-diagnostic.log` records the allocation alternation;
`numeric-side-final.log` records the final identity-bound fixture passing.
