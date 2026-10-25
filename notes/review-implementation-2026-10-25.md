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

## Rooted operation contracts

The numeric read and length certificates now validate each operation by
def-use relationships and scratch-slot effects. They no longer require key 1,
one table, two reads, an exact number of TMPREF/CARG nodes, or an arbitrary
eight-call ceiling. A call consumes the currently written input roots; a
status guard must immediately follow it, and a numeric result load must use
the matching output root with its own post-helper snapshot. The independent
post-RA walk also proves distinct scratch GPRs and full-width table spill
ownership. Unrelated helper calls and raw heap operations remain rejected.

The native matrix covers argument keys, fractional numeric hash keys,
induction keys, two tables and computed keys, INT results, mixed length/read
effects, and five reads per iteration. Every case produced one native trace
and 20 exits across 20 calls, with correct post-GC reentry. Both ARM64 and
arm64e/BTI passed the complete rooted-read contract, including independent
semantic/allocated mutation fixtures, aligned table-spill negatives, original
refusal/type-change/resize/GC/STOPREQ lifecycle tests, and the registered M6
case. The compiled general IR-admission fixture also passed. Log:
`/tmp/ljmt-generic-rooted-contract.log`.

The replaced source-string checks are now covered by executable positives and
malformed-neighbor tests. The production-ordering checks remain separate.
Other review work streams are still in progress; this result does not certify
the final combined implementation.

## Immutable root publication proof

Root publication now seals the final compact body with an exact identity tuple
and a monotonically allocated serial. The body, IR/snapshot pointers and
extents, prototype, original instruction, admission kind, trace number, and
machine-code/exit geometry must all match at entry. This is not a reusable
boolean or a collision-prone checksum. Compaction rollback preserves the
allocation serial but clears the seal; a new allocation cannot reuse it.

Both independent admission passes still precede publication. The final compact
layout is checked again before the certificate is release-published. Release
entry compares its bounded tuple while retaining the TG lifetime publication,
slot/body revalidation, live bytecode, topology, retirement, request, and PAC
checks. Assertion builds additionally rescan immutable contents to detect
illegal writes. Mutable snapshot counters and exit targets remain outside the
seal and retain their existing ownership rules.

The complete root-entry contract passed on ARM64, arm64e/BTI, and an ARM64 build
without `LUA_USE_ASSERT`. Certificate corruption covers every tuple field,
missing publication, and same-address/new-allocation serial mismatch. Existing
layout negatives now also require the publication scanner to refuse sealing;
assertion entry still detects their illegal post-publication mutations. The
same run passed source-generation, retirement/request-race, emitted caller ABI,
first-side ingress, and real hot-loop checks. Log:
`/tmp/ljmt-root-certificate-contract.log`. Release performance and combined
validation remain separate work items.
