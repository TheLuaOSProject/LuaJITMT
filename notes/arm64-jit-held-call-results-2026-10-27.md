# Scalar CALLXS results held across another call

## Diagnosis and change

The previous scalar-table checkpoint left a real ordinary-runtime gap:

```lua
local a = fn(t.value)
local b = fn(t.value)
sum = sum + a
sum = sum + b
```

Results were correct through interpretation, but assembly declined with
IR_RENAME. This is the generic allocated-view rejection marker, not evidence
of an actual bad rename. A narrowly instrumented baseline locates the refusal
in `arm64_postra_callxs_generic`'s result-register predicate. An integer result
held in X27 and a double result held in D11 both fail the old predicate;
the analogous float control already compiles. Log:
`/tmp/ljmt-held-results-allocated-red.log`.

The production change is five insertions and five deletions in the ARM64
admission header. It permits an unspilled scalar GPR result in X19-X28 and
a scalar FPR result in D8-D15. The FPR range was already allowed for FLOAT;
NUM now has the same allocation permission. The existing `asm_setupresult`
and `ra_destreg` paths already move X0/S0/D0 into the allocated destination.
No emitter, allocator, shared runtime, GC2 or x64 production change is needed.

The independent register-bank, spill width/alignment/bounds, call-crossing
liveness, snapshot-location and outgoing-area checks remain in force. This
does not admit arbitrary caller-clobbered destinations. U32 results retain
their existing exact U32-to-NUM conversion and consumer restrictions.

## Focused evidence

The ordinary assertion runtime compiles the original held-result case and
INT, NUM and FLOAT producer/consumer variants which pass the first result to
the second call and use it again afterward. Logs:
`/tmp/ljmt-held-results-green-ir.log` and
`/tmp/ljmt-held-results-types.log`.

The dedicated native fixture covers four distinct runner prototypes: INT,
NUM, FLOAT and U32. An active waiting peer exercises the multithreaded path.
The U32 helper cycles through zero, one, signed-boundary, high-bit, maximum
and alternating-bit values. It consumes those values as doubles. Checks
include exact results, producer/consumer C counts, errno, both native-frame
identities, trace pins, frame and XSAVE cleanup, and a published FORL trace
with both calls in the loop.

Each type separately forces a POSTCALL exit after its producer and after
its consumer. The saved continuation must be the bytecode immediately after
that particular CALL. Exact C counts exclude replay. After restoring the
epoch, the same trace is executed again. All eight forced exits pass.

The admission fixture clones four authentic traces and independently checks
each original semantic and allocated view. Fifteen allocated-view mutations
must fail: caller-clobbered held location, wrong register bank, out-of-frame
spill, and misaligned 64-bit spill. Spill escapes are removed when testing
register errors. Out-of-frame indices are representable in the 8-bit field.

All 18 fixtures in the extended table-call contract pass ARM64 assertions:
`/tmp/ljmt-held-results-focused.log`. The new fixtures are also registered in
the broader M7 scalar CALLXS suite. The first draft's final assertion wrongly
expected `mt_active` to clear after join; it was corrected to respect the
existing sticky threading latch. No production behavior changed for that
fixture correction.

## Frozen checkpoint validation

Source checkpoint `9f5ee1430a96324ed04909968b7da957e5807108` passes all 18
focused fixtures in all four profiles. Each profile checks the four authentic
semantic/allocated positives, 15 allocated negatives and all eight forced
post-call exits described above, alongside the existing table-call coverage.

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `92930edf2e82d644d8b91f1c624d1b5123ca3a82795985dbaa5d43272b88e437` |
| arm64e/BTI assertion | `85aa15b7992246cae3c8b98a4015ed45a5c33bfef612a3a031cc0e3fc497adc9` |
| ARM64 release | `a6201fdf7cd71c749282ccb2d84bd3f3159470181fc24b23075f5d837dbe7707` |
| arm64e/BTI release | `038a2ca80fa9388d7b4546fce29b3413d809b1164a43f8d687b98a464a55b7d8` |

The complete M7 scalar CALLXS suite also passes all four profiles, including
the new fixtures, existing boxed results, I64 arguments, variadics,
callbacks, unwind, native-frame lifetime and remote-flush cases. Logs:
`/tmp/ljmt-held-results-four-profile.log` and
`/tmp/ljmt-held-results-m7-{arm64,arm64e,arm64-release,arm64e-release}.log`.

Ordinary ARM64 assertion profile
`d4957573449c85e9a187a6f85423915f7137e6c1e380b083275e644a162f35c3`
passes 15 held-result runs with exact counts/errno and same-trace reuse.
Eleven scalar-table, five nested-read, nine I64-argument and nine callable
runs also pass, as do type-change retry/reuse, bit operations and rooted
cells/upvalues. Both stock source and original-bytecode import corpora pass
509 cases. Signed x64/Rosetta assertion profile
`21b956255114d509f1db2348a66e802e1b518def2a4a24a63158f68fe5e742c3`
passes both 509-case corpora too. This is bounded compatibility evidence,
not a complete x64 umbrella or a native Intel performance measurement.

The matrix verifies unchanged source and restores ordinary ARM64 assertion
profile `92c6da033c2be52e417490a0726626ade430c4ddd75e895344e930a741300e8b`.
That runtime repeats all 15 held-result runs. The final boundary probe also
requires the original table-held and accumulated variants to compile while
fractional NUM-to-C-int narrowing remains correctly interpreted and closed.
Code-sign checks pass; selected test helpers and out-of-line atomic imports
are absent. Logs: `/tmp/ljmt-held-results-matrix.log`, matching
ordinary/stock/x64 logs, and `/tmp/ljmt-held-results-boundaries.log`.

The preceding scalar-table checkpoint passed both complete experimental and
bootstrap umbrellas. This allocation-only increment refreshes all four
focused and M7 profiles, ordinary cases, and ARM64/x64 stock compatibility;
it does not claim a new full umbrella run.

## Main checkout

Main `v2.1` was fast-forwarded to the source checkpoint and rebuilt with
ordinary bootstrap/experimental/assertion flags and no test hooks. Profile
`10739c7e7c6261dfea504cc5d499e539ee40495f176c3336ce96088c79ce0de5`
passes all 49 smoke runs above, the held/narrowing boundary probe, bit
operations, rooted cells/upvalues, and both 509-case stock corpora.
Code-sign and atomic/test-helper symbol checks pass.
Log: `/tmp/ljmt-held-results-main.log`.

- Runtime SHA-256: `572e3ec2d7465aa91791a14b97fa456aaf33a4f7bc29e9a7d5709466319b774e`
- Archive SHA-256: `932df0bbbb18aac9bbc7c1cc44bc0ea64e429becfe76c034e356a0cc79076bc6`
- Build-signature SHA-256: `ab26e7c5102e9796be190e8d0c24d62b806ea455cbe4e457973c75b047b9dfba`

## Remaining work

The port remains experimental and incomplete. Fractional NUM-to-C-int
narrowing, general table PHIs, arbitrary Lua calls/metamethod effects and
other uncertified shapes remain closed. These held-result tests do not add
a new outgoing-stack-pressure or arbitrary nested-callback certificate.
