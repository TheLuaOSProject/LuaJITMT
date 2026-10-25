# Implementing the architecture review

Starting point: `fdd7adf9`. The user requested implementation of all review
recommendations, with replacement of example-specific trace templates as the
first priority. This is the live implementation ledger; completed evidence is
recorded at each commit rather than inferred from the starting gates.

## Work streams

1. Replace whole-trace rooted read/length shapes with operation-level contracts:
   operand provenance, root slots, call effects, retry/type guards, snapshots,
   and independently checked allocated locations. Grow keys/results, stores,
   and allocations only through their corresponding runtime authorities.
2. Replace first-side scalar templates and handle numeric transitions through
   actual native continuation. Report entries, exits, and rejection reasons.
3. Replace FFI signature/ordinal/register templates with ABI-class contracts;
   retain rooted metadata, native-frame and result ownership, errno, PAC/BTI,
   and independently checked allocation. Exercise real generated-frame unwind.
4. Publish an immutable root-entry certificate bound to the exact trace body,
   incarnation, layout, and machine code. Keep dynamic lifetime/identity,
   bytecode, request, and topology checks on every entry.
5. Centralize test build/run profiles and verify runtime/fixture identity.
   Replace brittle text matching with executable negative controls where
   possible. Keep semantic and post-RA verdicts independent.
6. Measure lease admission/retries and rescan-credit contention, document
   progress and suspension contracts, and test stalled participants. Optimize
   only after measurements identify a worthwhile change.
7. Validate the combined result sequentially with complete bootstrap and
   experimental gates, stock source/import corpora on ARM64 and x86-64,
   sustained concurrency, appropriate sanitizers, and release measurements.

## Coordination and acceptance

The primary agent owns rooted operations and root-entry certification. Luna
agents own scalar side transitions, FFI contracts/unwind, and test/profile plus
progress measurements. They work in isolated snapshots and return reviewed
patches. Focused builds may run separately; full umbrella gates are serialized.

No new locks, unchecked raw heap effects, or admission by an earlier validator's
verdict. Source shapes, IR graph meaning, allocated locations, snapshots,
emitted execution, lifecycle races, and adjacent malformed inputs all remain
part of acceptance. A generic contract replaces a template only when the
additional accepted shapes execute and their unsafe neighbors still reject.
