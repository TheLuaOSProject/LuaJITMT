# Durable overflow and Huge-tail regression handoff

The isolated integration on `ff2a6ca0260c0b0c55e2fa708e4f603fd9d4070f` passes
the five strict fixtures and all four affected canonical registrations. No
shared production or test files were edited, and no commit was made. Build and
functional processes have finished; CPU 31 remains free.

Apply `source-and-tests.patch` to that base, or apply `source.patch` followed
by `tests-integration.patch`. The source patch is byte-identical to the frozen
28de Huge-tail study: SHA-256
`dca24e13fdc47d886a1ffc07b42a604c4e430090fa5f2fd9b1f8111678a8ee4a`.
The tests patch is
`9fe099f7b2df7bb78513437f480193f9e7f16985bdc1ff57b8b21ca6a1991637`;
the combined patch is
`2a7f49960f70123898715bd6dc5445cdd776f54d62917a8d3e3890b21443fa49`.
`source-test-manifest.json` has every changed file hash and a successful clean
combined patch dry run. The candidate includes four production files and ten
test/helper/registration files. It adds no VM/JIT emitted reset changes.

The source prerequisite is the complete dense-W plus Huge-tail candidate,
including its two test-only PRE_MODE/POST_MODE hook stages. The existing current
`8cea705d` truthful FNEW fixture/helper repair is preserved by the ff2 base.
Do not apply an obsolete full FNEW fixture from the early dense prototype.
The test patch extends current fixtures rather than adding copied old fixtures.

The only obsolete terminal-exhaustion assumptions found in the focused source
inventory were two inline UINT32_MAX seeds in
`tests/t-gc2-sweep-table-coalescing.c` and the dirty-saturation portion of
`tests/t-gc2-traverse.c`. They now exhaust the full era/serial authority. The
existing cycle-namespace veto test remains unchanged. Ordinary inline overflow
is separately proved to promote and continue collection without a global veto.

| File / canonical case | Durable coverage | Build and fixture requirements |
| --- | --- | --- |
| `tests/t-gc2-sweep-table-coalescing.c` / `m3_gc2_sweep_table_coalescing` | Existing graph/coalescing cases; small/Huge old inline and wide scanners in legacy/exact modes; paused mode publishers with peer progress; twelve real full collections per kind; post-store promotion while calloc is denied; full namespace containment | Runtime and fixture: `-DLJ_GC2_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT`; fixture additionally `-DLJ_TEST_WRAP_CALLOC`, GNU ld `--wrap=calloc`; existing Linux guard retained |
| `tests/t-gc2-traverse.c` / existing `m3_gc2_scaffold` traversal entry | Full namespace veto; real protected Huge-tail W during header-only DEFER_FREE in MARK/WEAK/SWEEP; protected small W during FREE completion | Existing `-DLJ_GC2_TEST_HELPERS`; no new wrappers; protection probes guarded by `LJ_TARGET_LINUX`; ordinary token tests retain inline coverage |
| `tests/t-x64-tnew-empty-inline.c` / `m5_x64_tnew_empty_inline` | Existing TNEW cases plus cells 1536/1537, both pending-token refusals, inline reset with persistent W and subsequent promotion, neighboring proof/token guards | Runtime and fixture now `-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS`; no wrappers |
| `tests/t-jit-fnew-bump.c` / `m6_jit_fnew_bump` | Existing repaired full FNEW; emitted two-start reuse at cells 1536/1537; persistent W, unchanged token generations, both pending-token refusals, neighboring guards, no unfinished constructors | Existing `-DLJ_FUNC_TEST_HELPERS -DLJ_TAB_TEST_HELPERS`; no registration flag change or wrappers |
| `tests/t-arena-huge-tail.c` / new `m2_arena_huge_tail` | All logical boundary bytes, no payload/W overlap, mincore untouched tail, rejected out-of-bounds readers, private resize, published traversable refusal, old plain reader/deferred handoff, real map and locator failures, transfer/fini, exact whole-map unmaps | Runtime and fixture: `-DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT`; GNU ld wraps `mmap`, `mmap64`, `munmap`, `calloc`, `free`; Linux only; included in M2 aggregate order |

All fixtures link the test runtime archive plus `-lm -ldl -pthread` on this
Linux host. Canonical default fixture flags are
`-std=gnu11 -O2 -Wall -Wextra -Werror -mcx16`. The two new small headers,
`tests/lib/gc2_wide_fixture_helpers.h` and
`tests/lib/gc2_wide_reuse_helpers.h`, provide admitted/private test namespace
compression and neighboring proof/token guards. They are test-only, do not
allocate on production paths, and do not change production reset behavior.

`strict-results.json` records all five successful compilation and execution
commands against the static archive built with GC2, TAB, ARENA, FUNC, TRACE
helpers and `LUA_USE_ASSERT`. Full traversal completes in 0.150 s; full repaired
FNEW plus high-cell controls completes in 0.066 s. `strict-snapshot.json` retains
the exact archive, runtime, and fixture hashes at that boundary.

`canonical-results.json` records each real runner command with `JOBS=4`, all
on CPUs 0-15, and the complete compiler/fixture output. The four cases complete
successfully in 27.3, 27.7, 13.7, and 13.8 seconds including their builds and
required default-build restoration. The new M2 case is in `M2_ORDER`; the
extended M3/M5/M6 cases retain their existing aggregate registrations. No full
M3 aggregate was rerun just to repeat its unrelated dependencies; its complete
traversal fixture was executed directly under strict helpers/assertions.

After these runs, one helper precondition comment was clarified to require
retained storage authority and exclusion of concurrent proof updates. No
executable statement changed. `final-validation.json` records the exact
comment-only difference against `strict-snapshot.json`; all other fixture and
production hashes match their tested snapshots. There were no failed builds,
test executions, or timeouts in this latest-base integration.

No timing or new ASan run was performed in this integration tree. The immutable
storage-source ASan/stock/negative-control and 380-process cost evidence remains
at `/tmp/lj-dense-huge-tail-20260905-djqhqfe8/audit.md`, with its explicit 28de
source qualification. That study's three bad-source controls demonstrate a
last-user-byte W overwrite, an invalid old realloc extent decision, and an
actual protected-W fault from a header-only consumer. The current tests preserve
those same causal storage/protection assertions. The newly repaired full FNEW
run here is an additional latest-base strict/canonical result, not a claim of
having rerun that full fixture under the older ASan archive.

The measured storage choice remains a tradeoff: removing each Huge W heap
allocation saves its allocator call/chunk, while promotion can add a 4 KiB page
and 16 size residues per 64 KiB quantum add a full virtual mapping quantum.
Published TRAVERSABLE realloc still refuses; only private direct resize is
accepted in the geometry controls. Full namespace exhaustion, general table
fallback waits, plain-arena writer waits, mapping/page-fault progress and the
broader lockless-runtime objective remain outside this completed handoff.
