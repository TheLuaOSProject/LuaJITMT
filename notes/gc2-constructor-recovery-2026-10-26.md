# GC2 constructor/recovery overlap (2026-10-26)

## Observed failure and scope

The combined experimental gate at boxed-result checkpoint
`9651f9372cb815f3e2b00980ad48c96c23e35abd` aborted in the ARM64 assertion
`TNEW` fixture at array capacity 4. Capacity 0 had passed. Profile
`6b56498951aed613af6c548f346ac09226bd5d6c07550cf5a36a7756453d5ca2`
reported `gc_linkobj_new_pending_at: pending constructed root membership
commit lost`. The log is `/tmp/ljmt-i64-experimental.log`; the enclosing
matrix log is `/tmp/ljmt-i64-matrix.log`. Bootstrap and stock-corpus stages
were not reached. The focused four-profile boxed-I64/U64 contract passed
before this failure; see the [boxed-result note](arm64-jit-boxed64-results-2026-10-26.md).

The shared GC production source was unchanged from main `45b347f8` at that
failure. Ten subsequent unchanged capacity-4 processes passed under profile
`227828d41ff34606d8624abc830f7165e0c1750d4f8194a13969688b23dbb522`
(`/tmp/ljmt-pending-root-current.log`). These passes do not explain the abort.
The intermittent failure did not capture its lifetime lane, so neither of
the deterministic races below is claimed as its uniquely identified cause.

## Ownership correction

An initialized constructor retains its exact allocation base and root
`LINKING` identity. Recovery can borrow lifetime `CONSTRUCT` in two ways:
a counted publisher uses `RECOVERY`, while a recovery drainer uses `MUTATING`.
Both borrowers already restore `LIVE` when constructor commit or abandonment
has changed the root to `MEMBER` or `NONE`.

The constructor previously rejected a drainer's `MUTATING` lane, and could
also lose a sampled `CONSTRUCT -> LIVE` CAS to a recovery publisher after its
root edge was already visible. Commit now rechecks a lost CAS and accepts
either recovery borrower while preserving the borrower's lifetime ownership.
The private fresh-object root validator still requires the exact base/cell,
READY/TRAVERSABLE allocation and root `LINKING`. Source review found that the
only existing-allocation `CONSTRUCT -> MUTATING` transition is recovery drain;
other mutation claims start at `LIVE` or `FREE`.

Abandonment similarly remembers whether this invocation already won
`LINKING -> NONE`, so a lost lifetime CAS can retry without losing a completed
root cancellation. `MUTATING + NONE` without that retained proof remains
rejected. Neither constructor path takes lifetime ownership from recovery.

Recovery's release still requires its initial held-lane CAS to succeed.
After successfully surrendering the lane, its symmetric `CONSTRUCT -> LIVE`
repair is best-effort: another authorized owner may already have advanced
lifetime. Reading that later owner's lane and treating it as corruption was
incorrect. Recovery count reservation, side-state publication/coalescing,
drain claiming and replay are unchanged. No lock or new lifetime state is
introduced.

## Deterministic regression evidence

The test-only constructor hook pauses after sampling `CONSTRUCT`, then a
real recovery publisher takes `RECOVERY` and parks before its side item is
published. Resuming the constructor first forces the stale-sample CAS loss.
At old production behavior plus pause hooks, profile
`72aa208920c45162dc02cc540ac63eac0f3a672663e4347155c728d35c5acae9`
reproduced both the lost-commit assertion and the lost-abandon result
(`/tmp/ljmt-constructor-red-sampled-{commit,abandon}.log`). The initial drain
test versions instead stopped at an incorrect expected-lane assertion; those
runs are not behavioral red evidence for drain overlap.

The corrected tests also park a real drainer after `PENDING -> CLAIMED` and
complete actual constructor commit/abandon while it owns `MUTATING`. They
check preserved ownership, eventual `LIVE/IDLE`, exact recovery counts,
one root occurrence for committed objects, and clean free after abandonment.
A separate negative checks that unrelated `MUTATING + NONE` is rejected.

