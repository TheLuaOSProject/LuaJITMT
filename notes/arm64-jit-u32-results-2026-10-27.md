# U32 numeric FFI results on Darwin ARM64

## Scope and proof

This increment admits unsigned 32-bit integer CALLXS results converted to Lua
numbers by the existing ARM64 emitter. The ABI must be an unsigned four-byte
integer in a GPR. Each retained result has exactly one unguarded U32-to-NUM
conversion while its native frame is active; the completed-call snapshot at
native leave must restore that exact number to the source CALL's caller slot.
A discarded result may lose its conversion to DCE, but still requires the
completed-call snapshot and exact continuation. Raw U32 carriers are forbidden
in Lua snapshots and cannot escape through other IR consumers.

The semantic and allocated-IR passes derive these facts independently. The
latter additionally requires the result to survive native leave in an aligned
full-width spill or preserved FP location. Snapshot locations now apply the
validated RENAME suffix using the same ordering and snapshot scope as
`lj_snap.c:snap_renameref`. Retained-result loops can keep the old and new
numbers in distinct snapshot-specific FP registers.

The CALLXS grammar also admits the existing exact guarded NUM-to-INT conversion
only while the native frame is closed. Numeric accumulator loops require this
shape after optimization. Its input remains independently validated scalar IR,
and ordinary guard snapshot, allocated-location and native-call liveness checks
still apply. This intentionally composes the existing scalar conversion with
CALLXS; it does not require that every numeric input originate from U32.
No recorder, optimizer, allocator, emitter, shared runtime or x86 code changed.

## Validation

The focused contract is `tools/ci/arm64_jit_u32_contract.sh`. Its runtime and
independent admission fixtures are also registered in the M7 scalar CALLXS
case. The native runtime fixture passes its first ARM64 assertion execution:
ten boundary/high-bit patterns, retained loop results, two distinct native
functions, fixed and variadic signatures, typedef/const results, errno,
forced retained/discarded POSTCALL exits without replay, and full frame cleanup.
A threshold-returning helper drives a real checked NUM-to-INT exit after a
large unsigned result, with exact result and native-call counts.

Initial bring-up distinguished published traces from native execution: an
integer-initialized sum could be recorded with an entry guard that rejected the
already-large sum. Ordinary accumulation tests use a fractional initial value;
the dedicated threshold case exercises the checked narrowing path explicitly.
The two-call fixture loads its second callable before the loop, avoiding the
separate unsupported in-loop global-table lookup shape.

The focused ARM64 assertion contract passes, with 915 observed native calls,
four authentic positive traces and independent malformed-view checks. The
older scalar and pointer CALLXS admission fixtures also pass. The ordinary
ARM64 assertion build passes 26 U32 result smoke cases with exact values, call
counts and published trace metadata. The instrumented C fixture supplies the
native-execution evidence. Initial focused log: `/tmp/ljmt-u32-tenth.log`;
ordinary smoke: `/tmp/ljmt-u32-ordinary-preliminary.log`.

Frozen checkpoint `0e871779ca81d191331a156ef74739995a12313e` passes the
complete experimental and bootstrap gates, all four focused profiles, and
both 509-case stock source and original-bytecode import corpora on ARM64 and
signed x64/Rosetta. Bootstrap passes its 387 applicable stock cases. The
experimental gate includes all four M7 **scalar CALLXS** profiles, including
callbacks, remote flush and C++ unwind; this is not the complete M7 umbrella.

| Focused profile | Profile ID |
| --- | --- |
| ARM64 assertion | `4a90b8db7736d3e9a142ebde5dc62a46a96962c90e53ec803b3ea934d96270bf` |
| arm64e assertion/BTI | `c9d2cafe4d6263014f248f1aac7ffeee52a98aefdfa5ee68e25e797993ff73df` |
| ARM64 release | `405bb2fe634f5574b0cead59258b89325455b23f7d60c7f10067bb619d047c73` |
| arm64e release/BTI | `e6a4f4573ec66e66ea5da5a538789f828a80839304353ca4e6d5f3d6d88fe73e` |

Each focused profile records 915 native calls, four semantic/allocated positive
traces, 21 semantic rejections and 29 allocated-IR rejections. Ordinary ARM64
assertion profile `b9f5effe7564ac93baa5c4a504e7226d1baaf43d41d119d164d53282dbf7b92b`
also passes all 26 U32 smoke cases and the bitops/cell/upvalue regressions.
Signed x64 assertion profile is
`c8fa04682690b8189d81a58230ef56101b5cdc65ca4aa37944db5a5d47165d13`.
The matrix verifies unchanged source and restores an ordinary ARM64 assertion
runtime. Logs: `/tmp/ljmt-u32-matrix.log`, `/tmp/ljmt-u32-four-profile.log`,
`/tmp/ljmt-u32-experimental.log`, `/tmp/ljmt-u32-bootstrap.log` and matching
`stock-source`, `stock-import`, `x64-stock-source`, `x64-stock-import` logs.

## Follow-up: renamed locations across calls

Independent review after freezing the initial checkpoint found that native-call
liveness checked a value's default allocation, while snapshot restoration now
honors its RENAME allocation. A malformed clone could keep the default in D15
but rename the old live result to caller-clobbered D0 before the next native
call. The new mutation is accepted by the initial checker, reproducing the gap
in `/tmp/ljmt-u32-rename-red.log` (allocated mutation 29).

