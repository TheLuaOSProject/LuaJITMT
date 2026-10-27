# ARM64 table-loaded I64/U64 FFI arguments

This increment composes the existing bounded rooted table hit with fixed
owned I64/U64 CALLXS arguments in FORL roots. The only production change is
in the two independent payload admission passes: a guarded CData VLOAD may
now be the payload source alongside a guarded SLOAD or prior boxed CALLXS
result. The enclosing graph must already certify the exact rooted hit,
success guard, scratch provenance, retry PC and destination snapshot.

Both payload passes retain the exact CTYPEID guard, terminal integer CType,
64-bit signedness and fixed ABI consumer checks. The native-call proof still
binds the original CData source to its exact XSAVE argument slot. A payload
register or spill is not an enumerated GC root. No emitter, read helper,
native frame, GC gate, atomic authority or x64 production path changes.

## Initial focused evidence

An ordinary ARM64 two-argument probe at `f6dc7fbd` computes the correct result
through interpretation but rejects trace assembly at IR 101. With the two
source checks extended, the same probe publishes a compiled loop and returns
the same result. Logs: `/tmp/ljmt-table-i64-red.log` and
`/tmp/ljmt-table-i64-green.log`. Native-frame witnesses in the C fixtures are
required in addition to that trace metadata.

The fixtures cover signed/unsigned high-bit payloads, nine fixed arguments,
typedef/const aliases, mixed signedness, a fixed variadic prefix, same-CType
peer replacement and forced POSTCALL continuation without duplicate effects.
The lifetime fixture uses a unique target installed only after compilation,
overwrites the helper scratch root with a second CData read, removes the
target's sole strong table edge from a peer, and requests major GC before
native-frame publication. It verifies deferred phase entry, exact staged
CData roots, full-width foreign arguments and eventual weak reclamation.

Initial fixture development exposed two test setup issues. Nested registry
subtables must be selected before the loop because table-valued reads inside
the CALLXS graph remain outside this increment. A two-iteration lifetime run
also missed its native pause once after setup collection; the fixture now
drains deferred collection work and requires the native-entry gate open
before arming its pause. The existing callable lifetime fixture hit the same
entry assumption. Both now permit at most 32 complete two-iteration attempts
while no native call/pause or peer mutation occurred, counting every foreign
effect across all attempts. They still require the actual paused native
handoff and exact final native-frame observations. These are fixture changes,
not new production admission exceptions; the original intermittent misses do
not establish a production defect or identify one specific refusal cause.

The existing table-call contract and M7 scalar CALLXS suite include the new
runtime, independent admission and lifecycle fixtures. Initial ARM64 assertion
run `/tmp/ljmt-table-i64-native2.log` passes the runtime fixture and four
consecutive argument-lifetime runs. The later
`/tmp/ljmt-table-i64-admission2.log` passes the completed independent admission
fixture plus both lifetime fixtures with the bounded entry handling. Its
profile is `b91a4b09738365f9d5d2a242ccafe1d2df9a9ca748f213d395a70f4c59d7e834`.
Three authentic signed/unsigned table-read traces pass both views; 26
structural mutations fail independently in each view, and six additional
scratch/spill mutations fail the allocated view. The unsigned trace checks
the ninth stack argument. Fixed signature checks require eight GPR arguments
and an eight-byte ninth slot.

The ordinary ARM64 runtime without test hooks also passes nine argument smoke
cases, including reuse of the same trace after same-CType peer replacement.
It checks full-width foreign validation, helper counts, errno and each
runner's actual JFORL trace number. Log:
`/tmp/ljmt-table-i64-ordinary-initial.log`. These metadata checks supplement
the native-frame evidence above. Frozen-profile and main-checkout validation
will be recorded below once completed.

## Scope

The full ARM64 port remains incomplete and opt-in. This change does not admit
general table-valued loads inside CALLXS loops, arbitrary CData arithmetic or
conversions, CData references/PHIs, variadic I64 suffixes, aggregate calls or
foreign-effect side traces. It reuses the certified CData read and existing
fixed-scalar ABI and native-frame protocol.
