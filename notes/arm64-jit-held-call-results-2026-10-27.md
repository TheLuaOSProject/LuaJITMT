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

## Validation scope and remaining work

The frozen-checkpoint matrix and main-checkout rebuild are recorded below
after completion. The preceding scalar-table checkpoint passed both complete
experimental/bootstrap umbrellas. This allocation-only increment refreshes
all four focused and M7 profiles, ordinary runtime cases, and ARM64/x64 stock
source and import compatibility; it does not claim a new full umbrella run.

The port remains experimental and incomplete. Fractional NUM-to-C-int
narrowing, general table PHIs, arbitrary Lua calls/metamethod effects and
other uncertified shapes remain closed. These held-result tests do not add
a new outgoing-stack-pressure or arbitrary nested-callback certificate.
