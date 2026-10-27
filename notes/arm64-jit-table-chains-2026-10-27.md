# ARM64 nested table reads feeding CALLXS

This increment admits guarded table-valued results from the existing bounded
rooted hit helper inside scalar FORL CALLXS roots. A later hit may use that
exact validated VLOAD as its receiver. The semantic and allocated passes
independently bind scratch roots, success guards, source bytecode, retry and
result snapshots, and the next lookup's saved receiver. The result may feed
a table-loaded callable or an owned I64/U64 argument. Other table operations
and call signatures do not become admissible through this change.

## Callback root certificate

A table pointer surviving in a callee-saved register or full-width spill is
not sufficient protection during a Lua callback. Callback suspension clears
the TG's published JIT activity; the suspended frame scanner pins the trace
and its explicit result box but does not scan native registers or spills.
The current XSAVE must therefore retain every live GC value as an enumerable
Lua stack value. The recorder already normally supplies these roots; this
increment adds the missing independent admission checks, rather than claiming
that an actual use-after-free was observed.

Both passes now derive live GC values from actual IR reference operands,
later snapshots, loop-invariant uses in the next iteration, and current
native argument/callable owners. Bounded pointer ADD/PHI dependency walks
retain GC owners hidden behind address carriers. Raw P32/P64 carriers are
excluded from Lua GC values even though the broad IR type-range macro includes
them. A fresh VLOAD or older CNEW needs an exact, ordinary, materialized slot
in this call's XSAVE. A NORESTORE or substituted root does not suffice.

The exceptions are narrow: trace constants and identities guarded against
them remain pinned; the current producer box has its separately certified
native-frame result root; an unchanged guarded, non-PHI SLOAD may retain its
original stack slot. The latter proof checks every subsequent XSAVE for slot
clobbers, including later loop iterations. This graph admits no stack/cell
stores. The independent allocated snapshot-location and native register/spill
survival checks remain required.

No emitter, helper, native-frame, callback, GC2 runtime, atomic authority or
x64 production code is changed.

## Focused development evidence

At `d99d18ba`, the ordinary nested-read and read-across-call probes interpret
correctly but reject trace assembly. The updated ordinary runtime publishes
both loops with the same results. Logs are
`/tmp/ljmt-table-chains-red.log` and
`/tmp/ljmt-table-chains-probe-debug.log`.

The new fixtures cover:

- TGETS, TGETV and TGETB chains, same-CType peer replacement without replacing
  the trace, nested I64 roots across a completed call, exact native-frame
  observations, errno and forced POSTCALL continuation without replay.
- One authentic nested-call trace accepted independently by both admission
  passes, 28 structural mutations rejected by both, and six additional
  allocated scratch/spill corruptions rejected by the post-RA pass. Mutations
  include substituting or marking NORESTORE the live table root at the earlier
  unrelated native call.
- A unique target table installed after trace compilation. A later table read
  overwrites the helper scratch root; a peer removes the outer edge and
  requests major GC before native entry. The phase remains deferred, the
  sentinel call's XSAVE roots the target, the later target call executes, and
  the weak witness eventually clears. Native-entry retries are bounded and
  allowed only before any observed native effect or peer mutation.
- A native-only callback gate. The callback verifies the suspended frame and
  exact target stack root, removes the only outer edge, forces collection,
  and verifies survival. Native leave takes the existing callback POSTCALL
  exit; subsequent target/I64 operations and counts remain correct. After
  roots are released, the weak witness clears.

The three new admission/lifetime fixtures pass the initial ARM64 assertion
run in `/tmp/ljmt-table-chains-lifetimes3.log`; the runtime fixture also passes
in `/tmp/ljmt-table-chains-third.log`. Development caught fixture setup errors
(missing peer, extra stack entries, unsupported table PHIs and a scalar
rename shape) and an overly broad GC type classification. The fixtures now
exercise the intended bounded grammar. These fixes do not open those adjacent
IR shapes. Frozen-profile evidence is recorded below.

The older pointer admission fixture had treated a PHI input changed from its
boxed result to an old SLOAD as a structural positive. That makes the SLOAD
live across a body call whose XSAVE overwrites its original slot with the
boxed result. It is now an explicit negative in both passes. The authentic
pointer trace remains a positive; this does not change runtime behavior.

All 14 fixtures in the extended focused contract now pass the ARM64 assertion
profile, including the established callable, I64, variadic and pointer
admission cases. Log: `/tmp/ljmt-table-chains-all-assert2.log`. The contract
restored the ordinary ARM64 assertion runtime after completing the fixtures.

