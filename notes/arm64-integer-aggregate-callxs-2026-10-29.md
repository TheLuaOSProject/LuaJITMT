# Darwin ARM64 integer aggregate CALLXS arguments (2026-10-29)

This checkpoint admits one narrowly bounded Darwin AAPCS64 argument shape in ARM64 FFI traces: a by-value C struct up to eight bytes, represented in one available GPR, whose direct members are nonoverlapping integer, boolean, or enum scalars. Supported total widths are 1, 2, 4, and 8 bytes. The aggregate must occupy a fixed named parameter and have a free argument GPR. Aggregate variadic-tail arguments are rejected by the recorder. A variadic function signature can still have an aggregate in its fixed named prefix; that combination is not exercised by this fixture. The recorder loads the exact CData payload width, while the original CData TValue remains rooted in its Lua frame slot through native XSAVE/enter/leave.

The classifier rejects unions, nested composites, HFA/float members, bitfields, vectors, references, over-aligned or oversized aggregates, and stack-passed aggregate arguments. It does not add aggregate return support. These limits are deliberate; total size alone is not treated as sufficient ABI evidence.

The semantic and post-register-allocation admission walks independently bind the CData source and CType identity, exact payload XLOAD width and offset, fixed source argument position, CARG/CALLXS association, GPR allocation, and saved snapshot roots. The fixture mutates the payload offset and call argument binding in each pass and requires rejection, in addition to existing wrong-type/source and physical-register negatives. A live peer remains attached during native evidence; per-width native finish counters require selected roots with the matching XLOAD width. After a full GC, the pair case requires a candidate root containing the U64 aggregate CALLXS/XLOAD to be visible, observes another candidate native finish, and requires a native exit. The test does not assert that the post-GC TraceNo is the same generation observed before collection.

Validation: `tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed `arm64-assert`, `arm64e-assert`, `arm64-release`, and `arm64e-release` on the current branch. The contract runs the independent scalar CALLXS admission fixture before the new aggregate fixture in each profile. A separate macOS x86_64 GC64 full build passed with normal warnings. `lj_crecord.c` and `lj_asm.o` also compiled with `-Wall -Wextra -Werror`; the global strict build stopped on the pre-existing `lj_api.c:879` unused-but-set `topofs` warning. Full ARM64 profile output: `/Users/frityet/Scratch/ljmt-int-aggregate-20261029/final/four-profile.log`.

The private CALLXS fixture copies only the readable `[nk,nins)` biased IR range. An earlier test-fixture version copied from the biased `T->ir` base and crashed in `memmove`; the final helper avoids reading below `nk`. For clone-based CALLXS negatives, the narrow test-only initializer remaps root-owned SNAP_FRAME KNUM continuation PCs alongside snapshot footers, while preserving only the independently derived FFI `__call` meta_pc. Its semantic fixture J is embedded in a private `GG_State` so `J2G(J)` remains valid; no production J2G behavior was changed.

Integration runs the new fixture inside the existing `m7_ffi_callxs_arm64_scalar`
profile, so the full experimental gate covers it in its four existing FFI
builds. The focused table-call contract also includes it using its shared
library and profiles. The standalone aggregate contract remains available for
isolated work; the full gate does not invoke that additional four-build matrix.
The integration wiring has passed Lua/shell syntax and whitespace checks;
integrated runtime results will be recorded after the full gate runs.

An ordinary-build edge probe at `cbb707874` also covers signed-byte minimum,
signed half/byte fields with padding, a negative enum, bool/int fields with
padding, `UINT32_MAX`, and a high-bit uint64 payload. On each architecture,
all six cases pass 25 exact expected-result comparisons and interpreted-oracle
comparisons, including full GC, with exactly 1,600 foreign calls and 25 selected
native-root exits per case. This is ordinary ARM64/x86_64 evidence, not an
additional four-profile helper run. The source, universal library and logs are
in `/Users/frityet/Scratch/ljmt-int-aggregate-edges-20261029/`.

The initial integrated full gate at `d0e4ecaa1` passed through the combined Lua
call and numeric-spill contracts, then stopped when the bitops fusion fixture
included the new optional CALLXS initializer without using it. Strict
compilation rejected that unused static function. `edb6cd4dc` makes the helper
`static inline`; no production code changed. The gate is being resumed from
the failed bitops contract with an explicit fresh profile build, retaining the
earlier log. Evidence is under
`/Users/frityet/Scratch/ljmt-vararg-aggregate-full-20261029/`.
