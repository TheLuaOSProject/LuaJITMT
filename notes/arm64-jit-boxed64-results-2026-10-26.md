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
registered in the M7 scalar CALLXS suite.

## Frozen four-profile validation and combined-gate failure

At `9651f9372cb815f3e2b00980ad48c96c23e35abd`, the focused contract passed:

| Target/runtime | Profile |
| --- | --- |
| ARM64 assertion | `10b96f60714444d99a40d6c85f0db0c93fb3edd717db1bc187fb7e230d33aa7f` |
| arm64e assertion/BTI | `3ce68559bf1754c73e714774fc2426260b7af63696b9167a8ea043e569658896` |
| ARM64 release | `03d1fa7fc68ccee79dda16e5128a9253b45f439fdec4c0db4b1f9ba9c0876557` |
| arm64e release/BTI | `e16159973b27cc274ee9fef9130b92dfd132d57d309d46798684e4e07bfa5e05` |

Each profile recorded 1,722 native boxed-I64/U64 results and 46 native scalar
CALLM producer observations. Semantic and post-RA fixtures independently
passed 8 positives and rejected 25 mutations, without skipping the duplicate
store case. Existing pointer admission also passed. The log is
`/tmp/ljmt-i64-four-profile.log`.

The subsequent experimental umbrella aborted during the capacity-4 native
TNEW contract on an intermittent shared GC constructor membership assertion.
The earlier portions of that umbrella passed, but the whole gate did not;
bootstrap and stock-corpus stages were not reached. Shared GC source was
unchanged by the boxed-result implementation. The failure, deterministic
constructor/recovery regressions and repair are recorded in the
[constructor/recovery note](gc2-constructor-recovery-2026-10-26.md).

## Repaired combined checkpoint and main rebuild

Frozen checkpoint `2ba1e91b87bd69c49bbd703cd7bb04cb9934531f` passes the
focused boxed-result contract again, with the same 1,722 native results,
46 scalar CALLM producer observations and independent 8-positive/25-negative
counts per profile:

| Target/runtime | Profile |
| --- | --- |
| ARM64 assertion | `2eade2a23ff8d51dcd3e327dd9714a44ab7da15119e59446dbfe3b8101327812` |
| arm64e assertion/BTI | `5a5446f3e75026f807b4e8508cb18c6ca4f1e32a473947098c26e1829c18c6db` |
| ARM64 release | `f2ad1ea954a4c19fd858a352ecf49b7ca430f15888fb1f46b7c8424ce97217cd` |
| arm64e release/BTI | `7631d851f52eda8076323f4d2476d2febce5f28b874f5dd904872c4e87740be8` |

The full experimental gate and bootstrap gate pass at this frozen source.
The experimental umbrella includes all four M7 **scalar CALLXS** profiles;
this is not a claim that the entire M7 umbrella ran in this matrix. Bootstrap
passes its 387 applicable stock cases, native roots/GC2, callbacks, errno
unwind, and interpreter aggregate/variadic FFI checks. Native TNEW at
capacities 0/4/8257 passes on ARM64 and arm64e/BTI.

Ordinary ARM64 assertion profile
`7202b1a4258d64ced44546a16bdc1d3ca3da148257c6fe3eee71842522aced68`
passes the nine boxed-result smoke cases, bitops, rooted local cells and
upvalues, plus both 509-case stock source and original-v2 import corpora.
Signed x64/Rosetta assertion profile
`6f4b62bf5c7ccd77ef680c987fd01bade886300278f2b01e73c226d9cb24702c`
also passes both 509-case corpora. These are compatibility regressions, not a
complete x64 test umbrella or a native Intel performance result. Logs use
the `/tmp/ljmt-i64-gc2-` prefix; `/tmp/ljmt-i64-gc2-matrix.log` records the
frozen-source checks and successful ordinary ARM64 restoration.

The main `v2.1` checkout was fast-forwarded to the same source and rebuilt
with only `LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL` and
`LUA_USE_ASSERT`. Main profile
`d20e13d892019e2d53e42d1fb53ca7e7fea158f606626285b26fd03b2a2bebf8`
passes all nine ordinary boxed-result cases, bitops/cells/upvalues and both
509-case corpora (`/tmp/ljmt-i64-gc2-main.log`). The nine smoke cases check
exact results/counts and published trace metadata; the C fixtures above
provide native-execution observations. Code-sign validation passes, selected
test-helper symbols are absent, and the executable has no out-of-line atomic
runtime dependency.

Main artifacts built at `2ba1e91b`:

- Runtime SHA-256: `2fa38b25a1443c0dd19450745aa0112e4270c0940634fc831e5bb4f0d5e31943`.
- Archive SHA-256: `8bdbaba1a58abdea86ab93bb4823dc765a73dedf2932e2fd9b8f9f61ee7f9bb6`.
- Recorded build-signature SHA-256: `ad56f67f4024f00d9828ce03a57580b1d786c71312b3b085a738c75e908ed883`.

Subsequent evidence-note edits do not change the tested production source.

## Remaining scope

This is one result-representation increment toward the full lockless ARM64
port. It does not admit general 64-bit cdata arithmetic/arguments, boxed
CALLM producers, enum boxes, arbitrary root/side graphs, aggregates/sret,
or unproven variadic cdata forms. The broader remaining VM/JIT/FFI, profiler,
unwind, and stress gaps in `README.md` remain open.
