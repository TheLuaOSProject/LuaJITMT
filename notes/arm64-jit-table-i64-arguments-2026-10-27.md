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
is recorded below.

## Frozen checkpoint validation

Source checkpoint `39396f1811c2523e0ffef12ce6f9d464998f8f00` passes all ten
fixtures in the extended table-call contract in all four profiles:

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `9761e323b1ce42f8123307cddce82cc584a3cea44f1c1ef6649ee788f5ef22dd` |
| arm64e assertion/BTI | `aaff952a3349c4422bfa1546d864e83e477c00ee4f6ba83e5568eb8539ddf34c` |
| ARM64 release | `5e6d3295b9b8e0312856076aecfd1ac348598b9e6bfe86b28ab2c2bbf52acf57` |
| arm64e release/BTI | `5d72c856480548fde9e24c838b7c93ad985489d45f1a02cecc2bab7b82e30245` |

The broader M7 scalar CALLXS suite passes in all four profiles, retaining its
scalar, float, pointer, boxed/fixed I64/U64, U32/CALLM, variadic, callback,
flush and C++ unwind coverage. Logs:
`/tmp/ljmt-table-i64-four-profile.log` and
`/tmp/ljmt-table-i64-m7-<profile>.log`.

Ordinary ARM64 assertion profile
`8e3d37124ff15e10c5b6dfee6eabf4c72c7e7d9cc6343b8f685e871e9a8b2df6`
passes nine argument smoke cases, the preceding nine table-callable smoke
cases, bit operations, rooted cells/upvalues, 509 stock source cases and 509
original-bytecode imports. Signed x64/Rosetta assertion profile
`df4dd3ed05a690467e4e6100447ff0839d9b66ec902733fe89866eb7943db50c`
passes both 509-case corpora. This is bounded compatibility evidence, not a
complete x64 umbrella or native Intel performance result.

The matrix verifies unchanged source and restores ordinary ARM64 assertion
profile `a079df8ac47bdcb03b8b1c1307d888f6598b01619cc2079ccb1033f1924fdb23`.
Code-sign verification passes, and selected test-helper symbols and
out-of-line atomic imports are absent. Logs: `/tmp/ljmt-table-i64-matrix.log`
and the matching ordinary, source/import and x64 logs. Complete experimental
and bootstrap umbrellas were not rerun for this target-local increment.

## Main checkout

Main `v2.1` was fast-forwarded to the source checkpoint and rebuilt with the
ordinary bootstrap/experimental/assertion flags, without test hooks. Profile
`3fe35dfafba93345a51ba1605bec990cef3b4573baf0090f1b742eda30992cb4`
passes the same nine argument and nine table-callable smoke cases, bit
operations, rooted cells/upvalues and both 509-case stock corpora. Code-sign
and atomic/test-helper symbol checks pass. Log:
`/tmp/ljmt-table-i64-main.log`.

- Runtime SHA-256: `e2d7643574a414537c15cc836a7516f9a355b9f2d2359b82d5e468becaeb4a4b`
- Archive SHA-256: `714c226798688187051beff7c964fe2cab5b377ec8526ea1e655a86b15e08b73`
- Build-signature SHA-256: `eac2dab0dde57108b8ee1b8f993fdf49c1741e869667bf2338c3681c680c82a2`

## Scope

The full ARM64 port remains incomplete and opt-in. This change does not admit
general table-valued loads inside CALLXS loops, arbitrary CData arithmetic or
conversions, CData references/PHIs, variadic I64 suffixes, aggregate calls or
foreign-effect side traces. It reuses the certified CData read and existing
fixed-scalar ABI and native-frame protocol.

The next bounded composition is a certified table-valued hit feeding a later
table lookup, such as `registry.values[key]`. The CALLXS graphs currently
require each read receiver to be a guarded SLOAD and admit CData hit results.
Any extension must independently prove the table VLOAD's earlier hit and map
the later lookup's receiver snapshot slot to that exact value. The omitted
snapshot-slot fallback must remain specific to an unchanged SLOAD. Replacement,
outer-edge removal during GC, exact continuation and malformed receiver/root
views need native and independent admission evidence before opening that path.