## Frozen checkpoint validation

Source checkpoint `662cd828c66a7d105214fee982c13f0849c39608` passes all 14
fixtures in the extended table-call contract in all four profiles:

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `38f656f54451bc7a768b579312650638b0e92d21bc828385f0f24377db2945e3` |
| arm64e assertion/BTI | `1aba3f8c63b41b5c6c82871cb3b02b942471c50651bd81d6761ff653edfc8254` |
| ARM64 release | `14a099d8e2864c1c77b75e04793b77853583c1ef21a0ddf3de9274fb85a40ae7` |
| arm64e release/BTI | `6be67f13674f483ebca4ef5aa73a7b8b1f34e4e06cfc29c9b862a87f007b894a` |

Each profile includes the four new nested-read fixtures and the ten existing
table-callable, I64-argument, variadic and admission fixtures. The broader M7
scalar CALLXS suite also passes in all four profiles, retaining scalar,
float, pointer, boxed/fixed I64/U64, U32/CALLM, variadic, callback, remote-flush
and C++ unwind coverage. Logs: `/tmp/ljmt-table-chains-four-profile.log` and
`/tmp/ljmt-table-chains-m7-<profile>.log`.

Ordinary ARM64 assertion profile
`92f98969cbed9a4dba6de820a5b4ef1450ec3bd4d5676efd4c4cd1164a112d57`
passes five nested-read smoke cases, the preceding nine I64-argument and nine
table-callable smoke cases, bit operations and rooted cells/upvalues without
test hooks. The new smoke checks exact values, foreign-call counts, errno,
published JFORL trace numbers and reuse of the same trace after same-CType
peer replacement. This ordinary-runtime evidence supplements the native-frame
and callback witnesses in the C fixtures.

Both stock source and original-bytecode import corpora pass all 509 cases on
that ARM64 runtime. Signed x64/Rosetta assertion profile
`b0a4b0efaf3497e63d41c5b9e56b162266c5a1044ae627d070e40e4fa735087b`
also passes both 509-case corpora. These are bounded compatibility checks,
not a complete x64 umbrella or native Intel performance measurement.

The matrix verifies unchanged source and restores ordinary ARM64 assertion
profile `d37c6ec85a67e3a9984c2d6e900f4d3734a9a850564c40afafe2f3f2d9bb3d89`.
The restored runtime repeats the five nested-read smoke cases. Code-sign
verification passes; selected test-helper symbols and out-of-line atomic
imports are absent. Logs: `/tmp/ljmt-table-chains-matrix.log` and matching
ordinary, stock source/import and x64 logs. Complete experimental and
bootstrap umbrellas were not rerun for this target-local increment.

## Main checkout

Main `v2.1` was fast-forwarded to the source checkpoint and rebuilt with the
ordinary bootstrap/experimental/assertion flags, without test hooks. Profile
`39554464ec6d5a682cab69cf009ee55bb0f7e0c1e194728a1d04775a5b59ea46`
passes the same five nested-read, nine I64-argument and nine table-callable
smoke cases, bit operations, rooted cells/upvalues and both 509-case stock
corpora. Code-sign and atomic/test-helper symbol checks pass. Log:
`/tmp/ljmt-table-chains-main.log`.

- Runtime SHA-256: `c4e1bff3661c31dcec925adcf97f8dc703d9434c2b1952be78f942e0c4439da8`
- Archive SHA-256: `d0cafcf2c2db00e4e2747eb9c0633fb474087f458380b2e80e7480f7dd1add73`
- Build-signature SHA-256: `57e23548f02283156319a595ea9800e027db669f6a9cdab7f90d617eacfbd7a1`

## Remaining scope

This is an incremental experimental JIT opening, not completion of the ARM64
port. General table PHIs, arbitrary Lua calls/metamethod effects and other
uncertified table/call shapes remain outside this increment. Whole bootstrap
and experimental umbrellas have not been rerun for this change.

A next bounded composition is a scalar table read passed to a fixed scalar
FFI argument, such as `native_set(config.value)`. The recorder and general
rooted-effect validators already support the bounded scalar-read helper,
while the CALLXS graph accepts the GC-hit helper and table/CData results.
Opening numeric or boolean results needs independent helper, status-guard,
source/result snapshot and XSAVE argument-slot certificates, the existing ABI
and allocated-location checks, and native peer-replacement/no-replay evidence.
This is a source-audit candidate, not implemented or validated in this change.
