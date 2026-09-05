# FNEW fixture: truthful allocator identity, 2026-09-05

The FNEW fixture now tests allocator refusal without creating impossible arena
ownership state. Previously it set `g->allocf_arena = 0` while retaining
`lj_arena_allocf`, then performed real constructors. Allocation claimed
CONSTRUCT/LINKING, but root publication treated the false identity as EXEMPT
and left those lanes unfinished. A passing run of that setup was not valid
construction or collection evidence.

A helper guarded by `LJ_FUNC_TEST_HELPERS` calls the actual
`func_bump_alloc_ready` predicate. The fixture requires accepted, refused under
false identity, and accepted after restoration. Identity is restored before
assertions or constructors execute. Bump state, allocation counters, pending
roots, and helper counters must remain unchanged through the pure check.

Real VM and C fallback constructors instead use an MT-entry contender with
truthful allocator identity. The VM requires exactly one `lj_tab_new0` helper
call; closure fallback counters and return semantics remain checked. The
canonical M6 case enables `LJ_TAB_TEST_HELPERS` alongside
`LJ_FUNC_TEST_HELPERS` so it runs that exact witness. The separate VM
allocator-identity branch is not dynamically exercised under false identity;
its fallback is witnessed through MT entry, and the C identity predicate is
tested directly.

The fixture also publishes the full SSB carried through preserve-abort before
asserting new-buffer capacity, and explicitly drains carried frontier work
and requests native exit before its one-quantum root-snapshot close. It does
not rewind producer cursors or discard work. All original semantic, capacity,
root-snapshot, and publication assertions remain, with added checks for
unfinished constructor lanes after returned allocation calls.

The exact validation base is
`28de50a622e489019fa22845d6454e029b210582`, including the integrated arena and
scalar-read changes. Only `src/lj_func.c`, `src/lj_func.h`,
`tests/t-jit-fnew-bump.c`, and its M6 registration differ. The
[durable evidence](evidence/fnew-fixture-valid-allocator-2026-09-05) contains the
four-file patch, complete source/object/archive/executable identities in
`final-validation.json`, commands and flags in `results.json`, and text output.
`artifact-manifest.json` hashes the package and this note.

| Check | Result |
| --- | --- |
| Canonical `tools/test.lua m6_jit_fnew_bump`, FUNC+TAB helpers | Pass. |
| Complete fixture with GC2, TAB, FUNC, TRACE, ARENA helpers and `LUA_USE_ASSERT` | Pass. |
| Preserved malformed-identity control against that strict archive | Expected SIGABRT at the unfinished-CONSTRUCT assertion. |
| Normal `lj_func.c` preprocessing with helpers absent | Byte-identical before and after; both SHA-256 values are `b6dc8768ef86ea2306df17e0aa9120bb1cef3233fbca145ec8082f08bedab2b2`. |
| Whitespace validation of the four-file change | Pass. |

The canonical archive SHA-256 is
`bb8cfa4f9590c5dd560c7bfde44d5548b8d2d919195fee4b559375c6948abbd4`;
the strict archive is
`4dd0f6967d4761546d8307cdcd00d065f558b0a55c7eafe7674da80ee13554e1`.
Builds and fixtures ran on CPUs 0–15. No new performance or ASan claim is made.

Earlier SSB-capacity and root-scheduling failures, plus the constructor-stall
diagnosis, remain in the
[corrected AoS study](gc-table-authority-wide-corrected-prototype-2026-09-05.md).
`earlier-boundary-evidence.json` gives exact relative paths and hashes. Those
runs used the earlier d680 baseline or isolated AoS layout; they are distinct
from this final production-base validation. No prototype runtime is included
in this repair.