Two additional release-repair cases park a real drainer immediately after
its held-lane release restores `CONSTRUCT`. The actual constructor then
commits or abandons, and a fixture-owned `LIVE -> MUTATING` CAS models a
subsequent layout mutator's claim before its side-owner validation. That
validation would reject `CLAIMED` and restore `LIVE`; no mutation body is
admitted or exercised by this fixture. The old drainer must preserve that owner's
lane, avoid false recovery failure, and retain exactly one pending identity.
After the modeled owner releases, the pending identity must drain normally.
This explicitly models the second claimant; it is not evidence of a second
attached TG executing a complete mutation operation.

Initial repaired ARM64 assertion profile
`8e5e36edc7cae2e3b3eb796ad729da105878f440ca9530940c7b77684d54866b`
passed all five isolated cases, the complete recovery fixture, both pending
root fixtures, native TNEW capacities 0/4/8257, and ten additional capacity-4
processes (`/tmp/ljmt-constructor-green-v2.log`). The first repaired run
stopped on a stale test expectation that publisher release retained RECOVERY;
the corrected test checks RECOVERY before release and LIVE afterward.

The new cases run by default in `m3_gc2_recovery`. The focused
`tools/ci/gc2_constructor_recovery_contract.sh` binds runtime/archive/fixture
identity across ARM64 and arm64e assertion/release, signed x64/Rosetta
assertion/release, and ARM64/x64 paranoia profiles. All eight passed the full
recovery fixture (including both post-release cases) and both pending-root
fixtures in `/tmp/ljmt-constructor-eight-profiles.log`:

| Target/runtime | Profile |
| --- | --- |
| ARM64 assertion | `89333fbf366718505350116422a4a6a1a1c655fb37152a81c45544a3bc9a5077` |
| arm64e assertion/BTI | `b9454641cc58e8a484441d9cf838df0aee03a2d6f05bb5a8d7b0b34de9672e4c` |
| ARM64 release | `9bf7260fb68b46ea05e4d535c147b86b5ac725dfc8dbc6150ab9965e858925d1` |
| arm64e release/BTI | `53c75dd80e04d972ad8edfa2342e4f7414fd50ef61696b9b4dee42eabffba4cb` |
| x64 assertion | `640b982c1534862ea50de6af760f5ccc24664c01a453417d0c9fd9e4f24c1df1` |
| x64 release | `bbf92cf87d134266c4b81b51ad74b0cffe610a92d68bafdcac9e4f2d992487f2` |
| ARM64 paranoia | `508bd5bf43fb1afec5e5a57677de9a0a01cb1ca0bb2fb05af972ae346bda3361` |
| x64 paranoia | `e84663cbed7a92ba3085d3ed15195a7daf7e5c178324968d94bab368f3857def` |

## Renewed combined validation

Frozen source `2ba1e91b87bd69c49bbd703cd7bb04cb9934531f` passes the complete
experimental and bootstrap gates. In particular, native TNEW capacities
0/4/8257 pass on both targets:

- ARM64 assertion profile
  `be52ec75533d86a56feb9962c9784485d49390f030f6b2d9892e7b2456364112`:
  7,458 / 5,122 / 8,830 native observations respectively.
- arm64e assertion/BTI profile
  `2bad1ae9b1d4eae6856cbcdcea948951400427a7fc513586be88485c0468ba37`:
  4,627 / 7,350 / 9,913 native observations respectively.

The four-profile boxed-result contract, all four M7 scalar CALLXS profiles
inside the experimental gate, and both 509-case source/import stock corpora
on ordinary ARM64 and signed x64/Rosetta also pass. The main checkout was
rebuilt and rechecked without test helpers. Exact profiles, artifact hashes
and logs are in the [boxed-result evidence](arm64-jit-boxed64-results-2026-10-26.md).

These passing runs do not retrospectively identify the precise lifetime lane
at the original intermittent abort. The full ARM64 port, long-duration
concurrency and weak-memory coverage remain incomplete.
