# ARM64 HFA aggregate results

The Darwin ARM64 CALLXS path supports direct aggregate results containing one
`float`, one `double`, or two contiguous `float` members. The classifier records
the exact member count, offsets, member type, and declared CType. One-member
results use the raw S0/D0 carrier; a two-float result uses an adjacent
`IR_HIOP` for S1 alongside the ordinary S0 `IR_CALLXS` result. The recorder
stores each raw lane into its exact offset in a fresh CData box before native
leave. Lua receives the rooted box, never a scalar carrier.

Both semantic and post-RA admission independently check the callable
signature, result IR type, declared CData box, return snapshot slot, and
CALLXS-to-XSTORE flow. The raw floating carriers must not appear in any
snapshot. The ordinary scalar-result snapshot rule is skipped for these boxed
aggregate results. For two-float results, post-RA admission also checks that
the allocated result registers are D0 and D1. The HFA classifier rejects
CData references and declared `CTA_ALIGN` wrappers that exceed natural
alignment.

The existing integer-aggregate CALLXS contract compares candidate and
JIT-disabled oracle values and exact returned CData type, witnesses native
CALLXS/CNEW/XSTORE on the selected candidate root, forces a mismatched-result
exit, and verifies native re-entry and effect counts after full GC. Private IR
clones independently reject bad result snapshots, XSTORE types and sources,
wrong CData types, and—on two-float results—bad high-lane offsets and result
registers. For scalar and integer aggregate results, a negative substitutes a
different earlier CALLXS with the same IR type, checking exact result-source
binding in both independent walks. A loop-unrolled trace exposed stale
high-lane store state across an `IR_EQ` native-frame close; both independent
admission walks now reset that state on closure.

The two-float fixture ends a 32-iteration native-root loop with inputs that
return distinct raw lane bytes for positive/negative zero, positive/negative
infinity, and two different NaN payloads. It compares against the independent
interpreter oracle and explicit expected bytes, and requires a selected-root
CALLXS/HIOP finish with a live peer.

## Limits

Only one-float, one-double, and direct two-float results are classified.
Nested structs, arrays, three-member HFAs, double pairs, padding, unions,
vectors, and other HFA result layouts remain closed. Over-aligned HFA results
remain closed. HFA formals in variadic function signatures are rejected; this
does not open HFA variadic tails, stack-passed HFA values, or stack crossing.

## Validation

`tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all four native
macOS profiles: arm64 and arm64e, with and without assertions. The final run
log after the source-binding and raw-bit test corrections is
`/Users/frityet/Scratch/ljmt-hfa-pair-crash-20261029/followup-four-profile-2.log`.

A strict Darwin x86_64 build with `CFLAGS=-Werror` passed. Its log is
`/Users/frityet/Scratch/ljmt-hfa-pair-crash-20261029/x64-strict2.log`. This is
compile compatibility evidence only, not an x86_64 runtime HFA claim.

The ordinary ARM64 ten-case probe used the no-test-helper build at
`/Users/frityet/Scratch/ljmt-hfa-pair-crash-20261029/ordinary-arm64/`. Its
`sse-result-retained` row (two-float result) passed 25 oracle comparisons and
reported selected root 2 with two CALLXS, two CNEWs, four XSTOREs, and 25
selected-root exits at `pc16:CGET`. The directory contains the immutable
runtime, archive, matching `jit` and `threading.lua` modules, probe and C
library sources, DSO, output, and SHA-256 manifest.

## Earlier one-member validation

All four profiles passed on native macOS ARM64. The captured log is
`/Users/frityet/Scratch/ljmt-hfa-results-20261029/four-profile.log`. The
contract's three expected fail-closed trace aborts per profile are existing
negative probes; the script completed successfully and restored the ordinary
ARM64 assertion build. No lock or shared mutable register state was added.

## Ordinary integrated builds

Root integration `0af2319d6`, followed by the expanded portable probe at
`63156cc14`, passes ordinary ARM64 and x64 assertion builds. The ten aggregate
rows perform 250 interpreter comparisons and 27,520 foreign calls per target.
The new single-float and single-double argument/result rows each publish a
loop with two CALLXS, two CNEW and two XSTORE instructions and produce 25
selected-root exits on both targets. Each row retains all 50 returned objects
through a full collection and checks their declared CType and fields against
independent fixture arithmetic. The retained two-float result remains x64-only.

Source ID, build/run logs, immutable runtimes, archives, modules and fixture
libraries, and SHA-256 hashes are saved in
`/Users/frityet/Scratch/ljmt-hfa-results-integration-20261029/`.
The complete gate at `b549a895b` predates this follow-up; this check does not
claim a complete gate of the HFA-result extension.
