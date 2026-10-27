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
IR shapes. Full frozen-profile evidence will be recorded after validation.

The older pointer admission fixture had treated a PHI input changed from its
boxed result to an old SLOAD as a structural positive. That makes the SLOAD
live across a body call whose XSAVE overwrites its original slot with the
boxed result. It is now an explicit negative in both passes. The authentic
pointer trace remains a positive; this does not change runtime behavior.

All 14 fixtures in the extended focused contract now pass the ARM64 assertion
profile, including the established callable, I64, variadic and pointer
admission cases. Log: `/tmp/ljmt-table-chains-all-assert2.log`. The contract
restored the ordinary ARM64 assertion runtime after completing the fixtures.

## Remaining scope

This is an incremental experimental JIT opening, not completion of the ARM64
port. General table PHIs, arbitrary Lua calls/metamethod effects and other
uncertified table/call shapes remain outside this increment. Whole bootstrap
and experimental umbrellas have not yet been rerun for this change.