The allocated snapshot pass now retains the selected rename's scope, follows
exactly the runtime's reverse suffix selection, and checks any CALLS/CALLXS
between that scope and the target snapshot. A caller-clobbered effective
register cannot carry a value across those calls. The interval begins after
the value's definition, so its producing call is excluded. Aligned spills and
preserved registers retain their previous rules. A separate positive clone
moves the value to D0 only after the last native leave; its earlier preserved
mapping remains valid across calls. No emitter or runtime changes are needed.

The focused ARM64 assertion run now passes: 915 native calls, four semantic
positive traces with 21 rejections, and five allocated-view positives with 30
rejections. The fifth positive is the post-call D0 rename. The older pointer
fixture had marked an X0 rename of a live string as valid; it now rejects that
unsafe allocation, accepts the preserved-register variant, and retains the
independent collision negative. Original scalar and pointer fixtures both
pass. Log: `/tmp/ljmt-u32-rename-green2.log`.

The follow-up is limited to CALLXS allocated snapshot validation. Frozen
checkpoint `34265a96b7cfb8613c44f17d8a140f7102a5dcb3` passes the focused
contract and all four M7 scalar CALLXS profiles:

| Final focused profile | Profile ID |
| --- | --- |
| ARM64 assertion | `7140cdbb5fbebc24064fa421351d8dd0b42a1c72daf16c5507033faca985d467` |
| arm64e assertion/BTI | `f805429e2090f7f5e6eb336b357776f719766728dfd9169198c342b4aacd501a` |
| ARM64 release | `e4c47306d366cb670c1ba0887e58c0878d20c3e69122eb96a36d561c4bf227e0` |
| arm64e release/BTI | `f6bca9f8fc598b13e6b3a352e3c4b9eab5c3eb4833c00b36f224e5d80ef25355` |

All four retain the 915 native observations, semantic 4/21 and allocated-view
5/30 positive/rejection counts. M7 additionally validates the preceding scalar,
float, variadic, pointer, boxed-I64/U64 and fixed-I64/U64 call paths, callback
lifecycle, remote flush and C++ unwind under assertion/release and BTI profiles.

Ordinary ARM64 assertion profile
`0f9c9de334f4e7ccdc042c7b8bfc03c82ab702030bc2796ea87463c143c722f2`
passes the 26 U32 smoke cases, bitops, rooted cells/upvalues, and both 509-case
stock source and original-bytecode import corpora. Signed x64/Rosetta assertion
profile `eb6eccf2fa21324ee1407cefc5681c5b50cbf3f4e08f26954a70a1acfb226e6f`
also passes both 509-case corpora. Unchanged source is checked, and ordinary
ARM64 assertion profile
`bae8ed29b650033f2bb5399c483c6390118d272a74e9123fd000ad836a8c2385`
is restored without selected test helpers or out-of-line atomic dependencies.
These x64 checks are targeted compatibility evidence, not a full x64 test
umbrella or native Intel performance claim.

Logs: `/tmp/ljmt-u32-survival-matrix.log`,
`/tmp/ljmt-u32-survival-four-profile.log`, the four
`/tmp/ljmt-u32-survival-m7-<profile>.log` files, and corresponding ordinary,
stock source/import and x64 source/import logs. The complete experimental and
bootstrap gates above ran at `0e871779`; after the restrictive rename follow-up,
the affected four-profile FFI suite and compatibility checks were rerun.

## Main checkout

The main `v2.1` checkout was fast-forwarded to `34265a96` and rebuilt with only
`LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL` and
`LUA_USE_ASSERT`. Main profile
`4971e2cb933fefcc20dda437940140d6cf2693f469930bbbd75b3021c68bdba6`
passes all 26 U32 smoke cases, bitops, rooted cells/upvalues and both 509-case
source/import corpora. Code-sign verification passes; selected test-hook
symbols and out-of-line atomic imports are absent. Log:
`/tmp/ljmt-u32-survival-main.log`.

Main artifacts built at `34265a96`:

- Runtime SHA-256: `f24cab7c1065c4defc2fde0e911b779805511973a7e80c1406d97f88d0618960`.
- Archive SHA-256: `438d6ba483f3a75660e683abb63678c1b7ca0377abe11ada4e9f4bd2c334ea8b`.
- Build-signature SHA-256: `3973f2c27ad5dc90f4f3408893d2b77ffdeab43bb2786cf92447fe2f221e2ddd`.

Subsequent validation-note edits leave the tested production source unchanged.

## Remaining boundaries

The U32 CALLM boundary below was subsequently opened with independent source
and lifetime checks; see `arm64-jit-u32-callm-2026-10-27.md` for the implementation,
folded-float compatibility follow-up and validated checkpoint.

At this checkpoint, U32 CALLM producer/consumer chains remained closed: the
CALLM source proof did not yet bind a converted U32 number as its producer. U32 enum results,
general 64-bit arithmetic, aggregate/sret results, arbitrary trace shapes and
full sustained multithreading parity remain outside this increment.

The next bounded FFI extension was to carry the exact certified U32-to-NUM
result as a CALLM producer. Both validators must retain that converted ref
across leave and bind the consumer's final argument to it, preserving the
existing bytecode adjacency, frame, variadic-tail and exact-root proofs. Raw
U32 carriers and unrelated conversions must remain rejected.
