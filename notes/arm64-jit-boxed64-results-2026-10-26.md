# ARM64 boxed 64-bit FFI results (2026-10-26)

## Implementation

ARM64 experimental CALLXS admission now accepts signed and unsigned 64-bit
integer results through the existing recorder, allocator, native-frame root,
X0 return, and typed payload-store paths. Only the ARM64 admission header
changes production behavior; no new runtime helpers, locks, or emitted
instruction paths are introduced.

Both semantic and post-allocation passes independently prove the exact ABI
result CType and signedness, CNEW root, native-entry root argument, unique
XSTORE from that CALLXS, and `box + sizeof(GCcdata)` payload address. CData
PHI inputs independently prove their incoming boxes and typed stores. The
result rule composes with existing fixed and promoted-variadic argument
certificates, including a scalar CALL producer feeding a boxed-result CALLM
consumer. Boxed results cannot become direct scalar CALLM producers.

The audit found an existing pointer-certificate gap: generic snapshot bounds
and physical locations did not bind the exact hidden root and caller result
slots to the allocation. Both passes now independently derive those slots
and check an exact restorable entry in the XSAVE and native-leave snapshots.
The shared reader only validates bounded snapshot metadata; it does not
replace either pass's source, lifecycle, ABI, or allocation proof. The hidden
slot follows the synthetic meta-call frame's callable cdata and arguments.
The caller result occupies CALL/CALLM register A; B=1 discards the result.
Discarding still requires the exact DONE snapshot at native leave, so an
exit resumes after the foreign effect even though no result slot is retained.

Const-qualified integer CTypes retain their distinct terminal CTypeID.
Tests resolve that exact declared type and verify interpreter, native-frame,
and returned-box identity. A typedef resolving to plain int64 keeps its
ordinary raw CTypeID.

## Initial validation

The ARM64 assertion development profile
`b961ba8ac3ca450fbae1e7fd07e251d1b23f8d99227b31f62958107096c94a29`
passes both new C fixtures. The lifecycle fixture checks exact signed and
unsigned payloads, typedef/const declarations, fixed and promoted-variadic
calls, scalar-to-boxed CALLM, native frame roots, errno, forced POSTCALL exits
without replay, collection, flush, and frame cleanup. The LOOP fixture
confirms correct interpreter execution and closed native admission; these
CALLXS certificates still require FORL roots.

Existing integer/double and pointer admission fixtures also pass this profile.
The pointer fixture previously expected an unrelated string substituted into
the hidden result-root slot to pass semantic admission. That expectation now
correctly requires rejection in both passes, matching the tightened root
binding rather than preserving the old acceptance gap.

The admission fixture first admits each authentic semantic and post-RA
clone independently, then rejects result type/width, CType, payload, store
source, entry root, PHI, variadic separator, and separate hidden/caller
snapshot substitutions. Substitutions use an earlier valid CNEW. Snapshot
NORESTORE and nil substitutions are checked independently too. The initial
checkpoint skipped the duplicate-store mutation because no spare instruction
existed in the captured trace. The completed fixture now inserts/rebases one
private canonical NOP, proves both passes accept that clone, then replaces
the NOP with a second XSTORE and requires both passes to reject it.

The follow-up ARM64 assertion development profile
`543c899433ba31bf0ab4e34f40b6fef49ab32d25a79ee52ad21408847775c781`
passes the expanded fixtures. They add all 13 signed and 12 unsigned payload
patterns, including 32-bit boundaries and alternating bits; native discarded
I64/U64 results and forced discarded-result POSTCALL exits; independent
discarded-result continuation mutations; and a well-defined integer-vararg
consumer confirming boxed CALLM producers remain closed. No duplicate-store
mutation is skipped.

The first development runs exposed fixture issues, not weakened admission:
const was incorrectly expected to have the unqualified CTypeID, and a
tail-returning C-harness driver lacked the Lua caller geometry required by
the existing recorder. The fixture now preserves a normal Lua caller frame,
as the existing pointer lifecycle fixture does. A standalone ordinary ARM64
trace also showed an X0 payload store and exact INT64_MIN result.

The focused `tools/ci/arm64_jit_boxed_i64_contract.sh` binds each runtime,
archive, and fixture to an exact profile and restores ordinary ARM64 after
the four assertion/release ARM64/arm64e targets. Both new fixtures are also
registered in the full M7 suite. The final four-profile and combined gates
have not yet been run for this change.

## Remaining scope

This is one result-representation increment toward the full lockless ARM64
port. It does not admit general 64-bit cdata arithmetic/arguments, boxed
CALLM producers, enum boxes, arbitrary root/side graphs, aggregates/sret,
or unproven variadic cdata forms. The broader remaining VM/JIT/FFI, profiler,
unwind, and stress gaps in `README.md` remain open.
